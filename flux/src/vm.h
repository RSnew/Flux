// vm.h — Flux 字节码虚拟机（栈式 VM）
// Feature G: 替换树遍历解释器，提升 5-10x 性能
// Turbo Mode: NaN-boxing + 内联调用栈 + computed goto → 逼近原生性能
#pragma once
#include "interpreter.h"   // Value, Environment, ModuleRuntime, Interpreter
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <cstring>
#include "jit.h"  // MachineCode

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

    OP_COUNT         // 操作码总数（用于 dispatch table 大小）
};

// ── 单条指令 ──────────────────────────────────────────────
struct Instruction {
    OpCode  op;
    int32_t a = 0;   // 主操作数（常量 / 名字下标 / 跳转目标）
    int32_t b = 0;   // 次操作数（参数个数 / 元素个数）
};

// ═══════════════════════════════════════════════════════════
// NaN-boxing — 将所有值压缩到 8 字节
// ═══════════════════════════════════════════════════════════
// IEEE 754 double: 如果指数全1且尾数非0 → NaN
// 我们用 quiet NaN 空间编码非 double 值：
//   普通 double → 原样存储
//   Nil:    0x7FFC000000000000
//   True:   0x7FFE000000000001
//   False:  0x7FFE000000000000
//   Object: 0xFFFA000000000000 | 48-bit pointer → Value*（堆上的完整 Value）

class NanVal {
public:
    static constexpr uint64_t TAG_NAN     = 0x7FF8000000000000ULL; // quiet NaN base
    static constexpr uint64_t TAG_NIL     = 0x7FFC000000000000ULL;
    static constexpr uint64_t TAG_TRUE    = 0x7FFE000000000001ULL;
    static constexpr uint64_t TAG_FALSE   = 0x7FFE000000000000ULL;
    static constexpr uint64_t TAG_UNSET   = 0x7FFD000000000000ULL; // 未初始化槽标记
    static constexpr uint64_t TAG_OBJ     = 0xFFFA000000000000ULL; // object pointer
    static constexpr uint64_t MASK_PTR    = 0x0000FFFFFFFFFFFFULL; // 48-bit pointer mask

    uint64_t bits;

    NanVal() : bits(TAG_NIL) {}
    NanVal(uint64_t b) : bits(b) {}

    // ── 构造 ──
    static NanVal fromDouble(double d) {
        NanVal v;
        std::memcpy(&v.bits, &d, 8);
        return v;
    }
    static NanVal nil()         { return NanVal(TAG_NIL); }
    static NanVal unset()       { return NanVal(TAG_UNSET); }
    static NanVal boolean(bool b) { return NanVal(b ? TAG_TRUE : TAG_FALSE); }
    static NanVal fromObj(Value* ptr) {
        return NanVal(TAG_OBJ | (reinterpret_cast<uint64_t>(ptr) & MASK_PTR));
    }

    // ── 类型检查 ──
    bool isDouble() const { return (bits & TAG_NAN) != TAG_NAN || bits == 0x7FF8000000000000ULL; }
    bool isNil()    const { return bits == TAG_NIL; }
    bool isUnset()  const { return bits == TAG_UNSET; }
    bool isBool()   const { return (bits & 0xFFFE000000000000ULL) == 0x7FFE000000000000ULL; }
    bool isTrue()   const { return bits == TAG_TRUE; }
    bool isFalse()  const { return bits == TAG_FALSE; }
    bool isObj()    const { return (bits & 0xFFFF000000000000ULL) == TAG_OBJ; }

    // ── 取值 ──
    double asDouble() const {
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    bool asBool() const { return bits == TAG_TRUE; }
    Value* asObj() const {
        return reinterpret_cast<Value*>(bits & MASK_PTR);
    }

    // ── 真值判断 ──
    bool isTruthy() const {
        if (isNil() || isFalse()) return false;
        if (isTrue()) return true;
        if (isDouble()) return asDouble() != 0.0;
        if (isObj()) return asObj()->isTruthy();
        return true;
    }
};

// ═══════════════════════════════════════════════════════════
// 字节码块（一个函数或顶层程序的全部指令）
// ═══════════════════════════════════════════════════════════
struct Chunk {
    std::vector<Instruction> code;
    std::vector<Value>       constants;   // 字面量池
    std::vector<std::string> names;       // 名字池（变量、函数、方法名）
    std::vector<ASTNode*>    ast_nodes;   // AST 节点引用（EVAL_AST 用）

    // 常量池去重索引
    std::unordered_map<double, int>      numConstIdx_;   // 数字常量去重
    std::unordered_map<std::string, int> strConstIdx_;   // 字符串常量去重
    // 名字池去重索引
    std::unordered_map<std::string, int> nameIdx_;       // O(1) 名字查找

    // Turbo 模式预编译：NaN-boxed 常量池
    std::vector<NanVal> nanConstants;

    // JIT 缓存：names[i] 对应的 JIT 函数指针（nullptr = 无 JIT）
    using JitFn = double(*)(double*, int);
    std::vector<JitFn> jitCache;

    // 预编译 NaN 常量池
    void prepareNanConstants() {
        nanConstants.resize(constants.size());
        for (size_t i = 0; i < constants.size(); ++i) {
            auto& c = constants[i];
            if (c.type == Value::Type::Number)
                nanConstants[i] = NanVal::fromDouble(c.number);
            else if (c.type == Value::Type::Bool)
                nanConstants[i] = NanVal::boolean(c.boolean);
            else if (c.type == Value::Type::Nil)
                nanConstants[i] = NanVal::nil();
            else
                nanConstants[i] = NanVal::nil(); // 非基础类型用 nil 占位（通过慢路径处理）
        }
    }

    // 向常量池添加值（数字和字符串自动去重）
    int addConst(const Value& v) {
        if (v.type == Value::Type::Number) {
            auto it = numConstIdx_.find(v.number);
            if (it != numConstIdx_.end()) return it->second;
            int idx = (int)constants.size();
            constants.push_back(v);
            numConstIdx_[v.number] = idx;
            return idx;
        }
        if (v.type == Value::Type::String) {
            auto it = strConstIdx_.find(v.string);
            if (it != strConstIdx_.end()) return it->second;
            int idx = (int)constants.size();
            constants.push_back(v);
            strConstIdx_[v.string] = idx;
            return idx;
        }
        constants.push_back(v);
        return (int)constants.size() - 1;
    }

    // 向 AST 节点池添加节点引用（EVAL_AST 委托用）
    int addASTNode(ASTNode* node) {
        ast_nodes.push_back(node);
        return (int)ast_nodes.size() - 1;
    }

    // 向名字池添加名字（O(1) 去重）
    int addName(const std::string& s) {
        auto it = nameIdx_.find(s);
        if (it != nameIdx_.end()) return it->second;
        int idx = (int)names.size();
        names.push_back(s);
        nameIdx_[s] = idx;
        return idx;
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
// Turbo 模式调用栈帧
// ═══════════════════════════════════════════════════════════
struct TurboFrame {
    Chunk*  chunk;       // 当前执行的字节码块
    size_t  ip;          // 保存的指令指针
    int     localsBase;  // 局部变量基地址（在 locals_ 数组中的偏移）
    int     numLocals;   // 该帧的局部变量槽数
    size_t  stackBase;   // 保存的栈基（用于返回时清理）
};

// ═══════════════════════════════════════════════════════════
// 栈式虚拟机
// ═══════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════
// 字节码 JIT — Chunk 直接编译为 x86-64 原生代码
// ═══════════════════════════════════════════════════════════
struct NativeFunc {
    // 函数签名：double(double* args, int nargs)
    // args[0] = 第一个参数，args[1] = 第二个参数...
    using Fn = double(*)(double*, int);
    std::shared_ptr<MachineCode> code;
    Fn fn = nullptr;
};

class BytecodeJIT {
public:
    // 尝试将一个纯数值函数的 Chunk 编译为 x86-64 原生代码
    // 返回 nullptr 如果函数不适合 JIT（含字符串/数组/对象操作等）
    std::shared_ptr<NativeFunc> compile(const std::string& name,
                                        Chunk& chunk, FnDecl* fnDecl);

    // 查找已编译的原生函数
    NativeFunc::Fn find(const std::string& name) const {
        auto it = compiled_.find(name);
        return it != compiled_.end() ? it->second->fn : nullptr;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<NativeFunc>> compiled_;

    bool canJIT(Chunk& chunk, const std::string& selfName) const;
    // 检测简单累积递归并编译为循环
    std::shared_ptr<NativeFunc> compileAsLoop(const std::string& name,
                                              Chunk& chunk, FnDecl* fnDecl);
};

class VM {
public:
    explicit VM(Interpreter& interp);

    // 执行一个 Chunk，返回最终栈顶值（或 Nil）
    Value run(Chunk& chunk,
              std::shared_ptr<Environment> env,
              ModuleRuntime* mod = nullptr);

    // 预编译所有已注册的用户函数为字节码（在 run 之前调用）
    void compileFunctions();

private:
    Interpreter& interp_;
    std::vector<Value> stack_;

    // 已编译的函数字节码缓存
    std::unordered_map<std::string, Chunk> compiledFns_;

    // 字节码 JIT 编译器
    BytecodeJIT jit_;

    // ── Turbo 模式成员 ──
    static constexpr int TURBO_MAX_LOCALS = 8192;  // 局部变量槽总数
    static constexpr int TURBO_MAX_FRAMES = 512;   // 最大调用深度
    std::vector<NanVal>      locals_;      // 平坦局部变量数组
    std::vector<TurboFrame>  frames_;      // 调用栈
    std::vector<NanVal>      nanStack_;    // NaN-boxed 值栈

    // Turbo 模式：高性能执行编译后的函数
    // 使用 NaN-boxing + 平坦局部变量 + 内联调用栈
    Value runTurbo(Chunk& chunk, FnDecl* fn,
                   std::vector<Value>& args, ModuleRuntime* mod);

    void  push(Value v)          { stack_.push_back(std::move(v)); }
    Value pop()                  { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
    Value& peek(int offset = 0)  { return stack_[stack_.size() - 1 - (size_t)offset]; }

    // 调用方法（复用 Interpreter 中的方法分发逻辑）
    Value dispatchMethod(Value& obj, const std::string& method,
                         std::vector<Value> args, ModuleRuntime* mod);
};
