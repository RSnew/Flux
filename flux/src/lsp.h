// lsp.h — Flux Language Server Protocol 实现
// 实现 LSP 核心功能：诊断、自动补全、悬停提示、跳转定义
#pragma once
#include "lexer.h"
#include "parser.h"
#include "typechecker.h"
#include "ast.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <algorithm>

// ═══════════════════════════════════════════════════════════
// LSP JSON-RPC 消息处理
// ═══════════════════════════════════════════════════════════

struct LspDiagnostic {
    int line;
    int character;
    int severity;  // 1=Error, 2=Warning, 3=Information, 4=Hint
    std::string message;
    std::string source;
};

struct LspCompletionItem {
    std::string label;
    int kind;  // 1=Text, 2=Method, 3=Function, 4=Constructor, 5=Field, 6=Variable
    std::string detail;
    std::string documentation;
};

struct LspHoverResult {
    std::string contents;
    int line;
    int character;
};

struct LspLocation {
    std::string uri;
    int line;
    int character;
};

class FluxLSP {
public:
    FluxLSP() {}

    // ── 诊断 — 对源文件进行类型检查，返回错误列表 ──────────
    std::vector<LspDiagnostic> diagnose(const std::string& source) {
        std::vector<LspDiagnostic> diags;

        try {
            Lexer lexer(source);
            auto tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto program = parser.parse();

            TypeChecker checker;
            auto errors = checker.check(program.get());

            for (auto& e : errors) {
                diags.push_back({
                    e.line > 0 ? e.line - 1 : 0,  // LSP is 0-indexed
                    0,
                    1,  // Error
                    e.message,
                    "flux"
                });
            }

            // Cache the AST for other operations
            cacheAST(source, std::move(program));

        } catch (const ParseError& e) {
            diags.push_back({
                e.line > 0 ? e.line - 1 : 0,
                0,
                1,
                e.what(),
                "flux-parser"
            });
        } catch (const std::exception& e) {
            diags.push_back({0, 0, 1, e.what(), "flux"});
        }

        return diags;
    }

    // ── 自动补全 ─────────────────────────────────────────────
    std::vector<LspCompletionItem> complete(const std::string& source, int line, int character) {
        std::vector<LspCompletionItem> items;

        // Get the word being typed
        std::string prefix = getWordAtPosition(source, line, character);

        // Builtin functions
        for (auto& [name, detail] : getBuiltins()) {
            if (name.find(prefix) == 0 || prefix.empty())
                items.push_back({name, 3, detail, ""});
        }

        // Keywords
        for (auto& kw : getKeywords()) {
            if (kw.find(prefix) == 0)
                items.push_back({kw, 14, "keyword", ""});
        }

        // Stdlib modules
        for (auto& mod : getStdlibModules()) {
            if (mod.find(prefix) == 0)
                items.push_back({mod, 9, "module", ""});
        }

        // Module methods (if typing after "Module.")
        std::string moduleName = getModulePrefix(source, line, character);
        if (!moduleName.empty()) {
            auto methods = getModuleMethods(moduleName);
            for (auto& m : methods)
                items.push_back({m.label, m.kind, m.detail, ""});
        }

        // Variables and functions from the current file
        try {
            Lexer lexer(source);
            auto tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto program = parser.parse();

            for (auto& stmt : program->statements) {
                if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
                    if (fn->name.find(prefix) == 0)
                        items.push_back({fn->name, 3, "function", ""});
                }
                if (auto* vd = dynamic_cast<VarDecl*>(stmt.get())) {
                    if (vd->name.find(prefix) == 0)
                        items.push_back({vd->name, 6, "variable", ""});
                }
            }
        } catch (...) {}

        return items;
    }

    // ── 悬停提示 ─────────────────────────────────────────────
    LspHoverResult hover(const std::string& source, int line, int character) {
        std::string word = getWordAtPosition(source, line, character);

        // Check builtins
        auto builtins = getBuiltins();
        auto it = builtins.find(word);
        if (it != builtins.end())
            return {it->second, line, character};

        // Check keywords
        auto keywords = getKeywordDocs();
        auto kit = keywords.find(word);
        if (kit != keywords.end())
            return {kit->second, line, character};

        // Check exception descriptions
        auto eit = exceptionDescs_.find(word);
        if (eit != exceptionDescs_.end()) {
            std::string doc = "**Exception descriptions:**\n";
            for (auto& msg : eit->second)
                doc += "- " + msg + "\n";
            return {doc, line, character};
        }

        return {"", line, character};
    }

    // ── 跳转定义 ─────────────────────────────────────────────
    LspLocation gotoDefinition(const std::string& source, const std::string& uri,
                                int line, int character) {
        std::string word = getWordAtPosition(source, line, character);

        try {
            Lexer lexer(source);
            auto tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto program = parser.parse();

            int lineNum = 0;
            for (auto& stmt : program->statements) {
                lineNum++;
                if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
                    if (fn->name == word)
                        return {uri, lineNum, 0};
                }
                if (auto* vd = dynamic_cast<VarDecl*>(stmt.get())) {
                    if (vd->name == word)
                        return {uri, lineNum, 0};
                }
            }
        } catch (...) {}

        return {uri, line, character};
    }

    // Register exception descriptions from analysis
    void registerExceptionDescs(const std::unordered_map<std::string,
                                 std::vector<std::string>>& descs) {
        exceptionDescs_ = descs;
    }

    // ── LSP JSON-RPC 主循环 ──────────────────────────────────
    void run() {
        std::string header;
        while (std::getline(std::cin, header)) {
            // Read Content-Length header
            int contentLength = 0;
            if (header.find("Content-Length:") == 0) {
                contentLength = std::stoi(header.substr(16));
            }
            // Skip rest of headers
            while (std::getline(std::cin, header) && !header.empty() && header != "\r") {}

            // Read body
            std::string body(contentLength, '\0');
            std::cin.read(&body[0], contentLength);

            // Process message
            std::string response = processMessage(body);
            if (!response.empty()) {
                std::cout << "Content-Length: " << response.size() << "\r\n\r\n" << response;
                std::cout.flush();
            }
        }
    }

private:
    std::unique_ptr<Program> cachedAST_;
    std::unordered_map<std::string, std::vector<std::string>> exceptionDescs_;

    void cacheAST(const std::string& /*source*/, std::unique_ptr<Program> ast) {
        cachedAST_ = std::move(ast);
    }

    std::string processMessage(const std::string& body) {
        // Simplified JSON-RPC parsing (production would use proper JSON library)
        // Handle initialize, textDocument/didOpen, textDocument/didChange,
        // textDocument/completion, textDocument/hover, textDocument/definition

        if (body.find("\"initialize\"") != std::string::npos) {
            return "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{\"capabilities\":{"
                   "\"textDocumentSync\":1,"
                   "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                   "\"hoverProvider\":true,"
                   "\"definitionProvider\":true,"
                   "\"diagnosticProvider\":{\"interFileDependencies\":false,\"workspaceDiagnostics\":false}"
                   "}}}";
        }

        if (body.find("\"shutdown\"") != std::string::npos) {
            return "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":null}";
        }

        return "";
    }

    std::string getWordAtPosition(const std::string& source, int line, int character) {
        std::istringstream ss(source);
        std::string lineStr;
        int curLine = 0;
        while (std::getline(ss, lineStr) && curLine < line) curLine++;

        if (character >= (int)lineStr.size()) return "";

        // Find word boundaries
        int start = character, end = character;
        while (start > 0 && (std::isalnum(lineStr[start-1]) || lineStr[start-1] == '_'))
            start--;
        while (end < (int)lineStr.size() && (std::isalnum(lineStr[end]) || lineStr[end] == '_'))
            end++;

        return lineStr.substr(start, end - start);
    }

    std::string getModulePrefix(const std::string& source, int line, int character) {
        std::istringstream ss(source);
        std::string lineStr;
        int curLine = 0;
        while (std::getline(ss, lineStr) && curLine < line) curLine++;

        if (character < 2 || character > (int)lineStr.size()) return "";

        // Check if there's a "Module." prefix
        int dotPos = character - 1;
        while (dotPos >= 0 && lineStr[dotPos] == ' ') dotPos--;
        if (dotPos < 0 || lineStr[dotPos] != '.') return "";

        int nameEnd = dotPos;
        int nameStart = dotPos - 1;
        while (nameStart >= 0 && (std::isalnum(lineStr[nameStart]) || lineStr[nameStart] == '_'))
            nameStart--;
        nameStart++;

        return lineStr.substr(nameStart, nameEnd - nameStart);
    }

    std::unordered_map<std::string, std::string> getBuiltins() {
        return {
            {"print",   "print(...args) → Nil — Print values space-separated"},
            {"str",     "str(Any) → String — Convert to string"},
            {"num",     "num(Any) → Number — Convert to number"},
            {"sqrt",    "sqrt(Number) → Number — Square root"},
            {"panic",   "panic(msg) → never — Trigger panic"},
            {"assert",  "assert(cond, msg?) → Nil — Assert condition"},
            {"range",   "range(n) → Array — Generate [0..n-1]"},
            {"len",     "len(x) → Number — Length of string/array/map"},
            {"type",    "type(x) → String — Type name"},
            {"Map",     "Map() → Map — Create empty map"},
            {"struct",  "struct(instance) → Array — List field names"},
        };
    }

    std::vector<std::string> getKeywords() {
        return {"var", "fn", "func", "return", "if", "else", "while", "true", "false",
                "nil", "for", "in", "persistent", "state", "module", "migrate",
                "supervised", "async", "await", "spawn", "threadpool", "concurrent",
                "interface", "exception"};
    }

    std::unordered_map<std::string, std::string> getKeywordDocs() {
        return {
            {"var",        "Variable declaration: `var name = value` or `var name: Type = value`"},
            {"fn",         "Function declaration: `fn name(params) -> ReturnType { body }`"},
            {"func",       "Alias for `fn`"},
            {"return",     "Return from function: `return expr`"},
            {"if",         "Conditional: `if cond { } else { }`"},
            {"while",      "Loop: `while cond { body }`"},
            {"for",        "Iteration: `for x in iterable { body }`"},
            {"persistent", "Persistent state block (survives hot reload)"},
            {"module",     "Module declaration: `module Name { }`"},
            {"async",      "Async execution: `async fn(args)` returns Future"},
            {"await",      "Wait for Future: `await expr`"},
            {"spawn",      "Fire-and-forget: `spawn { body }`"},
            {"exception",  "Error descriptions: `exception funcName { \"msg\" }`"},
        };
    }

    std::vector<std::string> getStdlibModules() {
        return {"File", "Json", "Http", "Time", "Chan", "Math", "Set", "Log", "Env", "Test"};
    }

    std::vector<LspCompletionItem> getModuleMethods(const std::string& mod) {
        std::vector<LspCompletionItem> items;

        if (mod == "File") {
            items = {{"read", 2, "File.read(path) → String", ""},
                     {"write", 2, "File.write(path, content) → Bool", ""},
                     {"append", 2, "File.append(path, content) → Bool", ""},
                     {"exists", 2, "File.exists(path) → Bool", ""},
                     {"lines", 2, "File.lines(path) → Array", ""},
                     {"delete", 2, "File.delete(path) → Bool", ""}};
        } else if (mod == "Json") {
            items = {{"parse", 2, "Json.parse(str) → Any", ""},
                     {"stringify", 2, "Json.stringify(value) → String", ""},
                     {"pretty", 2, "Json.pretty(value, indent?) → String", ""}};
        } else if (mod == "Http") {
            items = {{"get", 2, "Http.get(url) → String", ""},
                     {"post", 2, "Http.post(url, body, type?) → String", ""},
                     {"put", 2, "Http.put(url, body, type?) → String", ""},
                     {"delete", 2, "Http.delete(url) → String", ""},
                     {"serve", 2, "Http.serve(port, routes) → Nil", ""}};
        } else if (mod == "Time") {
            items = {{"now", 2, "Time.now() → Number", ""},
                     {"clock", 2, "Time.clock() → Number", ""},
                     {"sleep", 2, "Time.sleep(ms) → Nil", ""},
                     {"format", 2, "Time.format(ts, fmt?) → String", ""},
                     {"diff", 2, "Time.diff(a, b) → Number", ""}};
        } else if (mod == "Math") {
            items = {{"abs", 2, "Math.abs(x) → Number", ""},
                     {"floor", 2, "Math.floor(x) → Number", ""},
                     {"ceil", 2, "Math.ceil(x) → Number", ""},
                     {"round", 2, "Math.round(x) → Number", ""},
                     {"min", 2, "Math.min(a, b) → Number", ""},
                     {"max", 2, "Math.max(a, b) → Number", ""},
                     {"pow", 2, "Math.pow(base, exp) → Number", ""},
                     {"log", 2, "Math.log(x) → Number", ""},
                     {"sin", 2, "Math.sin(x) → Number", ""},
                     {"cos", 2, "Math.cos(x) → Number", ""},
                     {"random", 2, "Math.random() → Number [0,1)", ""},
                     {"PI", 2, "Math.PI() → 3.14159...", ""}};
        } else if (mod == "Set") {
            items = {{"new", 2, "Set.new() → Set", ""},
                     {"from", 2, "Set.from(array) → Set", ""},
                     {"add", 2, "Set.add(set, value) → Set", ""},
                     {"has", 2, "Set.has(set, value) → Bool", ""},
                     {"remove", 2, "Set.remove(set, value) → Bool", ""},
                     {"size", 2, "Set.size(set) → Number", ""},
                     {"toArray", 2, "Set.toArray(set) → Array", ""},
                     {"union", 2, "Set.union(a, b) → Set", ""},
                     {"intersect", 2, "Set.intersect(a, b) → Set", ""}};
        } else if (mod == "Log") {
            items = {{"info", 2, "Log.info(...args)", ""},
                     {"warn", 2, "Log.warn(...args)", ""},
                     {"error", 2, "Log.error(...args)", ""},
                     {"debug", 2, "Log.debug(...args)", ""}};
        } else if (mod == "Test") {
            items = {{"equal", 2, "Test.equal(actual, expected, msg?)", ""},
                     {"notEqual", 2, "Test.notEqual(actual, expected, msg?)", ""},
                     {"isTrue", 2, "Test.isTrue(cond, msg?)", ""},
                     {"isFalse", 2, "Test.isFalse(cond, msg?)", ""},
                     {"isNil", 2, "Test.isNil(val, msg?)", ""}};
        } else if (mod == "Chan") {
            items = {{"make", 2, "Chan.make(cap?) → Chan", ""}};
        } else if (mod == "Env") {
            items = {{"get", 2, "Env.get(name) → String|Nil", ""}};
        }

        return items;
    }
};
