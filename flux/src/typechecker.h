#pragma once
#include "ast.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <stdexcept>
#include <sstream>

// ═══════════════════════════════════════════════════════════
// FluxType  —  Flux 类型系统的核心表示
// ═══════════════════════════════════════════════════════════
enum class TypeKind {
    Unknown,    // 尚未推断（内部用）
    Any,        // 无类型注解时的动态类型（允许所有操作）
    Int,
    Float,
    String,
    Bool,
    Nil,
    Fn,         // 函数类型（含参数/返回类型列表）
};

struct FluxType {
    explicit FluxType(TypeKind k = TypeKind::Any) : kind(k) {}
    TypeKind kind = TypeKind::Any;

    // Fn 类型的参数和返回类型
    std::vector<FluxType> paramTypes;
    std::shared_ptr<FluxType> returnType;

    // ── 工厂方法 ──────────────────────────────────────────
    static FluxType Any()     { return FluxType(TypeKind::Any); }
    static FluxType Int()     { return FluxType(TypeKind::Int); }
    static FluxType Float()   { return FluxType(TypeKind::Float); }
    static FluxType String()  { return FluxType(TypeKind::String); }
    static FluxType Bool()    { return FluxType(TypeKind::Bool); }
    static FluxType Nil()     { return FluxType(TypeKind::Nil); }
    static FluxType Unknown() { return FluxType(TypeKind::Unknown); }

    std::string name() const {
        switch (kind) {
            case TypeKind::Any:     return "Any";
            case TypeKind::Int:     return "Int";
            case TypeKind::Float:   return "Float";
            case TypeKind::String:  return "String";
            case TypeKind::Bool:    return "Bool";
            case TypeKind::Nil:     return "Nil";
            case TypeKind::Fn:      return "Fn";
            case TypeKind::Unknown: return "Unknown";
        }
        return "?";
    }

    // 两种类型是否相容（Any 总是相容）
    bool compatibleWith(const FluxType& other) const {
        if (kind == TypeKind::Any || other.kind == TypeKind::Any) return true;
        if (kind == TypeKind::Unknown || other.kind == TypeKind::Unknown) return true;
        // Int 和 Float 相互兼容（数值宽化）
        if ((kind == TypeKind::Int || kind == TypeKind::Float) &&
            (other.kind == TypeKind::Int || other.kind == TypeKind::Float)) return true;
        return kind == other.kind;
    }

    bool isNumeric() const {
        return kind == TypeKind::Int || kind == TypeKind::Float || kind == TypeKind::Any;
    }

    bool operator==(const FluxType& o) const { return kind == o.kind; }
    bool operator!=(const FluxType& o) const { return !(*this == o); }
};

// ── 从字符串名称解析类型 ────────────────────────────────
inline FluxType parseTypeName(const std::string& name) {
    if (name == "Int")    return FluxType::Int();
    if (name == "Float")  return FluxType::Float();
    if (name == "String") return FluxType::String();
    if (name == "Bool")   return FluxType::Bool();
    if (name == "Nil")    return FluxType::Nil();
    if (name == "Any" || name.empty()) return FluxType::Any();
    return FluxType::Any(); // 未知类型降级为 Any
}

// ═══════════════════════════════════════════════════════════
// TypeEnv  —  类型环境（作用域链）
// ═══════════════════════════════════════════════════════════
class TypeEnv {
public:
    explicit TypeEnv(std::shared_ptr<TypeEnv> parent = nullptr)
        : parent_(std::move(parent)) {}

    void define(const std::string& name, FluxType type) {
        vars_[name] = std::move(type);
    }

    FluxType lookup(const std::string& name) const {
        auto it = vars_.find(name);
        if (it != vars_.end()) return it->second;
        if (parent_) return parent_->lookup(name);
        return FluxType::Any(); // 未定义变量当 Any（容错）
    }

    bool has(const std::string& name) const {
        if (vars_.count(name)) return true;
        if (parent_) return parent_->has(name);
        return false;
    }

    void update(const std::string& name, FluxType type) {
        if (vars_.count(name)) { vars_[name] = type; return; }
        if (parent_) parent_->update(name, type);
    }

private:
    std::unordered_map<std::string, FluxType> vars_;
    std::shared_ptr<TypeEnv>                  parent_;
};

// ═══════════════════════════════════════════════════════════
// TypeError  —  类型错误记录
// ═══════════════════════════════════════════════════════════
struct TypeError {
    std::string message;
    int         line;

    TypeError(std::string msg, int ln = 0)
        : message(std::move(msg)), line(ln) {}
};

// ═══════════════════════════════════════════════════════════
// FnSignature  —  函数签名注册表
// ═══════════════════════════════════════════════════════════
struct FnSignature {
    std::vector<FluxType> paramTypes;
    FluxType              returnType;
    bool                  isVariadic = false; // 内置函数可以是可变参数
};

// ═══════════════════════════════════════════════════════════
// TypeChecker  —  类型推断 + 检查的 AST 遍历器
// ═══════════════════════════════════════════════════════════
class TypeChecker {
public:
    TypeChecker();

    // 主入口：检查整个程序，返回错误列表
    std::vector<TypeError> check(Program* program);

    // 模块内部检查
    void checkModule(ModuleDecl* mod, std::shared_ptr<TypeEnv> env);

private:
    std::unordered_map<std::string, FnSignature>  globalFns_;   // 用户函数签名
    std::unordered_map<std::string, FnSignature>  builtins_;    // 内置函数签名
    std::vector<TypeError>                        errors_;

    // 当前函数的期望返回类型（用于检查 return 语句）
    FluxType currentReturnType_ = FluxType::Any();

    void registerBuiltins();
    void registerFunctions(Program* program, std::shared_ptr<TypeEnv> env);
    void registerModuleFunctions(ModuleDecl* mod, std::shared_ptr<TypeEnv> env);

    // 推断表达式类型
    FluxType inferExpr(ASTNode* node, std::shared_ptr<TypeEnv> env);

    // 检查语句（不返回类型）
    void checkStmt(ASTNode* node, std::shared_ptr<TypeEnv> env);
    void checkBlock(const std::vector<NodePtr>& stmts, std::shared_ptr<TypeEnv> env);

    // 错误收集
    void error(const std::string& msg, int line = 0);
};
