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
    Int,        // 有符号整数
    UInt,       // 无符号整数
    Float,      // 浮点数
    Natural,    // 自然数（>= 0）
    Byte,       // 0-255
    Addr,       // 内存地址（用于硬件操作）
    String,
    Bool,
    Nil,
    Array,      // 数组类型
    Map,        // Map 类型
    Fn,         // 函数类型（含参数/返回类型列表）
    Struct,     // 结构体类型
    Interface,  // 接口类型
    AI,         // AI 原生类型
};

struct FluxType {
    explicit FluxType(TypeKind k = TypeKind::Any) : kind(k) {}
    TypeKind kind = TypeKind::Any;

    // Fn 类型的参数和返回类型
    std::vector<FluxType> paramTypes;
    std::shared_ptr<FluxType> returnType;

    // ── 工厂方法 ──────────────────────────────────────────
    static FluxType Any()       { return FluxType(TypeKind::Any); }
    static FluxType Int()       { return FluxType(TypeKind::Int); }
    static FluxType UInt()      { return FluxType(TypeKind::UInt); }
    static FluxType Float()     { return FluxType(TypeKind::Float); }
    static FluxType Natural()   { return FluxType(TypeKind::Natural); }
    static FluxType Byte()      { return FluxType(TypeKind::Byte); }
    static FluxType Addr()      { return FluxType(TypeKind::Addr); }
    static FluxType String()    { return FluxType(TypeKind::String); }
    static FluxType Bool()      { return FluxType(TypeKind::Bool); }
    static FluxType Nil()       { return FluxType(TypeKind::Nil); }
    static FluxType ArrayT()    { return FluxType(TypeKind::Array); }
    static FluxType MapT()      { return FluxType(TypeKind::Map); }
    static FluxType StructT()   { return FluxType(TypeKind::Struct); }
    static FluxType InterfaceT(){ return FluxType(TypeKind::Interface); }
    static FluxType AIT()       { return FluxType(TypeKind::AI); }
    static FluxType Unknown()   { return FluxType(TypeKind::Unknown); }

    std::string name() const {
        switch (kind) {
            case TypeKind::Any:       return "Any";
            case TypeKind::Int:       return "Int";
            case TypeKind::UInt:      return "UInt";
            case TypeKind::Float:     return "Float";
            case TypeKind::Natural:   return "Natural";
            case TypeKind::Byte:      return "Byte";
            case TypeKind::Addr:      return "Addr";
            case TypeKind::String:    return "String";
            case TypeKind::Bool:      return "Bool";
            case TypeKind::Nil:       return "Nil";
            case TypeKind::Array:     return "Array";
            case TypeKind::Map:       return "Map";
            case TypeKind::Fn:        return "Fn";
            case TypeKind::Struct:    return "Struct";
            case TypeKind::Interface: return "Interface";
            case TypeKind::AI:        return "AI";
            case TypeKind::Unknown:   return "Unknown";
        }
        return "?";
    }

    // 两种类型是否相容（Any 总是相容）
    bool compatibleWith(const FluxType& other) const {
        if (kind == TypeKind::Any || other.kind == TypeKind::Any) return true;
        if (kind == TypeKind::Unknown || other.kind == TypeKind::Unknown) return true;
        // 所有数字类型相互兼容（数值宽化）
        if (isNumeric() && other.isNumeric()) return true;
        return kind == other.kind;
    }

    bool isNumeric() const {
        return kind == TypeKind::Int || kind == TypeKind::UInt ||
               kind == TypeKind::Float || kind == TypeKind::Natural ||
               kind == TypeKind::Byte || kind == TypeKind::Addr ||
               kind == TypeKind::Any;
    }

    bool operator==(const FluxType& o) const { return kind == o.kind; }
    bool operator!=(const FluxType& o) const { return !(*this == o); }
};

// ── 从字符串名称解析类型 ────────────────────────────────
inline FluxType parseTypeName(const std::string& name) {
    if (name == "Int")     return FluxType::Int();
    if (name == "UInt")    return FluxType::UInt();
    if (name == "Float")   return FluxType::Float();
    if (name == "Natural") return FluxType::Natural();
    if (name == "Byte")    return FluxType::Byte();
    if (name == "Addr")    return FluxType::Addr();
    if (name == "String")  return FluxType::String();
    if (name == "Bool")    return FluxType::Bool();
    if (name == "Nil")     return FluxType::Nil();
    if (name == "Array")   return FluxType::ArrayT();
    if (name == "Map")     return FluxType::MapT();
    if (name == "AI")      return FluxType::AIT();
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
    std::string code;       // 错误代码 (E1001, E1002, ...)
    std::string category;   // 错误类别 (type-mismatch, undefined-var, ...)
    std::string suggestion; // 修复建议

    TypeError(std::string msg, int ln = 0, std::string c = "", std::string cat = "", std::string sug = "")
        : message(std::move(msg)), line(ln), code(std::move(c))
        , category(std::move(cat)), suggestion(std::move(sug)) {}

    // 输出为 JSON 字符串（供 flux check --json 使用）
    std::string toJson() const {
        std::string json = "{";
        json += "\"code\":\"" + code + "\",";
        json += "\"category\":\"" + category + "\",";
        json += "\"message\":\"" + message + "\",";
        json += "\"line\":" + std::to_string(line) + ",";
        json += "\"suggestion\":\"" + suggestion + "\"";
        json += "}";
        return json;
    }
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

    // Spec v1.0 检查
    void checkInterfaceConformance(Program* program, std::shared_ptr<TypeEnv> env);
    void checkExceptionRefs(Program* program, std::shared_ptr<TypeEnv> env);

    // 接口/结构体注册表（名字 → 方法名列表）
    std::unordered_map<std::string, std::vector<std::string>> interfaces_;
    std::unordered_map<std::string, std::vector<std::string>> structTypes_;
    std::unordered_map<std::string, std::string>              structToIface_; // struct → interface

    // 错误收集
    void error(const std::string& msg, int line = 0);
};
