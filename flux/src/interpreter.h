#pragma once
#include "ast.h"
#include "concurrency.h"
#include "threadpool.h"
#include "taint.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <variant>
#include <functional>
#include <vector>
#include <stdexcept>
#include <memory>
#include <future>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <optional>

// ── Value 类型 ────────────────────────────────────────────
// 前向声明
struct Value;
struct FutureVal;
struct ChanVal;
struct StructTypeInfo;
struct StructInstInfo;
struct InterfaceInfo;
struct FuncVal;
using ValueArray      = std::shared_ptr<std::vector<Value>>;
using ValueMap        = std::shared_ptr<std::unordered_map<std::string, Value>>;
using ValueFuture     = std::shared_ptr<FutureVal>;
using ValueChan       = std::shared_ptr<ChanVal>;
using ValueStructType = std::shared_ptr<StructTypeInfo>;
using ValueStructInst = std::shared_ptr<StructInstInfo>;
using ValueInterface  = std::shared_ptr<InterfaceInfo>;
using ValueFunc       = std::shared_ptr<FuncVal>;

// 前向声明 SpecifyTypeInfo（定义在 Value 之后）
struct SpecifyTypeInfo;
using ValueSpecify = std::shared_ptr<SpecifyTypeInfo>;

struct Value {
    enum class Type { Nil, Number, String, Bool, Array, Map, Future, Chan,
                      StructType, StructInst, Interface, Function, Addr, Specify };
    Type type = Type::Nil;

    double      number  = 0;
    uintptr_t   addr    = 0;    // Addr 类型：无精度损失的指针地址
    std::string string;
    bool        boolean = false;
    ValueArray      array;       // Array 类型
    ValueMap        map;         // Map 类型
    ValueFuture     future;      // Future 类型（async 返回值）
    ValueChan       chan;        // Chan 类型（并发通道）
    ValueStructType structType;  // StructType（结构体定义）
    ValueStructInst structInst;  // StructInst（结构体实例）
    ValueInterface  iface;       // Interface（接口定义）
    ValueFunc       func;        // Function（一等公民函数值）
    ValueSpecify    specify;     // Specify（规格声明类型值）

    static Value Nil()    { return {}; }
    static Value Num(double n)       { Value v; v.type = Type::Number; v.number = n; return v; }
    static Value AddrVal(uintptr_t a){ Value v; v.type = Type::Addr;   v.addr = a;   return v; }
    static Value AddrVal(void* p)    { Value v; v.type = Type::Addr;   v.addr = reinterpret_cast<uintptr_t>(p); return v; }
    static Value Str(std::string s)  { Value v; v.type = Type::String; v.string = std::move(s); return v; }
    static Value Bool(bool b)        { Value v; v.type = Type::Bool;   v.boolean = b; return v; }
    static Value Arr(ValueArray a)   { Value v; v.type = Type::Array;  v.array = std::move(a); return v; }
    static Value Array()             {
        Value v; v.type = Type::Array;
        v.array = std::make_shared<std::vector<Value>>();
        return v;
    }
    static Value MapVal() {
        Value v; v.type = Type::Map;
        v.map = std::make_shared<std::unordered_map<std::string, Value>>();
        return v;
    }
    static Value MapOf(ValueMap m) {
        Value v; v.type = Type::Map; v.map = std::move(m); return v;
    }
    static Value Future(ValueFuture f) {
        Value v; v.type = Type::Future; v.future = std::move(f); return v;
    }
    static Value Chan(ValueChan c) {
        Value v; v.type = Type::Chan; v.chan = std::move(c); return v;
    }
    static Value StructTypeV(ValueStructType s) {
        Value v; v.type = Type::StructType; v.structType = std::move(s); return v;
    }
    static Value StructInstV(ValueStructInst s) {
        Value v; v.type = Type::StructInst; v.structInst = std::move(s); return v;
    }
    static Value InterfaceV(ValueInterface i) {
        Value v; v.type = Type::Interface; v.iface = std::move(i); return v;
    }
    static Value FuncV(ValueFunc f) {
        Value v; v.type = Type::Function; v.func = std::move(f); return v;
    }
    static Value SpecifyV(ValueSpecify a) {
        Value v; v.type = Type::Specify; v.specify = std::move(a); return v;
    }

    bool isTruthy() const {
        if (type == Type::Nil)    return false;
        if (type == Type::Bool)   return boolean;
        if (type == Type::Number) return number != 0;
        if (type == Type::String) return !string.empty();
        if (type == Type::Array)  return array && !array->empty();
        if (type == Type::Map)    return map   && !map->empty();
        if (type == Type::Future)     return future     != nullptr;
        if (type == Type::Chan)       return chan        != nullptr;
        if (type == Type::StructType) return structType != nullptr;
        if (type == Type::StructInst) return structInst != nullptr;
        if (type == Type::Interface)  return iface      != nullptr;
        if (type == Type::Function)  return func       != nullptr;
        if (type == Type::Specify)   return specify     != nullptr;
        if (type == Type::Addr)      return addr       != 0;
        return false;
    }

    std::string toString() const {
        if (type == Type::Nil)    return "null";
        if (type == Type::Number) {
            if (number == (long long)number) return std::to_string((long long)number);
            return std::to_string(number);
        }
        if (type == Type::String) return string;
        if (type == Type::Bool)   return boolean ? "true" : "false";
        if (type == Type::Array) {
            std::string s = "[";
            if (array) {
                for (size_t i = 0; i < array->size(); i++) {
                    if (i > 0) s += ", ";
                    s += (*array)[i].toString();
                }
            }
            s += "]";
            return s;
        }
        if (type == Type::Map) {
            std::string s = "{";
            if (map) {
                bool first = true;
                for (auto& kv : *map) {
                    if (!first) s += ", ";
                    s += kv.first + ": " + kv.second.toString();
                    first = false;
                }
            }
            return s + "}";
        }
        if (type == Type::Future)     return "<Future>";
        if (type == Type::Chan)       return "<Chan>";
        // Struct types: full toString() implemented in interpreter.cpp (after complete types)
        if (type == Type::StructType) return "<StructType>";
        if (type == Type::StructInst) return "<StructInst>";
        if (type == Type::Interface)  return "<Interface>";
        if (type == Type::Function)  return "<Function>";
        if (type == Type::Specify) return toStringSpecify();
        if (type == Type::Addr) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)addr);
            return std::string(buf);
        }
        return "null";
    }

    // Specify toString (defined after SpecifyTypeInfo is complete)
    std::string toStringSpecify() const;
    // Full toString for struct instances (defined after StructInstInfo is complete)
    std::string toStringFull() const;
};

// ── Specify 类型信息（规格声明的运行时表示）──────────────────
struct SpecifyTypeInfo {
    std::string name;         // 变量名
    std::string intent;       // 意图描述
    Value       input;        // 输入 schema（通常是 Map）
    Value       output;       // 输出 schema
    Value       constraints;  // 约束列表（Array of String）
    Value       examples;     // 示例列表（Array of Map）
};

// ── FutureVal — async 操作的异步结果持有器 ────────────────
struct FutureVal {
    std::shared_future<Value> fut;
    explicit FutureVal(std::shared_future<Value> f) : fut(std::move(f)) {}

    // 无超时等待（释放 GIL 以允许其他 Flux 线程运行）
    Value get() {
        GILRelease release;
        return fut.get();
    }

    // 带超时等待：timeout_ms < 0 表示无限等待
    // 超时抛出 runtime_error，其余同 get()
    Value getWithTimeout(int timeout_ms) {
        GILRelease release;
        if (timeout_ms < 0) return fut.get();
        auto status = fut.wait_for(std::chrono::milliseconds(timeout_ms));
        if (status == std::future_status::timeout)
            throw std::runtime_error("future.await() timed out after " +
                                     std::to_string(timeout_ms) + "ms");
        return fut.get();
    }

    bool isReady() const {
        return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
};

// ── ChanVal — 线程安全的有界/无界消息通道 ─────────────────
struct ChanVal {
    std::deque<Value>       q;
    mutable std::mutex      mu;
    std::condition_variable cv_recv;
    std::condition_variable cv_send;
    size_t                  cap;       // 0 = 无界
    bool                    closed_ = false;

    explicit ChanVal(size_t capacity = 0) : cap(capacity) {}

    // 发送（有界通道满时阻塞，期间释放 GIL）
    bool send(Value v);
    // 阻塞接收（空时阻塞，期间释放 GIL）
    std::optional<Value> recv();
    // 非阻塞接收（空返回 nullopt，不阻塞）
    std::optional<Value> tryRecv();
    void  close();
    bool  isClosed() const { std::unique_lock<std::mutex> lk(mu); return closed_; }
    size_t len()     const { std::unique_lock<std::mutex> lk(mu); return q.size(); }
};

// ── 结构体类型信息（结构体定义）──────────────────────────
struct StructTypeInfo {
    std::string name;
    std::string interfaceName;  // 实现的接口名（可为空）

    struct Field {
        std::string                name;
        std::shared_ptr<ASTNode>   defaultExpr;   // 可为 null
    };
    struct Method {
        std::string                              name;
        std::vector<Param>                       params;
        std::string                              returnType;
        std::vector<std::shared_ptr<ASTNode>>    body;
    };
    std::vector<Field>  fields;
    std::vector<Method> methods;

    const Method* findMethod(const std::string& n) const {
        for (auto& m : methods) if (m.name == n) return &m;
        return nullptr;
    }
    const Field* findField(const std::string& n) const {
        for (auto& f : fields) if (f.name == n) return &f;
        return nullptr;
    }
};

// ── 结构体实例 ────────────────────────────────────────────
struct StructInstInfo {
    std::shared_ptr<StructTypeInfo>        type;
    std::unordered_map<std::string, Value> fields;
};

// ── 接口定义 ──────────────────────────────────────────────
struct InterfaceInfo {
    std::string name;
    struct MethodSig {
        std::string        name;
        std::vector<Param> params;
    };
    std::vector<MethodSig> methods;
};

// ── Panic 信号（区别于普通 runtime_error）────────────────
struct PanicSignal {
    std::string message;
};


struct ReturnSignal {
    Value value;
};

// ── 环境（变量作用域）────────────────────────────────────
class Environment {
public:
    explicit Environment(std::shared_ptr<Environment> parent = nullptr)
        : parent_(std::move(parent)) {}

    void set(const std::string& name, Value val) { vars_[name] = std::move(val); }

    Value get(const std::string& name) const {
        auto it = vars_.find(name);
        if (it != vars_.end()) return it->second;
        if (parent_) return parent_->get(name);
        throw std::runtime_error("undefined variable: " + name);
    }

    bool assign(const std::string& name, Value val) {
        auto it = vars_.find(name);
        if (it != vars_.end()) { it->second = std::move(val); return true; }
        if (parent_) return parent_->assign(name, val);
        return false;
    }

    bool has(const std::string& name) const {
        if (vars_.count(name)) return true;
        if (parent_) return parent_->has(name);
        return false;
    }

    // VM 用于 POP_SCOPE
    std::shared_ptr<Environment> parent() const { return parent_; }

private:
    std::unordered_map<std::string, Value>  vars_;
    std::shared_ptr<Environment>            parent_;
};

// ── 函数值（一等公民函数 / 匿名函数 / 闭包）──────────────
struct FuncVal {
    std::string        name;     // 具名函数名，匿名则为 ""
    std::vector<Param> params;
    // 函数体：匿名函数 → 拥有 AST（owned），具名引用 → 借用（borrowed）
    std::vector<std::shared_ptr<ASTNode>> ownedBody;  // 匿名函数拥有的 body
    FnDecl*                               fnDecl = nullptr; // 具名函数引用
    std::shared_ptr<Environment>          closure;     // 闭包捕获的环境
};

// ── 内置函数类型 ──────────────────────────────────────────
using BuiltinFn = std::function<Value(std::vector<Value>)>;
using StdlibFn  = std::function<Value(std::vector<Value>)>;

// ── 模块运行时（每个模块独立的状态空间）─────────────────
struct ModuleRuntime {
    std::string                                name;
    std::unordered_map<std::string, FnDecl*>   functions;
    std::unordered_map<std::string, Value>     persistentStore;
    std::unordered_set<std::string>            knownFields;

    // ── 监督器状态 ──────────────────────────────────────
    RestartPolicy  restartPolicy = RestartPolicy::None;
    int            maxRetries    = 3;
    int            crashCount    = 0;
    bool           crashed       = false;   // .never 策略下崩溃后永久停止
    ModuleDecl*    decl          = nullptr; // 指回 AST，用于重启时重新初始化
};

// ── 解释器 ───────────────────────────────────────────────
class Compiler;  // forward declaration
class VM;        // forward declaration

class Interpreter {
public:
    Interpreter();
    ~Interpreter();          // 等待所有 pending async 任务完成
    void  execute(Program* program);
    // REPL 模式：保留 globalEnv_ 和 functions_（增量执行）
    void  executeRepl(Program* program);
    // VM 模式：只做注册 + 初始化 + 模块声明，不执行普通语句
    void  initProgram(Program* program);
    void  registerBuiltin(const std::string& name, BuiltinFn fn);
    void  setNoTest(bool v) { noTest_ = v; }

    // ── VM / Compiler 需要调用的公共接口 ─────────────────
    Value callFunction(const std::string& name, std::vector<Value> args,
                       ModuleRuntime* mod = nullptr);
    Value callModuleFunction(const std::string& modName,
                             const std::string& fnName,
                             std::vector<Value> args);

    // ── VM / Compiler 直接访问的公共成员 ─────────────────
    std::shared_ptr<Environment>               globalEnv_;
    std::unordered_map<std::string, FnDecl*>   functions_;
    std::unordered_map<std::string, Value>     persistentStore_;

private:
    friend class VM;
    friend class Compiler;

    std::unordered_map<std::string, BuiltinFn> builtins_;

    // ── 模块表：moduleName → ModuleRuntime ──────────────
    std::unordered_map<std::string, ModuleRuntime> modules_;

    // ── 标准库模块表（C++ 实现）──────────────────────────
    std::unordered_map<std::string, std::unordered_map<std::string, StdlibFn>> stdlibModules_;

    void  registerBuiltins();
    void  registerStdlibModule(const std::string& name,
                                std::unordered_map<std::string, StdlibFn> fns);
    void  registerStdlib();   // 在 stdlib.cpp 中实现
    void  executeModule(ModuleDecl* decl, std::shared_ptr<Environment> env);

    Value evalNode(ASTNode* node, std::shared_ptr<Environment> env,
                   ModuleRuntime* mod = nullptr);
    Value evalBinary(BinaryExpr* node, std::shared_ptr<Environment> env,
                     ModuleRuntime* mod = nullptr);

    // ── 并发任务追踪（Feature K）──────────────────────────
    // 析构时 join 所有未完成的 async/spawn 任务，防止悬空引用
    std::vector<std::shared_future<Value>> pendingTasks_;

    // ── 线程池管理（Feature K v2）────────────────────────
    // poolName → ThreadPool 实例（@threadpool 声明时创建）
    std::unordered_map<std::string, std::shared_ptr<ThreadPool>> pools_;
    // moduleName → poolName（@concurrent 绑定时注册）
    std::unordered_map<std::string, std::string> modulePools_;

    // 获取模块绑定的线程池（nullptr 表示无绑定）
    ThreadPool* getModulePool(const std::string& moduleName);

    // ── conf 常量名集合（运行时只读）──────────────────────
    std::unordered_set<std::string> constants_;

    // ── Spec v1.0 新增 ────────────────────────────────────
    // exception 描述表：target → [message1, message2, ...]
    std::unordered_map<std::string, std::vector<std::string>> exceptionDescs_;
    // 内联 exception 描述（当前作用域中最后遇到的描述）
    std::vector<std::string> lastInlineExceptionDescs_;
    // default 声明表：target → DefaultDecl body (AST nodes)
    std::unordered_map<std::string, std::vector<ASTNode*>> defaultBodies_;

    // 创建结构体实例（StructTypeInfo + 具名参数）
    Value createStructInst(std::shared_ptr<StructTypeInfo> type,
                           const std::vector<std::pair<std::string,Value>>& initFields,
                           std::shared_ptr<Environment> env, ModuleRuntime* mod);

    // 调用函数值（匿名函数 / 闭包 / 具名函数引用）
    Value callFuncVal(std::shared_ptr<FuncVal> fv, std::vector<Value> args,
                      ModuleRuntime* mod = nullptr);

    // ── --no-test 模式标志 ────────────────────────────────
    bool noTest_ = false;

    // ── APC Phase 2: Cross-module taint tracking ────────────
    TaintTracker taintTracker_;
};
