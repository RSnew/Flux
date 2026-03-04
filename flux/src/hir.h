// hir.h — Flux High-level Intermediate Representation
// AST → HIR: 语法脱糖 + 名称解析 + 类型标注
#pragma once
#include "ast.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════
// HIR 节点 — 对 AST 进行脱糖后的中间表示
// ═══════════════════════════════════════════════════════════

// HIR 类型标注
struct HIRType {
    enum Kind { Unknown, Int, UInt, Float, Natural, Byte, Addr,
                String, Bool, Nil, Array, Map, Fn, Struct, Interface, Any };
    Kind kind = Kind::Unknown;
    std::string name;               // 具名类型（struct/interface 名）
    std::vector<HIRType> params;    // 泛型参数 / 函数参数类型
    HIRType* returnType = nullptr;  // 函数返回类型

    static HIRType make(Kind k) { HIRType t; t.kind = k; return t; }
    static HIRType named(Kind k, const std::string& n) {
        HIRType t; t.kind = k; t.name = n; return t;
    }
    bool isNumeric() const {
        return kind == Int || kind == UInt || kind == Float ||
               kind == Natural || kind == Byte || kind == Addr;
    }
};

// HIR 节点基类
struct HIRNode {
    int line = 0;
    HIRType type;  // 推断后的类型
    virtual ~HIRNode() = default;
};
using HIRNodePtr = std::shared_ptr<HIRNode>;

// ── HIR 表达式 ──────────────────────────────────────────

struct HIRLiteral : HIRNode {
    enum LitKind { Nil, Bool, Int, Float, String };
    LitKind litKind;
    double numVal = 0;
    bool boolVal = false;
    std::string strVal;
};

struct HIRVarRef : HIRNode {
    std::string name;
    int scopeDepth = 0;   // 名称解析后的作用域深度
    int slotIndex = -1;   // 局部变量槽位（-1 = 全局）
};

struct HIRBinary : HIRNode {
    std::string op;
    HIRNodePtr left, right;
};

struct HIRUnary : HIRNode {
    std::string op;
    HIRNodePtr operand;
};

struct HIRCall : HIRNode {
    HIRNodePtr callee;
    std::vector<HIRNodePtr> args;
    bool isAsync = false;       // 脱糖后标记
    std::string targetPool;     // 跨 pool 异步调用的目标池
};

struct HIRIndex : HIRNode {
    HIRNodePtr object;
    HIRNodePtr index;
};

struct HIRFieldAccess : HIRNode {
    HIRNodePtr object;
    std::string field;
};

struct HIRArrayLit : HIRNode {
    std::vector<HIRNodePtr> elements;
};

struct HIRMapLit : HIRNode {
    std::vector<std::pair<std::string, HIRNodePtr>> entries;
};

struct HIRStructCreate : HIRNode {
    std::string structName;
    std::vector<std::pair<std::string, HIRNodePtr>> fields;
};

struct HIRLambda : HIRNode {
    struct Param { std::string name; HIRType type; };
    std::vector<Param> params;
    HIRType returnType;
    std::vector<HIRNodePtr> body;
    std::vector<std::string> captures;  // 闭包捕获列表
};

// ── HIR 语句 ────────────────────────────────────────────

struct HIRVarDecl : HIRNode {
    std::string name;
    HIRType declaredType;
    HIRNodePtr init;
    bool isMutable = true;
    bool isHotSwap = false;  // !var
};

struct HIRAssign : HIRNode {
    std::string name;
    HIRNodePtr value;
    int scopeDepth = 0;
    int slotIndex = -1;
};

struct HIRReturn : HIRNode {
    HIRNodePtr value;
};

struct HIRIf : HIRNode {
    HIRNodePtr condition;
    std::vector<HIRNodePtr> thenBranch;
    std::vector<HIRNodePtr> elseBranch;
};

struct HIRWhile : HIRNode {
    HIRNodePtr condition;
    std::vector<HIRNodePtr> body;
};

struct HIRForIn : HIRNode {
    std::string varName;
    HIRNodePtr iterable;
    std::vector<HIRNodePtr> body;
};

struct HIRBlock : HIRNode {
    std::vector<HIRNodePtr> stmts;
};

struct HIRFnDecl : HIRNode {
    std::string name;
    std::vector<HIRLambda::Param> params;
    HIRType returnType;
    std::vector<HIRNodePtr> body;
    bool isHotSwap = false;  // !func
};

struct HIRModuleDecl : HIRNode {
    std::string name;
    std::vector<HIRNodePtr> body;
    std::string restartPolicy;
    std::string boundPool;
};

struct HIRSpawn : HIRNode {
    std::vector<HIRNodePtr> body;
};

struct HIRAwait : HIRNode {
    HIRNodePtr expr;
    int timeoutMs = -1;
};

// ═══════════════════════════════════════════════════════════
// HIR Program — 顶层容器
// ═══════════════════════════════════════════════════════════
struct HIRProgram {
    std::vector<HIRNodePtr> decls;
    // 符号表（名称解析后生成）
    struct Symbol {
        std::string name;
        HIRType type;
        int scopeDepth;
        int slot;
    };
    std::unordered_map<std::string, Symbol> symbols;
};

// ═══════════════════════════════════════════════════════════
// HIR Lowering — AST → HIR 转换
// ═══════════════════════════════════════════════════════════
class HIRLowering {
public:
    HIRProgram lower(Program* program) {
        HIRProgram hir;
        for (auto& stmt : program->statements)
            hir.decls.push_back(lowerNode(stmt.get()));
        resolveNames(hir);
        return hir;
    }

private:
    int scopeDepth_ = 0;
    int nextSlot_ = 0;
    std::vector<std::unordered_map<std::string, int>> scopes_ = {{}};

    HIRNodePtr lowerNode(ASTNode* node) {
        if (!node) return nullptr;

        // ── Literals ────────────────────────────────────
        if (auto* n = dynamic_cast<NumberLit*>(node)) {
            auto h = std::make_shared<HIRLiteral>();
            h->litKind = HIRLiteral::Float;
            h->numVal = n->value;
            h->type = HIRType::make(HIRType::Float);
            return h;
        }
        if (auto* n = dynamic_cast<StringLit*>(node)) {
            auto h = std::make_shared<HIRLiteral>();
            h->litKind = HIRLiteral::String;
            h->strVal = n->value;
            h->type = HIRType::make(HIRType::String);
            return h;
        }
        if (auto* n = dynamic_cast<BoolLit*>(node)) {
            auto h = std::make_shared<HIRLiteral>();
            h->litKind = HIRLiteral::Bool;
            h->boolVal = n->value;
            h->type = HIRType::make(HIRType::Bool);
            return h;
        }
        if (dynamic_cast<NilLit*>(node)) {
            auto h = std::make_shared<HIRLiteral>();
            h->litKind = HIRLiteral::Nil;
            h->type = HIRType::make(HIRType::Nil);
            return h;
        }

        // ── Variables ───────────────────────────────────
        if (auto* n = dynamic_cast<Identifier*>(node)) {
            auto h = std::make_shared<HIRVarRef>();
            h->name = n->name;
            return h;
        }

        // ── Binary ─────────────────────────────────────
        if (auto* n = dynamic_cast<BinaryExpr*>(node)) {
            auto h = std::make_shared<HIRBinary>();
            h->op = n->op;
            h->left = lowerNode(n->left.get());
            h->right = lowerNode(n->right.get());
            return h;
        }

        // ── Unary ──────────────────────────────────────
        if (auto* n = dynamic_cast<UnaryExpr*>(node)) {
            auto h = std::make_shared<HIRUnary>();
            h->op = n->op;
            h->operand = lowerNode(n->operand.get());
            return h;
        }

        // ── VarDecl ─────────────────────────────────────
        if (auto* n = dynamic_cast<VarDecl*>(node)) {
            auto h = std::make_shared<HIRVarDecl>();
            h->name = n->name;
            h->init = n->initializer ? lowerNode(n->initializer.get()) : nullptr;
            h->isHotSwap = n->forceOverride;
            declareVar(h->name);
            return h;
        }

        // ── Assignment ─────────────────────────────────
        if (auto* n = dynamic_cast<Assign*>(node)) {
            auto h = std::make_shared<HIRAssign>();
            h->name = n->name;
            h->value = lowerNode(n->value.get());
            return h;
        }

        // ── Return ─────────────────────────────────────
        if (auto* n = dynamic_cast<ReturnStmt*>(node)) {
            auto h = std::make_shared<HIRReturn>();
            h->value = n->value ? lowerNode(n->value.get()) : nullptr;
            return h;
        }

        // ── FnDecl ─────────────────────────────────────
        if (auto* n = dynamic_cast<FnDecl*>(node)) {
            auto h = std::make_shared<HIRFnDecl>();
            h->name = n->name;
            h->isHotSwap = n->forceOverride;
            for (auto& p : n->params)
                h->params.push_back({p.name, HIRType::make(HIRType::Any)});
            pushScope();
            for (auto& p : n->params) declareVar(p.name);
            for (auto& s : n->body)
                h->body.push_back(lowerNode(s.get()));
            popScope();
            declareVar(h->name);
            return h;
        }

        // ── If ─────────────────────────────────────────
        if (auto* n = dynamic_cast<IfStmt*>(node)) {
            auto h = std::make_shared<HIRIf>();
            h->condition = lowerNode(n->condition.get());
            pushScope();
            for (auto& s : n->thenBlock) h->thenBranch.push_back(lowerNode(s.get()));
            popScope();
            pushScope();
            for (auto& s : n->elseBlock) h->elseBranch.push_back(lowerNode(s.get()));
            popScope();
            return h;
        }

        // ── While ──────────────────────────────────────
        if (auto* n = dynamic_cast<WhileStmt*>(node)) {
            auto h = std::make_shared<HIRWhile>();
            h->condition = lowerNode(n->condition.get());
            pushScope();
            for (auto& s : n->body) h->body.push_back(lowerNode(s.get()));
            popScope();
            return h;
        }

        // ── ForIn ──────────────────────────────────────
        if (auto* n = dynamic_cast<ForIn*>(node)) {
            auto h = std::make_shared<HIRForIn>();
            h->varName = n->var;
            h->iterable = lowerNode(n->iterable.get());
            pushScope();
            declareVar(h->varName);
            for (auto& s : n->body) h->body.push_back(lowerNode(s.get()));
            popScope();
            return h;
        }

        // ── CallExpr ────────────────────────────────────
        if (auto* n = dynamic_cast<CallExpr*>(node)) {
            auto h = std::make_shared<HIRCall>();
            auto callee = std::make_shared<HIRVarRef>();
            callee->name = n->name;
            h->callee = callee;
            for (auto& a : n->args) h->args.push_back(lowerNode(a.get()));
            return h;
        }

        // Default: wrap as opaque block (for nodes not yet lowered)
        return std::make_shared<HIRBlock>();
    }

    void pushScope() {
        scopeDepth_++;
        scopes_.push_back({});
    }

    void popScope() {
        scopeDepth_--;
        scopes_.pop_back();
    }

    void declareVar(const std::string& name) {
        scopes_.back()[name] = nextSlot_++;
    }

    void resolveNames(HIRProgram& /*hir*/) {
        // Post-pass: resolve HIRVarRef scopeDepth/slotIndex
        // In production, walk the HIR tree and resolve each VarRef
    }
};
