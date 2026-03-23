#include "lexer.h"
#include "parser.h"
#include "typechecker.h"
#include "interpreter.h"
#include "compiler.h"
#include "vm.h"
#include "formatter.h"
#include "watcher.h"
#include "pkgmgr.h"
#include "lsp.h"
#include "debugger.h"
#include "gc.h"
#include "hir.h"
#include "mir.h"
#include "jit.h"
#include "codegen.h"
#include "hotswap.h"
#include "profiler.h"
#include "fluz.h"
#include "llvm_jit.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <functional>

// ── ANSI 颜色 ─────────────────────────────────────────────
#define CLR_RESET  "\033[0m"
#define CLR_BOLD   "\033[1m"
#define CLR_CYAN   "\033[36m"
#define CLR_GREEN  "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_RED    "\033[31m"
#define CLR_GRAY   "\033[90m"

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

static void printBanner(const std::string& file) {
    std::cout << CLR_BOLD CLR_CYAN
              << "╔══════════════════════════════════════╗\n"
              << "║       Flux Language Runtime          ║\n"
              << "║       Hot Reload: ON  🔥             ║\n"
              << "╚══════════════════════════════════════╝\n"
              << CLR_RESET;
    std::cout << CLR_GRAY << "  Watching: " << file << CLR_RESET << "\n\n";
}

// ── 编译并执行（热更新时复用解释器状态）──────────────────
static bool g_useVM  = false;   // --vm 标志
static bool g_noTest = false;   // --no-test 标志

static void runSource(const std::string& source, Interpreter& interp, bool isReload) {
    interp.setNoTest(g_noTest);
    try {
        Lexer  lexer(source);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        // ── 类型检查（在执行前）──────────────────────────
        TypeChecker checker;
        auto typeErrors = checker.check(program.get());
        if (!typeErrors.empty()) {
            std::cerr << CLR_RED CLR_BOLD
                      << "\n🔴 Type errors found — execution blocked:\n"
                      << CLR_RESET;
            for (auto& e : typeErrors) {
                std::cerr << CLR_RED << "   ";
                if (e.line > 0) std::cerr << "line " << e.line << ": ";
                std::cerr << e.message << CLR_RESET << "\n";
            }
            std::cerr << "\n";
            return;
        }

        if (isReload) {
            std::cout << CLR_YELLOW << "\n🔄 Hot reload detected..." << CLR_RESET << "\n";
            std::cout << CLR_GRAY   << "─────────────────────────────\n" << CLR_RESET;
        }

        if (g_useVM) {
            // ── 字节码 VM 路径 ─────────────────────────────
            interp.initProgram(program.get());
            Chunk chunk;
            Compiler compiler(chunk, interp);
            compiler.compile(program.get());
            VM vm(interp);
            vm.compileFunctions();
            vm.run(chunk, interp.globalEnv_);
        } else {
            interp.execute(program.get());
        }

        if (isReload)
            std::cout << CLR_GREEN << "✅ Hot reload complete\n" << CLR_RESET;

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "\n❌ Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "\n❌ Runtime error: " << e.what() << CLR_RESET << "\n";
    }
}

// ── REPL 专用执行（保留跨调用状态）────────────────────────
// keepAlive: REPL 会话中所有 Program 的生命周期容器，
// 防止 FnDecl* 等原始指针在 Interpreter::functions_ 中悬空。
static void runSourceRepl(const std::string& source,
                          Interpreter& interp,
                          std::vector<std::unique_ptr<Program>>& keepAlive) {
    try {
        Lexer  lexer(source);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        TypeChecker checker;
        auto typeErrors = checker.check(program.get());
        if (!typeErrors.empty()) {
            for (auto& e : typeErrors) {
                std::cerr << CLR_RED << "  ";
                if (e.line > 0) std::cerr << "line " << e.line << ": ";
                std::cerr << e.message << CLR_RESET << "\n";
            }
            return;
        }

        interp.executeRepl(program.get());
        // 将 program 所有权转移到 keepAlive，防止 AST 被销毁
        keepAlive.push_back(std::move(program));

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "❌ Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "❌ Runtime error: " << e.what() << CLR_RESET << "\n";
    }
}

// ═══════════════════════════════════════════════════════════
// flux run <file> — 运行一次，无热更新循环
// ═══════════════════════════════════════════════════════════
static int cmdRun(const std::string& filepath) {
    Interpreter interp;
    try {
        runSource(readFile(filepath), interp, false);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
        return 1;
    }
}

// ═══════════════════════════════════════════════════════════
// flux check <file> — 类型检查，不执行
// ═══════════════════════════════════════════════════════════
static int cmdCheck(const std::string& filepath, bool jsonOutput = false) {
    try {
        std::string src = readFile(filepath);

        Lexer  lexer(src);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        TypeChecker checker;
        auto errors = checker.check(program.get());

        if (jsonOutput) {
            // JSON 结构化输出
            std::cout << "{\"file\":\"" << filepath << "\",\"errors\":[\n";
            for (size_t i = 0; i < errors.size(); i++) {
                std::cout << "  " << errors[i].toJson();
                if (i + 1 < errors.size()) std::cout << ",";
                std::cout << "\n";
            }
            std::cout << "]}\n";
            return errors.empty() ? 0 : 1;
        }

        if (errors.empty()) {
            std::cout << CLR_GREEN << "✅ No type errors found in: "
                      << filepath << CLR_RESET << "\n";
            return 0;
        }

        std::cerr << CLR_RED CLR_BOLD << "Type errors in: "
                  << filepath << CLR_RESET << "\n";
        for (auto& e : errors) {
            std::cerr << CLR_RED << "  ";
            if (e.line > 0) std::cerr << "line " << e.line << ": ";
            std::cerr << e.message << CLR_RESET << "\n";
        }
        return 1;

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
        return 1;
    }
}

// ═══════════════════════════════════════════════════════════
// flux fmt <file> [--write] — 格式化 Flux 源代码
//   默认输出到 stdout；--write / -w 覆写原文件
// ═══════════════════════════════════════════════════════════
static int cmdFmt(const std::string& filepath, bool writeBack) {
    try {
        std::string src = readFile(filepath);

        Lexer  lexer(src);
        auto   tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        Formatter fmt;
        std::string formatted = fmt.format(program.get());

        if (writeBack) {
            std::ofstream f(filepath);
            if (!f) {
                std::cerr << CLR_RED << "Cannot write to: " << filepath
                          << CLR_RESET << "\n";
                return 1;
            }
            f << formatted;
            std::cout << CLR_GREEN << "✅ Formatted: " << filepath
                      << CLR_RESET << "\n";
        } else {
            std::cout << formatted;
        }
        return 0;

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
        return 1;
    }
}

// ═══════════════════════════════════════════════════════════
// REPL 多行支持：统计 { } 深度，决定何时执行
// ═══════════════════════════════════════════════════════════
static void updateBraceDepth(const std::string& line, int& depth) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"' && (i == 0 || line[i-1] != '\\')) { inStr = !inStr; continue; }
        if (inStr) continue;
        if (c == '/' && i + 1 < line.size() && line[i+1] == '/') break; // 注释
        if (c == '{') depth++;
        else if (c == '}') depth--;
    }
}

// ── REPL 模式（改进：多行输入 + 命令历史）──────────────
static void cmdRepl() {
    Interpreter interp;
    std::cout << CLR_BOLD CLR_CYAN
              << "Flux REPL  (Ctrl+D to exit)\n"
              << CLR_RESET;
    std::cout << CLR_GRAY
              << "Multi-line blocks supported. Special commands:\n"
              << "  .clear   — reset interpreter state\n"
              << "  .history — show command history\n"
              << "  .help    — show this help\n\n"
              << CLR_RESET;

    // keepAlive: 让 Program 的 AST 在整个 REPL 会话中存活，
    // 避免 interpreter 的 functions_ 持有悬空 FnDecl*。
    std::vector<std::unique_ptr<Program>> keepAlive;

    std::vector<std::string> history;
    std::string buffer;
    int braceDepth = 0;

    while (true) {
        // 主提示符 vs 续行提示符
        if (buffer.empty()) {
            std::cout << CLR_CYAN << "flux> " << CLR_RESET;
        } else {
            std::cout << CLR_CYAN << "...   " << CLR_RESET;
        }
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) break;  // Ctrl+D

        if (line.empty() && buffer.empty()) continue;

        // ── 特殊 REPL 命令 ─────────────────────────────
        if (buffer.empty()) {
            if (line == ".clear") {
                interp = Interpreter();
                keepAlive.clear();  // AST 可以安全销毁（functions_ 已随 interp 重置）
                std::cout << CLR_GRAY << "(Interpreter state cleared)\n" << CLR_RESET;
                continue;
            }
            if (line == ".history") {
                if (history.empty()) {
                    std::cout << CLR_GRAY << "(no history)\n" << CLR_RESET;
                } else {
                    for (size_t i = 0; i < history.size(); i++) {
                        // 多行条目缩进显示
                        std::string entry = history[i];
                        bool first = true;
                        std::istringstream ss(entry);
                        std::string part;
                        while (std::getline(ss, part)) {
                            std::cout << CLR_GRAY;
                            if (first) std::cout << "[" << (i + 1) << "] ";
                            else       std::cout << "    ";
                            std::cout << CLR_RESET << part << "\n";
                            first = false;
                        }
                    }
                }
                continue;
            }
            if (line == ".help") {
                std::cout << CLR_GRAY
                          << "  .clear   — reset interpreter state\n"
                          << "  .history — show command history\n"
                          << "  .help    — show this help\n"
                          << CLR_RESET;
                continue;
            }
        }

        // ── 积累输入 ────────────────────────────────────
        if (!buffer.empty()) buffer += "\n";
        buffer += line;
        updateBraceDepth(line, braceDepth);

        // 所有大括号已闭合 → 执行
        if (braceDepth <= 0) {
            braceDepth = 0;
            if (!buffer.empty()) {
                history.push_back(buffer);
                runSourceRepl(buffer, interp, keepAlive);
            }
            buffer.clear();
        }
    }

    // 若有未完成的输入（Ctrl+D 打断），尝试执行
    if (!buffer.empty()) {
        history.push_back(buffer);
        runSource(buffer, interp, false);
    }

    std::cout << "\n" << CLR_GRAY << "Bye!\n" << CLR_RESET;
}

// ── 文件执行 + 热更新模式 ─────────────────────────────────
static void runFile(const std::string& filepath) {
    Interpreter interp;

    printBanner(filepath);

    // 首次执行
    try {
        runSource(readFile(filepath), interp, false);
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error reading file: " << e.what() << CLR_RESET << "\n";
        return;
    }

    // 启动文件监听（热更新）
    std::mutex reloadMutex;
    FileWatcher watcher(filepath, [&](const std::string& path) {
        std::lock_guard<std::mutex> lock(reloadMutex);
        try {
            std::string src = readFile(path);
            runSource(src, interp, true);
        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "File read error: " << e.what() << CLR_RESET << "\n";
        }
    });

    watcher.start();
    std::cout << CLR_GRAY << "\n[Waiting for file changes... Ctrl+C to exit]\n" << CLR_RESET;

    // 主线程阻塞
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ═══════════════════════════════════════════════════════════
// flux dev <file> — 开发模式：热更新 + 增量编译 + 自动测试
// ═══════════════════════════════════════════════════════════
static std::string g_lastSourceHash;

// 全局函数版本哈希表：funcName → FNV hash of function body
static std::unordered_map<std::string, uint64_t> g_functionHashes;

static std::string hashSource(const std::string& src) {
    return std::to_string(fnvHash(src));
}

// 提取每个函数的源码哈希（用于增量更新检测）
static std::unordered_map<std::string, uint64_t> computeFunctionHashes(Program* program) {
    std::unordered_map<std::string, uint64_t> hashes;
    for (auto& stmt : program->statements) {
        if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
            // Hash function by name + param count + body statement count
            // (a proxy for body content since we can't easily serialize AST)
            std::string sig = fn->name + "(" + std::to_string(fn->params.size()) + "){"
                            + std::to_string(fn->body.size()) + "}";
            hashes[fn->name] = fnvHash(sig);
        }
    }
    return hashes;
}

// Run tests from a test file if it exists
static void runTestFile(const std::string& mainFile, Interpreter& interp) {
    // Look for corresponding test file
    std::string testFile;
    auto dot = mainFile.rfind('.');
    if (dot != std::string::npos)
        testFile = mainFile.substr(0, dot) + "_test" + mainFile.substr(dot);
    else
        testFile = mainFile + "_test";

    // Also check tests/ directory
    std::string testsDir;
    auto slash = mainFile.rfind('/');
    if (slash != std::string::npos) {
        testsDir = mainFile.substr(0, slash) + "/tests/test.flux";
    } else {
        testsDir = "tests/test.flux";
    }

    std::string testSrc;
    bool foundTest = false;
    std::string testPath;

    // Try test file variants
    for (auto& path : {testFile, testsDir}) {
        std::ifstream f(path);
        if (f.good()) {
            testSrc = std::string(std::istreambuf_iterator<char>(f), {});
            foundTest = true;
            testPath = path;
            break;
        }
    }

    if (!foundTest) return;

    std::cout << CLR_GRAY << "\n🧪 Running tests: " << testPath << CLR_RESET << "\n";

    auto start = std::chrono::high_resolution_clock::now();
    try {
        Lexer  lexer(testSrc);
        auto   tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        interp.execute(program.get());

        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << CLR_GREEN << "✅ All tests passed (" << ms << "ms)" << CLR_RESET << "\n";

    } catch (const PanicSignal& p) {
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cerr << CLR_RED << "❌ Test failed (" << ms << "ms): "
                  << p.message << CLR_RESET << "\n";
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "❌ Test error: " << e.what() << CLR_RESET << "\n";
    }
}

static void runDevSource(const std::string& source, const std::string& filepath,
                          Interpreter& interp, bool isReload) {
    // Incremental compilation: skip if source hasn't changed
    std::string newHash = hashSource(source);
    if (isReload && newHash == g_lastSourceHash) {
        std::cout << CLR_GRAY << "  (no changes detected, skipping)" << CLR_RESET << "\n";
        return;
    }
    g_lastSourceHash = newHash;

    auto compileStart = std::chrono::high_resolution_clock::now();

    try {
        Lexer  lexer(source);
        auto   tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        // Type check
        TypeChecker checker;
        auto typeErrors = checker.check(program.get());
        if (!typeErrors.empty()) {
            std::cerr << CLR_RED CLR_BOLD
                      << "\n🔴 Type errors — execution blocked:\n"
                      << CLR_RESET;
            for (auto& e : typeErrors) {
                std::cerr << CLR_RED << "   ";
                if (e.line > 0) std::cerr << "line " << e.line << ": ";
                std::cerr << e.message << CLR_RESET << "\n";
            }
            return;
        }

        auto compileEnd = std::chrono::high_resolution_clock::now();
        auto compileMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            compileEnd - compileStart).count();

        // Per-function incremental change detection
        auto newHashes = computeFunctionHashes(program.get());
        int changedFns = 0, totalFns = (int)newHashes.size();
        std::vector<std::string> changedNames;

        if (isReload) {
            for (auto& [name, hash] : newHashes) {
                auto it = g_functionHashes.find(name);
                if (it == g_functionHashes.end() || it->second != hash) {
                    changedFns++;
                    changedNames.push_back(name);
                }
            }
            // Detect removed functions
            for (auto& [name, hash] : g_functionHashes) {
                if (newHashes.find(name) == newHashes.end())
                    changedNames.push_back("-" + name);
            }
        } else {
            changedFns = totalFns;
        }
        g_functionHashes = newHashes;

        if (isReload) {
            std::cout << CLR_YELLOW << "\n🔄 Hot reload detected..."
                      << CLR_GRAY << " (compiled in " << compileMs << "ms, "
                      << changedFns << "/" << totalFns << " functions changed)"
                      << CLR_RESET << "\n";
            if (!changedNames.empty()) {
                std::cout << CLR_GRAY << "  changed: ";
                for (size_t i = 0; i < changedNames.size(); i++) {
                    if (i > 0) std::cout << ", ";
                    std::cout << changedNames[i];
                }
                std::cout << CLR_RESET << "\n";
            }
            std::cout << CLR_GRAY << "─────────────────────────────\n" << CLR_RESET;
        }

        // Execute
        interp.execute(program.get());

        if (isReload)
            std::cout << CLR_GREEN << "✅ Hot reload complete" << CLR_RESET << "\n";

        // Auto-run tests
        runTestFile(filepath, interp);

    } catch (const ParseError& e) {
        std::cerr << CLR_RED << "\n❌ Parse error at line " << e.line
                  << ": " << e.what() << CLR_RESET << "\n";
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "\n❌ Runtime error: " << e.what() << CLR_RESET << "\n";
    }
}

static void cmdDev(const std::string& filepath) {
    Interpreter interp;

    std::cout << CLR_BOLD CLR_CYAN
              << "╔══════════════════════════════════════╗\n"
              << "║       Flux Dev Mode                  ║\n"
              << "║       Hot Reload + Auto Test  🔥     ║\n"
              << "╚══════════════════════════════════════╝\n"
              << CLR_RESET;
    std::cout << CLR_GRAY << "  Watching: " << filepath << CLR_RESET << "\n\n";

    // Initial run
    try {
        runDevSource(readFile(filepath), filepath, interp, false);
    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error reading file: " << e.what() << CLR_RESET << "\n";
        return;
    }

    // Watch for changes
    std::mutex reloadMutex;
    FileWatcher watcher(filepath, [&](const std::string& path) {
        std::lock_guard<std::mutex> lock(reloadMutex);
        try {
            runDevSource(readFile(path), path, interp, true);
        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "File read error: " << e.what() << CLR_RESET << "\n";
        }
    });

    watcher.start();
    std::cout << CLR_GRAY << "\n[Dev mode active — Ctrl+C to exit]\n" << CLR_RESET;

    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ═══════════════════════════════════════════════════════════
// flux lsp — LSP 语言服务器
// ═══════════════════════════════════════════════════════════
static void cmdLsp() {
    FluxLSP lsp;
    lsp.run();
}

// ═══════════════════════════════════════════════════════════
// flux debug <file> — 交互式调试器
// ═══════════════════════════════════════════════════════════
static int cmdDebug(const std::string& filepath) {
    Interpreter interp;
    Debugger debugger;

    std::cout << CLR_BOLD CLR_CYAN
              << "Flux Debugger  (type 'help' for commands)\n"
              << CLR_RESET;
    std::cout << CLR_GRAY << "  File: " << filepath << CLR_RESET << "\n\n";

    try {
        std::string src = readFile(filepath);
        Lexer  lexer(src);
        auto   tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        auto   program = parser.parse();

        // Type check first
        TypeChecker checker;
        auto typeErrors = checker.check(program.get());
        if (!typeErrors.empty()) {
            for (auto& e : typeErrors) {
                std::cerr << CLR_RED << "  ";
                if (e.line > 0) std::cerr << "line " << e.line << ": ";
                std::cerr << e.message << CLR_RESET << "\n";
            }
            return 1;
        }

        // Interactive debugger loop
        std::string cmd;
        while (true) {
            std::cout << CLR_CYAN << "debug> " << CLR_RESET;
            std::cout.flush();
            if (!std::getline(std::cin, cmd)) break;

            if (cmd == "quit" || cmd == "q") break;
            if (cmd == "help" || cmd == "h") {
                std::cout
                    << "  b <line>     — set breakpoint at line\n"
                    << "  d <id>       — delete breakpoint\n"
                    << "  bl           — list breakpoints\n"
                    << "  r / run      — run program\n"
                    << "  c / continue — continue execution\n"
                    << "  n / next     — step over\n"
                    << "  s / step     — step into\n"
                    << "  out          — step out\n"
                    << "  bt           — show call stack\n"
                    << "  p <expr>     — evaluate expression\n"
                    << "  state        — show persistent state\n"
                    << "  gc           — run cycle detection\n"
                    << "  q / quit     — exit debugger\n";
                continue;
            }
            if (cmd.substr(0, 2) == "b ") {
                int line = std::stoi(cmd.substr(2));
                int id = debugger.addBreakpoint(filepath, line);
                std::cout << CLR_GREEN << "Breakpoint " << id
                          << " set at line " << line << CLR_RESET << "\n";
                continue;
            }
            if (cmd.substr(0, 2) == "d ") {
                int id = std::stoi(cmd.substr(2));
                if (debugger.removeBreakpoint(id))
                    std::cout << CLR_GRAY << "Breakpoint " << id << " removed\n" << CLR_RESET;
                else
                    std::cout << CLR_RED << "No breakpoint with id " << id << "\n" << CLR_RESET;
                continue;
            }
            if (cmd == "bl") {
                auto bps = debugger.getBreakpoints();
                if (bps.empty()) {
                    std::cout << CLR_GRAY << "(no breakpoints)\n" << CLR_RESET;
                } else {
                    for (auto& bp : bps)
                        std::cout << "  [" << bp.id << "] line " << bp.line
                                  << (bp.enabled ? "" : " (disabled)") << "\n";
                }
                continue;
            }
            if (cmd == "r" || cmd == "run") {
                debugger.setStepMode(StepMode::Continue);
                try {
                    interp.execute(program.get());
                    std::cout << CLR_GREEN << "Program finished\n" << CLR_RESET;
                } catch (const std::exception& e) {
                    std::cerr << CLR_RED << "Runtime error: " << e.what() << CLR_RESET << "\n";
                }
                continue;
            }
            if (cmd == "state") {
                auto vars = debugger.getPersistentState(interp.persistentStore_);
                if (vars.empty()) {
                    std::cout << CLR_GRAY << "(no persistent state)\n" << CLR_RESET;
                } else {
                    for (auto& v : vars)
                        std::cout << "  " << v.name << " = " << v.value
                                  << " : " << v.type << "\n";
                }
                continue;
            }
            if (cmd == "gc") {
                CycleDetector detector;
                auto cycles = detector.detect(interp.globalEnv_);
                if (cycles.empty()) {
                    std::cout << CLR_GREEN << "No cycles detected\n" << CLR_RESET;
                } else {
                    for (auto& c : cycles)
                        std::cout << CLR_YELLOW << "  " << c.description
                                  << " (refcount=" << c.refCount << ")\n" << CLR_RESET;
                }
                continue;
            }
            if (cmd == "bt") {
                auto stack = debugger.getCallStack();
                if (stack.empty()) {
                    std::cout << CLR_GRAY << "(no active call stack)\n" << CLR_RESET;
                } else {
                    for (auto& f : stack)
                        std::cout << "  #" << f.id << " " << f.name
                                  << " at " << f.file << ":" << f.line << "\n";
                }
                continue;
            }
            if (cmd.substr(0, 2) == "p ") {
                std::string expr = cmd.substr(2);
                try {
                    Lexer  lex(expr);
                    auto   toks = lex.tokenize();
                    Parser par(std::move(toks));
                    auto   prog = par.parse();
                    interp.executeRepl(prog.get());
                } catch (const std::exception& e) {
                    std::cerr << CLR_RED << e.what() << CLR_RESET << "\n";
                }
                continue;
            }
            std::cout << CLR_GRAY << "Unknown command. Type 'help'.\n" << CLR_RESET;
        }
        return 0;

    } catch (const std::exception& e) {
        std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
        return 1;
    }
}

// ── 帮助信息 ──────────────────────────────────────────────
static void printHelp() {
    std::cout
        << CLR_BOLD << "Flux Language — Toolchain\n\n" << CLR_RESET
        << CLR_BOLD << "Usage:\n" << CLR_RESET
        << "  flux                        Start interactive REPL\n"
        << "  flux repl                   Start interactive REPL (explicit)\n"
        << "  flux <file.flux>            Run file with hot-reload (watch mode)\n"
        << "  flux dev   <file.flux>      Dev mode — hot-reload + file watcher\n"
        << "  flux run   <file.flux>      Run file once, no file watcher\n"
        << "  flux check <file.flux> [--json]  Type-check file (--json for structured output)\n"
        << "  flux dev   <file.flux>      Dev mode (hot reload + auto test)\n"
        << "  flux fmt   <file.flux>      Format file to stdout\n"
        << "  flux fmt   -w <file.flux>   Format file in-place (overwrite)\n"
        << "  flux --vm  <file.flux>      Run file with bytecode VM (Feature G)\n"
        << "  flux jit   <file.flux>      JIT compile eligible functions\n"
        << "  flux llvm  <file.flux>      LLVM JIT compile and run (single-shot)\n"
        << "  flux --llvm <file.flux>     LLVM JIT + hot reload watch mode\n"
        << "  flux pack  <file> [-o app.fluz]  Pack (obfuscated + encrypted)\n"
        << "  flux pack  <file> --debug   Pack without protection (debuggable)\n"
        << "  flux profile <file.flux>    Run with profiling on all functions\n"
        << "  flux compile <file> [-o out]  Compile to native binary (x86_64, arm64, riscv64)\n"
        << "  flux live   <file.flux>      AOT compile + hot-swap (dlopen reload)\n"
        << "  flux codegen <arch> <file>   Generate assembly (x86_64, arm64, riscv64)\n"
        << "  flux lsp                    Start Language Server Protocol server\n"
        << "  flux debug <file.flux>      Start interactive debugger\n"
        << "  flux registry [url]         Get/set central package registry URL\n"
        << "  flux --help                 Show this help\n"
        << "\n"
        << CLR_BOLD << "Tooling:\n" << CLR_RESET
        << "  flux inspect <file> [--json]  Export program symbols & structure\n"
        << "  flux eval \"<code>\" [--json]   Evaluate code snippet (pipe-friendly)\n"
        << "\n"
        << CLR_BOLD << "Flags:\n" << CLR_RESET
        << "  --no-test                   Strip all test declarations\n"
        << "\n"
        << CLR_BOLD << "Package Manager (Feature L):\n" << CLR_RESET
        << "  flux new    <name>          Create a new Flux project\n"
        << "  flux build  [script]        Build & run project (default: run)\n"
        << "  flux add    <pkg>[@ver]     Add dependency to flux.toml\n"
        << "  flux add    <pkg> path=..   Add local path dependency\n"
        << "  flux remove <pkg>           Remove dependency\n"
        << "  flux install                Install all dependencies from flux.toml\n"
        << "  flux publish                Publish to local registry (~/.flux/packages)\n"
        << "  flux search [query]         Search local registry\n"
        << "  flux info   <pkg>           Show package info\n"
        << "  flux list                   List project dependencies\n"
        << "\n"
        << CLR_BOLD << "Examples:\n" << CLR_RESET
        << "  flux new myapp\n"
        << "  flux add mathlib@1.0.0\n"
        << "  flux install\n"
        << "  flux build\n"
        << "  flux run examples/hello.flux\n"
        << "  flux check examples/module_demo.flux\n"
        << "  flux fmt -w examples/stdlib_demo.flux\n"
        << "  flux --vm examples/language_demo.flux\n";
}

// ── 入口 ──────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc == 1) {
        cmdRepl();
        return 0;
    }

    // ── 解析全局标志（在子命令之前）──────────────────────
    // 将 argv 中的全局标志提取出来，剩余参数前移
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--no-test") { g_noTest = true; continue; }
        args.push_back(a);
    }
    if (args.empty()) {
        cmdRepl();
        return 0;
    }
    std::string sub = args[0];

    // 重建 argc/argv 供后续使用（去除已解析的全局标志）
    // 注意：args[0] = sub, args[1..] = 子命令参数
    // 为简化兼容，将 argc 和 argv 指针更新
    static std::vector<char*> newArgv;
    static std::string progName = argv[0];
    newArgv.push_back(&progName[0]);
    for (auto& s : args) newArgv.push_back(&s[0]);
    argc = (int)newArgv.size();
    argv = newArgv.data();

    // ── --help ───────────────────────────────────────────
    if (sub == "--help" || sub == "-h" || sub == "help") {
        printHelp();
        return 0;
    }

    // ── flux repl ────────────────────────────────────────
    if (sub == "repl") {
        cmdRepl();
        return 0;
    }

    // ── flux dev <file> — 开发模式（热更新 + 文件监听）──
    if (sub == "dev") {
        if (argc < 3) {
            std::cerr << "Usage: flux dev <file.flux>\n";
            return 1;
        }
        runFile(argv[2]);
        return 0;
    }

    // ── flux run <file> ──────────────────────────────────
    if (sub == "run") {
        if (argc < 3) {
            std::cerr << "Usage: flux run <file.flux>\n";
            return 1;
        }
        return cmdRun(argv[2]);
    }

    // ── flux dev <file> — 开发模式 ─────────────────────────
    if (sub == "dev") {
        if (argc < 3) {
            std::cerr << "Usage: flux dev <file.flux>\n";
            return 1;
        }
        cmdDev(argv[2]);
        return 0;
    }

    // ── flux lsp — 语言服务器 ────────────────────────────
    if (sub == "lsp") {
        cmdLsp();
        return 0;
    }

    // ── flux debug <file> — 交互式调试器 ─────────────────
    if (sub == "debug") {
        if (argc < 3) {
            std::cerr << "Usage: flux debug <file.flux>\n";
            return 1;
        }
        return cmdDebug(argv[2]);
    }

    // ── flux check <file> [--json] ────────────────────────
    if (sub == "check") {
        if (argc < 3) {
            std::cerr << "Usage: flux check <file.flux> [--json]\n";
            return 1;
        }
        bool jsonOutput = false;
        std::string filepath = argv[2];
        for (int i = 3; i < argc; i++) {
            if (std::string(argv[i]) == "--json") jsonOutput = true;
        }
        return cmdCheck(filepath, jsonOutput);
    }

    // ── flux fmt [-w] <file> ─────────────────────────────
    if (sub == "fmt") {
        if (argc < 3) {
            std::cerr << "Usage: flux fmt [-w] <file.flux>\n";
            return 1;
        }
        bool writeBack = false;
        std::string filepath = argv[2];
        if (filepath == "-w" || filepath == "--write") {
            writeBack = true;
            if (argc < 4) {
                std::cerr << "Usage: flux fmt -w <file.flux>\n";
                return 1;
            }
            filepath = argv[3];
        }
        return cmdFmt(filepath, writeBack);
    }

    // ══════════════════════════════════════════════════════
    // Feature L: 包管理器命令
    // ══════════════════════════════════════════════════════

    // ── flux new <name> ──────────────────────────────────
    if (sub == "new") {
        if (argc < 3) {
            std::cerr << "Usage: flux new <project-name>\n";
            return 1;
        }
        try { pkgNew(argv[2]); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux build [script] ──────────────────────────────
    // 读取 flux.toml，合并 flux_packages/*.flux + 入口文件，然后执行
    if (sub == "build") {
        std::string script = argc > 2 ? argv[2] : "run";
        try {
            auto r = pkgBuild(".", script);
            if (!r.ok) {
                std::cerr << CLR_RED << "Build error: " << r.error << CLR_RESET << "\n";
                return 1;
            }
            Interpreter interp;
            runSource(r.source, interp, false);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux add <pkg>[@version] [path=../lib] ───────────
    if (sub == "add") {
        if (argc < 3) {
            std::cerr << "Usage: flux add <pkg>[@version]  or  flux add <pkg> path=<dir>\n";
            return 1;
        }
        // 收集 argv[2..] 拼成一个 spec 字符串（兼容 "pkg path=../lib"）
        std::string spec = argv[2];
        if (argc > 3) spec += std::string(" ") + argv[3];
        try { pkgAdd(spec); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux remove <pkg> ────────────────────────────────
    if (sub == "remove") {
        if (argc < 3) {
            std::cerr << "Usage: flux remove <pkg>\n";
            return 1;
        }
        try { pkgRemove(argv[2]); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux install ─────────────────────────────────────
    if (sub == "install") {
        try { pkgInstall(); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux publish ─────────────────────────────────────
    if (sub == "publish") {
        try { pkgPublish(); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux search [query] ──────────────────────────────
    if (sub == "search") {
        std::string query = argc > 2 ? argv[2] : "";
        try { pkgSearch(query); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux info <pkg> ──────────────────────────────────
    if (sub == "info") {
        if (argc < 3) {
            std::cerr << "Usage: flux info <pkg>\n";
            return 1;
        }
        try { pkgInfo(argv[2]); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux list ────────────────────────────────────────
    if (sub == "list") {
        try { pkgList(); return 0; }
        catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux --llvm <file> — LLVM JIT + hot reload watch mode ──
    if (sub == "--llvm") {
        if (argc < 3) {
            std::cerr << "Usage: flux --llvm <file.flux>\n";
            return 1;
        }
        try {
            std::string filepath = argv[2];

            // Print banner
            std::cout << CLR_BOLD CLR_CYAN
                      << "╔══════════════════════════════════════╗\n"
                      << "║    Flux LLVM JIT Hot Reload          ║\n"
                      << "║    Watch Mode: ON                    ║\n"
                      << "╚══════════════════════════════════════╝\n"
                      << CLR_RESET;
            std::cout << CLR_GRAY << "  Watching: " << filepath << CLR_RESET << "\n\n";

            // Create persistent interpreter and LLVM JIT compiler
            Interpreter interp;
            interp.setNoTest(g_noTest);

            LLVMJITCompiler llvmJit;
            llvmJit.setInterpreter(&interp);

            // Initial compile and run
            {
                std::string src = readFile(filepath);
                Lexer  lexer(src);
                auto   tokens = lexer.tokenize();
                Parser parser(std::move(tokens));
                auto   program = parser.parse();

                TypeChecker checker;
                auto typeErrors = checker.check(program.get());
                if (!typeErrors.empty()) {
                    std::cerr << CLR_RED CLR_BOLD
                              << "\n  Type errors found:\n" << CLR_RESET;
                    for (auto& e : typeErrors) {
                        std::cerr << CLR_RED << "   ";
                        if (e.line > 0) std::cerr << "line " << e.line << ": ";
                        std::cerr << e.message << CLR_RESET << "\n";
                    }
                }

                interp.initProgram(program.get());

                HIRLowering lowering;
                auto hir = lowering.lower(program.get());

                std::cerr << CLR_CYAN << "[LLVM JIT] Compiling " << filepath
                          << "..." << CLR_RESET << "\n";

                double result = llvmJit.compileAndRun(hir);
                llvmJit.dumpStats();

                if (result != 0.0) {
                    std::cerr << CLR_GRAY << "[LLVM JIT] Exit value: "
                              << result << CLR_RESET << "\n";
                }
            }

            // Start file watcher for hot reload
            std::mutex reloadMutex;
            FileWatcher watcher(filepath, [&](const std::string& path) {
                std::lock_guard<std::mutex> lock(reloadMutex);
                try {
                    std::string src = readFile(path);
                    Lexer  lexer(src);
                    auto   tokens = lexer.tokenize();
                    Parser parser(std::move(tokens));
                    auto   program = parser.parse();

                    // Re-init interpreter functions (but persistent state is preserved)
                    interp.initProgram(program.get());

                    HIRLowering lowering;
                    auto hir = lowering.lower(program.get());

                    std::cerr << CLR_CYAN << "\n[LLVM JIT] Recompiling " << path
                              << "..." << CLR_RESET << "\n";

                    double result = llvmJit.recompile(hir);
                    llvmJit.dumpStats();

                    if (result != 0.0) {
                        std::cerr << CLR_GRAY << "[LLVM JIT] Exit value: "
                                  << result << CLR_RESET << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << CLR_RED << "[LLVM JIT] Reload error: "
                              << e.what() << CLR_RESET << "\n";
                }
            });

            watcher.start();
            std::cout << CLR_GRAY << "\n[LLVM JIT watching for file changes... Ctrl+C to exit]\n" << CLR_RESET;

            // Block main thread
            while (true) std::this_thread::sleep_for(std::chrono::seconds(1));

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux --vm <file> ────────────────────────────────
    if (sub == "--vm") {
        if (argc < 3) {
            std::cerr << "Usage: flux --vm <file.flux>\n";
            return 1;
        }
        g_useVM  = true;
        // VM 模式：直接执行，不启动文件监听（run-and-exit）
        try {
            Interpreter interp;
            interp.setNoTest(g_noTest);
            std::string source = readFile(argv[2]);
            runSource(source, interp, false);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // ── flux live <file.flux> [-v] — AOT 编译 + 热替换运行 ──
    if (sub == "live") {
        if (argc < 3) {
            std::cerr << "Usage: flux live <file.flux> [-v]\n";
            return 1;
        }
        try {
            std::string filepath = argv[2];
            bool verbose = false;
            for (int i = 3; i < argc; i++) {
                if (std::string(argv[i]) == "-v" || std::string(argv[i]) == "--verbose")
                    verbose = true;
            }

            HotSwapEngine engine(verbose);

            std::cerr << CLR_CYAN << "[live] Compiling " << filepath
                      << " → native .so ..." << CLR_RESET << "\n";

            if (!engine.loadFromSource(filepath)) {
                std::cerr << CLR_RED << "[live] Initial compilation failed." << CLR_RESET << "\n";
                return 1;
            }

            std::cerr << CLR_GREEN << "[live] Running (native AOT)..." << CLR_RESET << "\n";
            engine.executeMain();

            std::cerr << CLR_CYAN << "\n[live] Watching " << filepath
                      << " for changes (Ctrl+C to quit)" << CLR_RESET << "\n";
            std::cerr << CLR_GRAY << "[live] Edit & save → auto recompile → dlopen hot-swap"
                      << CLR_RESET << "\n\n";

            engine.startWatching();

            // 主线程等待，直到 Ctrl+C
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux compile <file> [-o output] [--arch <arch>] [--keep-asm] [-v] — 编译到二进制
    if (sub == "compile") {
        if (argc < 3) {
            std::cerr << "Usage: flux compile <file.flux> [-o output] [--arch x86_64|arm64|riscv64] [--keep-asm] [-v]\n";
            return 1;
        }
        try {
            std::string filepath = argv[2];
            CompileOptions opts;
            opts.arch = detectHostArch();
            opts.output = "a.out";

            // Parse flags
            for (int i = 3; i < argc; i++) {
                std::string flag = argv[i];
                if ((flag == "-o" || flag == "--output") && i + 1 < argc) {
                    opts.output = argv[++i];
                } else if (flag == "--arch" && i + 1 < argc) {
                    std::string a = argv[++i];
                    if (a == "x86_64" || a == "x86-64" || a == "x64")
                        opts.arch = TargetArch::X86_64;
                    else if (a == "arm64" || a == "aarch64")
                        opts.arch = TargetArch::ARM64;
                    else if (a == "riscv64" || a == "riscv")
                        opts.arch = TargetArch::RISCV64;
                    else {
                        std::cerr << CLR_RED << "Unknown arch: " << a
                                  << " (supported: x86_64, arm64, riscv64)" << CLR_RESET << "\n";
                        return 1;
                    }
                } else if (flag == "--keep-asm") {
                    opts.keepAsm = true;
                } else if (flag == "-v" || flag == "--verbose") {
                    opts.verbose = true;
                }
            }

            std::string src = readFile(filepath);

            // Pipeline: Source → AST → TypeCheck → HIR → MIR → optimize → codegen → assemble → link
            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            TypeChecker checker;
            auto typeErrors = checker.check(program.get());
            if (!typeErrors.empty()) {
                std::cerr << CLR_RED << "Type errors — compile aborted:\n" << CLR_RESET;
                for (auto& e : typeErrors)
                    std::cerr << CLR_RED << "  " << e.message << CLR_RESET << "\n";
                return 1;
            }

            HIRLowering lowering;
            auto hir = lowering.lower(program.get());
            MIRBuilder builder;
            auto mir = builder.build(hir);
            mirOptimize(mir);

            if (compileToBinary(mir, opts)) {
                std::cerr << CLR_GREEN << "Compiled " << filepath << " → " << opts.output
                          << " (" << archName(opts.arch) << ")" << CLR_RESET << "\n";
                return 0;
            } else {
                std::cerr << CLR_RED << "Compilation failed." << CLR_RESET << "\n";
                return 1;
            }

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux codegen <arch> <file> — 代码生成 ─────────────
    if (sub == "codegen") {
        if (argc < 4) {
            std::cerr << "Usage: flux codegen <x86_64|arm64|riscv64> <file.flux>\n";
            return 1;
        }
        try {
            std::string archStr = argv[2];
            std::string filepath = argv[3];
            std::string src = readFile(filepath);

            TargetArch arch;
            if (archStr == "x86_64" || archStr == "x86-64" || archStr == "x64")
                arch = TargetArch::X86_64;
            else if (archStr == "arm64" || archStr == "aarch64")
                arch = TargetArch::ARM64;
            else if (archStr == "riscv64" || archStr == "riscv")
                arch = TargetArch::RISCV64;
            else {
                std::cerr << CLR_RED << "Unknown target: " << archStr
                          << " (supported: x86_64, arm64, riscv64)" << CLR_RESET << "\n";
                return 1;
            }

            // Pipeline: Source → AST → HIR → MIR → optimize → codegen
            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            HIRLowering lowering;
            auto hir = lowering.lower(program.get());

            MIRBuilder builder;
            auto mir = builder.build(hir);
            mirOptimize(mir);

            auto gen = createCodeGenerator(arch);
            auto result = gen->generate(mir);

            if (!result.ok) {
                std::cerr << CLR_RED << "Codegen error: " << result.error << CLR_RESET << "\n";
                return 1;
            }

            std::cout << result.assembly;
            std::cerr << CLR_GREEN << "// Generated " << archName(arch)
                      << " assembly from: " << filepath << CLR_RESET << "\n";
            return 0;

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux jit <file> — JIT 编译执行 ────────────────────
    if (sub == "jit") {
        if (argc < 3) {
            std::cerr << "Usage: flux jit <file.flux>\n";
            return 1;
        }
        try {
            std::string src = readFile(argv[2]);

            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            // HIR → MIR → optimize
            HIRLowering lowering;
            auto hir = lowering.lower(program.get());
            MIRBuilder builder;
            auto mir = builder.build(hir);
            mirOptimize(mir);

            // Try JIT for eligible functions
            JITCompiler jit;
            int compiled = 0;
            for (auto& fn : mir.functions) {
                if (fn.name == "__main__") continue;
                auto result = jit.compile(fn);
                if (result) compiled++;
            }

            std::cerr << CLR_CYAN << "[JIT] Compiled " << compiled
                      << " function(s) to native code" << CLR_RESET << "\n";
            jit.dumpStats();

            // Fall back to interpreter for execution
            Interpreter interp;
            runSource(src, interp, false);
            return 0;

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux llvm <file> — LLVM JIT compile and execute ────────
    if (sub == "llvm") {
        if (argc < 3) {
            std::cerr << "Usage: flux llvm <file.flux>\n";
            return 1;
        }
        try {
            std::string filepath = argv[2];
            std::string src = readFile(filepath);

            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            // Type check
            TypeChecker checker;
            auto typeErrors = checker.check(program.get());
            if (!typeErrors.empty()) {
                std::cerr << CLR_RED CLR_BOLD
                          << "\n  Type errors found:\n" << CLR_RESET;
                for (auto& e : typeErrors) {
                    std::cerr << CLR_RED << "   ";
                    if (e.line > 0) std::cerr << "line " << e.line << ": ";
                    std::cerr << e.message << CLR_RESET << "\n";
                }
            }

            // AST -> HIR
            HIRLowering lowering;
            auto hir = lowering.lower(program.get());

            // Create interpreter for runtime bridge
            Interpreter interp;
            interp.setNoTest(g_noTest);
            interp.initProgram(program.get());

            // LLVM JIT compile and run
            LLVMJITCompiler llvmJit;
            llvmJit.setInterpreter(&interp);

            std::cerr << CLR_CYAN << "[LLVM JIT] Compiling " << filepath
                      << "..." << CLR_RESET << "\n";

            double result = llvmJit.compileAndRun(hir);

            llvmJit.dumpStats();

            if (result != 0.0) {
                std::cerr << CLR_GRAY << "[LLVM JIT] Exit value: "
                          << result << CLR_RESET << "\n";
            }

            return 0;

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux pack <file> [-o out.fluz] [--debug] — 发布打包 ──
    if (sub == "pack") {
        if (argc < 3) {
            std::cerr << "Usage: flux pack <file.flux> [-o output.fluz] [--debug]\n";
            return 1;
        }
        try {
            std::string filepath = argv[2];
            std::string output = "app.fluz";  // default output
            bool debugMode = false;
            // Parse flags
            for (int i = 3; i < argc; i++) {
                if (std::string(argv[i]) == "-o" && i + 1 < argc) {
                    output = argv[++i];
                } else if (std::string(argv[i]) == "--debug") {
                    debugMode = true;
                }
            }

            std::string src = readFile(filepath);

            // Pipeline: Source → Lex → Parse → TypeCheck → HIR → MIR → FluzCodeGen
            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            TypeChecker checker;
            auto typeErrors = checker.check(program.get());
            if (!typeErrors.empty()) {
                std::cerr << CLR_RED << "Type errors — pack aborted:\n" << CLR_RESET;
                for (auto& e : typeErrors)
                    std::cerr << CLR_RED << "  " << e.message << CLR_RESET << "\n";
                return 1;
            }

            HIRLowering lowering;
            auto hir = lowering.lower(program.get());
            MIRBuilder builder;
            auto mir = builder.build(hir);
            mirOptimize(mir);

            FluzCodeGen codegen;
            auto pkg = codegen.generate(mir, src);

            // 保护：混淆 + 加密（debug 模式跳过）
            protectPackage(pkg, debugMode);

            writeFluz(output, pkg);

            int unitCount = (int)pkg.units.size();
            std::cerr << CLR_GREEN << "Packed " << unitCount
                      << " unit(s) → " << output;
            if (debugMode)
                std::cerr << " (debug mode — NOT protected)";
            else
                std::cerr << " (obfuscated + encrypted)";
            std::cerr << CLR_RESET << "\n";

            // 仅 debug 模式显示单元详情
            if (debugMode) {
                for (auto& u : pkg.units) {
                    std::cerr << CLR_GRAY << "  " << u.name
                              << " (" << u.code.size() << " instructions, hash="
                              << std::hex << u.versionHash << std::dec << ")"
                              << CLR_RESET << "\n";
                }
            }
            return 0;

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux unpack — 已禁用（防止源码泄漏）─────────────────
    if (sub == "unpack") {
        // 仅允许反汇编带 debug 标志的 .fluz 文件
        if (argc < 3) {
            std::cerr << "Usage: flux unpack <file.fluz>\n";
            return 1;
        }
        try {
            auto pkg = readFluz(argv[2]);

            // 检查是否为 debug 包
            if (!(pkg.flags & FLUZ_FLAG_DEBUG)) {
                std::cerr << CLR_RED
                    << "Error: this .fluz file is protected (obfuscated + encrypted).\n"
                    << "Disassembly is not available for production builds.\n"
                    << "Use `flux pack --debug` to create a debuggable bundle."
                    << CLR_RESET << "\n";
                return 1;
            }

            std::cout << "FLUZ v" << pkg.version
                      << " (debug, " << pkg.units.size() << " units)\n\n";
            for (auto& unit : pkg.units) {
                std::cout << "── " << unit.name << " (hash="
                          << std::hex << unit.versionHash << std::dec << ") ──\n";
                Chunk chunk;
                chunk.code      = unit.code;
                chunk.constants = unit.constants;
                chunk.names     = unit.names;
                chunk.dump(unit.name);
                std::cout << "\n";
            }
            return 0;
        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux profile <file.flux> — 性能分析运行 ────────────
    if (sub == "profile") {
        if (argc < 3) {
            std::cerr << "Usage: flux profile <file.flux>\n";
            return 1;
        }
        // "report" 子命令兼容
        std::string target = argv[2];
        if (target == "report" && argc > 3)
            target = argv[3];

        try {
            std::string src = readFile(target);
            Interpreter interp;
            interp.setNoTest(g_noTest);
            Profiler::instance().clear();

            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            TypeChecker checker;
            auto typeErrors = checker.check(program.get());
            if (!typeErrors.empty()) {
                for (auto& e : typeErrors)
                    std::cerr << CLR_RED << "  " << e.message << CLR_RESET << "\n";
                return 1;
            }

            // Mark ALL user-defined functions for profiling
            for (auto& stmt : program->statements) {
                if (auto* fn = dynamic_cast<FnDecl*>(stmt.get()))
                    Profiler::instance().markFunction(fn->name);
                if (auto* pfn = dynamic_cast<ProfiledFnDecl*>(stmt.get())) {
                    auto* fn = dynamic_cast<FnDecl*>(pfn->fnDecl.get());
                    if (fn) Profiler::instance().markFunction(fn->name);
                }
            }

            interp.execute(program.get());
            Profiler::instance().report();
            return 0;

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux registry <url> — 设置中心包仓库 URL ─────────
    if (sub == "registry") {
        if (argc < 3) {
            // 显示当前 registry
            std::cout << "Current registry: " << getRegistryUrl() << "\n";
            return 0;
        }
        setRegistryUrl(argv[2]);
        std::cout << CLR_GREEN << "Registry set to: " << argv[2] << CLR_RESET << "\n";
        return 0;
    }

    // ══════════════════════════════════════════════════════
    // 工具友好命令
    // ══════════════════════════════════════════════════════

    // ── flux inspect <file> [--json] — 导出程序结构（AST/类型/符号）──
    if (sub == "inspect") {
        if (argc < 3) {
            std::cerr << "Usage: flux inspect <file.flux> [--json]\n";
            return 1;
        }
        try {
            bool jsonMode = false;
            std::string filepath = argv[2];
            for (int i = 3; i < argc; i++) {
                if (std::string(argv[i]) == "--json") jsonMode = true;
            }

            std::string src = readFile(filepath);
            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            // 收集符号信息
            struct SymbolInfo {
                std::string kind;   // "function" / "variable" / "module" / "ai" / "enum"
                std::string name;
                std::string detail;
                int line;
            };
            std::vector<SymbolInfo> symbols;

            for (auto& stmt : program->statements) {
                if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
                    std::string params;
                    for (size_t i = 0; i < fn->params.size(); i++) {
                        if (i > 0) params += ", ";
                        params += fn->params[i].name;
                        if (!fn->params[i].type.empty()) params += ": " + fn->params[i].type;
                    }
                    std::string detail = "(" + params + ")";
                    if (!fn->returnType.empty()) detail += " -> " + fn->returnType;
                    if (!fn->preconditions_.empty())
                        detail += " [requires: " + std::to_string(fn->preconditions_.size()) + " conditions]";
                    if (!fn->postconditions_.empty())
                        detail += " [ensures: " + std::to_string(fn->postconditions_.size()) + " conditions]";
                    symbols.push_back({"function", fn->name, detail, 0});
                }
                if (auto* vd = dynamic_cast<VarDecl*>(stmt.get())) {
                    std::string detail = vd->typeAnnotation.empty() ? "Any" : vd->typeAnnotation;
                    if (vd->isInterface) detail = "interface";
                    symbols.push_back({"variable", vd->name, detail, 0});
                }
                if (auto* md = dynamic_cast<ModuleDecl*>(stmt.get())) {
                    std::string detail;
                    int fnCount = 0;
                    for (auto& item : md->body)
                        if (dynamic_cast<FnDecl*>(item.get())) fnCount++;
                    detail = std::to_string(fnCount) + " functions";
                    if (md->restartPolicy != RestartPolicy::None) detail += ", supervised";
                    if (!md->poolName.empty()) detail += ", pool=" + md->poolName;
                    symbols.push_back({"module", md->name, detail, 0});
                }
                if (auto* ad = dynamic_cast<SpecifyDecl*>(stmt.get())) {
                    std::string detail;
                    for (auto& f : ad->fields) {
                        if (f.key == "intent") {
                            if (auto* s = dynamic_cast<StringLit*>(f.value.get()))
                                detail = s->value;
                        }
                    }
                    symbols.push_back({"specify", ad->name, detail, 0});
                }
                if (auto* ed = dynamic_cast<EnumDecl*>(stmt.get())) {
                    std::string detail = std::to_string(ed->variants.size()) + " variants";
                    symbols.push_back({"enum", ed->name, detail, 0});
                }
            }

            if (jsonMode) {
                // JSON 输出
                std::cout << "{\"file\":\"" << filepath << "\",\"symbols\":[\n";
                for (size_t i = 0; i < symbols.size(); i++) {
                    auto& s = symbols[i];
                    std::cout << "  {\"kind\":\"" << s.kind
                              << "\",\"name\":\"" << s.name
                              << "\",\"detail\":\"" << s.detail << "\"}";
                    if (i + 1 < symbols.size()) std::cout << ",";
                    std::cout << "\n";
                }
                std::cout << "]}\n";
            } else {
                // 人类可读输出
                std::cout << CLR_BOLD << "Flux Inspect: " << filepath << CLR_RESET << "\n";
                std::cout << CLR_GRAY << "─────────────────────────────────────\n" << CLR_RESET;
                for (auto& s : symbols) {
                    std::string kindColor = CLR_CYAN;
                    if (s.kind == "function") kindColor = CLR_GREEN;
                    if (s.kind == "module")   kindColor = CLR_YELLOW;
                    if (s.kind == "specify")  kindColor = "\033[35m"; // magenta
                    std::cout << kindColor << "  [" << s.kind << "] " << CLR_RESET
                              << CLR_BOLD << s.name << CLR_RESET;
                    if (!s.detail.empty())
                        std::cout << CLR_GRAY << "  " << s.detail << CLR_RESET;
                    std::cout << "\n";
                }
                std::cout << CLR_GRAY << "\nTotal: " << symbols.size() << " symbols\n" << CLR_RESET;
            }
            return 0;

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux eval "<code>" [--json] — 管道式代码执行 ─────────
    if (sub == "eval") {
        if (argc < 3) {
            std::cerr << "Usage: flux eval \"<code>\" [--json]\n";
            return 1;
        }
        try {
            bool jsonOutput = false;
            std::string code = argv[2];
            for (int i = 3; i < argc; i++) {
                if (std::string(argv[i]) == "--json") jsonOutput = true;
            }

            Interpreter interp;
            interp.setNoTest(g_noTest);

            // 捕获 stdout
            std::stringstream capturedOut;
            auto* oldBuf = std::cout.rdbuf(capturedOut.rdbuf());

            Lexer  lexer(code);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            TypeChecker checker;
            auto typeErrors = checker.check(program.get());

            std::string errorStr;
            if (!typeErrors.empty()) {
                for (auto& e : typeErrors)
                    errorStr += e.message + "\n";
            }

            std::string resultStr;
            if (errorStr.empty()) {
                interp.execute(program.get());
                resultStr = "ok";
            }

            std::cout.rdbuf(oldBuf);

            if (jsonOutput) {
                std::string stdout_content = capturedOut.str();
                // 转义 JSON 字符串
                std::string escaped;
                for (char c : stdout_content) {
                    if (c == '"') escaped += "\\\"";
                    else if (c == '\\') escaped += "\\\\";
                    else if (c == '\n') escaped += "\\n";
                    else if (c == '\t') escaped += "\\t";
                    else escaped += c;
                }
                std::string errEscaped;
                for (char c : errorStr) {
                    if (c == '"') errEscaped += "\\\"";
                    else if (c == '\\') errEscaped += "\\\\";
                    else if (c == '\n') errEscaped += "\\n";
                    else errEscaped += c;
                }
                std::cout << "{\"status\":\"" << (errorStr.empty() ? "ok" : "error")
                          << "\",\"stdout\":\"" << escaped
                          << "\",\"errors\":\"" << errEscaped << "\"}\n";
            } else {
                std::cout << capturedOut.str();
                if (!errorStr.empty())
                    std::cerr << CLR_RED << errorStr << CLR_RESET;
            }
            return errorStr.empty() ? 0 : 1;

        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "Error: " << e.what() << CLR_RESET << "\n";
            return 1;
        }
    }

    // ── flux <file.flux> — 热更新模式 ───────────────────
    runFile(sub);
    return 0;
}
