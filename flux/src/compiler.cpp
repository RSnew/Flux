// compiler.cpp — Flux AST → 字节码编译器实现
#include "compiler.h"
#include <stdexcept>
#include <string>

Compiler::Compiler(Chunk& chunk, Interpreter& interp)
    : chunk_(chunk), interp_(interp) {}

// ═══════════════════════════════════════════════════════════
// 顶层入口：两遍编译
// ═══════════════════════════════════════════════════════════
void Compiler::compile(Program* program) {
    // 假设 initProgram() 已提前执行（注册函数、初始化 persistent、执行模块声明）
    // 这里只编译常规顶层语句（跳过 FnDecl / PersistentBlock / MigrateBlock / ModuleDecl）
    for (auto& stmt : program->statements) {
        if (dynamic_cast<FnDecl*>(stmt.get()))          continue;
        if (dynamic_cast<PersistentBlock*>(stmt.get())) continue;
        if (dynamic_cast<MigrateBlock*>(stmt.get()))    continue;
        if (dynamic_cast<ModuleDecl*>(stmt.get()))      continue;
        if (dynamic_cast<SpecifyDecl*>(stmt.get()))       continue; // Specify 类型由解释器处理
        compileNode(stmt.get());
    }
    chunk_.emit(OpCode::RETURN_NIL);
}

// ═══════════════════════════════════════════════════════════
// 编译函数体（供 VM 预编译函数用）
// ═══════════════════════════════════════════════════════════
void Compiler::compileFunctionBody(const std::vector<NodePtr>& body) {
    for (auto& stmt : body)
        compileNode(stmt.get());
    chunk_.emit(OpCode::RETURN_NIL);
}

// ═══════════════════════════════════════════════════════════
// 节点分发
// ═══════════════════════════════════════════════════════════
void Compiler::compileNode(ASTNode* node) {

    // ── 字面量 ──────────────────────────────────────────
    if (auto* n = dynamic_cast<NumberLit*>(node)) {
        chunk_.emit(OpCode::PUSH_CONST, chunk_.addConst(Value::Num(n->value)));
        return;
    }
    if (auto* n = dynamic_cast<StringLit*>(node)) {
        chunk_.emit(OpCode::PUSH_CONST, chunk_.addConst(Value::Str(n->value)));
        return;
    }
    if (auto* n = dynamic_cast<BoolLit*>(node)) {
        chunk_.emit(n->value ? OpCode::PUSH_TRUE : OpCode::PUSH_FALSE);
        return;
    }
    if (dynamic_cast<NilLit*>(node)) {
        chunk_.emit(OpCode::PUSH_NIL);
        return;
    }

    // ── 变量 ────────────────────────────────────────────
    if (auto* n = dynamic_cast<Identifier*>(node)) {
        chunk_.emit(OpCode::LOAD, chunk_.addName(n->name));
        return;
    }
    if (auto* n = dynamic_cast<VarDecl*>(node)) {
        if (n->initializer) compileNode(n->initializer.get());
        else                chunk_.emit(OpCode::PUSH_NIL);
        chunk_.emit(OpCode::DEFINE, chunk_.addName(n->name));
        return;
    }
    if (auto* n = dynamic_cast<Assign*>(node)) {
        compileNode(n->value.get());
        chunk_.emit(OpCode::STORE, chunk_.addName(n->name));
        return;
    }

    // ── 持久状态 ────────────────────────────────────────
    if (auto* n = dynamic_cast<StateAccess*>(node)) {
        chunk_.emit(OpCode::LOAD_STATE, chunk_.addName(n->field));
        return;
    }
    if (auto* n = dynamic_cast<StateAssign*>(node)) {
        compileNode(n->value.get());
        chunk_.emit(OpCode::STORE_STATE, chunk_.addName(n->field));
        return;
    }

    // ── 数组字面量 ──────────────────────────────────────
    if (auto* n = dynamic_cast<ArrayLit*>(node)) {
        for (auto& e : n->elements) compileNode(e.get());
        chunk_.emit(OpCode::MAKE_ARRAY, 0, (int)n->elements.size());
        return;
    }

    // ── 下标访问 arr[i] ─────────────────────────────────
    if (auto* n = dynamic_cast<IndexExpr*>(node)) {
        compileNode(n->object.get());
        compileNode(n->index.get());
        chunk_.emit(OpCode::INDEX_GET);
        return;
    }

    // ── 下标赋值 arr[i] = v ─────────────────────────────
    if (auto* n = dynamic_cast<IndexAssign*>(node)) {
        compileNode(n->object.get());
        compileNode(n->index.get());
        compileNode(n->value.get());
        chunk_.emit(OpCode::INDEX_SET);
        return;
    }

    // ── 方法调用 obj.method(args) ────────────────────────
    if (auto* n = dynamic_cast<MethodCall*>(node)) {
        compileMethodCall(n);
        return;
    }

    // ── 二元表达式 ──────────────────────────────────────
    if (auto* n = dynamic_cast<BinaryExpr*>(node)) {
        compileBinary(n);
        return;
    }

    // ── 一元表达式 ──────────────────────────────────────
    if (auto* n = dynamic_cast<UnaryExpr*>(node)) {
        compileNode(n->operand.get());
        if (n->op == "!")  chunk_.emit(OpCode::NOT);
        else if (n->op == "-") chunk_.emit(OpCode::NEG);
        else throw std::runtime_error("Compiler: unknown unary op: " + n->op);
        return;
    }

    // ── 函数调用 fn(args) ───────────────────────────────
    if (auto* n = dynamic_cast<CallExpr*>(node)) {
        for (auto& a : n->args) compileNode(a.get());
        chunk_.emit(OpCode::CALL, chunk_.addName(n->name), (int)n->args.size());
        return;
    }

    // ── 跨模块 / 方法回退调用 Module.fn(args) ────────────
    if (auto* n = dynamic_cast<ModuleCall*>(node)) {
        compileModuleCall(n);
        return;
    }

    // ── if ──────────────────────────────────────────────
    if (auto* n = dynamic_cast<IfStmt*>(node)) {
        compileIf(n);
        return;
    }

    // ── while ───────────────────────────────────────────
    if (auto* n = dynamic_cast<WhileStmt*>(node)) {
        compileWhile(n);
        return;
    }

    // ── for-in ──────────────────────────────────────────
    if (auto* n = dynamic_cast<ForIn*>(node)) {
        compileForIn(n);
        return;
    }

    // ── return ──────────────────────────────────────────
    if (auto* n = dynamic_cast<ReturnStmt*>(node)) {
        if (n->value) { compileNode(n->value.get()); chunk_.emit(OpCode::RETURN); }
        else            chunk_.emit(OpCode::RETURN_NIL);
        return;
    }

    // ── 表达式语句（丢弃返回值）──────────────────────────
    if (auto* n = dynamic_cast<ExprStmt*>(node)) {
        compileNode(n->expr.get());
        chunk_.emit(OpCode::POP);
        return;
    }

    // ── 字段赋值 obj.field = value ──────────────────────
    if (auto* n = dynamic_cast<FieldAssign*>(node)) {
        compileNode(n->object.get());                    // 对象压栈
        compileNode(n->value.get());                     // 值压栈
        chunk_.emit(OpCode::FIELD_SET, chunk_.addName(n->field));
        return;
    }

    // ── async call(args) — 异步执行函数调用 ─────────────
    if (auto* n = dynamic_cast<AsyncExpr*>(node)) {
        // 提取被调用节点：CallExpr 或 ModuleCall
        if (auto* call = dynamic_cast<CallExpr*>(n->call.get())) {
            for (auto& a : call->args) compileNode(a.get());
            chunk_.emit(OpCode::ASYNC_CALL, chunk_.addName(call->name), (int)call->args.size());
        } else if (auto* mc = dynamic_cast<ModuleCall*>(n->call.get())) {
            for (auto& a : mc->args) compileNode(a.get());
            std::string combined = mc->module + "." + mc->fn;
            chunk_.emit(OpCode::ASYNC_MODULE, chunk_.addName(combined), (int)mc->args.size());
        } else {
            // Fallback to interpreter for complex cases
            chunk_.emit(OpCode::EVAL_AST, chunk_.addASTNode(node));
        }
        return;
    }

    // ── await expr — 等待 Future ────────────────────────
    if (dynamic_cast<AwaitExpr*>(node)) {
        auto* n = dynamic_cast<AwaitExpr*>(node);
        compileNode(n->expr.get());
        chunk_.emit(OpCode::AWAIT);
        return;
    }

    // ── spawn { body } — fire-and-forget 后台任务 ───────
    if (dynamic_cast<SpawnStmt*>(node)) {
        chunk_.emit(OpCode::SPAWN_TASK, chunk_.addASTNode(node));
        return;
    }

    // ── Module.fn.async(args) — 跨 pool 异步调用 ───────
    if (auto* n = dynamic_cast<AsyncCall*>(node)) {
        for (auto& a : n->args) compileNode(a.get());
        std::string combined = n->module + "." + n->fn;
        chunk_.emit(OpCode::ASYNC_MODULE, chunk_.addName(combined), (int)n->args.size());
        return;
    }

    // ── 匿名函数 / 闭包 ─────────────────────────────────
    if (dynamic_cast<FuncExpr*>(node)) {
        chunk_.emit(OpCode::MAKE_CLOSURE, chunk_.addASTNode(node));
        return;
    }

    // ── 结构体实例化 Type(field: val, ...) ──────────────
    if (auto* n = dynamic_cast<StructCreate*>(node)) {
        // 栈布局: field_name1, field_val1, field_name2, field_val2, ...
        for (auto& f : n->fields) {
            chunk_.emit(OpCode::PUSH_CONST, chunk_.addConst(Value::Str(f.name)));
            compileNode(f.value.get());
        }
        chunk_.emit(OpCode::STRUCT_CREATE,
                    chunk_.addName(n->typeName),
                    (int)n->fields.size());
        return;
    }

    // ── 结构体/接口/exception/default/Specify → EVAL_AST ────────
    if (dynamic_cast<StructLit*>(node)     ||
        dynamic_cast<InterfaceLit*>(node)  ||
        dynamic_cast<ExceptionDecl*>(node) ||
        dynamic_cast<IntervalRange*>(node) ||
        dynamic_cast<DefaultStmt*>(node) ||
        dynamic_cast<DefaultDecl*>(node) ||
        dynamic_cast<SpecifyDecl*>(node)) {
        chunk_.emit(OpCode::EVAL_AST, chunk_.addASTNode(node));
        return;
    }

    // ── 顶层跳过节点 ────────────────────────────────────
    if (dynamic_cast<FnDecl*>(node))          return;
    if (dynamic_cast<PersistentBlock*>(node)) return;
    if (dynamic_cast<MigrateBlock*>(node))    return;
    if (dynamic_cast<ThreadPoolDecl*>(node))  return;
    if (dynamic_cast<ModuleDecl*>(node))      return;

    throw std::runtime_error("Compiler: unhandled AST node type");
}

// ── 别名，供 compileBlock 用 ─────────────────────────────
void Compiler::compileExpr(ASTNode* node) { compileNode(node); }

// ── 语句块（可选作用域）─────────────────────────────────
void Compiler::compileBlock(const std::vector<NodePtr>& stmts, bool ownScope) {
    if (ownScope) chunk_.emit(OpCode::PUSH_SCOPE);
    for (auto& s : stmts) compileNode(s.get());
    if (ownScope) chunk_.emit(OpCode::POP_SCOPE);
}

// ═══════════════════════════════════════════════════════════
// 二元表达式（含短路 && / ||）
// ═══════════════════════════════════════════════════════════
void Compiler::compileBinary(BinaryExpr* n) {
    const std::string& op = n->op;

    // 短路 &&
    // JUMP_IF_FALSE 已弹出条件值，无需额外 POP
    if (op == "&&") {
        compileNode(n->left.get());
        int jf = chunk_.emitJump(OpCode::JUMP_IF_FALSE);   // 弹出并检查：若 false 跳走
        compileNode(n->right.get());                        // 左值已被弹出，直接求右值
        int jend = chunk_.emitJump(OpCode::JUMP);
        chunk_.patchJump(jf, chunk_.here());
        chunk_.emit(OpCode::PUSH_FALSE);                    // false 分支：压入 false
        chunk_.patchJump(jend, chunk_.here());
        return;
    }
    // 短路 ||
    // JUMP_IF_TRUE 已弹出条件值，无需额外 POP
    if (op == "||") {
        compileNode(n->left.get());
        int jt = chunk_.emitJump(OpCode::JUMP_IF_TRUE);    // 弹出并检查：若 true 跳走
        compileNode(n->right.get());                        // 左值已被弹出，直接求右值
        int jend = chunk_.emitJump(OpCode::JUMP);
        chunk_.patchJump(jt, chunk_.here());
        chunk_.emit(OpCode::PUSH_TRUE);                     // true 分支：压入 true
        chunk_.patchJump(jend, chunk_.here());
        return;
    }

    // 普通双目：先算两个操作数，再发指令
    compileNode(n->left.get());
    compileNode(n->right.get());

    if      (op == "+")  chunk_.emit(OpCode::ADD);
    else if (op == "-")  chunk_.emit(OpCode::SUB);
    else if (op == "*")  chunk_.emit(OpCode::MUL);
    else if (op == "/")  chunk_.emit(OpCode::DIV);
    else if (op == "%")  chunk_.emit(OpCode::MOD);
    else if (op == "==") chunk_.emit(OpCode::EQ);
    else if (op == "!=") chunk_.emit(OpCode::NEQ);
    else if (op == "<")  chunk_.emit(OpCode::LT);
    else if (op == ">")  chunk_.emit(OpCode::GT);
    else if (op == "<=") chunk_.emit(OpCode::LEQ);
    else if (op == ">=") chunk_.emit(OpCode::GEQ);
    else throw std::runtime_error("Compiler: unknown binary op: " + op);
}

// ═══════════════════════════════════════════════════════════
// if / else
// ═══════════════════════════════════════════════════════════
void Compiler::compileIf(IfStmt* n) {
    compileNode(n->condition.get());
    int jf = chunk_.emitJump(OpCode::JUMP_IF_FALSE);

    compileBlock(n->thenBlock, true);

    if (!n->elseBlock.empty()) {
        int jend = chunk_.emitJump(OpCode::JUMP);
        chunk_.patchJump(jf, chunk_.here());
        compileBlock(n->elseBlock, true);
        chunk_.patchJump(jend, chunk_.here());
    } else {
        chunk_.patchJump(jf, chunk_.here());
    }
}

// ═══════════════════════════════════════════════════════════
// while
// ═══════════════════════════════════════════════════════════
void Compiler::compileWhile(WhileStmt* n) {
    int loopStart = chunk_.here();
    compileNode(n->condition.get());
    int jf = chunk_.emitJump(OpCode::JUMP_IF_FALSE);
    compileBlock(n->body, true);
    chunk_.emit(OpCode::JUMP, loopStart);
    chunk_.patchJump(jf, chunk_.here());
}

// ═══════════════════════════════════════════════════════════
// for-in → 展开为基于索引的 while 循环
// ═══════════════════════════════════════════════════════════
void Compiler::compileForIn(ForIn* n) {
    int id = iterNonce_++;
    std::string iterName = "__iter_" + std::to_string(id);
    std::string idxName  = "__idx_"  + std::to_string(id);
    std::string lenName  = "__len_"  + std::to_string(id);

    // 1. 求迭代对象（Array / String / Map → 统一转 Array）
    compileNode(n->iterable.get());
    chunk_.emit(OpCode::DEFINE, chunk_.addName(iterName));

    // 2. 索引 = 0
    chunk_.emit(OpCode::PUSH_CONST, chunk_.addConst(Value::Num(0)));
    chunk_.emit(OpCode::DEFINE, chunk_.addName(idxName));

    // 3. 预先缓存数组长度，避免每次迭代调用 .len()
    chunk_.emit(OpCode::LOAD, chunk_.addName(iterName));
    chunk_.emit(OpCode::CALL_METHOD, chunk_.addName("len"), 0);
    chunk_.emit(OpCode::DEFINE, chunk_.addName(lenName));

    // 4. 循环头：检查 idx < cachedLen
    int loopStart = chunk_.here();
    chunk_.emit(OpCode::LOAD, chunk_.addName(idxName));
    chunk_.emit(OpCode::LOAD, chunk_.addName(lenName));
    chunk_.emit(OpCode::LT);
    int jf = chunk_.emitJump(OpCode::JUMP_IF_FALSE);

    // 5. 循环体（新作用域）
    chunk_.emit(OpCode::PUSH_SCOPE);

    //    取 iter[idx] → 定义循环变量
    chunk_.emit(OpCode::LOAD, chunk_.addName(iterName));
    chunk_.emit(OpCode::LOAD, chunk_.addName(idxName));
    chunk_.emit(OpCode::INDEX_GET);
    chunk_.emit(OpCode::DEFINE, chunk_.addName(n->var));

    for (auto& s : n->body) compileNode(s.get());

    chunk_.emit(OpCode::POP_SCOPE);

    // 6. idx = idx + 1
    chunk_.emit(OpCode::LOAD,  chunk_.addName(idxName));
    chunk_.emit(OpCode::PUSH_CONST, chunk_.addConst(Value::Num(1)));
    chunk_.emit(OpCode::ADD);
    chunk_.emit(OpCode::STORE, chunk_.addName(idxName));

    // 7. 回到循环头
    chunk_.emit(OpCode::JUMP, loopStart);
    chunk_.patchJump(jf, chunk_.here());
}

// ═══════════════════════════════════════════════════════════
// ModuleCall: Module.fn(args) 或变量方法回退
// ═══════════════════════════════════════════════════════════
void Compiler::compileModuleCall(ModuleCall* n) {
    for (auto& a : n->args) compileNode(a.get());
    // "Module.fn" 作为组合名字存入 names 池
    std::string combined = n->module + "." + n->fn;
    chunk_.emit(OpCode::CALL_MODULE,
                chunk_.addName(combined),
                (int)n->args.size());
}

// ═══════════════════════════════════════════════════════════
// MethodCall: obj.method(args)
// ═══════════════════════════════════════════════════════════
void Compiler::compileMethodCall(MethodCall* n) {
    compileNode(n->object.get());                       // 对象压栈
    for (auto& a : n->args) compileNode(a.get());      // 参数压栈
    chunk_.emit(OpCode::CALL_METHOD,
                chunk_.addName(n->method),
                (int)n->args.size());
}
