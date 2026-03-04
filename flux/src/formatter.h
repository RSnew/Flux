// formatter.h — Flux AST → 格式化源代码（Feature J: flux fmt）
#pragma once
#include "ast.h"
#include <string>
#include <sstream>
#include <cmath>

// ═══════════════════════════════════════════════════════════
// Formatter — 将 AST 还原为规范格式化的 Flux 源代码
//
// 规范风格：
//   - 4 空格缩进
//   - 开括号 { 与关键字同行
//   - 运算符两侧各一个空格
//   - 语句以换行结束
//   - 顶层块之间插入空行
// ═══════════════════════════════════════════════════════════
class Formatter {
public:
    // 主入口：格式化整个程序，返回规范化的 Flux 源代码字符串
    std::string format(Program* program) {
        std::string out;
        bool prevWasBlock = false;
        for (auto& stmt : program->statements) {
            // 块级结构之前插入空行，便于阅读
            bool isBlock = dynamic_cast<FnDecl*>(stmt.get())    ||
                           dynamic_cast<ModuleDecl*>(stmt.get()) ||
                           dynamic_cast<IfStmt*>(stmt.get())     ||
                           dynamic_cast<WhileStmt*>(stmt.get())  ||
                           dynamic_cast<ForIn*>(stmt.get());
            if ((prevWasBlock || isBlock) && !out.empty())
                out += "\n";
            out += fmtStmt(stmt.get());
            prevWasBlock = isBlock;
        }
        return out;
    }

private:
    int indent_ = 0;

    // ── 缩进字符串 ─────────────────────────────────────────
    std::string ind() const { return std::string(indent_ * 4, ' '); }

    // ── 运算符优先级（用于正确插入括号）──────────────────
    static int prec(const std::string& op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 10;
    }

    // ── 表达式格式化（无缩进，无换行）───────────────────
    // minPrec: 调用方期望的最小优先级；若该节点优先级更低则加括号
    std::string fmtExpr(ASTNode* node, int minPrec = 0) {
        if (!node) return "nil";

        // nil 字面量
        if (dynamic_cast<NilLit*>(node)) return "nil";
        // 数字字面量
        if (auto* n = dynamic_cast<NumberLit*>(node)) {
            std::string s = fmtNum(n->value);
            return (minPrec > 0 && n->value < 0) ? "(" + s + ")" : s;
        }
        // 字符串字面量
        if (auto* n = dynamic_cast<StringLit*>(node)) {
            return "\"" + escapeStr(n->value) + "\"";
        }
        // 布尔字面量
        if (auto* n = dynamic_cast<BoolLit*>(node)) {
            return n->value ? "true" : "false";
        }
        // 标识符
        if (auto* n = dynamic_cast<Identifier*>(node)) {
            return n->name;
        }
        // 二元表达式（含优先级括号逻辑）
        if (auto* n = dynamic_cast<BinaryExpr*>(node)) {
            int p = prec(n->op);
            // 左子表达式：若优先级更低则加括号
            std::string left  = fmtExpr(n->left.get(),  p);
            // 右子表达式：同优先级也加括号（保证左结合性）
            std::string right = fmtExpr(n->right.get(), p + 1);
            std::string expr  = left + " " + n->op + " " + right;
            return (p < minPrec) ? "(" + expr + ")" : expr;
        }
        // 一元表达式
        if (auto* n = dynamic_cast<UnaryExpr*>(node)) {
            std::string operand = fmtExpr(n->operand.get(), 7);
            std::string expr    = n->op + operand;
            return (7 < minPrec) ? "(" + expr + ")" : expr;
        }
        // 函数调用
        if (auto* n = dynamic_cast<CallExpr*>(node)) {
            return n->name + "(" + fmtArgList(n->args) + ")";
        }
        // 模块调用 Module.fn(args)
        if (auto* n = dynamic_cast<ModuleCall*>(node)) {
            return n->module + "." + n->fn + "(" + fmtArgList(n->args) + ")";
        }
        // 方法调用 obj.method(args)
        if (auto* n = dynamic_cast<MethodCall*>(node)) {
            return fmtExpr(n->object.get()) + "." + n->method
                   + "(" + fmtArgList(n->args) + ")";
        }
        // 下标访问 arr[i]
        if (auto* n = dynamic_cast<IndexExpr*>(node)) {
            return fmtExpr(n->object.get()) + "[" + fmtExpr(n->index.get()) + "]";
        }
        // 数组字面量 [a, b, c]
        if (auto* n = dynamic_cast<ArrayLit*>(node)) {
            return "[" + fmtArgList(n->elements) + "]";
        }
        // state.field 访问
        if (auto* n = dynamic_cast<StateAccess*>(node)) {
            return "state." + n->field;
        }
        // ExprStmt 内嵌（在表达式上下文中剥掉外层）
        if (auto* n = dynamic_cast<ExprStmt*>(node)) {
            return fmtExpr(n->expr.get(), minPrec);
        }
        // Module.fn.async(args) — 跨 pool 异步调用
        if (auto* n = dynamic_cast<AsyncCall*>(node)) {
            return n->module + "." + n->fn + ".async(" + fmtArgList(n->args) + ")";
        }
        // async <call> — 异步表达式（向后兼容）
        if (auto* n = dynamic_cast<AsyncExpr*>(node)) {
            return "async " + fmtExpr(n->call.get());
        }
        // await <expr> — 等待 Future
        if (auto* n = dynamic_cast<AwaitExpr*>(node)) {
            return "await " + fmtExpr(n->expr.get());
        }

        // ── StructCreate: Point(x: 3, y: 4) ─────────────────
        if (auto* n = dynamic_cast<StructCreate*>(node)) {
            std::string s = n->typeName + "(";
            for (size_t i = 0; i < n->fields.size(); ++i) {
                if (i) s += ", ";
                s += n->fields[i].name + ": " + fmtExpr(n->fields[i].value.get());
            }
            return s + ")";
        }

        // ── IntervalRange: [1, 5] / [1, 5) ──────────────────
        if (auto* n = dynamic_cast<IntervalRange*>(node)) {
            return "[" + fmtExpr(n->start.get()) + ", " + fmtExpr(n->end.get()) +
                   (n->inclusive ? "]" : ")");
        }

        return "/* ? */";
    }

    // ── 参数列表（逗号分隔，无换行）────────────────────────
    std::string fmtArgList(const std::vector<NodePtr>& args) {
        std::string s;
        for (size_t i = 0; i < args.size(); i++) {
            if (i) s += ", ";
            s += fmtExpr(args[i].get());
        }
        return s;
    }

    // ── 语句格式化（含缩进前缀和末尾换行）──────────────────
    std::string fmtStmt(ASTNode* node) {
        if (!node) return "";

        // ── 变量声明 ────────────────────────────────────────
        if (auto* n = dynamic_cast<VarDecl*>(node)) {
            std::string kw = n->forceOverride ? "!var " : "var ";
            std::string s = ind() + kw + n->name;
            if (n->isInterface) s += ": interface";
            if (!n->typeAnnotation.empty()) s += ": " + n->typeAnnotation;
            if (n->initializer) s += " = " + fmtExpr(n->initializer.get());
            return s + "\n";
        }

        // ── 赋值 ────────────────────────────────────────────
        if (auto* n = dynamic_cast<Assign*>(node)) {
            return ind() + n->name + " = " + fmtExpr(n->value.get()) + "\n";
        }

        // ── 持久状态赋值 state.field = expr ─────────────────
        if (auto* n = dynamic_cast<StateAssign*>(node)) {
            return ind() + "state." + n->field
                   + " = " + fmtExpr(n->value.get()) + "\n";
        }

        // ── 下标赋值 obj[idx] = val ──────────────────────────
        if (auto* n = dynamic_cast<IndexAssign*>(node)) {
            return ind()
                   + fmtExpr(n->object.get()) + "["
                   + fmtExpr(n->index.get())  + "] = "
                   + fmtExpr(n->value.get())  + "\n";
        }

        // ── return ───────────────────────────────────────────
        if (auto* n = dynamic_cast<ReturnStmt*>(node)) {
            if (n->value) return ind() + "return " + fmtExpr(n->value.get()) + "\n";
            return ind() + "return\n";
        }

        // ── 表达式语句 ───────────────────────────────────────
        // 注意：赋值节点（StateAssign、Assign、IndexAssign）被解析器包裹在
        // ExprStmt 中；这里解包它们以便正确格式化。
        if (auto* n = dynamic_cast<ExprStmt*>(node)) {
            ASTNode* inner = n->expr.get();
            if (auto* sa = dynamic_cast<StateAssign*>(inner))
                return ind() + "state." + sa->field
                       + " = " + fmtExpr(sa->value.get()) + "\n";
            if (auto* a = dynamic_cast<Assign*>(inner))
                return ind() + a->name
                       + " = " + fmtExpr(a->value.get()) + "\n";
            if (auto* ia = dynamic_cast<IndexAssign*>(inner))
                return ind() + fmtExpr(ia->object.get())
                       + "[" + fmtExpr(ia->index.get()) + "] = "
                       + fmtExpr(ia->value.get()) + "\n";
            return ind() + fmtExpr(inner) + "\n";
        }

        // ── if / else ────────────────────────────────────────
        if (auto* n = dynamic_cast<IfStmt*>(node)) {
            std::string s = ind() + "if " + fmtExpr(n->condition.get()) + " {\n";
            indent_++;
            for (auto& st : n->thenBlock) s += fmtStmt(st.get());
            indent_--;
            if (!n->elseBlock.empty()) {
                s += ind() + "} else {\n";
                indent_++;
                for (auto& st : n->elseBlock) s += fmtStmt(st.get());
                indent_--;
            }
            return s + ind() + "}\n";
        }

        // ── while ────────────────────────────────────────────
        if (auto* n = dynamic_cast<WhileStmt*>(node)) {
            std::string s = ind() + "while " + fmtExpr(n->condition.get()) + " {\n";
            indent_++;
            for (auto& st : n->body) s += fmtStmt(st.get());
            indent_--;
            return s + ind() + "}\n";
        }

        // ── for-in ───────────────────────────────────────────
        if (auto* n = dynamic_cast<ForIn*>(node)) {
            std::string s = ind() + "for " + n->var + " in "
                            + fmtExpr(n->iterable.get()) + " {\n";
            indent_++;
            for (auto& st : n->body) s += fmtStmt(st.get());
            indent_--;
            return s + ind() + "}\n";
        }

        // ── 函数声明 ─────────────────────────────────────────
        if (auto* n = dynamic_cast<FnDecl*>(node)) {
            std::string kw = n->forceOverride ? "!func " : "func ";
            std::string s = ind() + kw + n->name + "(";
            for (size_t i = 0; i < n->params.size(); i++) {
                if (i) s += ", ";
                s += n->params[i].name;
                if (!n->params[i].type.empty()) s += ": " + n->params[i].type;
            }
            s += ")";
            if (!n->returnType.empty()) s += " -> " + n->returnType;
            s += " {\n";
            indent_++;
            for (auto& st : n->body) s += fmtStmt(st.get());
            indent_--;
            return s + ind() + "}\n";
        }

        // ── persistent { } ───────────────────────────────────
        if (auto* n = dynamic_cast<PersistentBlock*>(node)) {
            std::string s = ind() + "persistent {\n";
            indent_++;
            for (auto& f : n->fields)
                s += ind() + f.name + ": " + fmtExpr(f.defaultValue.get()) + "\n";
            indent_--;
            return s + ind() + "}\n";
        }

        // ── migrate { } ──────────────────────────────────────
        if (auto* n = dynamic_cast<MigrateBlock*>(node)) {
            std::string s = ind() + "migrate {\n";
            indent_++;
            for (auto& f : n->fields)
                s += ind() + f.name + ": " + fmtExpr(f.value.get()) + "\n";
            indent_--;
            return s + ind() + "}\n";
        }

        // ── @threadpool(name: "io-pool", size: 4) ────────────
        if (auto* n = dynamic_cast<ThreadPoolDecl*>(node)) {
            return ind() + "@threadpool(name: \"" + n->name
                   + "\", size: " + std::to_string(n->size) + ")\n";
        }

        // ── module [...] { } ─────────────────────────────────
        if (auto* n = dynamic_cast<ModuleDecl*>(node)) {
            std::string s = ind();
            // @concurrent 注解（优先于 @supervised）
            if (!n->poolName.empty()) {
                s += "@concurrent(pool: \"" + n->poolName + "\"";
                if (n->poolQueue != 100) s += ", queue: " + std::to_string(n->poolQueue);
                if (n->poolOverflow != "block") s += ", overflow: ." + n->poolOverflow;
                s += ")\n" + ind();
            }
            if (n->restartPolicy == RestartPolicy::Always) {
                s += "@supervised(restart: .always, maxRetries: "
                     + std::to_string(n->maxRetries) + ")\n" + ind();
            } else if (n->restartPolicy == RestartPolicy::Never) {
                s += "@supervised(restart: .never, maxRetries: "
                     + std::to_string(n->maxRetries) + ")\n" + ind();
            }
            s += "module " + n->name + " {\n";
            indent_++;
            for (auto& st : n->body) s += fmtStmt(st.get());
            indent_--;
            return s + ind() + "}\n";
        }

        // ── spawn { } ────────────────────────────────────────
        if (auto* n = dynamic_cast<SpawnStmt*>(node)) {
            std::string s = ind() + "spawn {\n";
            indent_++;
            for (auto& st : n->body) s += fmtStmt(st.get());
            indent_--;
            return s + ind() + "}\n";
        }

        // ── exception declaration ────────────────────────────
        if (auto* n = dynamic_cast<ExceptionDecl*>(node)) {
            std::string s = ind() + "exception";
            if (!n->target.empty()) s += " " + n->target;
            s += " {\n";
            indent_++;
            for (auto& m : n->messages) s += ind() + "\"" + m + "\"\n";
            indent_--;
            return s + ind() + "}\n";
        }

        // ── struct literal ────────────────────────────────────
        if (auto* n = dynamic_cast<StructLit*>(node)) {
            std::string s = ind();
            if (!n->interfaceName.empty()) s += n->interfaceName + " ";
            s += "{\n";
            indent_++;
            for (auto& f : n->fields)
                s += ind() + f.name + ": " + fmtExpr(f.defaultValue.get()) + "\n";
            for (auto& m : n->methods) {
                s += ind() + "func " + m.name + "(";
                for (size_t i = 0; i < m.params.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += m.params[i].name;
                    if (!m.params[i].type.empty()) s += ": " + m.params[i].type;
                }
                s += ") {\n";
                indent_++;
                for (auto& st : m.body) s += fmtStmt(st.get());
                indent_--;
                s += ind() + "}\n";
            }
            indent_--;
            return s + ind() + "}\n";
        }

        // ── interface literal ─────────────────────────────────
        if (auto* n = dynamic_cast<InterfaceLit*>(node)) {
            std::string s = ind() + "{\n";
            indent_++;
            for (auto& m : n->methods) {
                s += ind() + "func " + m.name + "(";
                for (size_t i = 0; i < m.params.size(); ++i) {
                    if (i > 0) s += ", ";
                    s += m.params[i].name;
                }
                s += ")\n";
            }
            indent_--;
            return s + ind() + "}\n";
        }

        // 回退：直接尝试表达式格式化
        return ind() + fmtExpr(node) + "\n";
    }

    // ── 数字格式化：整数无小数点，浮点保留精度 ─────────────
    std::string fmtNum(double v) {
        if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
            return std::to_string((long long)v);
        }
        std::ostringstream oss;
        oss << v;
        return oss.str();
    }

    // ── 字符串转义 ───────────────────────────────────────────
    std::string escapeStr(const std::string& s) {
        std::string r;
        r.reserve(s.size() + 4);
        for (unsigned char c : s) {
            if      (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else if (c == '\t') r += "\\t";
            else if (c == '\r') r += "\\r";
            else if (c < 32)    r += "\\x" + hexByte(c);
            else                r += (char)c;
        }
        return r;
    }

    static std::string hexByte(unsigned char c) {
        const char* h = "0123456789abcdef";
        return { h[c >> 4], h[c & 0xf] };
    }
};
