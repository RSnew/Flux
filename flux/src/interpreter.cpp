#include "interpreter.h"
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>

// ── 构造 ──────────────────────────────────────────────────
Interpreter::Interpreter() {
    globalEnv_ = std::make_shared<Environment>();
    registerBuiltins();
}

void Interpreter::registerBuiltins() {
    registerBuiltin("print", [](std::vector<Value> args) -> Value {
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) std::cout << " ";
            std::cout << args[i].toString();
        }
        std::cout << "\n";
        return Value::Nil();
    });
    registerBuiltin("str", [](std::vector<Value> args) -> Value {
        return args.empty() ? Value::Str("") : Value::Str(args[0].toString());
    });
    registerBuiltin("num", [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value::Num(0);
        if (args[0].type == Value::Type::Number) return args[0];
        try { return Value::Num(std::stod(args[0].string)); } catch (...) { return Value::Num(0); }
    });
    registerBuiltin("sqrt", [](std::vector<Value> args) -> Value {
        if (args.empty() || args[0].type != Value::Type::Number) return Value::Num(0);
        return Value::Num(std::sqrt(args[0].number));
    });
    registerBuiltin("panic", [](std::vector<Value> args) -> Value {
        std::string msg = args.empty() ? "explicit panic" : args[0].toString();
        throw PanicSignal{msg};
        return Value::Nil();
    });
    registerBuiltin("assert", [](std::vector<Value> args) -> Value {
        if (args.empty() || !args[0].isTruthy()) {
            std::string msg = args.size() > 1 ? args[1].toString() : "assertion failed";
            throw PanicSignal{msg};
        }
        return Value::Nil();
    });
    // range(n) → [0, 1, ..., n-1]
    registerBuiltin("range", [](std::vector<Value> args) -> Value {
        if (args.empty() || args[0].type != Value::Type::Number)
            return Value::Array();
        auto arr = std::make_shared<std::vector<Value>>();
        int n = (int)args[0].number;
        for (int i = 0; i < n; i++) arr->push_back(Value::Num(i));
        return Value::Arr(arr);
    });
    // len(x) → number (string length or array length)
    registerBuiltin("len", [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value::Num(0);
        if (args[0].type == Value::Type::String)
            return Value::Num((double)args[0].string.size());
        if (args[0].type == Value::Type::Array && args[0].array)
            return Value::Num((double)args[0].array->size());
        return Value::Num(0);
    });
    registerBuiltin("type", [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value::Str("nil");
        switch (args[0].type) {
            case Value::Type::Number: return Value::Str("Number");
            case Value::Type::String: return Value::Str("String");
            case Value::Type::Bool:   return Value::Str("Bool");
            default:                  return Value::Str("Nil");
        }
    });
}

void Interpreter::registerBuiltin(const std::string& name, BuiltinFn fn) {
    builtins_[name] = std::move(fn);
}

// ── 主执行入口 ────────────────────────────────────────────
void Interpreter::execute(Program* program) {
    // 热更新：重建全局环境（普通变量重置），持久化存储保留
    globalEnv_ = std::make_shared<Environment>();

    // 第一遍：注册全局函数 + 初始化全局 persistent
    functions_.clear();
    for (auto& stmt : program->statements) {
        if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
            functions_[fn->name] = fn;
        } else if (auto* pb = dynamic_cast<PersistentBlock*>(stmt.get())) {
            for (auto& field : pb->fields) {
                if (!persistentStore_.count(field.name)) {
                    persistentStore_[field.name] =
                        field.defaultValue
                            ? evalNode(field.defaultValue.get(), globalEnv_)
                            : Value::Nil();
                }
            }
        }
    }

    // 第二遍：执行顶层语句（模块声明 + 普通语句）
    for (auto& stmt : program->statements) {
        if (dynamic_cast<FnDecl*>(stmt.get()))          continue;
        if (dynamic_cast<PersistentBlock*>(stmt.get())) continue;
        try {
            evalNode(stmt.get(), globalEnv_);
        } catch (ReturnSignal&) {}
    }
}

// ── 模块执行（含迁移检测）────────────────────────────────
void Interpreter::executeModule(ModuleDecl* decl, std::shared_ptr<Environment> env) {
    auto& mod = modules_[decl->name];
    mod.name          = decl->name;
    mod.restartPolicy = decl->restartPolicy;
    mod.maxRetries    = decl->maxRetries;
    mod.decl          = decl;   // 保存 AST 指针供重启使用
    mod.functions.clear();

    // ── 收集本次声明的 persistent 字段 ──
    std::vector<PersistentField*> persistFields;
    MigrateBlock*                 migrateBlock = nullptr;

    for (auto& item : decl->body) {
        if (auto* pb = dynamic_cast<PersistentBlock*>(item.get()))
            for (auto& f : pb->fields) persistFields.push_back(&f);
        if (auto* mb = dynamic_cast<MigrateBlock*>(item.get()))
            migrateBlock = mb;
        if (auto* fn = dynamic_cast<FnDecl*>(item.get()))
            mod.functions[fn->name] = fn;
    }

    // ── 检测新增字段（只在热更新时检测，首次运行直接初始化）──
    // 判断依据：persistentStore 中已有任意字段 = 曾经运行过
    bool isHotReload = !mod.persistentStore.empty() || !mod.knownFields.empty();

    std::vector<std::string> newFields;
    if (isHotReload) {
        for (auto* f : persistFields) {
            if (!mod.knownFields.count(f->name))
                newFields.push_back(f->name);
        }
    }

    if (!newFields.empty()) {
        // ── 这是一次有结构变更的热更新 ──

        // 构建 migrate 提供的字段集合
        std::unordered_map<std::string, NodePtr*> migrateValues;
        if (migrateBlock) {
            for (auto& mf : migrateBlock->fields)
                migrateValues[mf.name] = &mf.value;
        }

        // 检查是否所有新字段都被 migrate 覆盖
        std::vector<std::string> uncovered;
        for (auto& fname : newFields)
            if (!migrateValues.count(fname))
                uncovered.push_back(fname);

        if (!uncovered.empty()) {
            // ❌ 阻断热更新，给出清晰错误信息
            std::string fieldList;
            for (size_t i = 0; i < uncovered.size(); i++) {
                if (i > 0) fieldList += ", ";
                fieldList += uncovered[i];
            }
            std::string hint = "\n\n  Add to module '" + decl->name + "':\n"
                               "  migrate {\n";
            for (auto& f : uncovered)
                hint += "      " + f + ": <default_value>,\n";
            hint += "  }";

            throw std::runtime_error(
                "\033[1;31m\n"
                "❌ Hot reload blocked! Persistent state schema changed.\n"
                "   Module      : " + decl->name + "\n"
                "   New fields  : [" + fieldList + "]\n"
                "   Problem     : No migrate { } block covers these fields."
                "\033[0m" + hint
            );
        }

        // ✅ migrate 覆盖完整，执行迁移：写入新字段的默认值
        auto migrateEnv = std::make_shared<Environment>(env);
        for (auto& fname : newFields) {
            Value val = evalNode((*migrateValues[fname]).get(), migrateEnv, &mod);
            mod.persistentStore[fname] = val;
            mod.knownFields.insert(fname);

            std::cout << "\033[32m"
                      << "   ✓ migrate: " << decl->name
                      << ".state." << fname
                      << " = " << val.toString()
                      << "\033[0m\n";
        }

    } else {
        // ── 无结构变更，正常初始化未知字段（首次运行）──
        for (auto* f : persistFields) {
            if (!mod.persistentStore.count(f->name)) {
                mod.persistentStore[f->name] =
                    f->defaultValue
                        ? evalNode(f->defaultValue.get(), env, &mod)
                        : Value::Nil();
                mod.knownFields.insert(f->name);
            }
        }
    }

    // ── 执行模块顶层语句（初始化逻辑，热更新时重跑）──
    auto modEnv = std::make_shared<Environment>(env);
    for (auto& item : decl->body) {
        if (dynamic_cast<FnDecl*>(item.get()))          continue;
        if (dynamic_cast<PersistentBlock*>(item.get())) continue;
        if (dynamic_cast<MigrateBlock*>(item.get()))    continue;
        try {
            evalNode(item.get(), modEnv, &mod);
        } catch (ReturnSignal&) {}
    }
}

// ── 模块函数调用（含崩溃隔离）────────────────────────────
Value Interpreter::callModuleFunction(const std::string& modName,
                                       const std::string& fnName,
                                       std::vector<Value> args) {
    auto mit = modules_.find(modName);
    if (mit == modules_.end())
        throw std::runtime_error("undefined module: " + modName);

    ModuleRuntime& mod = mit->second;

    // ── 检查模块是否已永久崩溃（.never 策略）──
    if (mod.crashed) {
        std::cerr << "\033[33m"
                  << "⚠️  [" << modName << "] module is crashed and will not restart"
                  << "\033[0m\n";
        return Value::Nil();
    }

    auto fit = mod.functions.find(fnName);
    if (fit == mod.functions.end())
        throw std::runtime_error("undefined function: " + modName + "." + fnName);

    FnDecl* fn = fit->second;
    auto fnEnv = std::make_shared<Environment>(globalEnv_);
    for (size_t i = 0; i < fn->params.size(); i++)
        fnEnv->set(fn->params[i].name, i < args.size() ? args[i] : Value::Nil());

    try {
        for (auto& stmt : fn->body)
            evalNode(stmt.get(), fnEnv, &mod);
        return Value::Nil();
    } catch (ReturnSignal& ret) {
        return ret.value;

    } catch (PanicSignal& p) {
        // ── 模块崩溃，交给监督器处理 ──
        mod.crashCount++;

        std::cerr << "\033[1;31m"
                  << "\n💥 Module '" << modName << "' panicked: " << p.message
                  << "\033[0m\n";
        std::cerr << "\033[90m"
                  << "   crash #" << mod.crashCount
                  << "  policy=" << (mod.restartPolicy == RestartPolicy::Always ? "always"
                                   : mod.restartPolicy == RestartPolicy::Never  ? "never" : "none")
                  << "\033[0m\n";

        if (mod.restartPolicy == RestartPolicy::Always) {
            if (mod.crashCount <= mod.maxRetries) {
                std::cerr << "\033[33m"
                          << "🔄 Restarting module '" << modName << "'..."
                          << "\033[0m\n";
                // 重新初始化模块（函数重新注册，persistent 状态保留）
                if (mod.decl) executeModule(mod.decl, globalEnv_);
                std::cerr << "\033[32m"
                          << "✅ Module '" << modName << "' restarted"
                          << "\033[0m\n";
            } else {
                mod.crashed = true;
                std::cerr << "\033[1;31m"
                          << "❌ Module '" << modName
                          << "' exceeded maxRetries (" << mod.maxRetries
                          << "), giving up\033[0m\n";
            }
        } else if (mod.restartPolicy == RestartPolicy::Never) {
            mod.crashed = true;
            std::cerr << "\033[1;31m"
                      << "❌ Module '" << modName
                      << "' crashed with .never policy, permanently stopped\033[0m\n";
        } else {
            // None — 没有监督，错误向上传播
            throw std::runtime_error("unhandled panic in " + modName + ": " + p.message);
        }

        return Value::Nil(); // 崩溃后返回 nil，调用者继续

    } catch (std::exception& e) {
        // 普通运行时错误也按同样策略处理
        mod.crashCount++;
        std::cerr << "\033[1;31m"
                  << "\n💥 Module '" << modName << "' error: " << e.what()
                  << "\033[0m\n";

        if (mod.restartPolicy == RestartPolicy::Always && mod.crashCount <= mod.maxRetries) {
            std::cerr << "\033[33m🔄 Restarting '" << modName << "'...\033[0m\n";
            if (mod.decl) executeModule(mod.decl, globalEnv_);
            std::cerr << "\033[32m✅ '" << modName << "' restarted\033[0m\n";
        } else if (mod.restartPolicy != RestartPolicy::None) {
            mod.crashed = true;
        } else {
            throw;
        }
        return Value::Nil();
    }
}

// ── 普通函数调用 ──────────────────────────────────────────
Value Interpreter::callFunction(const std::string& name,
                                 std::vector<Value> args,
                                 ModuleRuntime* mod) {
    // 内置函数
    auto bit = builtins_.find(name);
    if (bit != builtins_.end()) return bit->second(std::move(args));

    // 用户定义函数（模块内优先，再查全局）
    FnDecl* fn = nullptr;
    if (mod) {
        auto fit = mod->functions.find(name);
        if (fit != mod->functions.end()) fn = fit->second;
    }
    if (!fn) {
        auto fit = functions_.find(name);
        if (fit != functions_.end()) fn = fit->second;
    }
    if (!fn)
        throw std::runtime_error("undefined function: " + name);

    auto fnEnv = std::make_shared<Environment>(globalEnv_);
    for (size_t i = 0; i < fn->params.size(); i++)
        fnEnv->set(fn->params[i].name, i < args.size() ? args[i] : Value::Nil());

    try {
        for (auto& stmt : fn->body)
            evalNode(stmt.get(), fnEnv, mod);
    } catch (ReturnSignal& ret) {
        return ret.value;
    }
    // PanicSignal 不在这里捕获，让它传播到 callModuleFunction 的边界
    return Value::Nil();
}

// ── 节点求值 ──────────────────────────────────────────────
Value Interpreter::evalNode(ASTNode* node, std::shared_ptr<Environment> env,
                              ModuleRuntime* mod) {

    // ── 数组字面量 ────────────────────────────────────────
    if (auto* n = dynamic_cast<ArrayLit*>(node)) {
        auto arr = std::make_shared<std::vector<Value>>();
        for (auto& e : n->elements)
            arr->push_back(evalNode(e.get(), env, mod));
        return Value::Arr(arr);
    }

    // ── 下标访问 arr[i] ───────────────────────────────────
    if (auto* n = dynamic_cast<IndexExpr*>(node)) {
        Value obj = evalNode(n->object.get(), env, mod);
        Value idx = evalNode(n->index.get(),  env, mod);
        if (obj.type == Value::Type::Array) {
            if (!obj.array) throw std::runtime_error("null array");
            int i = (int)idx.number;
            if (i < 0) i = (int)obj.array->size() + i; // 负索引
            if (i < 0 || i >= (int)obj.array->size())
                throw std::runtime_error("array index out of bounds: " + std::to_string(i));
            return (*obj.array)[i];
        }
        if (obj.type == Value::Type::String) {
            int i = (int)idx.number;
            if (i < 0 || i >= (int)obj.string.size())
                throw std::runtime_error("string index out of bounds");
            return Value::Str(std::string(1, obj.string[i]));
        }
        throw std::runtime_error("cannot index into " + obj.toString());
    }

    // ── 下标赋值 arr[i] = value ───────────────────────────
    if (auto* n = dynamic_cast<IndexAssign*>(node)) {
        Value obj = evalNode(n->object.get(), env, mod);
        Value idx = evalNode(n->index.get(),  env, mod);
        Value val = evalNode(n->value.get(),  env, mod);
        if (obj.type != Value::Type::Array || !obj.array)
            throw std::runtime_error("cannot index-assign into non-array");
        int i = (int)idx.number;
        if (i < 0) i = (int)obj.array->size() + i;
        if (i < 0 || i >= (int)obj.array->size())
            throw std::runtime_error("array index out of bounds: " + std::to_string(i));
        (*obj.array)[i] = val;
        return val;
    }

    // ── 方法调用 arr.push(x) / arr.len() ─────────────────
    if (auto* n = dynamic_cast<MethodCall*>(node)) {
        Value obj = evalNode(n->object.get(), env, mod);
        std::vector<Value> margs;
        for (auto& a : n->args) margs.push_back(evalNode(a.get(), env, mod));

        if (obj.type == Value::Type::Array && obj.array) {
            if (n->method == "push" || n->method == "append") {
                for (auto& v : margs) obj.array->push_back(v);
                return Value::Num((double)obj.array->size());
            }
            if (n->method == "pop") {
                if (obj.array->empty()) throw std::runtime_error("pop from empty array");
                Value last = obj.array->back();
                obj.array->pop_back();
                return last;
            }
            if (n->method == "len" || n->method == "size") {
                return Value::Num((double)obj.array->size());
            }
            if (n->method == "first") {
                if (obj.array->empty()) return Value::Nil();
                return obj.array->front();
            }
            if (n->method == "last") {
                if (obj.array->empty()) return Value::Nil();
                return obj.array->back();
            }
            if (n->method == "contains") {
                if (margs.empty()) return Value::Bool(false);
                std::string needle = margs[0].toString();
                for (auto& v : *obj.array)
                    if (v.toString() == needle) return Value::Bool(true);
                return Value::Bool(false);
            }
            if (n->method == "join") {
                std::string sep = margs.empty() ? "" : margs[0].toString();
                std::string result;
                for (size_t i = 0; i < obj.array->size(); i++) {
                    if (i > 0) result += sep;
                    result += (*obj.array)[i].toString();
                }
                return Value::Str(result);
            }
            if (n->method == "reverse") {
                std::reverse(obj.array->begin(), obj.array->end());
                return obj;
            }
        }
        if (obj.type == Value::Type::String) {
            if (n->method == "len" || n->method == "size")
                return Value::Num((double)obj.string.size());
            if (n->method == "upper") {
                std::string s = obj.string;
                for (auto& c : s) c = toupper(c);
                return Value::Str(s);
            }
            if (n->method == "lower") {
                std::string s = obj.string;
                for (auto& c : s) c = tolower(c);
                return Value::Str(s);
            }
            if (n->method == "split") {
                std::string sep = margs.empty() ? " " : margs[0].toString();
                auto arr = std::make_shared<std::vector<Value>>();
                size_t start = 0, pos2 = 0;
                while ((pos2 = obj.string.find(sep, start)) != std::string::npos) {
                    arr->push_back(Value::Str(obj.string.substr(start, pos2 - start)));
                    start = pos2 + sep.size();
                }
                arr->push_back(Value::Str(obj.string.substr(start)));
                return Value::Arr(arr);
            }
            if (n->method == "contains") {
                if (margs.empty()) return Value::Bool(false);
                return Value::Bool(obj.string.find(margs[0].toString()) != std::string::npos);
            }
            if (n->method == "trim") {
                std::string s = obj.string;
                size_t l = s.find_first_not_of(" \t\n\r");
                size_t r = s.find_last_not_of(" \t\n\r");
                return Value::Str(l == std::string::npos ? "" : s.substr(l, r - l + 1));
            }
        }
        throw std::runtime_error("unknown method '" + n->method + "' on " + obj.toString());
    }

    // ── for item in iterable { } ──────────────────────────
    if (auto* n = dynamic_cast<ForIn*>(node)) {
        Value iterable = evalNode(n->iterable.get(), env, mod);
        std::vector<Value>* items = nullptr;
        std::vector<Value> strChars; // 字符串迭代时的临时存储

        if (iterable.type == Value::Type::Array && iterable.array) {
            items = iterable.array.get();
        } else if (iterable.type == Value::Type::String) {
            for (char c : iterable.string)
                strChars.push_back(Value::Str(std::string(1, c)));
            items = &strChars;
        } else {
            throw std::runtime_error("'for-in' requires an Array or String, got " + iterable.toString());
        }

        for (auto& item : *items) {
            auto loopEnv = std::make_shared<Environment>(env);
            loopEnv->set(n->var, item);
            try {
                for (auto& s : n->body) evalNode(s.get(), loopEnv, mod);
            } catch (ReturnSignal&) { throw; }
        }
        return Value::Nil();
    }

    // ── 模块声明 ──
    if (auto* n = dynamic_cast<ModuleDecl*>(node)) {
        executeModule(n, env);
        return Value::Nil();
    }

    // ── 跨模块调用（含回退到变量方法调用）────────────────
    if (auto* n = dynamic_cast<ModuleCall*>(node)) {
        // 先检查是否是已注册的模块
        auto mit = modules_.find(n->module);
        if (mit != modules_.end()) {
            // 检查是否是 persistent 字段访问（零参，且不是函数）
            if (n->args.empty()) {
                auto& modrt = mit->second;
                auto fit = modrt.functions.find(n->fn);
                if (fit == modrt.functions.end()) {
                    // 不是函数——尝试作为 persistent 字段
                    auto pit = modrt.persistentStore.find(n->fn);
                    if (pit != modrt.persistentStore.end())
                        return pit->second;
                }
            }
            // 是模块调用
            std::vector<Value> args;
            for (auto& a : n->args)
                args.push_back(evalNode(a.get(), env, mod));
            return callModuleFunction(n->module, n->fn, std::move(args));
        }

        // 不是模块——当作变量的方法调用（arr.push / str.upper 等）
        Value obj = evalNode(std::make_unique<Identifier>(n->module).get(), env, mod);
        // 复用 MethodCall 逻辑：构造一个临时 MethodCall 并求值
        std::vector<Value> margs;
        for (auto& a : n->args) margs.push_back(evalNode(a.get(), env, mod));

        // 内联 MethodCall 处理（避免重复代码，直接复用 MethodCall 路径）
        struct TempMethodCall : MethodCall {
            TempMethodCall(Value* obj_ptr, std::string m, std::vector<Value> computed_args)
                : MethodCall(nullptr, std::move(m), {})
                , obj_val(obj_ptr), computed(std::move(computed_args)) {}
            Value* obj_val;
            std::vector<Value> computed;
        };

        // 直接内联 method dispatch
        if (obj.type == Value::Type::Array && obj.array) {
            if (n->fn == "push" || n->fn == "append") {
                for (auto& v : margs) obj.array->push_back(v);
                // 写回变量（数组是 shared_ptr，自动共享）
                return Value::Num((double)obj.array->size());
            }
            if (n->fn == "pop") {
                if (obj.array->empty()) throw std::runtime_error("pop from empty array");
                Value last = obj.array->back();
                obj.array->pop_back();
                return last;
            }
            if (n->fn == "len" || n->fn == "size")
                return Value::Num((double)obj.array->size());
            if (n->fn == "first")
                return obj.array->empty() ? Value::Nil() : obj.array->front();
            if (n->fn == "last")
                return obj.array->empty() ? Value::Nil() : obj.array->back();
            if (n->fn == "contains") {
                if (margs.empty()) return Value::Bool(false);
                std::string needle = margs[0].toString();
                for (auto& v : *obj.array)
                    if (v.toString() == needle) return Value::Bool(true);
                return Value::Bool(false);
            }
            if (n->fn == "join") {
                std::string sep = margs.empty() ? "" : margs[0].toString();
                std::string result;
                for (size_t i = 0; i < obj.array->size(); i++) {
                    if (i > 0) result += sep;
                    result += (*obj.array)[i].toString();
                }
                return Value::Str(result);
            }
            if (n->fn == "reverse") {
                std::reverse(obj.array->begin(), obj.array->end());
                return obj;
            }
        }
        if (obj.type == Value::Type::String) {
            if (n->fn == "len" || n->fn == "size")
                return Value::Num((double)obj.string.size());
            if (n->fn == "upper") {
                std::string s = obj.string;
                for (auto& c : s) c = toupper(c);
                return Value::Str(s);
            }
            if (n->fn == "lower") {
                std::string s = obj.string;
                for (auto& c : s) c = tolower(c);
                return Value::Str(s);
            }
            if (n->fn == "split") {
                std::string sep = margs.empty() ? " " : margs[0].toString();
                auto arr = std::make_shared<std::vector<Value>>();
                size_t start = 0, p2 = 0;
                while ((p2 = obj.string.find(sep, start)) != std::string::npos) {
                    arr->push_back(Value::Str(obj.string.substr(start, p2 - start)));
                    start = p2 + sep.size();
                }
                arr->push_back(Value::Str(obj.string.substr(start)));
                return Value::Arr(arr);
            }
            if (n->fn == "contains") {
                if (margs.empty()) return Value::Bool(false);
                return Value::Bool(obj.string.find(margs[0].toString()) != std::string::npos);
            }
            if (n->fn == "trim") {
                std::string s = obj.string;
                size_t l = s.find_first_not_of(" \t\n\r");
                size_t r = s.find_last_not_of(" \t\n\r");
                return Value::Str(l == std::string::npos ? "" : s.substr(l, r - l + 1));
            }
        }
        throw std::runtime_error("unknown method '" + n->fn + "' on " + obj.toString());
    }

    // ── 持久状态读取（查询当前模块或全局）──
    if (auto* n = dynamic_cast<StateAccess*>(node)) {
        auto& store = mod ? mod->persistentStore : persistentStore_;
        auto it = store.find(n->field);
        if (it == store.end())
            throw std::runtime_error("undefined persistent field: state." + n->field);
        return it->second;
    }

    // ── 持久状态写入 ──
    if (auto* n = dynamic_cast<StateAssign*>(node)) {
        auto& store = mod ? mod->persistentStore : persistentStore_;
        auto it = store.find(n->field);
        if (it == store.end())
            throw std::runtime_error("undefined persistent field: state." + n->field
                + " (declare it in a persistent { } block)");
        it->second = evalNode(n->value.get(), env, mod);
        return it->second;
    }

    // ── 字面量 ──
    if (auto* n = dynamic_cast<NumberLit*>(node))  return Value::Num(n->value);
    if (auto* n = dynamic_cast<StringLit*>(node))  return Value::Str(n->value);
    if (auto* n = dynamic_cast<BoolLit*>(node))    return Value::Bool(n->value);

    // ── 变量 ──
    if (auto* n = dynamic_cast<Identifier*>(node)) return env->get(n->name);

    // ── 声明 ──
    if (auto* n = dynamic_cast<VarDecl*>(node)) {
        Value v = n->initializer ? evalNode(n->initializer.get(), env, mod) : Value::Nil();
        env->set(n->name, v);
        return v;
    }

    // ── 赋值 ──
    if (auto* n = dynamic_cast<Assign*>(node)) {
        Value v = evalNode(n->value.get(), env, mod);
        if (!env->assign(n->name, v))
            throw std::runtime_error("assignment to undefined variable: " + n->name);
        return v;
    }

    // ── 二元表达式 ──
    if (auto* n = dynamic_cast<BinaryExpr*>(node))
        return evalBinary(n, env, mod);

    // ── 一元表达式 ──
    if (auto* n = dynamic_cast<UnaryExpr*>(node)) {
        Value v = evalNode(n->operand.get(), env, mod);
        if (n->op == "!") return Value::Bool(!v.isTruthy());
        if (n->op == "-" && v.type == Value::Type::Number) return Value::Num(-v.number);
        throw std::runtime_error("invalid unary op");
    }

    // ── 函数调用 ──
    if (auto* n = dynamic_cast<CallExpr*>(node)) {
        std::vector<Value> args;
        for (auto& a : n->args) args.push_back(evalNode(a.get(), env, mod));
        return callFunction(n->name, std::move(args), mod);
    }

    // ── if ──
    if (auto* n = dynamic_cast<IfStmt*>(node)) {
        Value cond = evalNode(n->condition.get(), env, mod);
        auto& block = cond.isTruthy() ? n->thenBlock : n->elseBlock;
        auto blockEnv = std::make_shared<Environment>(env);
        for (auto& s : block) evalNode(s.get(), blockEnv, mod);
        return Value::Nil();
    }

    // ── while ──
    if (auto* n = dynamic_cast<WhileStmt*>(node)) {
        while (evalNode(n->condition.get(), env, mod).isTruthy()) {
            auto loopEnv = std::make_shared<Environment>(env);
            try {
                for (auto& s : n->body) evalNode(s.get(), loopEnv, mod);
            } catch (ReturnSignal&) { throw; }
        }
        return Value::Nil();
    }

    // ── return ──
    if (auto* n = dynamic_cast<ReturnStmt*>(node)) {
        Value v = n->value ? evalNode(n->value.get(), env, mod) : Value::Nil();
        throw ReturnSignal{v};
    }

    // ── 表达式语句 ──
    if (auto* n = dynamic_cast<ExprStmt*>(node))
        return evalNode(n->expr.get(), env, mod);

    // ── 跳过顶层声明节点 ──
    if (dynamic_cast<FnDecl*>(node))          return Value::Nil();
    if (dynamic_cast<PersistentBlock*>(node)) return Value::Nil();
    if (dynamic_cast<MigrateBlock*>(node))    return Value::Nil();

    throw std::runtime_error("unknown AST node");
}

// ── 二元运算 ──────────────────────────────────────────────
Value Interpreter::evalBinary(BinaryExpr* node, std::shared_ptr<Environment> env,
                               ModuleRuntime* mod) {
    if (node->op == "&&") {
        Value l = evalNode(node->left.get(), env, mod);
        if (!l.isTruthy()) return Value::Bool(false);
        return Value::Bool(evalNode(node->right.get(), env, mod).isTruthy());
    }
    if (node->op == "||") {
        Value l = evalNode(node->left.get(), env, mod);
        if (l.isTruthy()) return Value::Bool(true);
        return Value::Bool(evalNode(node->right.get(), env, mod).isTruthy());
    }

    Value l = evalNode(node->left.get(), env, mod);
    Value r = evalNode(node->right.get(), env, mod);
    const std::string& op = node->op;

    if (op == "+" && (l.type == Value::Type::String || r.type == Value::Type::String))
        return Value::Str(l.toString() + r.toString());

    if (l.type == Value::Type::Number && r.type == Value::Type::Number) {
        double a = l.number, b = r.number;
        if (op == "+")  return Value::Num(a + b);
        if (op == "-")  return Value::Num(a - b);
        if (op == "*")  return Value::Num(a * b);
        if (op == "/")  { if (b == 0) throw std::runtime_error("division by zero"); return Value::Num(a / b); }
        if (op == "%")  return Value::Num(std::fmod(a, b));
        if (op == "<")  return Value::Bool(a < b);
        if (op == ">")  return Value::Bool(a > b);
        if (op == "<=") return Value::Bool(a <= b);
        if (op == ">=") return Value::Bool(a >= b);
        if (op == "==") return Value::Bool(a == b);
        if (op == "!=") return Value::Bool(a != b);
    }

    if (op == "==") return Value::Bool(l.toString() == r.toString());
    if (op == "!=") return Value::Bool(l.toString() != r.toString());

    throw std::runtime_error("invalid binary operation: " + op);
}
