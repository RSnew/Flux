#pragma once
#include "ast.h"
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <variant>
#include <functional>
#include <vector>
#include <stdexcept>
#include <memory>

// ── Value 类型 ────────────────────────────────────────────
// 前向声明（Array 需要 shared_ptr<vector<Value>>）
struct Value;
using ValueArray = std::shared_ptr<std::vector<Value>>;
using ValueMap   = std::shared_ptr<std::unordered_map<std::string, Value>>;

struct Value {
    enum class Type { Nil, Number, String, Bool, Array, Map };
    Type type = Type::Nil;

    double      number  = 0;
    std::string string;
    bool        boolean = false;
    ValueArray  array;          // Array 类型
    ValueMap    map;             // Map 类型

    static Value Nil()    { return {}; }
    static Value Num(double n)       { Value v; v.type = Type::Number; v.number = n; return v; }
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

    bool isTruthy() const {
        if (type == Type::Nil)    return false;
        if (type == Type::Bool)   return boolean;
        if (type == Type::Number) return number != 0;
        if (type == Type::String) return !string.empty();
        if (type == Type::Array)  return array && !array->empty();
        if (type == Type::Map)    return map   && !map->empty();
        return false;
    }

    std::string toString() const {
        if (type == Type::Nil)    return "nil";
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
        return "nil";
    }
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
    void  execute(Program* program);
    // REPL 模式：保留 globalEnv_ 和 functions_（增量执行）
    void  executeRepl(Program* program);
    // VM 模式：只做注册 + 初始化 + 模块声明，不执行普通语句
    void  initProgram(Program* program);
    void  registerBuiltin(const std::string& name, BuiltinFn fn);

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
};
