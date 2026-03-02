// compiler.h — Flux AST → 字节码编译器
// Feature G: 将 AST 树节点编译成 Chunk 字节码
#pragma once
#include "ast.h"
#include "interpreter.h"
#include "vm.h"

class Compiler {
public:
    // chunk   : 顶层字节码输出目标
    // interp  : 共享解释器（注册全局函数、访问 modules_）
    Compiler(Chunk& chunk, Interpreter& interp);

    // 编译整个 Program（两遍：先注册函数，再编译语句）
    void compile(Program* program);

private:
    Chunk&       chunk_;
    Interpreter& interp_;
    int          iterNonce_ = 0;   // for-in 临时变量唯一 ID

    // ── 表达式 / 语句 ────────────────────────────────────
    void compileNode(ASTNode* node);
    void compileExpr(ASTNode* node);
    void compileBlock(const std::vector<NodePtr>& stmts, bool ownScope);

    // ── 各 AST 节点 ──────────────────────────────────────
    void compileBinary(BinaryExpr* n);
    void compileIf(IfStmt* n);
    void compileWhile(WhileStmt* n);
    void compileForIn(ForIn* n);
    void compileFn(FnDecl* fn);        // 编译后存入 interp_.functions_ (VM 版)
    void compileModuleCall(ModuleCall* n);
    void compileMethodCall(MethodCall* n);
};
