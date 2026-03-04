#include "typechecker.h"
#include <iostream>
#include <sstream>

// ─────────────────────────────────────────────────────────
// 构造 + 内置函数签名注册
// ─────────────────────────────────────────────────────────
TypeChecker::TypeChecker() { registerBuiltins(); }

void TypeChecker::registerBuiltins() {
    // print(Any...) → Nil
    builtins_["print"] = {{}, FluxType::Nil(), /*variadic=*/true};
    // str(Any) → String
    builtins_["str"]   = {{FluxType::Any()}, FluxType::String()};
    // num(Any) → Float
    builtins_["num"]   = {{FluxType::Any()}, FluxType::Float()};
    // sqrt(Int|Float) → Float
    builtins_["sqrt"]  = {{FluxType::Float()}, FluxType::Float()};
    // type(Any) → String
    builtins_["type"]  = {{FluxType::Any()}, FluxType::String()};
    // panic(String) → Nil  (never returns, but we type it as Nil)
    builtins_["panic"] = {{FluxType::Any()}, FluxType::Nil()};
    // assert(Bool, String?) → Nil
    builtins_["assert"]= {{FluxType::Bool(), FluxType::Any()}, FluxType::Nil(), true};
}

void TypeChecker::error(const std::string& msg, int line) {
    errors_.push_back({msg, line});
}

// ─────────────────────────────────────────────────────────
// 主入口
// ─────────────────────────────────────────────────────────
std::vector<TypeError> TypeChecker::check(Program* program) {
    errors_.clear();

    auto globalEnv = std::make_shared<TypeEnv>();

    // 第一遍：注册所有顶层函数签名（允许前向引用）
    registerFunctions(program, globalEnv);

    // 第二遍：检查所有语句
    for (auto& stmt : program->statements) {
        if (auto* mod = dynamic_cast<ModuleDecl*>(stmt.get())) {
            checkModule(mod, globalEnv);
        } else {
            checkStmt(stmt.get(), globalEnv);
        }
    }

    // 第三遍：接口一致性检查
    checkInterfaceConformance(program, globalEnv);

    // 第四遍：exception 引用检查
    checkExceptionRefs(program, globalEnv);

    return errors_;
}

// ─────────────────────────────────────────────────────────
// 函数签名注册
// ─────────────────────────────────────────────────────────
void TypeChecker::registerFunctions(Program* program, std::shared_ptr<TypeEnv> env) {
    for (auto& stmt : program->statements) {
        if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
            FnSignature sig;
            for (auto& p : fn->params)
                sig.paramTypes.push_back(parseTypeName(p.type));
            sig.returnType = parseTypeName(fn->returnType);
            globalFns_[fn->name] = sig;

            // 在类型环境中注册函数名
            FluxType fnType;
            fnType.kind       = TypeKind::Fn;
            fnType.returnType = std::make_shared<FluxType>(sig.returnType);
            env->define(fn->name, fnType);
        }
    }
}

void TypeChecker::registerModuleFunctions(ModuleDecl* mod, std::shared_ptr<TypeEnv> env) {
    for (auto& item : mod->body) {
        if (auto* fn = dynamic_cast<FnDecl*>(item.get())) {
            FnSignature sig;
            for (auto& p : fn->params)
                sig.paramTypes.push_back(parseTypeName(p.type));
            sig.returnType = parseTypeName(fn->returnType);
            // 用 "ModuleName.fnName" 作为键
            std::string key = mod->name + "." + fn->name;
            globalFns_[key] = sig;
            env->define(fn->name, FluxType::Any()); // 模块内函数互相可见
        }
    }
}

// ─────────────────────────────────────────────────────────
// 模块检查
// ─────────────────────────────────────────────────────────
void TypeChecker::checkModule(ModuleDecl* mod, std::shared_ptr<TypeEnv> outerEnv) {
    auto modEnv = std::make_shared<TypeEnv>(outerEnv);

    // 注册模块内函数 + persistent 字段
    registerModuleFunctions(mod, modEnv);

    for (auto& item : mod->body) {
        if (auto* pb = dynamic_cast<PersistentBlock*>(item.get())) {
            for (auto& field : pb->fields) {
                FluxType ft = field.defaultValue
                    ? inferExpr(field.defaultValue.get(), modEnv)
                    : FluxType::Nil();
                modEnv->define("state." + field.name, ft);
            }
        }
    }

    // 检查每个函数体
    for (auto& item : mod->body) {
        if (auto* fn = dynamic_cast<FnDecl*>(item.get())) {
            auto fnEnv = std::make_shared<TypeEnv>(modEnv);
            for (auto& p : fn->params)
                fnEnv->define(p.name, parseTypeName(p.type));

            FluxType prevReturn = currentReturnType_;
            currentReturnType_  = parseTypeName(fn->returnType);
            checkBlock(fn->body, fnEnv);
            currentReturnType_ = prevReturn;
        }
        // 模块顶层语句
        else if (!dynamic_cast<PersistentBlock*>(item.get()) &&
                 !dynamic_cast<MigrateBlock*>(item.get())    &&
                 !dynamic_cast<FnDecl*>(item.get())) {
            checkStmt(item.get(), modEnv);
        }
    }
}

// ─────────────────────────────────────────────────────────
// 语句检查
// ─────────────────────────────────────────────────────────
void TypeChecker::checkBlock(const std::vector<NodePtr>& stmts,
                              std::shared_ptr<TypeEnv> env) {
    for (auto& s : stmts) checkStmt(s.get(), env);
}

void TypeChecker::checkStmt(ASTNode* node, std::shared_ptr<TypeEnv> env) {
    // ── 变量声明 ──────────────────────────────────────────
    if (auto* n = dynamic_cast<VarDecl*>(node)) {
        FluxType initType = n->initializer
            ? inferExpr(n->initializer.get(), env)
            : FluxType::Nil();

        // 如果有类型注解，严格检查
        if (!n->typeAnnotation.empty()) {
            FluxType declared = parseTypeName(n->typeAnnotation);
            if (!initType.compatibleWith(declared)) {
                std::ostringstream oss;
                oss << "type error: '" << n->name << "' declared as "
                    << declared.name() << " but initialized with " << initType.name();
                error(oss.str());
            }
            env->define(n->name, declared); // 用注解类型作为权威类型
        } else {
            env->define(n->name, initType); // 推断类型
        }
        return;
    }

    // ── 赋值 ─────────────────────────────────────────────
    if (auto* n = dynamic_cast<Assign*>(node)) {
        FluxType declared = env->lookup(n->name);
        FluxType assigned = inferExpr(n->value.get(), env);
        if (!declared.compatibleWith(assigned)) {
            std::ostringstream oss;
            oss << "type error: cannot assign " << assigned.name()
                << " to variable '" << n->name << "' of type " << declared.name();
            error(oss.str());
        }
        return;
    }

    // ── 持久状态赋值 state.x = ... ────────────────────────
    if (auto* n = dynamic_cast<StateAssign*>(node)) {
        FluxType declared = env->lookup("state." + n->field);
        FluxType assigned = inferExpr(n->value.get(), env);
        if (!declared.compatibleWith(assigned)) {
            std::ostringstream oss;
            oss << "type error: cannot assign " << assigned.name()
                << " to state." << n->field << " of type " << declared.name();
            error(oss.str());
        }
        return;
    }

    // ── return ────────────────────────────────────────────
    if (auto* n = dynamic_cast<ReturnStmt*>(node)) {
        FluxType retType = n->value
            ? inferExpr(n->value.get(), env)
            : FluxType::Nil();

        if (currentReturnType_.kind != TypeKind::Any &&
            !retType.compatibleWith(currentReturnType_)) {
            std::ostringstream oss;
            oss << "type error: return type mismatch — expected "
                << currentReturnType_.name() << ", got " << retType.name();
            error(oss.str());
        }
        return;
    }

    // ── if ────────────────────────────────────────────────
    if (auto* n = dynamic_cast<IfStmt*>(node)) {
        FluxType condType = inferExpr(n->condition.get(), env);
        if (condType.kind != TypeKind::Bool && condType.kind != TypeKind::Any) {
            error("type error: if condition must be Bool, got " + condType.name());
        }
        auto thenEnv = std::make_shared<TypeEnv>(env);
        auto elseEnv = std::make_shared<TypeEnv>(env);
        checkBlock(n->thenBlock, thenEnv);
        checkBlock(n->elseBlock, elseEnv);
        return;
    }

    // ── while ─────────────────────────────────────────────
    if (auto* n = dynamic_cast<WhileStmt*>(node)) {
        FluxType condType = inferExpr(n->condition.get(), env);
        if (condType.kind != TypeKind::Bool && condType.kind != TypeKind::Any) {
            error("type error: while condition must be Bool, got " + condType.name());
        }
        auto loopEnv = std::make_shared<TypeEnv>(env);
        checkBlock(n->body, loopEnv);
        return;
    }

    // ── 函数声明 ──────────────────────────────────────────
    if (auto* n = dynamic_cast<FnDecl*>(node)) {
        auto fnEnv = std::make_shared<TypeEnv>(env);
        for (auto& p : n->params)
            fnEnv->define(p.name, parseTypeName(p.type));

        FluxType prevReturn = currentReturnType_;
        currentReturnType_  = parseTypeName(n->returnType);
        checkBlock(n->body, fnEnv);
        currentReturnType_ = prevReturn;
        return;
    }

    // ── 表达式语句 ────────────────────────────────────────
    if (auto* n = dynamic_cast<ExprStmt*>(node)) {
        inferExpr(n->expr.get(), env);
        return;
    }

    // ── @profile fn ──────────────────────────────────────
    if (auto* n = dynamic_cast<ProfiledFnDecl*>(node)) {
        if (n->fnDecl) checkStmt(n->fnDecl.get(), env);
        return;
    }

    // ── enum 声明 ────────────────────────────────────────
    if (auto* n = dynamic_cast<EnumDecl*>(node)) {
        env->define(n->name, FluxType::MapT());
        // Natural 验证：枚举值必须 >= 0
        for (auto& v : n->variants) {
            if (v.value) {
                FluxType vt = inferExpr(v.value.get(), env);
                if (vt.kind == TypeKind::Int || vt.kind == TypeKind::Float ||
                    vt.kind == TypeKind::Any) {
                    // 如果是 NumberLit，检查 >= 0
                    if (auto* num = dynamic_cast<NumberLit*>(v.value.get())) {
                        if (num->value < 0) {
                            error("enum '" + n->name + "': variant '" + v.name
                                  + "' has negative value " + std::to_string((int)num->value)
                                  + " (Natural values must be >= 0)");
                        }
                    }
                }
            }
        }
        return;
    }

    // ── append 扩展 ─────────────────────────────────────
    if (dynamic_cast<AppendDecl*>(node)) return;
    // ── @platform ────────────────────────────────────────
    if (dynamic_cast<PlatformDecl*>(node)) return;
    // ── alloc/free ──────────────────────────────────────
    if (dynamic_cast<AllocExpr*>(node)) return;
    if (dynamic_cast<FreeStmt*>(node)) return;
    // ── asm ─────────────────────────────────────────────
    if (dynamic_cast<AsmBlock*>(node)) return;
    // ── default {} 默认值返回（语句级）─────────────────────
    if (auto* n = dynamic_cast<DefaultStmt*>(node)) {
        for (auto& s : n->body) checkStmt(s.get(), env);
        return;
    }

    // ── 其他节点（PersistentBlock, MigrateBlock 等）跳过 ──
}

// ─────────────────────────────────────────────────────────
// 表达式类型推断
// ─────────────────────────────────────────────────────────
FluxType TypeChecker::inferExpr(ASTNode* node, std::shared_ptr<TypeEnv> env) {

    // ── 字面量 ────────────────────────────────────────────
    if (dynamic_cast<NumberLit*>(node)) {
        auto* n = dynamic_cast<NumberLit*>(node);
        // 整数 vs 浮点
        if (n->value == (long long)n->value) return FluxType::Int();
        return FluxType::Float();
    }
    if (dynamic_cast<StringLit*>(node))  return FluxType::String();
    if (dynamic_cast<BoolLit*>(node))    return FluxType::Bool();

    // ── 变量 ──────────────────────────────────────────────
    if (auto* n = dynamic_cast<Identifier*>(node)) {
        return env->lookup(n->name);
    }

    // ── state.field ───────────────────────────────────────
    if (auto* n = dynamic_cast<StateAccess*>(node)) {
        return env->lookup("state." + n->field);
    }
    if (auto* n = dynamic_cast<StateAssign*>(node)) {
        return inferExpr(n->value.get(), env);
    }

    // ── 一元表达式 ────────────────────────────────────────
    if (auto* n = dynamic_cast<UnaryExpr*>(node)) {
        FluxType t = inferExpr(n->operand.get(), env);
        if (n->op == "!") return FluxType::Bool();
        if (n->op == "-") {
            if (!t.isNumeric())
                error("type error: unary '-' requires numeric type, got " + t.name());
            return t;
        }
        return FluxType::Any();
    }

    // ── 二元表达式 ────────────────────────────────────────
    if (auto* n = dynamic_cast<BinaryExpr*>(node)) {
        FluxType lhs = inferExpr(n->left.get(),  env);
        FluxType rhs = inferExpr(n->right.get(), env);
        const auto& op = n->op;

        // 比较运算 → Bool
        if (op == "==" || op == "!=" || op == "<" || op == ">" ||
            op == "<=" || op == ">=")
            return FluxType::Bool();

        // 逻辑运算 → Bool
        if (op == "&&" || op == "||") return FluxType::Bool();

        // 加法：字符串拼接 or 数值加法
        if (op == "+") {
            if (lhs.kind == TypeKind::String || rhs.kind == TypeKind::String)
                return FluxType::String();
            if (lhs.isNumeric() && rhs.isNumeric()) {
                // Int + Int = Int; Float + anything = Float
                if (lhs.kind == TypeKind::Float || rhs.kind == TypeKind::Float)
                    return FluxType::Float();
                return FluxType::Int();
            }
            if (lhs.kind == TypeKind::Any || rhs.kind == TypeKind::Any)
                return FluxType::Any();
            error("type error: '+' requires numeric or String operands, got "
                  + lhs.name() + " + " + rhs.name());
            return FluxType::Any();
        }

        // 算术：- * / %
        if (op == "-" || op == "*" || op == "/" || op == "%") {
            if (!lhs.isNumeric())
                error("type error: '" + op + "' left operand must be numeric, got " + lhs.name());
            if (!rhs.isNumeric())
                error("type error: '" + op + "' right operand must be numeric, got " + rhs.name());
            if (lhs.kind == TypeKind::Float || rhs.kind == TypeKind::Float)
                return FluxType::Float();
            return FluxType::Int();
        }

        return FluxType::Any();
    }

    // ── 函数调用 ──────────────────────────────────────────
    if (auto* n = dynamic_cast<CallExpr*>(node)) {
        // 推断参数类型
        std::vector<FluxType> argTypes;
        for (auto& a : n->args)
            argTypes.push_back(inferExpr(a.get(), env));

        // 内置函数查找
        auto bit = builtins_.find(n->name);
        if (bit != builtins_.end()) {
            auto& sig = bit->second;
            if (!sig.isVariadic) {
                if (argTypes.size() != sig.paramTypes.size()) {
                    std::ostringstream oss;
                    oss << "type error: '" << n->name << "' expects "
                        << sig.paramTypes.size() << " arguments, got "
                        << argTypes.size();
                    error(oss.str());
                } else {
                    for (size_t i = 0; i < argTypes.size(); i++) {
                        if (!argTypes[i].compatibleWith(sig.paramTypes[i])) {
                            std::ostringstream oss;
                            oss << "type error: '" << n->name << "' arg " << (i+1)
                                << " expected " << sig.paramTypes[i].name()
                                << ", got " << argTypes[i].name();
                            error(oss.str());
                        }
                    }
                }
            }
            return sig.returnType;
        }

        // 用户定义函数
        auto fit = globalFns_.find(n->name);
        if (fit != globalFns_.end()) {
            auto& sig = fit->second;
            if (argTypes.size() != sig.paramTypes.size()) {
                std::ostringstream oss;
                oss << "type error: '" << n->name << "' expects "
                    << sig.paramTypes.size() << " arguments, got "
                    << argTypes.size();
                error(oss.str());
            } else {
                for (size_t i = 0; i < argTypes.size(); i++) {
                    if (!argTypes[i].compatibleWith(sig.paramTypes[i])) {
                        std::ostringstream oss;
                        oss << "type error: '" << n->name << "' arg " << (i+1)
                            << " expected " << sig.paramTypes[i].name()
                            << ", got " << argTypes[i].name();
                        error(oss.str());
                    }
                }
            }
            return sig.returnType;
        }

        // 未知函数 → Any（运行时再报错）
        return FluxType::Any();
    }

    // ── 模块调用 Module.fn(...) ───────────────────────────
    if (auto* n = dynamic_cast<ModuleCall*>(node)) {
        std::vector<FluxType> argTypes;
        for (auto& a : n->args)
            argTypes.push_back(inferExpr(a.get(), env));

        std::string key = n->module + "." + n->fn;
        auto fit = globalFns_.find(key);
        if (fit != globalFns_.end()) {
            auto& sig = fit->second;
            if (argTypes.size() != sig.paramTypes.size()) {
                std::ostringstream oss;
                oss << "type error: '" << key << "' expects "
                    << sig.paramTypes.size() << " arguments, got "
                    << argTypes.size();
                error(oss.str());
            } else {
                for (size_t i = 0; i < argTypes.size(); i++) {
                    if (!argTypes[i].compatibleWith(sig.paramTypes[i])) {
                        std::ostringstream oss;
                        oss << "type error: '" << key << "' arg " << (i+1)
                            << " expected " << sig.paramTypes[i].name()
                            << ", got " << argTypes[i].name();
                        error(oss.str());
                    }
                }
            }
            return sig.returnType;
        }
        return FluxType::Any();
    }

    // ── alloc() → Addr ─────────────────────────────────
    if (dynamic_cast<AllocExpr*>(node)) return FluxType::Addr();


    return FluxType::Any();
}

// ─────────────────────────────────────────────────────────
// 接口一致性检查
// 扫描所有 VarDecl：若初始值是 InterfaceLit，注册接口；
// 若初始值是 StructLit 且 interfaceName 非空，检查方法完整性
// ─────────────────────────────────────────────────────────
void TypeChecker::checkInterfaceConformance(Program* program, std::shared_ptr<TypeEnv> /*env*/) {
    // Pass 1: collect interfaces
    for (auto& stmt : program->statements) {
        auto* vd = dynamic_cast<VarDecl*>(stmt.get());
        if (!vd || !vd->initializer) continue;

        if (auto* iface = dynamic_cast<InterfaceLit*>(vd->initializer.get())) {
            std::vector<std::string> methods;
            for (auto& m : iface->methods)
                methods.push_back(m.name);
            interfaces_[vd->name] = std::move(methods);
        }
    }

    // Pass 2: check struct conformance
    for (auto& stmt : program->statements) {
        auto* vd = dynamic_cast<VarDecl*>(stmt.get());
        if (!vd || !vd->initializer) continue;

        if (auto* sl = dynamic_cast<StructLit*>(vd->initializer.get())) {
            if (sl->interfaceName.empty()) continue;

            auto it = interfaces_.find(sl->interfaceName);
            if (it == interfaces_.end()) {
                error("type error: struct '" + vd->name + "' implements unknown interface '"
                      + sl->interfaceName + "'");
                continue;
            }

            // Collect struct methods
            std::vector<std::string> structMethods;
            for (auto& m : sl->methods)
                structMethods.push_back(m.name);
            structTypes_[vd->name] = structMethods;
            structToIface_[vd->name] = sl->interfaceName;

            // Check each interface method is present in the struct
            for (auto& required : it->second) {
                bool found = false;
                for (auto& m : sl->methods) {
                    if (m.name == required) { found = true; break; }
                }
                if (!found) {
                    error("type error: struct '" + vd->name + "' missing method '"
                          + required + "' required by interface '" + sl->interfaceName + "'");
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────
// exception 引用检查
// 确保 exception funcName { ... } 中的 funcName 实际存在
// ─────────────────────────────────────────────────────────
void TypeChecker::checkExceptionRefs(Program* program, std::shared_ptr<TypeEnv> /*env*/) {
    // Collect all exception declarations from top-level and inside modules
    auto checkExcInStmts = [&](const std::vector<NodePtr>& stmts) {
        for (auto& stmt : stmts) {
            auto* ed = dynamic_cast<ExceptionDecl*>(stmt.get());
            if (!ed || ed->target.empty()) continue;

            auto colonPos = ed->target.find(':');
            if (colonPos != std::string::npos) {
                // exception Type:method — check struct type and method
                std::string typeName   = ed->target.substr(0, colonPos);
                std::string methodName = ed->target.substr(colonPos + 1);
                auto sit = structTypes_.find(typeName);
                if (sit == structTypes_.end()) {
                    error("type error: exception target '" + ed->target
                          + "' references unknown struct type '" + typeName + "'");
                } else {
                    bool found = false;
                    for (auto& m : sit->second)
                        if (m == methodName) { found = true; break; }
                    if (!found)
                        error("type error: exception target '" + ed->target
                              + "' references unknown method '" + methodName
                              + "' on type '" + typeName + "'");
                }
            } else {
                // exception funcName — check function exists
                if (globalFns_.find(ed->target) == globalFns_.end() &&
                    builtins_.find(ed->target) == builtins_.end()) {
                    error("type error: exception target '" + ed->target
                          + "' references unknown function");
                }
            }
        }
    };

    checkExcInStmts(program->statements);
    for (auto& stmt : program->statements) {
        if (auto* mod = dynamic_cast<ModuleDecl*>(stmt.get()))
            checkExcInStmts(mod->body);
    }
}
