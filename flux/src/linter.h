#pragma once
#include "ast.h"
#include "token.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <iostream>

// ═══════════════════════════════════════════════════════════
// LintDiagnostic — 单条 lint 诊断信息
// ═══════════════════════════════════════════════════════════
enum class LintSeverity { Info, Warn, Error };

struct LintDiagnostic {
    std::string  file;
    int          line;
    LintSeverity severity;
    std::string  message;

    std::string severityStr() const {
        switch (severity) {
            case LintSeverity::Info:  return "info";
            case LintSeverity::Warn:  return "warn";
            case LintSeverity::Error: return "error";
        }
        return "?";
    }

    std::string toJson() const {
        auto escapeJson = [](const std::string& s) -> std::string {
            std::string out;
            for (char c : s) {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else if (c == '\t') out += "\\t";
                else out += c;
            }
            return out;
        };
        std::string json = "{";
        json += "\"file\":\"" + escapeJson(file) + "\",";
        json += "\"line\":" + std::to_string(line) + ",";
        json += "\"severity\":\"" + severityStr() + "\",";
        json += "\"message\":\"" + escapeJson(message) + "\"";
        json += "}";
        return json;
    }
};

// ═══════════════════════════════════════════════════════════
// Linter — AST-based code quality checker
// ═══════════════════════════════════════════════════════════
class Linter {
public:
    explicit Linter(const std::string& filename) : filename_(filename) {}

    // 从 token 列表建立 name→line 映射（近似行号定位）
    void buildLineMap(const std::vector<Token>& tokens) {
        for (size_t i = 0; i < tokens.size(); i++) {
            auto& tok = tokens[i];
            if (tok.type == TokenType::IDENTIFIER ||
                tok.type == TokenType::FUNC ||
                tok.type == TokenType::VAR ||
                tok.type == TokenType::CONF ||
                tok.type == TokenType::MODULE) {
                // 记录每个标识符第一次出现的行号
                // 用 "kind:name" 来区分
                if (tok.type == TokenType::VAR && i + 1 < tokens.size()) {
                    nameLines_[tokens[i + 1].value] = tokens[i + 1].line;
                }
                if (tok.type == TokenType::FUNC && i + 1 < tokens.size()) {
                    nameLines_["func:" + tokens[i + 1].value] = tokens[i + 1].line;
                }
                if (tok.type == TokenType::MODULE && i + 1 < tokens.size()) {
                    nameLines_["module:" + tokens[i + 1].value] = tokens[i + 1].line;
                }
                if (tok.type == TokenType::IDENTIFIER) {
                    // 记录所有标识符出现（取第一次作为声明位置）
                    if (nameLines_.find(tok.value) == nameLines_.end()) {
                        nameLines_[tok.value] = tok.line;
                    }
                }
            }
            // Track TRUE/FALSE token lines for constant condition detection
            if (tok.type == TokenType::TRUE || tok.type == TokenType::FALSE) {
                // Store with special key
                std::string key = (tok.type == TokenType::TRUE) ? "__bool_true__" : "__bool_false__";
                boolLitLines_[tok.line] = (tok.type == TokenType::TRUE);
            }
            // Track IF/WHILE token lines
            if (tok.type == TokenType::IF) {
                ifLines_.push_back(tok.line);
            }
            if (tok.type == TokenType::WHILE) {
                whileLines_.push_back(tok.line);
            }
            // Track RETURN token lines
            if (tok.type == TokenType::RETURN) {
                returnLines_.push_back(tok.line);
            }
        }

        // Second pass: build a more precise var declaration map
        // Pattern: VAR IDENTIFIER or CONF IDENTIFIER
        // Store ALL occurrences for shadow detection
        for (size_t i = 0; i + 1 < tokens.size(); i++) {
            if ((tokens[i].type == TokenType::VAR || tokens[i].type == TokenType::CONF) &&
                tokens[i + 1].type == TokenType::IDENTIFIER) {
                std::string vname = tokens[i + 1].value;
                int vline = tokens[i + 1].line;
                // First occurrence → varDeclLines_ (top-level)
                if (varDeclLines_.find(vname) == varDeclLines_.end()) {
                    varDeclLines_[vname] = vline;
                }
                // All occurrences → allVarDeclLines_ (for shadow inner detection)
                allVarDeclLines_[vname].push_back(vline);
            }
        }
        // Pattern: FUNC IDENTIFIER
        for (size_t i = 0; i + 1 < tokens.size(); i++) {
            if (tokens[i].type == TokenType::FUNC &&
                tokens[i + 1].type == TokenType::IDENTIFIER) {
                if (funcDeclLines_.find(tokens[i + 1].value) == funcDeclLines_.end()) {
                    funcDeclLines_[tokens[i + 1].value] = tokens[i + 1].line;
                }
            }
        }
    }

    // 主入口：检查整个程序
    std::vector<LintDiagnostic> lint(Program* program) {
        diagnostics_.clear();

        // Phase 1: 收集声明和使用信息
        collectTopLevel(program);

        // Phase 2: 遍历 AST 检查各种模式
        for (auto& stmt : program->statements) {
            checkStatement(stmt.get(), /* scopeDepth */ 0);
        }

        // Phase 3: 报告未使用的顶层变量
        for (auto& [name, info] : vars_) {
            if (!info.used && !info.isParam) {
                warn(info.line, "unused variable '" + name + "'");
            }
        }

        // Phase 4: 报告未使用的顶层函数（排除 main/example 等入口）
        for (auto& [name, info] : funcs_) {
            if (!info.called && !isEntryPoint(name)) {
                warn(info.line, "function '" + name + "' is never called");
            }
        }

        // Phase 5: 检查未使用的 module 函数
        for (auto& [modName, modInfo] : modules_) {
            for (auto& [fnName, fnInfo] : modInfo.funcs) {
                if (!fnInfo.calledExternally) {
                    std::string qualName = modName + "." + fnName;
                    warn(fnInfo.line, "module function '" + qualName + "' is never called externally");
                }
            }
        }

        // Phase 6: 样式检查 — snake_case
        checkNamingStyle();

        return diagnostics_;
    }

    // 输出格式化结果
    static void printDiagnostics(const std::vector<LintDiagnostic>& diags,
                                  const std::string& filename,
                                  bool jsonOutput) {
        if (jsonOutput) {
            std::cout << "{\"file\":\"" << filename << "\",\"diagnostics\":[\n";
            for (size_t i = 0; i < diags.size(); i++) {
                std::cout << "  " << diags[i].toJson();
                if (i + 1 < diags.size()) std::cout << ",";
                std::cout << "\n";
            }
            std::cout << "]}\n";
            return;
        }

        // 人类可读输出
        std::cout << "\033[1m=== Lint: " << filename << " ===\033[0m\n";

        for (auto& d : diags) {
            std::string color;
            switch (d.severity) {
                case LintSeverity::Error: color = "\033[31m"; break;  // red
                case LintSeverity::Warn:  color = "\033[33m"; break;  // yellow
                case LintSeverity::Info:  color = "\033[36m"; break;  // cyan
            }
            // 左对齐文件:行号
            std::string loc = d.file + ":" + std::to_string(d.line);
            // 补齐到一定宽度
            while (loc.size() < 20) loc += " ";

            std::cout << "  " << loc << " "
                      << color << d.severityStr() << "\033[0m"
                      << "   " << d.message << "\n";
        }

        // 汇总
        int warns = 0, infos = 0, errors = 0;
        for (auto& d : diags) {
            switch (d.severity) {
                case LintSeverity::Warn:  warns++;  break;
                case LintSeverity::Info:  infos++;  break;
                case LintSeverity::Error: errors++; break;
            }
        }
        std::cout << "\n\033[1m=== Summary ===\033[0m\n";
        std::cout << "  " << warns << " warnings, "
                  << infos << " info, "
                  << errors << " errors\n";
    }

private:
    std::string filename_;
    std::vector<LintDiagnostic> diagnostics_;

    // name → line 映射
    std::unordered_map<std::string, int> nameLines_;
    std::unordered_map<std::string, int> varDeclLines_;        // first decl line
    std::unordered_map<std::string, std::vector<int>> allVarDeclLines_;  // all decl lines
    std::unordered_map<std::string, int> funcDeclLines_;
    std::unordered_map<int, bool> boolLitLines_;  // line → is_true
    std::vector<int> ifLines_;
    std::vector<int> whileLines_;
    std::vector<int> returnLines_;
    int ifLineIdx_ = 0;
    int whileLineIdx_ = 0;
    int returnLineIdx_ = 0;

    // ── 跟踪信息 ──────────────────────────────────────────

    struct VarInfo {
        int  line = 0;
        bool used = false;
        bool isParam = false;
    };

    struct FuncInfo {
        int  line = 0;
        bool called = false;
    };

    struct ModuleFuncInfo {
        int  line = 0;
        bool calledExternally = false;
    };

    struct ModuleInfo {
        int line = 0;
        std::unordered_map<std::string, ModuleFuncInfo> funcs;
    };

    std::unordered_map<std::string, VarInfo>  vars_;
    std::unordered_map<std::string, FuncInfo> funcs_;
    std::unordered_map<std::string, ModuleInfo> modules_;

    // 作用域链用于 shadowing 检测
    struct Scope {
        std::unordered_set<std::string> vars;
    };
    std::vector<Scope> scopeChain_;

    // ── 辅助 ────────────────────────────────────────────────

    int lookupVarLine(const std::string& name) {
        auto it = varDeclLines_.find(name);
        if (it != varDeclLines_.end()) return it->second;
        auto it2 = nameLines_.find(name);
        if (it2 != nameLines_.end()) return it2->second;
        return 0;
    }

    // 返回第 N 次声明的行号（0-indexed）
    int lookupVarDeclNth(const std::string& name, int nth) {
        auto it = allVarDeclLines_.find(name);
        if (it != allVarDeclLines_.end() && nth < (int)it->second.size()) {
            return it->second[nth];
        }
        return lookupVarLine(name);
    }

    // 跟踪每个变量名已见到的声明次数
    std::unordered_map<std::string, int> varDeclSeenCount_;

    int lookupFuncLine(const std::string& name) {
        auto it = funcDeclLines_.find(name);
        if (it != funcDeclLines_.end()) return it->second;
        auto it2 = nameLines_.find("func:" + name);
        if (it2 != nameLines_.end()) return it2->second;
        return 0;
    }

    int nextIfLine() {
        if (ifLineIdx_ < (int)ifLines_.size()) return ifLines_[ifLineIdx_++];
        return 0;
    }

    int nextWhileLine() {
        if (whileLineIdx_ < (int)whileLines_.size()) return whileLines_[whileLineIdx_++];
        return 0;
    }

    int nextReturnLine() {
        if (returnLineIdx_ < (int)returnLines_.size()) return returnLines_[returnLineIdx_++];
        return 0;
    }

    void warn(int line, const std::string& msg) {
        diagnostics_.push_back({filename_, line, LintSeverity::Warn, msg});
    }

    void addInfo(int line, const std::string& msg) {
        diagnostics_.push_back({filename_, line, LintSeverity::Info, msg});
    }

    void error(int line, const std::string& msg) {
        diagnostics_.push_back({filename_, line, LintSeverity::Error, msg});
    }

    bool isEntryPoint(const std::string& name) {
        return name == "main" || name == "init";
    }

    // ── Phase 1: 收集顶层声明 ──────────────────────────────

    void collectTopLevel(Program* program) {
        for (auto& stmt : program->statements) {
            if (auto* vd = dynamic_cast<VarDecl*>(stmt.get())) {
                int line = lookupVarLine(vd->name);
                vars_[vd->name] = {line, false, false};
            }
            else if (auto* cd = dynamic_cast<ConfDecl*>(stmt.get())) {
                int line = lookupVarLine(cd->name);
                vars_[cd->name] = {line, false, false};
            }
            else if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
                int line = lookupFuncLine(fn->name);
                funcs_[fn->name] = {line, false};
            }
            else if (auto* pfn = dynamic_cast<ProfiledFnDecl*>(stmt.get())) {
                if (auto* fn = dynamic_cast<FnDecl*>(pfn->fnDecl.get())) {
                    int line = lookupFuncLine(fn->name);
                    funcs_[fn->name] = {line, false};
                }
            }
            else if (auto* mod = dynamic_cast<ModuleDecl*>(stmt.get())) {
                ModuleInfo mi;
                mi.line = lookupVarLine(mod->name);
                for (auto& item : mod->body) {
                    if (auto* fn = dynamic_cast<FnDecl*>(item.get())) {
                        int fline = lookupFuncLine(fn->name);
                        mi.funcs[fn->name] = {fline, false};
                    }
                }
                modules_[mod->name] = std::move(mi);
            }
            // 扫描顶层表达式中的使用
            collectUsages(stmt.get());
        }
    }

    // ── 收集标识符使用（递归）─────────────────────────────

    void collectUsages(ASTNode* node) {
        if (!node) return;

        if (auto* id = dynamic_cast<Identifier*>(node)) {
            markVarUsed(id->name);
        }
        else if (auto* call = dynamic_cast<CallExpr*>(node)) {
            markFuncCalled(call->name);
            for (auto& arg : call->args) collectUsages(arg.get());
        }
        else if (auto* mc = dynamic_cast<ModuleCall*>(node)) {
            markModuleFuncCalled(mc->module, mc->fn);
            for (auto& arg : mc->args) collectUsages(arg.get());
        }
        else if (auto* ac = dynamic_cast<AsyncCall*>(node)) {
            markModuleFuncCalled(ac->module, ac->fn);
            for (auto& arg : ac->args) collectUsages(arg.get());
        }
        else if (auto* be = dynamic_cast<BinaryExpr*>(node)) {
            collectUsages(be->left.get());
            collectUsages(be->right.get());
        }
        else if (auto* ue = dynamic_cast<UnaryExpr*>(node)) {
            collectUsages(ue->operand.get());
        }
        else if (auto* vd = dynamic_cast<VarDecl*>(node)) {
            collectUsages(vd->initializer.get());
        }
        else if (auto* cd = dynamic_cast<ConfDecl*>(node)) {
            collectUsages(cd->initializer.get());
        }
        else if (auto* as = dynamic_cast<Assign*>(node)) {
            // Assignment to a variable — the variable name is used for writing,
            // but the value expression may use other variables
            collectUsages(as->value.get());
        }
        else if (auto* ret = dynamic_cast<ReturnStmt*>(node)) {
            collectUsages(ret->value.get());
        }
        else if (auto* ifs = dynamic_cast<IfStmt*>(node)) {
            collectUsages(ifs->condition.get());
            for (auto& s : ifs->thenBlock) collectUsages(s.get());
            for (auto& s : ifs->elseBlock) collectUsages(s.get());
        }
        else if (auto* ws = dynamic_cast<WhileStmt*>(node)) {
            collectUsages(ws->condition.get());
            for (auto& s : ws->body) collectUsages(s.get());
        }
        else if (auto* fi = dynamic_cast<ForIn*>(node)) {
            collectUsages(fi->iterable.get());
            for (auto& s : fi->body) collectUsages(s.get());
        }
        else if (auto* es = dynamic_cast<ExprStmt*>(node)) {
            collectUsages(es->expr.get());
        }
        else if (auto* fn = dynamic_cast<FnDecl*>(node)) {
            for (auto& s : fn->body) collectUsages(s.get());
        }
        else if (auto* pfn = dynamic_cast<ProfiledFnDecl*>(node)) {
            collectUsages(pfn->fnDecl.get());
        }
        else if (auto* mod = dynamic_cast<ModuleDecl*>(node)) {
            for (auto& s : mod->body) collectUsages(s.get());
        }
        else if (auto* idx = dynamic_cast<IndexExpr*>(node)) {
            collectUsages(idx->object.get());
            collectUsages(idx->index.get());
        }
        else if (auto* ia = dynamic_cast<IndexAssign*>(node)) {
            collectUsages(ia->object.get());
            collectUsages(ia->index.get());
            collectUsages(ia->value.get());
        }
        else if (auto* fa = dynamic_cast<FieldAssign*>(node)) {
            collectUsages(fa->object.get());
            collectUsages(fa->value.get());
        }
        else if (auto* meth = dynamic_cast<MethodCall*>(node)) {
            collectUsages(meth->object.get());
            for (auto& a : meth->args) collectUsages(a.get());
        }
        else if (auto* arr = dynamic_cast<ArrayLit*>(node)) {
            for (auto& e : arr->elements) collectUsages(e.get());
        }
        else if (auto* ae = dynamic_cast<AsyncExpr*>(node)) {
            collectUsages(ae->call.get());
        }
        else if (auto* aw = dynamic_cast<AwaitExpr*>(node)) {
            collectUsages(aw->expr.get());
        }
        else if (auto* sp = dynamic_cast<SpawnStmt*>(node)) {
            for (auto& s : sp->body) collectUsages(s.get());
        }
        else if (auto* sc = dynamic_cast<StructCreate*>(node)) {
            for (auto& f : sc->fields) collectUsages(f.value.get());
        }
        else if (auto* sl = dynamic_cast<StructLit*>(node)) {
            for (auto& f : sl->fields) collectUsages(f.defaultValue.get());
            for (auto& m : sl->methods) {
                for (auto& s : m.body) collectUsages(s.get());
            }
        }
        else if (dynamic_cast<StateAccess*>(node)) {
            // state access — mark as usage (no-op)
        }
        else if (auto* sas = dynamic_cast<StateAssign*>(node)) {
            collectUsages(sas->value.get());
        }
        else if (auto* td = dynamic_cast<TestDecl*>(node)) {
            collectUsages(td->inner.get());
        }
        else if (auto* al = dynamic_cast<AllocExpr*>(node)) {
            collectUsages(al->size.get());
        }
        else if (auto* fs = dynamic_cast<FreeStmt*>(node)) {
            collectUsages(fs->ptr.get());
        }
        else if (auto* ds = dynamic_cast<DelStmt*>(node)) {
            collectUsages(ds->target.get());
        }
        else if (auto* dfs = dynamic_cast<DefaultStmt*>(node)) {
            for (auto& s : dfs->body) collectUsages(s.get());
        }
        else if (auto* dd = dynamic_cast<DefaultDecl*>(node)) {
            for (auto& s : dd->body) collectUsages(s.get());
        }
        else if (auto* ir = dynamic_cast<IntervalRange*>(node)) {
            collectUsages(ir->start.get());
            collectUsages(ir->end.get());
        }
        else if (auto* fe = dynamic_cast<FuncExpr*>(node)) {
            for (auto& s : fe->body) collectUsages(s.get());
        }
    }

    void markVarUsed(const std::string& name) {
        auto it = vars_.find(name);
        if (it != vars_.end()) it->second.used = true;
    }

    void markFuncCalled(const std::string& name) {
        auto it = funcs_.find(name);
        if (it != funcs_.end()) it->second.called = true;
    }

    void markModuleFuncCalled(const std::string& mod, const std::string& fn) {
        auto it = modules_.find(mod);
        if (it != modules_.end()) {
            auto it2 = it->second.funcs.find(fn);
            if (it2 != it->second.funcs.end()) {
                it2->second.calledExternally = true;
            }
        }
    }

    // ── Phase 2: 遍历检查 ──────────────────────────────────

    void checkStatement(ASTNode* node, int scopeDepth) {
        if (!node) return;

        if (auto* vd = dynamic_cast<VarDecl*>(node)) {
            // Shadow 检测
            {
                // Get the line for this specific declaration (Nth occurrence)
                int nth = varDeclSeenCount_[vd->name]++;
                int declLine = lookupVarDeclNth(vd->name, nth);
                checkShadow(vd->name, declLine, scopeDepth);
            }
        }
        else if (auto* fn = dynamic_cast<FnDecl*>(node)) {
            checkFunction(fn);
        }
        else if (auto* pfn = dynamic_cast<ProfiledFnDecl*>(node)) {
            if (auto* fn = dynamic_cast<FnDecl*>(pfn->fnDecl.get())) {
                checkFunction(fn);
            }
        }
        else if (auto* ifs = dynamic_cast<IfStmt*>(node)) {
            int ifLine = nextIfLine();

            // 常量条件检测
            if (auto* bl = dynamic_cast<BoolLit*>(ifs->condition.get())) {
                std::string val = bl->value ? "true" : "false";
                addInfo(ifLine, "constant condition: if(" + val + ")");
            }

            // 空块检测
            if (ifs->thenBlock.empty()) {
                warn(ifLine, "empty if body");
            }

            // 递归检查块内容
            pushScope();
            for (auto& s : ifs->thenBlock) checkStatement(s.get(), scopeDepth + 1);
            popScope();
            if (!ifs->elseBlock.empty()) {
                pushScope();
                for (auto& s : ifs->elseBlock) checkStatement(s.get(), scopeDepth + 1);
                popScope();
            }
        }
        else if (auto* ws = dynamic_cast<WhileStmt*>(node)) {
            int wLine = nextWhileLine();

            // 常量条件检测
            if (auto* bl = dynamic_cast<BoolLit*>(ws->condition.get())) {
                std::string val = bl->value ? "true" : "false";
                addInfo(wLine, "constant condition: while(" + val + ")");
            }

            // 空块检测
            if (ws->body.empty()) {
                warn(wLine, "empty while body");
            }

            pushScope();
            for (auto& s : ws->body) checkStatement(s.get(), scopeDepth + 1);
            popScope();
        }
        else if (auto* fi = dynamic_cast<ForIn*>(node)) {
            // 空块检测
            if (fi->body.empty()) {
                warn(0, "empty for body");
            }
            pushScope();
            for (auto& s : fi->body) checkStatement(s.get(), scopeDepth + 1);
            popScope();
        }
        else if (auto* mod = dynamic_cast<ModuleDecl*>(node)) {
            pushScope();
            for (auto& s : mod->body) checkStatement(s.get(), scopeDepth + 1);
            popScope();
        }
        else if (dynamic_cast<ExprStmt*>(node)) {
            // nothing special for expr statements at this level
        }
    }

    void checkFunction(FnDecl* fn) {
        if (!fn) return;

        // 检查函数体中的 dead code after return
        checkDeadCodeInBlock(fn->body);

        // 检查 non-void 函数是否所有路径都有 return
        if (!fn->returnType.empty() && fn->returnType != "Nil" && fn->returnType != "Any") {
            if (!blockAlwaysReturns(fn->body)) {
                int line = lookupFuncLine(fn->name);
                warn(line, "function '" + fn->name + "' may not return a value on all paths");
            }
        }

        // 检查函数参数 shadowing + 函数体内声明
        pushScope();
        for (auto& param : fn->params) {
            addToScope(param.name);
        }
        for (auto& s : fn->body) {
            checkStatement(s.get(), 1);
        }
        popScope();
    }

    void checkDeadCodeInBlock(const std::vector<NodePtr>& stmts) {
        bool returnSeen = false;
        int returnLine = 0;
        for (size_t i = 0; i < stmts.size(); i++) {
            if (returnSeen) {
                // dead code after return — report line after the return
                int deadLine = returnLine > 0 ? returnLine + 1 : 0;
                addInfo(deadLine, "dead code after return statement");
                break;  // 只报告一次
            }
            if (dynamic_cast<ReturnStmt*>(stmts[i].get())) {
                returnSeen = true;
                returnLine = nextReturnLine();
            }
        }
    }

    bool blockAlwaysReturns(const std::vector<NodePtr>& stmts) {
        for (auto& stmt : stmts) {
            if (dynamic_cast<ReturnStmt*>(stmt.get())) return true;
            if (auto* ifs = dynamic_cast<IfStmt*>(stmt.get())) {
                if (!ifs->elseBlock.empty() &&
                    blockAlwaysReturns(ifs->thenBlock) &&
                    blockAlwaysReturns(ifs->elseBlock)) {
                    return true;
                }
            }
        }
        return false;
    }

    // ── Shadow 检测 ────────────────────────────────────────

    void pushScope() {
        scopeChain_.push_back({});
    }

    void popScope() {
        if (!scopeChain_.empty()) scopeChain_.pop_back();
    }

    void addToScope(const std::string& name) {
        if (!scopeChain_.empty()) {
            scopeChain_.back().vars.insert(name);
        }
    }

    void checkShadow(const std::string& name, int line, int scopeDepth) {
        if (scopeDepth == 0) {
            // 顶层声明，加入最外层 scope
            addToScope(name);
            return;
        }

        // 检查是否在外层作用域中存在
        // 检查 vars_ (顶层)
        auto it = vars_.find(name);
        if (it != vars_.end()) {
            int outerLine = it->second.line;
            warn(line, "variable '" + name + "' shadows outer '" + name + "' (line " + std::to_string(outerLine) + ")");
        }

        // 也检查 scope 链
        for (int i = (int)scopeChain_.size() - 2; i >= 0; i--) {
            if (scopeChain_[i].vars.count(name)) {
                // 找到外层 shadow
                if (it == vars_.end()) {
                    warn(line, "variable '" + name + "' shadows outer '" + name + "'");
                }
                break;
            }
        }

        addToScope(name);
    }

    // ── 样式检查 ──────────────────────────────────────────

    bool isSnakeCase(const std::string& name) {
        if (name.empty()) return true;
        // Allow leading underscore
        size_t start = 0;
        if (name[0] == '_') start = 1;
        for (size_t i = start; i < name.size(); i++) {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') return false;
        }
        return true;
    }

    bool isPascalCase(const std::string& name) {
        if (name.empty()) return false;
        return name[0] >= 'A' && name[0] <= 'Z';
    }

    void checkNamingStyle() {
        // 变量和函数名应为 snake_case
        // 模块名/类型名可以是 PascalCase（不报告）
        for (auto& [vname, vinfo] : vars_) {
            if (!isSnakeCase(vname) && !isPascalCase(vname)) {
                // Mixed case — suggest snake_case
                addInfo(vinfo.line, "variable '" + vname + "': consider using snake_case");
            } else if (!isSnakeCase(vname)) {
                // PascalCase for a variable — might be a type, skip
                // Only warn for camelCase specifically
                if (vname.size() > 1 && vname[0] >= 'a' && vname[0] <= 'z') {
                    bool hasCap = false;
                    for (char c : vname) { if (c >= 'A' && c <= 'Z') { hasCap = true; break; } }
                    if (hasCap) {
                        addInfo(vinfo.line, "variable '" + vname + "': consider using snake_case");
                    }
                }
            }
        }
        for (auto& [fname, finfo] : funcs_) {
            if (fname.size() > 1 && fname[0] >= 'a' && fname[0] <= 'z') {
                bool hasCap = false;
                for (char c : fname) { if (c >= 'A' && c <= 'Z') { hasCap = true; break; } }
                if (hasCap) {
                    addInfo(finfo.line, "function '" + fname + "': consider using snake_case");
                }
            }
        }
    }
};
