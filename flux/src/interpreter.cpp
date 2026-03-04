#include "interpreter.h"
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>

// ── ChanVal 方法实现 ──────────────────────────────────────
// 锁顺序约束：先释放 GIL，再持有 channel 锁。
// 不允许同时持有 GIL + channel 锁（防止死锁）。

bool ChanVal::send(Value v) {
    GILRelease release;                          // 先释放 GIL
    std::unique_lock<std::mutex> lk(mu);         // 再持有 channel 锁
    if (cap > 0) {                               // 有界通道满时等待
        cv_send.wait(lk, [this] {
            return q.size() < cap || closed_;
        });
    }
    if (closed_) return false;
    q.push_back(std::move(v));
    cv_recv.notify_one();
    return true;
    // 析构顺序：lk 先释放，再 GILRelease 恢复 GIL（无死锁）
}

std::optional<Value> ChanVal::recv() {
    GILRelease release;                          // 先释放 GIL
    std::unique_lock<std::mutex> lk(mu);         // 再持有 channel 锁
    cv_recv.wait(lk, [this] {
        return !q.empty() || closed_;
    });
    if (q.empty()) return std::nullopt;          // 通道已关闭且空
    Value v = std::move(q.front());
    q.pop_front();
    cv_send.notify_one();
    return v;
}

std::optional<Value> ChanVal::tryRecv() {
    std::unique_lock<std::mutex> lk(mu);
    if (q.empty()) return std::nullopt;
    Value v = std::move(q.front());
    q.pop_front();
    cv_send.notify_one();
    return v;
}

void ChanVal::close() {
    std::unique_lock<std::mutex> lk(mu);
    closed_ = true;
    cv_recv.notify_all();
    cv_send.notify_all();
}

// ── 构造 & 析构 ───────────────────────────────────────────
Interpreter::Interpreter() {
    globalEnv_ = std::make_shared<Environment>();
    registerBuiltins();
    registerStdlib();
}

// 等待所有 async/spawn 任务完成，防止悬空引用
Interpreter::~Interpreter() {
    // 通知所有线程池停止（pool 析构会 join workers）
    pools_.clear();
    for (auto& f : pendingTasks_) {
        if (f.valid()) {
            GILRelease release;
            try { f.wait(); } catch (...) {}
        }
    }
}

// 获取模块绑定的线程池（nullptr = 无绑定或 pool 未声明）
ThreadPool* Interpreter::getModulePool(const std::string& moduleName) {
    auto it = modulePools_.find(moduleName);
    if (it == modulePools_.end()) return nullptr;
    auto pit = pools_.find(it->second);
    if (pit == pools_.end()) return nullptr;
    return pit->second.get();
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
    // len(x) → number (string/array/map length)
    registerBuiltin("len", [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value::Num(0);
        if (args[0].type == Value::Type::String)
            return Value::Num((double)args[0].string.size());
        if (args[0].type == Value::Type::Array && args[0].array)
            return Value::Num((double)args[0].array->size());
        if (args[0].type == Value::Type::Map && args[0].map)
            return Value::Num((double)args[0].map->size());
        return Value::Num(0);
    });
    registerBuiltin("type", [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value::Str("nil");
        switch (args[0].type) {
            case Value::Type::Number: return Value::Str("Number");
            case Value::Type::String: return Value::Str("String");
            case Value::Type::Bool:   return Value::Str("Bool");
            case Value::Type::Array:  return Value::Str("Array");
            case Value::Type::Map:    return Value::Str("Map");
            default:                  return Value::Str("Nil");
        }
    });
    // Map() → 创建空 Map
    registerBuiltin("Map", [](std::vector<Value>) -> Value {
        return Value::MapVal();
    });

    // struct(s) — 迭代结构体字段名（Spec v1.0）
    registerBuiltin("struct", [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("struct() requires one argument");
        auto& v = args[0];
        if (v.type == Value::Type::StructInst && v.structInst) {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& f : v.structInst->type->fields)
                arr->push_back(Value::Str(f.name));
            return Value::Arr(arr);
        }
        throw std::runtime_error("struct() requires a struct instance");
    });

    // __func_iter__(s) — 迭代结构体方法名 via func(s) syntax（Spec v1.0）
    registerBuiltin("__func_iter__", [](std::vector<Value> args) -> Value {
        if (args.empty()) throw std::runtime_error("func() requires one argument");
        auto& v = args[0];
        if (v.type == Value::Type::StructInst && v.structInst) {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& m : v.structInst->type->methods)
                arr->push_back(Value::Str(m.name));
            return Value::Arr(arr);
        }
        throw std::runtime_error("func() requires a struct instance");
    });
}

void Interpreter::registerBuiltin(const std::string& name, BuiltinFn fn) {
    builtins_[name] = std::move(fn);
}

void Interpreter::registerStdlibModule(const std::string& name,
                                        std::unordered_map<std::string, StdlibFn> fns) {
    stdlibModules_[name] = std::move(fns);
}

// ── VM 模式初始化（仅注册，不执行普通语句）────────────────
void Interpreter::initProgram(Program* program) {
    globalEnv_ = std::make_shared<Environment>();
    functions_.clear();

    // 第一遍：注册全局函数 + 初始化 persistent
    for (auto& stmt : program->statements) {
        if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
            // !func: 灰度切换 — 存入 pending，在下一调用边界生效
            if (fn->forceOverride) pendingFnUpdates_[fn->name] = fn;
            else                   functions_[fn->name] = fn;
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

    // 第二遍：执行模块声明（模块仍用树遍历解释器）
    for (auto& stmt : program->statements) {
        if (auto* md = dynamic_cast<ModuleDecl*>(stmt.get())) {
            try { executeModule(md, globalEnv_); }
            catch (ReturnSignal&) {}
        }
    }
}

// ── 主执行入口 ────────────────────────────────────────────
void Interpreter::execute(Program* program) {
    // 热更新：重建全局环境（普通变量重置），持久化存储保留
    globalEnv_ = std::make_shared<Environment>();

    // 第一遍：注册全局函数 + 初始化全局 persistent
    functions_.clear();
    for (auto& stmt : program->statements) {
        if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
            // !func: 灰度切换 — 存入 pending，在下一调用边界生效
            if (fn->forceOverride) pendingFnUpdates_[fn->name] = fn;
            else                   functions_[fn->name] = fn;
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
        } catch (ReturnSignal&) {
        } catch (PanicSignal& p) {
            throw std::runtime_error(p.message);
        }
    }
}

// ── REPL 增量执行（保留 globalEnv_ + functions_）──────────
// 不重置环境：变量和函数定义在 REPL 会话中持续积累。
void Interpreter::executeRepl(Program* program) {
    // 第一遍：仅注册新函数 + 初始化尚未存在的 persistent 字段
    for (auto& stmt : program->statements) {
        if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
            functions_[fn->name] = fn;  // REPL: 立即覆盖（交互模式无需灰度切换）
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
    // 第二遍：执行非声明语句
    for (auto& stmt : program->statements) {
        if (dynamic_cast<FnDecl*>(stmt.get()))          continue;
        if (dynamic_cast<PersistentBlock*>(stmt.get())) continue;
        try {
            evalNode(stmt.get(), globalEnv_);
        } catch (ReturnSignal&) {
        } catch (PanicSignal& p) {
            throw std::runtime_error(p.message);
        }
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

    // @concurrent 绑定：注册 module → pool 映射
    if (!decl->poolName.empty()) {
        modulePools_[decl->name] = decl->poolName;
    }

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
    // 调用边界：应用所有 !var / !func 灰度切换
    applyPendingUpdates();

    // ── 标准库模块优先 ──────────────────────────────────
    auto sit = stdlibModules_.find(modName);
    if (sit != stdlibModules_.end()) {
        auto fit = sit->second.find(fnName);
        if (fit == sit->second.end())
            throw std::runtime_error("undefined stdlib function: " + modName + "." + fnName);
        return fit->second(std::move(args));
    }

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

// ── !var / !func 灰度切换：在调用边界统一应用 pending 更新 ──
// 正在执行的任务用旧版本跑完；进入下一个函数调用时切换到新版本。
void Interpreter::applyPendingUpdates() {
    for (auto& [name, val] : pendingVarUpdates_)
        globalEnv_->set(name, val);
    pendingVarUpdates_.clear();

    for (auto& [name, fn] : pendingFnUpdates_)
        functions_[name] = fn;
    pendingFnUpdates_.clear();
}

// ── 普通函数调用 ──────────────────────────────────────────
Value Interpreter::callFunction(const std::string& name,
                                 std::vector<Value> args,
                                 ModuleRuntime* mod) {
    // 调用边界：应用所有 !var / !func 灰度切换
    applyPendingUpdates();

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
    if (!fn) {
        // Check if name resolves to a value (struct type or function value)
        try {
            Value v = globalEnv_->get(name);
            if (v.type == Value::Type::StructType && v.structType) {
                // Create instance with positional args filling fields in order
                std::vector<std::pair<std::string, Value>> initFields;
                for (size_t i = 0; i < args.size() && i < v.structType->fields.size(); ++i)
                    initFields.push_back({v.structType->fields[i].name, args[i]});
                return createStructInst(v.structType, initFields, globalEnv_, mod);
            }
            if (v.type == Value::Type::Function && v.func)
                return callFuncVal(v.func, std::move(args), mod);
        } catch (...) {}
        throw std::runtime_error("undefined function: " + name);
    }

    auto fnEnv = std::make_shared<Environment>(globalEnv_);
    for (size_t i = 0; i < fn->params.size(); i++)
        fnEnv->set(fn->params[i].name, i < args.size() ? args[i] : Value::Nil());

    try {
        for (auto& stmt : fn->body)
            evalNode(stmt.get(), fnEnv, mod);
    } catch (ReturnSignal& ret) {
        return ret.value;
    } catch (PanicSignal& p) {
        // 合并 exception 描述到错误信息（全局 + 内联）
        std::string merged = "[自动] " + p.message;
        bool hasDesc = false;
        auto dit = exceptionDescs_.find(name);
        if (dit != exceptionDescs_.end())
            for (auto& d : dit->second) { merged += "\n[描述] " + d; hasDesc = true; }
        for (auto& d : lastInlineExceptionDescs_) { merged += "\n[描述] " + d; hasDesc = true; }
        lastInlineExceptionDescs_.clear();
        if (hasDesc) throw PanicSignal{merged};
        throw;
    } catch (std::runtime_error& e) {
        std::string merged = "[自动] " + std::string(e.what());
        bool hasDesc = false;
        auto dit = exceptionDescs_.find(name);
        if (dit != exceptionDescs_.end())
            for (auto& d : dit->second) { merged += "\n[描述] " + d; hasDesc = true; }
        for (auto& d : lastInlineExceptionDescs_) { merged += "\n[描述] " + d; hasDesc = true; }
        lastInlineExceptionDescs_.clear();
        if (hasDesc) throw std::runtime_error(merged);
        throw;
    }
    return Value::Nil();
}

// ── 调用函数值（匿名函数 / 闭包 / 具名函数引用）──────────
Value Interpreter::callFuncVal(std::shared_ptr<FuncVal> fv, std::vector<Value> args,
                                ModuleRuntime* mod) {
    // 具名函数引用：委托回 callFunction
    if (fv->fnDecl) {
        auto fnEnv = std::make_shared<Environment>(fv->closure ? fv->closure : globalEnv_);
        for (size_t i = 0; i < fv->fnDecl->params.size(); i++)
            fnEnv->set(fv->fnDecl->params[i].name, i < args.size() ? args[i] : Value::Nil());
        try {
            for (auto& stmt : fv->fnDecl->body)
                evalNode(stmt.get(), fnEnv, mod);
        } catch (ReturnSignal& ret) {
            return ret.value;
        }
        return Value::Nil();
    }
    // 内置函数引用
    if (!fv->name.empty() && fv->ownedBody.empty()) {
        auto bit = builtins_.find(fv->name);
        if (bit != builtins_.end()) return bit->second(std::move(args));
    }
    // 匿名函数 / 闭包
    auto fnEnv = std::make_shared<Environment>(fv->closure ? fv->closure : globalEnv_);
    for (size_t i = 0; i < fv->params.size(); i++)
        fnEnv->set(fv->params[i].name, i < args.size() ? args[i] : Value::Nil());
    try {
        for (auto& s : fv->ownedBody)
            evalNode(s.get(), fnEnv, mod);
    } catch (ReturnSignal& ret) {
        return ret.value;
    }
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
        if (obj.type == Value::Type::Map) {
            if (!obj.map) return Value::Nil();
            auto it = obj.map->find(idx.toString());
            return it != obj.map->end() ? it->second : Value::Nil();
        }
        throw std::runtime_error("cannot index into " + obj.toString());
    }

    // ── 字段赋值 obj.field = value（struct 方法内 self.x = ...）──
    if (auto* n = dynamic_cast<FieldAssign*>(node)) {
        Value obj = evalNode(n->object.get(), env, mod);
        Value val = evalNode(n->value.get(),  env, mod);
        if (obj.type == Value::Type::StructInst && obj.structInst) {
            obj.structInst->fields[n->field] = val;
            // Update binding in environment so 'self' reflects mutated instance
            if (auto* id = dynamic_cast<Identifier*>(n->object.get())) {
                env->assign(id->name, obj);
                // Also update direct field local binding if present
                env->assign(n->field, val);
            }
            return val;
        }
        throw std::runtime_error("field assignment on non-struct-instance");
    }

    // ── 下标赋值 arr[i] = value ───────────────────────────
    if (auto* n = dynamic_cast<IndexAssign*>(node)) {
        Value obj = evalNode(n->object.get(), env, mod);
        Value idx = evalNode(n->index.get(),  env, mod);
        Value val = evalNode(n->value.get(),  env, mod);
        if (obj.type == Value::Type::Map) {
            if (!obj.map) throw std::runtime_error("null map");
            (*obj.map)[idx.toString()] = val;
            return val;
        }
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
        // ── Map 方法 ──────────────────────────────────────
        if (obj.type == Value::Type::Map && obj.map) {
            if (n->method == "len" || n->method == "size")
                return Value::Num((double)obj.map->size());
            if (n->method == "has")
                return Value::Bool(!margs.empty() && obj.map->count(margs[0].toString()) > 0);
            if (n->method == "get") {
                if (margs.empty()) return Value::Nil();
                auto it = obj.map->find(margs[0].toString());
                return it != obj.map->end() ? it->second
                       : (margs.size() > 1 ? margs[1] : Value::Nil());
            }
            if (n->method == "set") {
                if (margs.size() < 2) return Value::Nil();
                (*obj.map)[margs[0].toString()] = margs[1];
                return margs[1];
            }
            if (n->method == "delete" || n->method == "remove")
                return Value::Bool(!margs.empty() && obj.map->erase(margs[0].toString()) > 0);
            if (n->method == "keys") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& kv : *obj.map) arr->push_back(Value::Str(kv.first));
                return Value::Arr(arr);
            }
            if (n->method == "values") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& kv : *obj.map) arr->push_back(kv.second);
                return Value::Arr(arr);
            }
            if (n->method == "entries") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& kv : *obj.map) {
                    auto pair = std::make_shared<std::vector<Value>>();
                    pair->push_back(Value::Str(kv.first));
                    pair->push_back(kv.second);
                    arr->push_back(Value::Arr(pair));
                }
                return Value::Arr(arr);
            }
        }
        // ── Future 方法：future.await() / future.await(ms) / future.isReady() / future.get()
        if (obj.type == Value::Type::Future && obj.future) {
            if (n->method == "await" || n->method == "get" || n->method == "value") {
                int timeout_ms = margs.empty() ? -1 : (int)margs[0].number;
                return obj.future->getWithTimeout(timeout_ms);
            }
            if (n->method == "isReady") return Value::Bool(obj.future->isReady());
        }
        // ── Chan 方法（与 ModuleCall 路径共享实现）──────────
        if (obj.type == Value::Type::Chan && obj.chan) {
            if (n->method == "send") {
                if (margs.empty()) throw std::runtime_error("chan.send() requires an argument");
                bool ok = obj.chan->send(std::move(margs[0]));
                return Value::Bool(ok);
            }
            if (n->method == "recv") {
                auto val = obj.chan->recv();
                return val.has_value() ? *val : Value::Nil();
            }
            if (n->method == "tryRecv") {
                auto val = obj.chan->tryRecv();
                return val.has_value() ? *val : Value::Nil();
            }
            if (n->method == "close")   { obj.chan->close(); return Value::Nil(); }
            if (n->method == "len" || n->method == "size")
                return Value::Num((double)obj.chan->len());
            if (n->method == "isClosed") return Value::Bool(obj.chan->isClosed());
        }
        throw std::runtime_error("unknown method '" + n->method + "' on " + obj.toString());
    }

    // ── for item in iterable { } ──────────────────────────
    if (auto* n = dynamic_cast<ForIn*>(node)) {
        // 区间范围 [a, b] / [a, b)
        if (auto* ir = dynamic_cast<IntervalRange*>(n->iterable.get())) {
            Value sv = evalNode(ir->start.get(), env, mod);
            Value ev = evalNode(ir->end.get(),   env, mod);
            if (sv.type != Value::Type::Number || ev.type != Value::Type::Number)
                throw std::runtime_error("interval range requires numeric start and end");
            long long start = (long long)sv.number;
            long long end   = (long long)ev.number;
            long long limit = ir->inclusive ? end + 1 : end;
            for (long long i = start; i < limit; ++i) {
                auto loopEnv = std::make_shared<Environment>(env);
                loopEnv->set(n->var, Value::Num((double)i));
                try { for (auto& s : n->body) evalNode(s.get(), loopEnv, mod); }
                catch (ReturnSignal&) { throw; }
            }
            return Value::Nil();
        }

        Value iterable = evalNode(n->iterable.get(), env, mod);
        std::vector<Value>* items = nullptr;
        std::vector<Value> strChars; // 字符串/结构体迭代时的临时存储

        if (iterable.type == Value::Type::Array && iterable.array) {
            items = iterable.array.get();
        } else if (iterable.type == Value::Type::String) {
            for (char c : iterable.string)
                strChars.push_back(Value::Str(std::string(1, c)));
            items = &strChars;
        } else if (iterable.type == Value::Type::Map && iterable.map) {
            for (auto& kv : *iterable.map)
                strChars.push_back(Value::Str(kv.first));
            items = &strChars;
        } else if (iterable.type == Value::Type::StructInst && iterable.structInst) {
            // for x in struct_instance → iterate over field names
            for (auto& f : iterable.structInst->type->fields)
                strChars.push_back(Value::Str(f.name));
            items = &strChars;
        } else {
            throw std::runtime_error("'for-in' requires an Array, String, Map, or struct, got " + iterable.toString());
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

    // ── 线程池声明 @threadpool ──
    if (auto* n = dynamic_cast<ThreadPoolDecl*>(node)) {
        if (n->name.empty())
            throw std::runtime_error("@threadpool: name cannot be empty");
        OverflowPolicy ov = OverflowPolicy::Block;
        auto pool = std::make_shared<ThreadPool>(n->name, (size_t)n->size, 0, ov);
        pools_[n->name] = std::move(pool);
        return Value::Nil();
    }

    // ── 模块声明 ──
    if (auto* n = dynamic_cast<ModuleDecl*>(node)) {
        executeModule(n, env);
        return Value::Nil();
    }

    // ── Module.fn.async(args) — 跨 pool 异步调用 ──────────
    if (auto* n = dynamic_cast<AsyncCall*>(node)) {
        std::vector<Value> captArgs;
        for (auto& a : n->args)
            captArgs.push_back(evalNode(a.get(), env, mod));

        auto promise = std::make_shared<std::promise<Value>>();
        auto sfuture = promise->get_future().share();

        ThreadPool* pool = getModulePool(n->module);
        Interpreter* self = this;
        std::string  modName = n->module;
        std::string  fnName  = n->fn;

        if (pool) {
            // 提交到模块绑定的线程池
            bool submitted = pool->submit(
                [self, promise, modName, fnName, captArgs = std::move(captArgs)]() mutable {
                    GILGuard gil;
                    try {
                        promise->set_value(self->callModuleFunction(modName, fnName, std::move(captArgs)));
                    } catch (...) {
                        promise->set_exception(std::current_exception());
                    }
                });
            if (!submitted) {
                // Drop 策略：直接返回 nil Future
                promise->set_value(Value::Nil());
            }
        } else {
            // 无 pool 绑定：退化为普通 detached 线程（向后兼容）
            std::thread t(
                [self, promise, modName, fnName, captArgs = std::move(captArgs)]() mutable {
                    GILGuard gil;
                    try {
                        promise->set_value(self->callModuleFunction(modName, fnName, std::move(captArgs)));
                    } catch (...) {
                        promise->set_exception(std::current_exception());
                    }
                });
            t.detach();
        }

        pendingTasks_.push_back(sfuture);
        return Value::Future(std::make_shared<FutureVal>(std::move(sfuture)));
    }

    // ── 跨模块调用（含回退到变量方法调用）────────────────
    if (auto* n = dynamic_cast<ModuleCall*>(node)) {
        // 先检查标准库模块
        if (stdlibModules_.count(n->module)) {
            std::vector<Value> args;
            for (auto& a : n->args)
                args.push_back(evalNode(a.get(), env, mod));
            return callModuleFunction(n->module, n->fn, std::move(args));
        }
        // 再检查用户模块
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
        // ── Map 方法（变量回退路径）──────────────────────
        if (obj.type == Value::Type::Map && obj.map) {
            if (n->fn == "len" || n->fn == "size")
                return Value::Num((double)obj.map->size());
            if (n->fn == "has")
                return Value::Bool(!margs.empty() && obj.map->count(margs[0].toString()) > 0);
            if (n->fn == "get") {
                if (margs.empty()) return Value::Nil();
                auto it = obj.map->find(margs[0].toString());
                return it != obj.map->end() ? it->second
                       : (margs.size() > 1 ? margs[1] : Value::Nil());
            }
            if (n->fn == "set") {
                if (margs.size() < 2) return Value::Nil();
                (*obj.map)[margs[0].toString()] = margs[1];
                return margs[1];
            }
            if (n->fn == "delete" || n->fn == "remove")
                return Value::Bool(!margs.empty() && obj.map->erase(margs[0].toString()) > 0);
            if (n->fn == "keys") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& kv : *obj.map) arr->push_back(Value::Str(kv.first));
                return Value::Arr(arr);
            }
            if (n->fn == "values") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& kv : *obj.map) arr->push_back(kv.second);
                return Value::Arr(arr);
            }
            if (n->fn == "entries") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& kv : *obj.map) {
                    auto pair = std::make_shared<std::vector<Value>>();
                    pair->push_back(Value::Str(kv.first));
                    pair->push_back(kv.second);
                    arr->push_back(Value::Arr(pair));
                }
                return Value::Arr(arr);
            }
        }
        // ── Chan 方法 ─────────────────────────────────────
        if (obj.type == Value::Type::Chan && obj.chan) {
            if (n->fn == "send") {
                if (margs.empty()) throw std::runtime_error("Chan.send requires a value");
                bool ok = obj.chan->send(std::move(margs[0]));
                return Value::Bool(ok);
            }
            if (n->fn == "recv") {
                auto val = obj.chan->recv();
                return val.has_value() ? *val : Value::Nil();
            }
            if (n->fn == "tryRecv") {
                auto val = obj.chan->tryRecv();
                return val.has_value() ? *val : Value::Nil();
            }
            if (n->fn == "close") {
                obj.chan->close();
                return Value::Nil();
            }
            if (n->fn == "len" || n->fn == "size")
                return Value::Num((double)obj.chan->len());
            if (n->fn == "isClosed")
                return Value::Bool(obj.chan->isClosed());
        }
        // ── Future 方法 ───────────────────────────────────
        if (obj.type == Value::Type::Future && obj.future) {
            if (n->fn == "isReady")
                return Value::Bool(obj.future->isReady());
            if (n->fn == "await" || n->fn == "get" || n->fn == "value") {
                // await(timeout_ms?) — 支持可选超时参数
                int timeout_ms = margs.empty() ? -1 : (int)margs[0].number;
                return obj.future->getWithTimeout(timeout_ms);
            }
        }
        // ── 结构体实例 字段访问 / 方法调用 ──────────────────
        if (obj.type == Value::Type::StructInst && obj.structInst) {
            auto& inst = *obj.structInst;
            // Method call
            const auto* method = inst.type->findMethod(n->fn);
            if (method) {
                // Create a new env with 'self' bound to the instance, plus params
                auto methodEnv = std::make_shared<Environment>(env);
                // Bind 'self' so methods can access other methods/fields
                methodEnv->set("self", obj);
                if (margs.size() != method->params.size())
                    throw std::runtime_error("method '" + n->fn + "': expected " +
                        std::to_string(method->params.size()) + " args, got " +
                        std::to_string(margs.size()));
                for (size_t i = 0; i < method->params.size(); ++i)
                    methodEnv->set(method->params[i].name, margs[i]);
                // Bind all fields as locals for easy access
                for (auto& [fname, fval] : inst.fields)
                    methodEnv->set(fname, fval);
                try {
                    for (auto& s : method->body)
                        evalNode(s.get(), methodEnv, mod);
                } catch (ReturnSignal& rs) {
                    return rs.value;
                }
                return Value::Nil();
            }
            // Field access (0-arg)
            if (margs.empty()) {
                auto fit = inst.fields.find(n->fn);
                if (fit != inst.fields.end()) return fit->second;
            }
            throw std::runtime_error("struct '" + inst.type->name + "' has no field/method '" + n->fn + "'");
        }
        // ── 结构体类型 字段/方法名列表 ─────────────────────
        if (obj.type == Value::Type::StructType && obj.structType) {
            if (n->fn == "fields") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& f : obj.structType->fields) arr->push_back(Value::Str(f.name));
                return Value::Arr(arr);
            }
            if (n->fn == "methods") {
                auto arr = std::make_shared<std::vector<Value>>();
                for (auto& m : obj.structType->methods) arr->push_back(Value::Str(m.name));
                return Value::Arr(arr);
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
    if (dynamic_cast<NilLit*>(node))               return Value::Nil();

    // ── 变量（含函数引用）──
    if (auto* n = dynamic_cast<Identifier*>(node)) {
        // 先查环境变量
        if (env->has(n->name)) return env->get(n->name);
        // 再查具名函数表 → 返回函数值
        auto fit = functions_.find(n->name);
        if (fit != functions_.end()) {
            auto fv = std::make_shared<FuncVal>();
            fv->name   = fit->second->name;
            fv->params = fit->second->params;
            fv->fnDecl = fit->second;
            return Value::FuncV(std::move(fv));
        }
        // 再查内置函数
        if (builtins_.count(n->name)) {
            auto fv = std::make_shared<FuncVal>();
            fv->name = n->name;
            return Value::FuncV(std::move(fv));
        }
        return env->get(n->name);  // 抛出 undefined variable
    }

    // ── 声明 ──
    if (auto* n = dynamic_cast<VarDecl*>(node)) {
        Value v = n->initializer ? evalNode(n->initializer.get(), env, mod) : Value::Nil();
        if (n->isInterface && v.type == Value::Type::Interface && v.iface)
            v.iface->name = n->name;

        if (n->forceOverride) {
            // !var: 灰度切换 — 存入 pending 表，在下一次函数调用边界才生效。
            // 正在运行的任务继续使用旧值；下一个任务进来时看到新值。
            pendingVarUpdates_[n->name] = v;
            // 本次热更新循环内，变量仍沿用当前已存在的旧值（若无则返回新值）
            return env->has(n->name) ? env->get(n->name) : v;
        }

        // var: 热更新时保留已存在的值（不重置）
        if (env->has(n->name)) return env->get(n->name);
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
        // 先检查局部变量中是否存在同名函数值
        if (env->has(n->name)) {
            try {
                Value v = env->get(n->name);
                if (v.type == Value::Type::Function && v.func)
                    return callFuncVal(v.func, std::move(args), mod);
            } catch (...) {}
        }
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

    // ── 并发节点（Feature K）──────────────────────────────

    // async call(args) — 在新线程中运行函数调用，返回 Future
    if (auto* n = dynamic_cast<AsyncExpr*>(node)) {
        // 提取被调用函数名和已求值的参数
        std::string     funcName, modName;
        std::vector<Value> args;
        bool isModule = false;

        if (auto* call = dynamic_cast<CallExpr*>(n->call.get())) {
            funcName = call->name;
            for (auto& a : call->args) args.push_back(evalNode(a.get(), env, mod));
        } else if (auto* mc = dynamic_cast<ModuleCall*>(n->call.get())) {
            modName  = mc->module;
            funcName = mc->fn;
            isModule = true;
            for (auto& a : mc->args) args.push_back(evalNode(a.get(), env, mod));
        } else {
            throw std::runtime_error("async only supports function calls");
        }

        // 创建 promise/future 对
        auto promise = std::make_shared<std::promise<Value>>();
        auto sfuture = promise->get_future().share();

        Interpreter* self = this;
        std::thread t([self, promise, funcName, modName, isModule,
                        captArgs = std::move(args)]() mutable {
            GILGuard gil;  // 异步线程进入时获取 GIL
            try {
                Value result = isModule
                    ? self->callModuleFunction(modName, funcName, std::move(captArgs))
                    : self->callFunction(funcName, std::move(captArgs));
                promise->set_value(std::move(result));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        t.detach();

        pendingTasks_.push_back(sfuture);
        return Value::Future(std::make_shared<FutureVal>(std::move(sfuture)));
    }

    // await expr — 等待 Future 完成，返回结果值（释放 GIL 期间阻塞）
    if (auto* n = dynamic_cast<AwaitExpr*>(node)) {
        Value futVal = evalNode(n->expr.get(), env, mod);
        if (futVal.type != Value::Type::Future || !futVal.future)
            throw std::runtime_error("await requires a Future value");
        return futVal.future->get();  // GILRelease 在 FutureVal::get() 内部
    }

    // spawn { ... } — fire-and-forget 后台任务
    if (auto* n = dynamic_cast<SpawnStmt*>(node)) {
        // 复制 body 的 raw 指针列表（nodes 由 Program 所有，生命周期足够）
        auto promise = std::make_shared<std::promise<Value>>();
        auto sfuture = promise->get_future().share();

        // 捕获 body 节点的原始指针（由 AST 拥有）
        std::vector<ASTNode*> body;
        for (auto& s : n->body) body.push_back(s.get());

        Interpreter* self = this;
        auto spawnEnv = std::make_shared<Environment>(env);  // 快照当前作用域

        std::thread t([self, promise, body, spawnEnv, mod]() mutable {
            GILGuard gil;
            try {
                for (ASTNode* stmt : body)
                    self->evalNode(stmt, spawnEnv, mod);
                promise->set_value(Value::Nil());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        t.detach();

        pendingTasks_.push_back(sfuture);
        return Value::Nil();
    }

    // ── 跳过顶层声明节点 ──
    if (dynamic_cast<FnDecl*>(node))          return Value::Nil();
    if (dynamic_cast<PersistentBlock*>(node)) return Value::Nil();
    if (dynamic_cast<MigrateBlock*>(node))    return Value::Nil();

    // ══════════════════════════════════════════════════════
    // Spec v1.0 新节点
    // ══════════════════════════════════════════════════════

    // ── 结构体字面量 { field: val, ..., func method() {} } ──
    // 自动推断：无字段 + 所有方法均无函数体 → 接口
    if (auto* n = dynamic_cast<StructLit*>(node)) {
        if (n->interfaceName.empty() && n->fields.empty() && !n->methods.empty()) {
            bool allSignatures = true;
            for (auto& m : n->methods)
                if (!m.body.empty()) { allSignatures = false; break; }
            if (allSignatures) {
                // 自动推断为接口
                auto iface = std::make_shared<InterfaceInfo>();
                for (auto& m : n->methods) {
                    InterfaceInfo::MethodSig sig;
                    sig.name   = m.name;
                    sig.params = m.params;
                    iface->methods.push_back(std::move(sig));
                }
                return Value::InterfaceV(std::move(iface));
            }
        }

        auto typeInfo = std::make_shared<StructTypeInfo>();
        typeInfo->interfaceName = n->interfaceName;

        // Store fields
        for (auto& f : n->fields) {
            StructTypeInfo::Field fi;
            fi.name = f.name;
            if (f.defaultValue)
                fi.defaultExpr = std::shared_ptr<ASTNode>(f.defaultValue.get(),
                                                          [](ASTNode*){});  // non-owning
            typeInfo->fields.push_back(std::move(fi));
        }
        // Store methods (share body ownership through shared_ptr)
        for (auto& m : n->methods) {
            StructTypeInfo::Method mi;
            mi.name       = m.name;
            mi.params     = m.params;
            mi.returnType = m.returnType;
            for (auto& s : m.body)
                mi.body.push_back(std::shared_ptr<ASTNode>(s.get(), [](ASTNode*){}));
            typeInfo->methods.push_back(std::move(mi));
        }

        // Interface conformance check
        if (!n->interfaceName.empty()) {
            try {
                Value ifaceVal = env->get(n->interfaceName);
                if (ifaceVal.type == Value::Type::Interface && ifaceVal.iface) {
                    for (auto& sig : ifaceVal.iface->methods) {
                        if (!typeInfo->findMethod(sig.name))
                            throw std::runtime_error(
                                "struct missing method '" + sig.name +
                                "' required by interface '" + n->interfaceName + "'");
                    }
                    typeInfo->interfaceName = n->interfaceName;
                }
            } catch (std::runtime_error& e) {
                // Re-throw interface conformance errors
                if (std::string(e.what()).find("missing method") != std::string::npos)
                    throw;
                // "undefined variable" means the interface name wasn't found — ignore
            }
        }
        return Value::StructTypeV(std::move(typeInfo));
    }

    // ── 接口字面量 { func area() } ────────────────────────
    if (auto* n = dynamic_cast<InterfaceLit*>(node)) {
        auto iface = std::make_shared<InterfaceInfo>();
        iface->name = n->name;
        for (auto& m : n->methods) {
            InterfaceInfo::MethodSig sig;
            sig.name   = m.name;
            sig.params = m.params;
            iface->methods.push_back(std::move(sig));
        }
        return Value::InterfaceV(std::move(iface));
    }

    // ── 结构体具名构造 Point(x: 3, y: 4) ─────────────────
    if (auto* n = dynamic_cast<StructCreate*>(node)) {
        Value typeVal = env->get(n->typeName);
        if (typeVal.type != Value::Type::StructType || !typeVal.structType)
            throw std::runtime_error("'" + n->typeName + "' is not a struct type");
        std::vector<std::pair<std::string, Value>> initFields;
        for (auto& fi : n->fields)
            initFields.push_back({fi.name, evalNode(fi.value.get(), env, mod)});
        return createStructInst(typeVal.structType, initFields, env, mod);
    }

    // ── 匿名函数表达式 func(params) { body } ────────────────
    if (auto* n = dynamic_cast<FuncExpr*>(node)) {
        auto fv = std::make_shared<FuncVal>();
        fv->params  = n->params;
        fv->closure = env;  // 捕获当前作用域（闭包）
        for (auto& s : n->body)
            fv->ownedBody.push_back(std::shared_ptr<ASTNode>(s.get(), [](ASTNode*){}));
        return Value::FuncV(std::move(fv));
    }

    // ── exception 描述声明 ─────────────────────────────────
    if (auto* n = dynamic_cast<ExceptionDecl*>(node)) {
        if (!n->target.empty()) {
            // 执行前检查：target 函数/方法必须存在
            auto colonPos = n->target.find(':');
            if (colonPos != std::string::npos) {
                // exception Type:method — 检查结构体类型及方法
                std::string typeName   = n->target.substr(0, colonPos);
                std::string methodName = n->target.substr(colonPos + 1);
                bool found = false;
                if (env->has(typeName)) {
                    try {
                        Value tv = env->get(typeName);
                        if (tv.type == Value::Type::StructType && tv.structType)
                            found = tv.structType->findMethod(methodName) != nullptr;
                    } catch (...) {}
                }
                if (!found)
                    throw std::runtime_error("exception target not found: " + n->target
                        + " (struct type or method does not exist)");
            } else {
                // exception funcName — 检查全局函数或内置函数
                bool found = functions_.count(n->target) || builtins_.count(n->target);
                if (!found && env->has(n->target)) {
                    try {
                        Value tv = env->get(n->target);
                        if (tv.type == Value::Type::Function) found = true;
                    } catch (...) {}
                }
                if (!found)
                    throw std::runtime_error("exception target not found: " + n->target
                        + " (function does not exist)");
            }
            // 全局/方法描述：存入 exceptionDescs_
            auto& descs = exceptionDescs_[n->target];
            descs.insert(descs.end(), n->messages.begin(), n->messages.end());
        } else {
            // 内联 exception：存储当前描述（下次错误时合并输出）
            lastInlineExceptionDescs_ = n->messages;
        }
        return Value::Nil();
    }

    throw std::runtime_error("unknown AST node");
}

// ── 创建结构体实例 ─────────────────────────────────────────
Value Interpreter::createStructInst(std::shared_ptr<StructTypeInfo> type,
                                     const std::vector<std::pair<std::string,Value>>& initFields,
                                     std::shared_ptr<Environment> env, ModuleRuntime* mod) {
    auto inst = std::make_shared<StructInstInfo>();
    inst->type = type;

    // Initialize fields with defaults, then override with provided values
    for (auto& f : type->fields) {
        if (f.defaultExpr)
            inst->fields[f.name] = evalNode(f.defaultExpr.get(), env, mod);
        else
            inst->fields[f.name] = Value::Nil();
    }
    for (auto& [name, val] : initFields) {
        inst->fields[name] = val;  // override with provided value
    }
    return Value::StructInstV(std::move(inst));
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
