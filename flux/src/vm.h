// vm.h — Flux 字节码虚拟机（栈式 VM）
// Feature G: 替换树遍历解释器，提升 5-10x 性能
#pragma once
#include "interpreter.h"   // Value, Environment, ModuleRuntime, Interpreter
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

// ═══════════════════════════════════════════════════════════
// 指令集
// ═══════════════════════════════════════════════════════════
enum class OpCode : uint8_t {
    // ── 常量 ──────────────────────────────────────────────
    PUSH_NIL,        // 压入 nil
    PUSH_TRUE,       // 压入 true
    PUSH_FALSE,      // 压入 false
    PUSH_CONST,      // 压入 constants[a]

    // ── 变量 ──────────────────────────────────────────────
    LOAD,            // 压入 env[names[a]]
    STORE,           // 弹出 → env[names[a]]   (变量必须已声明)
    DEFINE,          // 弹出 → 在当前 env 中声明 names[a]

    // ── 持久状态 ──────────────────────────────────────────
    LOAD_STATE,      // 压入 state[names[a]]
    STORE_STATE,     // 弹出 → state[names[a]]

    // ── 算术 / 逻辑 ───────────────────────────────────────
    ADD, SUB, MUL, DIV, MOD,
    NEG,             // 一元负号
    NOT,             // 一元逻辑非

    // ── 比较 ──────────────────────────────────────────────
    EQ, NEQ, LT, GT, LEQ, GEQ,

    // ── 作用域 ────────────────────────────────────────────
    PUSH_SCOPE,      // 压入子 Environment
    POP_SCOPE,       // 弹回父 Environment
    POP,             // 丢弃栈顶

    // ── 控制流（a = 绝对指令下标）───────────────────────
    JUMP,            // 无条件跳转
    JUMP_IF_FALSE,   // 弹出条件，若 falsy 则跳转
    JUMP_IF_TRUE,    // 弹出条件，若 truthy 则跳转

    // ── 函数调用 ──────────────────────────────────────────
    CALL,            // 调用 names[a]，参数个数 = b
    CALL_MODULE,     // 调用 "Mod.fn" = names[a]，参数个数 = b
    CALL_METHOD,     // 调用方法 names[a]，参数个数 = b（对象在栈顶 + b 之上）
    RETURN,          // 返回栈顶值
    RETURN_NIL,      // 返回 nil

    // ── 集合 ──────────────────────────────────────────────
    MAKE_ARRAY,      // 从栈顶 b 个值创建 Array（最先入栈的在前）
    MAKE_MAP,        // 从栈顶 2*b 个值创建 Map（key, val 交替）
    INDEX_GET,       // 弹出 idx，弹出 obj，压入 obj[idx]
    INDEX_SET,       // 弹出 val，弹出 idx，弹出 obj，obj[idx]=val，压入 val

    // ── 并发 ──────────────────────────────────────────────
    ASYNC_CALL,      // 异步调用 names[a]，参数个数 = b，返回 Future
    ASYNC_MODULE,    // 异步跨 pool 调用 "Module.fn" = names[a]，参数个数 = b
    AWAIT,           // 弹出 Future，等待并压入结果

    // ── AST 委托（结构体/接口/exception/spawn）──────────
    EVAL_AST,        // 委托给解释器 evalNode(ast_nodes[a], env)，结果压栈

    // ── 原生结构体/闭包/spawn 指令 ──────────────────────
    MAKE_CLOSURE,    // 创建闭包：ast_nodes[a] = FuncExpr，捕获当前 env
    FIELD_GET,       // 弹出 obj，压入 obj.field（names[a]=字段名）
    FIELD_SET,       // 弹出 val，弹出 obj，obj.field=val，压入 val
    STRUCT_CREATE,   // names[a]=类型名，b=字段数（栈: 类型值, name1, val1, ...)
    SPAWN_TASK,      // ast_nodes[a] = SpawnStmt，fire-and-forget
};

// ── 单条指令 ──────────────────────────────────────────────
struct Instruction {
    OpCode  op;
    int32_t a = 0;   // 主操作数（常量 / 名字下标 / 跳转目标）
    int32_t b = 0;   // 次操作数（参数个数 / 元素个数）
};

// ═══════════════════════════════════════════════════════════
// 字节码块（一个函数或顶层程序的全部指令）
// ═══════════════════════════════════════════════════════════
struct Chunk {
    std::vector<Instruction> code;
    std::vector<Value>       constants;   // 字面量池
    std::vector<std::string> names;       // 名字池（变量、函数、方法名）
    std::vector<ASTNode*>    ast_nodes;   // AST 节点引用（EVAL_AST 用）

    // 向常量池添加值（自动去重数字与字符串）
    int addConst(const Value& v) {
        // 简单追加（不去重，保证正确性；优化可后续加入）
        constants.push_back(v);
        return (int)constants.size() - 1;
    }

    // 向 AST 节点池添加节点引用（EVAL_AST 委托用）
    int addASTNode(ASTNode* node) {
        ast_nodes.push_back(node);
        return (int)ast_nodes.size() - 1;
    }

    // 向名字池添加名字（去重）
    int addName(const std::string& s) {
        for (int i = 0; i < (int)names.size(); ++i)
            if (names[i] == s) return i;
        names.push_back(s);
        return (int)names.size() - 1;
    }

    // 发射一条指令
    void emit(OpCode op, int a = 0, int b = 0) {
        code.push_back({op, a, b});
    }

    // 发射占位跳转指令，返回其下标（用于后续 patch）
    int emitJump(OpCode op) {
        code.push_back({op, 0, 0});
        return (int)code.size() - 1;
    }

    // 将跳转目标填入占位指令
    void patchJump(int pos, int target) {
        code[pos].a = target;
    }

    // 当前指令下标（用作跳转目标）
    int here() const { return (int)code.size(); }

    // 调试：反汇编输出
    void dump(const std::string& title = "Chunk") const;
};

// ═══════════════════════════════════════════════════════════
// 栈式虚拟机
// ═══════════════════════════════════════════════════════════
class VM {
public:
    explicit VM(Interpreter& interp);

    // 执行一个 Chunk，返回最终栈顶值（或 Nil）
    Value run(Chunk& chunk,
              std::shared_ptr<Environment> env,
              ModuleRuntime* mod = nullptr);

private:
    Interpreter& interp_;
    std::vector<Value> stack_;

    void  push(Value v)          { stack_.push_back(std::move(v)); }
    Value pop()                  { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
    Value& peek(int offset = 0)  { return stack_[stack_.size() - 1 - (size_t)offset]; }

    // 调用方法（复用 Interpreter 中的方法分发逻辑）
    Value dispatchMethod(Value& obj, const std::string& method,
                         std::vector<Value> args, ModuleRuntime* mod);
};
