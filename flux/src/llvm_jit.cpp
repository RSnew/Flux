// llvm_jit.cpp — Flux LLVM JIT Compiler implementation
// HIR -> LLVM IR -> native code via ORC JIT
// All LLVM headers are confined to this file (PIMPL pattern).
#include "llvm_jit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>

// =====================================================================
// LLVM-enabled implementation
// =====================================================================
#ifdef FLUX_HAS_LLVM

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Target/TargetMachine.h>

// =====================================================================
// Runtime bridge: global interpreter pointer
// =====================================================================
static Interpreter* g_llvm_interp = nullptr;

// Thread-local string buffer for returning C strings from runtime helpers
static thread_local std::vector<std::string> g_string_pool;

static const char* intern_string(std::string s) {
    g_string_pool.push_back(std::move(s));
    return g_string_pool.back().c_str();
}

// =====================================================================
// Runtime bridge functions (extern "C")
// =====================================================================
extern "C" {

const char* flux_string_concat(const char* a, const char* b) {
    return intern_string(std::string(a ? a : "") + std::string(b ? b : ""));
}

const char* flux_string_from_double(double v) {
    if (v == (long long)v) return intern_string(std::to_string((long long)v));
    return intern_string(std::to_string(v));
}

void flux_print(const char* s) {
    std::printf("%s\n", s ? s : "null");
}

void flux_print_double(double v) {
    if (v == (long long)v) std::printf("%lld\n", (long long)v);
    else std::printf("%g\n", v);
}

void flux_print_bool(int v) {
    std::printf("%s\n", v ? "true" : "false");
}

void* flux_array_new() {
    return new std::vector<double>();
}
void flux_array_push(void* arr, double val) {
    if (arr) static_cast<std::vector<double>*>(arr)->push_back(val);
}
double flux_array_get(void* arr, int64_t idx) {
    if (!arr) return 0.0;
    auto* v = static_cast<std::vector<double>*>(arr);
    if (idx < 0 || idx >= (int64_t)v->size()) return 0.0;
    return (*v)[(size_t)idx];
}
void flux_array_set(void* arr, int64_t idx, double val) {
    if (!arr) return;
    auto* v = static_cast<std::vector<double>*>(arr);
    if (idx >= 0 && idx < (int64_t)v->size()) (*v)[(size_t)idx] = val;
}
int64_t flux_array_len(void* arr) {
    if (!arr) return 0;
    return (int64_t)static_cast<std::vector<double>*>(arr)->size();
}

void* flux_map_new() {
    return new std::unordered_map<std::string, double>();
}
void flux_map_set(void* m, const char* key, double val) {
    if (!m || !key) return;
    (*static_cast<std::unordered_map<std::string, double>*>(m))[key] = val;
}
double flux_map_get(void* m, const char* key) {
    if (!m || !key) return 0.0;
    auto* map = static_cast<std::unordered_map<std::string, double>*>(m);
    auto it = map->find(key);
    return it != map->end() ? it->second : 0.0;
}

double flux_call_function(const char* name, double* args, int nargs) {
    if (!g_llvm_interp || !name) return 0.0;
    try {
        std::vector<Value> vargs;
        for (int i = 0; i < nargs; i++) vargs.push_back(Value::Num(args[i]));
        Value r = g_llvm_interp->callFunction(name, std::move(vargs));
        return (r.type == Value::Type::Number) ? r.number : 0.0;
    } catch (...) { return 0.0; }
}

// 支持混合参数类型的桥接函数
// strArgs[i] 非 null 时使用字符串参数，否则使用 numArgs[i]
double flux_call_function_mixed(const char* name, double* numArgs,
                                 const char** strArgs, int nargs) {
    if (!g_llvm_interp || !name) return 0.0;
    try {
        std::vector<Value> vargs;
        for (int i = 0; i < nargs; i++) {
            if (strArgs && strArgs[i])
                vargs.push_back(Value::Str(strArgs[i]));
            else
                vargs.push_back(Value::Num(numArgs ? numArgs[i] : 0.0));
        }
        Value r = g_llvm_interp->callFunction(name, std::move(vargs));
        return (r.type == Value::Type::Number) ? r.number : 0.0;
    } catch (...) { return 0.0; }
}

double flux_call_module(const char* mod, const char* fn, double* args, int nargs) {
    if (!g_llvm_interp || !mod || !fn) return 0.0;
    try {
        std::vector<Value> vargs;
        for (int i = 0; i < nargs; i++) vargs.push_back(Value::Num(args[i]));
        Value r = g_llvm_interp->callModuleFunction(mod, fn, std::move(vargs));
        return (r.type == Value::Type::Number) ? r.number : 0.0;
    } catch (...) { return 0.0; }
}

// ── Persistent state bridge ─────────────────────────────
double flux_load_state(const char* field) {
    if (!g_llvm_interp || !field) return 0.0;
    auto it = g_llvm_interp->persistentStore_.find(field);
    if (it == g_llvm_interp->persistentStore_.end()) return 0.0;
    return (it->second.type == Value::Type::Number) ? it->second.number : 0.0;
}

void flux_store_state(const char* field, double val) {
    if (!g_llvm_interp || !field) return;
    g_llvm_interp->persistentStore_[field] = Value::Num(val);
}

int flux_state_has(const char* field) {
    if (!g_llvm_interp || !field) return 0;
    return g_llvm_interp->persistentStore_.count(field) ? 1 : 0;
}

void flux_state_init(const char* field, double defaultVal) {
    if (!g_llvm_interp || !field) return;
    if (!g_llvm_interp->persistentStore_.count(field))
        g_llvm_interp->persistentStore_[field] = Value::Num(defaultVal);
}

}  // extern "C"

// =====================================================================
// PIMPL implementation
// =====================================================================

struct LLVMJITCompiler::Impl {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;
    std::unique_ptr<llvm::orc::LLJIT> jit;

    // Resource tracker for hot-reload module replacement
    llvm::orc::ResourceTrackerSP resourceTracker;
    bool jitInitialized = false;

    Interpreter* interp = nullptr;

    // Compiled function map
    std::unordered_map<std::string, llvm::Function*> functions;

    // Variable storage during compilation
    struct VarInfo {
        llvm::AllocaInst* alloca;
        HIRType type;
    };
    std::vector<std::unordered_map<std::string, VarInfo>> varScopes;

    LLVMJITCompiler::Stats stats;

    Impl() {
        static bool initialized = false;
        if (!initialized) {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            llvm::InitializeNativeTargetAsmParser();
            initialized = true;
        }
        ctx = std::make_unique<llvm::LLVMContext>();
        module = std::make_unique<llvm::Module>("flux_module", *ctx);
        builder = std::make_unique<llvm::IRBuilder<>>(*ctx);
    }

    // ── Type helpers ──────────────────────────────────────
    llvm::Type* doubleTy() { return llvm::Type::getDoubleTy(*ctx); }
    llvm::Type* i64Ty()    { return llvm::Type::getInt64Ty(*ctx); }
    llvm::Type* i32Ty()    { return llvm::Type::getInt32Ty(*ctx); }
    llvm::Type* i1Ty()     { return llvm::Type::getInt1Ty(*ctx); }
    llvm::Type* voidTy()   { return llvm::Type::getVoidTy(*ctx); }
    llvm::Type* i8PtrTy()  { return llvm::PointerType::getUnqual(*ctx); }
    llvm::Type* dblPtrTy() { return llvm::PointerType::getUnqual(*ctx); }

    llvm::Type* mapType(const HIRType& t) {
        switch (t.kind) {
        case HIRType::Int: case HIRType::UInt: case HIRType::Natural:
        case HIRType::Byte: case HIRType::Addr: return i64Ty();
        case HIRType::Float:   return doubleTy();
        case HIRType::Bool:    return i1Ty();
        case HIRType::String:  return i8PtrTy();
        default:               return doubleTy();
        }
    }

    // ── Scope management ──────────────────────────────────
    void pushScope() { varScopes.push_back({}); }
    void popScope()  { if (!varScopes.empty()) varScopes.pop_back(); }

    llvm::AllocaInst* createEntryAlloca(llvm::Function* fn,
                                         const std::string& name, llvm::Type* ty) {
        llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        return tmp.CreateAlloca(ty, nullptr, name);
    }

    void declareVar(const std::string& name, llvm::AllocaInst* a, const HIRType& t) {
        if (varScopes.empty()) varScopes.push_back({});
        varScopes.back()[name] = {a, t};
    }

    VarInfo* lookupVar(const std::string& name) {
        for (int i = (int)varScopes.size() - 1; i >= 0; i--) {
            auto it = varScopes[i].find(name);
            if (it != varScopes[i].end()) return &it->second;
        }
        return nullptr;
    }

    // ── Runtime helper declarations ───────────────────────
    void declareRuntimeHelpers() {
        auto* d = doubleTy(); auto* v = voidTy();
        auto* s = i8PtrTy(); auto* i = i64Ty(); auto* i32 = i32Ty();

        module->getOrInsertFunction("flux_string_concat",
            llvm::FunctionType::get(s, {s, s}, false));
        module->getOrInsertFunction("flux_string_from_double",
            llvm::FunctionType::get(s, {d}, false));
        module->getOrInsertFunction("flux_print",
            llvm::FunctionType::get(v, {s}, false));
        module->getOrInsertFunction("flux_print_double",
            llvm::FunctionType::get(v, {d}, false));
        module->getOrInsertFunction("flux_print_bool",
            llvm::FunctionType::get(v, {i32}, false));

        module->getOrInsertFunction("flux_array_new",
            llvm::FunctionType::get(s, {}, false));
        module->getOrInsertFunction("flux_array_push",
            llvm::FunctionType::get(v, {s, d}, false));
        module->getOrInsertFunction("flux_array_get",
            llvm::FunctionType::get(d, {s, i}, false));
        module->getOrInsertFunction("flux_array_set",
            llvm::FunctionType::get(v, {s, i, d}, false));
        module->getOrInsertFunction("flux_array_len",
            llvm::FunctionType::get(i, {s}, false));

        module->getOrInsertFunction("flux_map_new",
            llvm::FunctionType::get(s, {}, false));
        module->getOrInsertFunction("flux_map_set",
            llvm::FunctionType::get(v, {s, s, d}, false));
        module->getOrInsertFunction("flux_map_get",
            llvm::FunctionType::get(d, {s, s}, false));

        module->getOrInsertFunction("flux_call_function",
            llvm::FunctionType::get(d, {s, dblPtrTy(), i32}, false));
        module->getOrInsertFunction("flux_call_function_mixed",
            llvm::FunctionType::get(d, {s, dblPtrTy(), llvm::PointerType::getUnqual(*ctx), i32}, false));
        module->getOrInsertFunction("flux_call_module",
            llvm::FunctionType::get(d, {s, s, dblPtrTy(), i32}, false));

        // Persistent state bridge
        module->getOrInsertFunction("flux_load_state",
            llvm::FunctionType::get(d, {s}, false));
        module->getOrInsertFunction("flux_store_state",
            llvm::FunctionType::get(v, {s, d}, false));
        module->getOrInsertFunction("flux_state_has",
            llvm::FunctionType::get(i32, {s}, false));
        module->getOrInsertFunction("flux_state_init",
            llvm::FunctionType::get(v, {s, d}, false));
    }

    // ── Expression compilation ────────────────────────────
    llvm::Value* compileExpr(HIRNode* node) {
        if (!node) return llvm::ConstantFP::get(doubleTy(), 0.0);

        if (auto* n = dynamic_cast<HIRLiteral*>(node))      return compileLiteral(n);
        if (auto* n = dynamic_cast<HIRVarRef*>(node))        return compileVarRef(n);
        if (auto* n = dynamic_cast<HIRBinary*>(node))        return compileBinary(n);
        if (auto* n = dynamic_cast<HIRUnary*>(node))         return compileUnary(n);
        if (auto* n = dynamic_cast<HIRCall*>(node))          return compileCall(n);
        if (auto* n = dynamic_cast<HIRIndex*>(node))         return compileIndex(n);
        if (auto* n = dynamic_cast<HIRFieldAccess*>(node))   return compileFieldAccess(n);
        if (auto* n = dynamic_cast<HIRStateAccess*>(node))   return compileStateAccess(n);
        if (auto* n = dynamic_cast<HIRArrayLit*>(node))      return compileArrayLit(n);
        if (auto* n = dynamic_cast<HIRMapLit*>(node))        return compileMapLit(n);
        if (auto* n = dynamic_cast<HIRLambda*>(node))        return compileLambda(n);

        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    llvm::Value* compileLiteral(HIRLiteral* lit) {
        switch (lit->litKind) {
        case HIRLiteral::Nil:    return llvm::ConstantFP::get(doubleTy(), 0.0);
        case HIRLiteral::Bool:   return llvm::ConstantFP::get(doubleTy(), lit->boolVal ? 1.0 : 0.0);
        case HIRLiteral::Int:
        case HIRLiteral::Float:  return llvm::ConstantFP::get(doubleTy(), lit->numVal);
        case HIRLiteral::String: return builder->CreateGlobalString(lit->strVal, "str");
        }
        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    llvm::Value* compileVarRef(HIRVarRef* ref) {
        if (auto* vi = lookupVar(ref->name))
            return builder->CreateLoad(vi->alloca->getAllocatedType(), vi->alloca, ref->name);
        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    llvm::Value* compileBinary(HIRBinary* bin) {
        auto* lhs = compileExpr(bin->left.get());
        auto* rhs = compileExpr(bin->right.get());
        if (!lhs || !rhs) return llvm::ConstantFP::get(doubleTy(), 0.0);

        // String concatenation
        if (bin->op == "+") {
            if (lhs->getType() == i8PtrTy() && rhs->getType() == i8PtrTy()) {
                auto* fn = module->getFunction("flux_string_concat");
                return fn ? builder->CreateCall(fn, {lhs, rhs}, "strcat") : lhs;
            }
            if (lhs->getType() == i8PtrTy() && rhs->getType() == doubleTy()) {
                auto* ts = module->getFunction("flux_string_from_double");
                auto* cc = module->getFunction("flux_string_concat");
                if (ts && cc) return builder->CreateCall(cc, {lhs, builder->CreateCall(ts, {rhs}, "rs")}, "sc");
            }
            if (lhs->getType() == doubleTy() && rhs->getType() == i8PtrTy()) {
                auto* ts = module->getFunction("flux_string_from_double");
                auto* cc = module->getFunction("flux_string_concat");
                if (ts && cc) return builder->CreateCall(cc, {builder->CreateCall(ts, {lhs}, "ls"), rhs}, "sc");
            }
        }

        // Ensure both double for numeric ops
        if (lhs->getType() != doubleTy() || rhs->getType() != doubleTy())
            return llvm::ConstantFP::get(doubleTy(), 0.0);

        if (bin->op == "+")  return builder->CreateFAdd(lhs, rhs, "add");
        if (bin->op == "-")  return builder->CreateFSub(lhs, rhs, "sub");
        if (bin->op == "*")  return builder->CreateFMul(lhs, rhs, "mul");
        if (bin->op == "/")  return builder->CreateFDiv(lhs, rhs, "div");
        if (bin->op == "%")  return builder->CreateFRem(lhs, rhs, "mod");

        // Comparisons -> double (1.0 / 0.0)
        llvm::Value* cmp = nullptr;
        if      (bin->op == "==") cmp = builder->CreateFCmpOEQ(lhs, rhs, "eq");
        else if (bin->op == "!=") cmp = builder->CreateFCmpONE(lhs, rhs, "ne");
        else if (bin->op == "<")  cmp = builder->CreateFCmpOLT(lhs, rhs, "lt");
        else if (bin->op == "<=") cmp = builder->CreateFCmpOLE(lhs, rhs, "le");
        else if (bin->op == ">")  cmp = builder->CreateFCmpOGT(lhs, rhs, "gt");
        else if (bin->op == ">=") cmp = builder->CreateFCmpOGE(lhs, rhs, "ge");
        if (cmp) return builder->CreateUIToFP(cmp, doubleTy(), "cmpd");

        auto zero = llvm::ConstantFP::get(doubleTy(), 0.0);
        if (bin->op == "&&") {
            auto* lb = builder->CreateFCmpONE(lhs, zero, "lb");
            auto* rb = builder->CreateFCmpONE(rhs, zero, "rb");
            return builder->CreateUIToFP(builder->CreateAnd(lb, rb, "and"), doubleTy(), "andd");
        }
        if (bin->op == "||") {
            auto* lb = builder->CreateFCmpONE(lhs, zero, "lb");
            auto* rb = builder->CreateFCmpONE(rhs, zero, "rb");
            return builder->CreateUIToFP(builder->CreateOr(lb, rb, "or"), doubleTy(), "ord");
        }

        return zero;
    }

    llvm::Value* compileUnary(HIRUnary* un) {
        auto* op = compileExpr(un->operand.get());
        if (!op) return llvm::ConstantFP::get(doubleTy(), 0.0);
        if (un->op == "-" && op->getType() == doubleTy())
            return builder->CreateFNeg(op, "neg");
        if (un->op == "!" && op->getType() == doubleTy()) {
            auto* z = builder->CreateFCmpOEQ(op, llvm::ConstantFP::get(doubleTy(), 0.0), "z");
            return builder->CreateUIToFP(z, doubleTy(), "not");
        }
        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    llvm::Value* compileCall(HIRCall* call) {
        std::string name;
        if (auto* ref = dynamic_cast<HIRVarRef*>(call->callee.get()))
            name = ref->name;

        // Built-in str() → convert double to string
        if (name == "str" && call->args.size() == 1) {
            auto* arg = compileExpr(call->args[0].get());
            if (arg && arg->getType() == doubleTy()) {
                return builder->CreateCall(
                    module->getFunction("flux_string_from_double"), {arg}, "str");
            }
            return arg ? arg : llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(i8PtrTy()));
        }

        // Built-in print
        if (name == "print" || name == "println") {
            if (!call->args.empty()) {
                auto* arg = compileExpr(call->args[0].get());
                if (arg) {
                    if (arg->getType() == i8PtrTy())
                        builder->CreateCall(module->getFunction("flux_print"), {arg});
                    else if (arg->getType() == doubleTy())
                        builder->CreateCall(module->getFunction("flux_print_double"), {arg});
                }
            }
            return llvm::ConstantFP::get(doubleTy(), 0.0);
        }

        // LLVM-compiled function
        auto it = functions.find(name);
        if (it != functions.end()) {
            auto* fn = it->second;
            auto* fnTy = fn->getFunctionType();
            std::vector<llvm::Value*> args;
            for (size_t i = 0; i < call->args.size(); i++) {
                auto* v = compileExpr(call->args[i].get());
                if (!v) v = llvm::ConstantFP::get(doubleTy(), 0.0);
                // 根据目标函数的参数类型进行匹配转换
                if (i < fnTy->getNumParams()) {
                    auto* expectedTy = fnTy->getParamType(i);
                    if (v->getType() != expectedTy) {
                        // 类型不匹配时尝试安全转换
                        if (expectedTy->isDoubleTy() && v->getType()->isIntegerTy())
                            v = builder->CreateSIToFP(v, doubleTy());
                        else if (expectedTy->isDoubleTy())
                            v = llvm::ConstantFP::get(doubleTy(), 0.0);
                    }
                }
                args.push_back(v);
            }
            while (args.size() < fn->arg_size())
                args.push_back(llvm::ConstantFP::get(doubleTy(), 0.0));
            while (args.size() > fn->arg_size()) args.pop_back();
            return builder->CreateCall(fn, args, name + "_ret");
        }

        // Bridge to interpreter — 编译参数并检测类型
        if (!name.empty()) {
            int nargs = (int)call->args.size();
            auto* nameStr = builder->CreateGlobalString(name, "fn");

            // 先编译所有参数
            std::vector<llvm::Value*> compiledArgs;
            bool hasStrArgs = false;
            for (int i = 0; i < nargs; i++) {
                auto* v = compileExpr(call->args[i].get());
                if (v && v->getType() == i8PtrTy()) hasStrArgs = true;
                compiledArgs.push_back(v);
            }

            if (hasStrArgs && nargs > 0) {
                // 使用 mixed 桥接：同时传递 double 和 string 参数
                auto* mixedFn = module->getFunction("flux_call_function_mixed");
                auto* numArrTy = llvm::ArrayType::get(doubleTy(), nargs);
                auto* numArr = builder->CreateAlloca(numArrTy, nullptr, "numargs");
                auto* strArrTy = llvm::ArrayType::get(i8PtrTy(), nargs);
                auto* strArr = builder->CreateAlloca(strArrTy, nullptr, "strargs");

                for (int i = 0; i < nargs; i++) {
                    auto* v = compiledArgs[i];
                    if (v && v->getType() == i8PtrTy()) {
                        builder->CreateStore(v, builder->CreateConstGEP2_32(strArrTy, strArr, 0, i));
                        builder->CreateStore(llvm::ConstantFP::get(doubleTy(), 0.0),
                                             builder->CreateConstGEP2_32(numArrTy, numArr, 0, i));
                    } else {
                        if (!v || v->getType() != doubleTy())
                            v = llvm::ConstantFP::get(doubleTy(), 0.0);
                        builder->CreateStore(v, builder->CreateConstGEP2_32(numArrTy, numArr, 0, i));
                        builder->CreateStore(
                            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(i8PtrTy())),
                            builder->CreateConstGEP2_32(strArrTy, strArr, 0, i));
                    }
                }
                return builder->CreateCall(mixedFn, {
                    nameStr,
                    builder->CreateConstGEP2_32(numArrTy, numArr, 0, 0),
                    builder->CreateConstGEP2_32(strArrTy, strArr, 0, 0),
                    llvm::ConstantInt::get(i32Ty(), nargs)
                }, "call_ret");
            }

            // 纯数值桥接
            auto* bridgeFn = module->getFunction("flux_call_function");
            if (bridgeFn) {
                if (nargs > 0) {
                    auto* arrTy = llvm::ArrayType::get(doubleTy(), nargs);
                    auto* arr = builder->CreateAlloca(arrTy, nullptr, "args");
                    for (int i = 0; i < nargs; i++) {
                        auto* v = compiledArgs[i];
                        if (!v || v->getType() != doubleTy())
                            v = llvm::ConstantFP::get(doubleTy(), 0.0);
                        builder->CreateStore(v, builder->CreateConstGEP2_32(arrTy, arr, 0, i));
                    }
                    return builder->CreateCall(bridgeFn, {
                        nameStr,
                        builder->CreateConstGEP2_32(arrTy, arr, 0, 0),
                        llvm::ConstantInt::get(i32Ty(), nargs)
                    }, "call_ret");
                } else {
                    return builder->CreateCall(bridgeFn, {
                        nameStr,
                        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(dblPtrTy())),
                        llvm::ConstantInt::get(i32Ty(), 0)
                    }, "call_ret");
                }
            }
        }
        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    llvm::Value* compileIndex(HIRIndex* idx) {
        auto* obj = compileExpr(idx->object.get());
        auto* index = compileExpr(idx->index.get());
        if (obj && obj->getType() == i8PtrTy() && index) {
            auto* fn = module->getFunction("flux_array_get");
            if (fn) {
                auto* i = builder->CreateFPToSI(index, i64Ty(), "idx");
                return builder->CreateCall(fn, {obj, i}, "aget");
            }
        }
        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    llvm::Value* compileFieldAccess(HIRFieldAccess*) {
        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    llvm::Value* compileStateAccess(HIRStateAccess* sa) {
        auto* fn = module->getFunction("flux_load_state");
        if (!fn) return llvm::ConstantFP::get(doubleTy(), 0.0);
        auto* nameStr = builder->CreateGlobalString(sa->field, "state_field");
        return builder->CreateCall(fn, {nameStr}, "state_val");
    }

    llvm::Value* compileArrayLit(HIRArrayLit* arr) {
        auto* newFn = module->getFunction("flux_array_new");
        auto* pushFn = module->getFunction("flux_array_push");
        if (!newFn || !pushFn)
            return llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(i8PtrTy()));
        auto* ptr = builder->CreateCall(newFn, {}, "arr");
        for (auto& e : arr->elements) {
            auto* v = compileExpr(e.get());
            if (v && v->getType() == doubleTy())
                builder->CreateCall(pushFn, {ptr, v});
        }
        return ptr;
    }

    llvm::Value* compileMapLit(HIRMapLit* m) {
        auto* newFn = module->getFunction("flux_map_new");
        auto* setFn = module->getFunction("flux_map_set");
        if (!newFn || !setFn)
            return llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(i8PtrTy()));
        auto* ptr = builder->CreateCall(newFn, {}, "map");
        for (auto& [k, v] : m->entries) {
            auto* kv = builder->CreateGlobalString(k, "mk");
            auto* vv = compileExpr(v.get());
            if (vv && vv->getType() == doubleTy())
                builder->CreateCall(setFn, {ptr, kv, vv});
        }
        return ptr;
    }

    llvm::Value* compileLambda(HIRLambda* lam) {
        static int count = 0;
        std::string lname = "__lambda_" + std::to_string(count++);

        std::vector<llvm::Type*> pts(lam->params.size(), doubleTy());
        auto* ft = llvm::FunctionType::get(doubleTy(), pts, false);
        auto* fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, lname, module.get());

        auto savedIP = builder->GetInsertBlock();
        auto savedScopes = varScopes;

        auto* entry = llvm::BasicBlock::Create(*ctx, "entry", fn);
        builder->SetInsertPoint(entry);
        pushScope();

        int pi = 0;
        for (auto& arg : fn->args()) {
            if (pi < (int)lam->params.size()) {
                arg.setName(lam->params[pi].name);
                auto* a = createEntryAlloca(fn, lam->params[pi].name, doubleTy());
                builder->CreateStore(&arg, a);
                declareVar(lam->params[pi].name, a, lam->params[pi].type);
            }
            pi++;
        }

        for (auto& s : lam->body) compileStmt(s.get());
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateRet(llvm::ConstantFP::get(doubleTy(), 0.0));

        popScope();

        if (llvm::verifyFunction(*fn, &llvm::errs())) {
            fn->eraseFromParent();
        } else {
            functions[lname] = fn;
        }

        varScopes = savedScopes;
        if (savedIP) builder->SetInsertPoint(savedIP);
        return llvm::ConstantFP::get(doubleTy(), 0.0);
    }

    // ── Statement compilation ─────────────────────────────
    void compileStmt(HIRNode* node) {
        if (!node) return;
        if (auto* n = dynamic_cast<HIRVarDecl*>(node))   { compileVarDecl(n); return; }
        if (auto* n = dynamic_cast<HIRAssign*>(node))     { compileAssign(n); return; }
        if (auto* n = dynamic_cast<HIRReturn*>(node))     { compileReturn(n); return; }
        if (auto* n = dynamic_cast<HIRIf*>(node))         { compileIf(n); return; }
        if (auto* n = dynamic_cast<HIRWhile*>(node))      { compileWhile(n); return; }
        if (auto* n = dynamic_cast<HIRForIn*>(node))      { compileForIn(n); return; }
        if (auto* n = dynamic_cast<HIRBlock*>(node))      { compileBlock(n); return; }
        if (auto* n = dynamic_cast<HIRFnDecl*>(node))     { compileFnDecl(n); return; }
        if (auto* n = dynamic_cast<HIRPersistentBlock*>(node)) { compilePersistentBlock(n); return; }
        if (auto* n = dynamic_cast<HIRStateAssign*>(node))     { compileStateAssign(n); return; }
        if (dynamic_cast<HIRModuleDecl*>(node))            return;
        if (dynamic_cast<HIRSpawn*>(node))                 return;
        compileExpr(node);
    }

    void compileVarDecl(HIRVarDecl* d) {
        auto* fn = builder->GetInsertBlock()->getParent();
        llvm::Type* ty = doubleTy();

        if (d->init) {
            if (auto* lit = dynamic_cast<HIRLiteral*>(d->init.get()))
                if (lit->litKind == HIRLiteral::String) ty = i8PtrTy();
            if (dynamic_cast<HIRArrayLit*>(d->init.get())) ty = i8PtrTy();
            if (dynamic_cast<HIRMapLit*>(d->init.get())) ty = i8PtrTy();
        }

        auto* a = createEntryAlloca(fn, d->name, ty);
        if (d->init) {
            auto* v = compileExpr(d->init.get());
            if (v && v->getType() == ty) {
                builder->CreateStore(v, a);
            } else if (ty == doubleTy()) {
                builder->CreateStore(llvm::ConstantFP::get(doubleTy(), 0.0), a);
            } else {
                builder->CreateStore(
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty)), a);
            }
        } else {
            if (ty == doubleTy())
                builder->CreateStore(llvm::ConstantFP::get(doubleTy(), 0.0), a);
            else
                builder->CreateStore(
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty)), a);
        }
        declareVar(d->name, a, d->declaredType);
    }

    void compileAssign(HIRAssign* a) {
        auto* vi = lookupVar(a->name);
        if (!vi) return;
        auto* v = compileExpr(a->value.get());
        if (v && v->getType() == vi->alloca->getAllocatedType())
            builder->CreateStore(v, vi->alloca);
    }

    void compileReturn(HIRReturn* r) {
        llvm::Value* v = r->value ? compileExpr(r->value.get()) : nullptr;
        if (v && v->getType() == doubleTy())
            builder->CreateRet(v);
        else
            builder->CreateRet(llvm::ConstantFP::get(doubleTy(), 0.0));
    }

    void compileIf(HIRIf* s) {
        auto* cond = compileExpr(s->condition.get());
        if (!cond) return;

        llvm::Value* condBool;
        if (cond->getType() == doubleTy())
            condBool = builder->CreateFCmpONE(cond, llvm::ConstantFP::get(doubleTy(), 0.0), "if");
        else if (cond->getType() == i1Ty())
            condBool = cond;
        else
            condBool = builder->CreateICmpNE(cond, llvm::Constant::getNullValue(cond->getType()), "if");

        auto* fn = builder->GetInsertBlock()->getParent();
        auto* thenBB  = llvm::BasicBlock::Create(*ctx, "if.then", fn);
        auto* elseBB  = llvm::BasicBlock::Create(*ctx, "if.else");
        auto* mergeBB = llvm::BasicBlock::Create(*ctx, "if.merge");

        builder->CreateCondBr(condBool, thenBB, elseBB);

        builder->SetInsertPoint(thenBB);
        pushScope();
        for (auto& st : s->thenBranch) compileStmt(st.get());
        popScope();
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);

        fn->insert(fn->end(), elseBB);
        builder->SetInsertPoint(elseBB);
        pushScope();
        for (auto& st : s->elseBranch) compileStmt(st.get());
        popScope();
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(mergeBB);

        fn->insert(fn->end(), mergeBB);
        builder->SetInsertPoint(mergeBB);
    }

    void compileWhile(HIRWhile* s) {
        auto* fn = builder->GetInsertBlock()->getParent();
        auto* condBB = llvm::BasicBlock::Create(*ctx, "while.cond", fn);
        auto* bodyBB = llvm::BasicBlock::Create(*ctx, "while.body");
        auto* exitBB = llvm::BasicBlock::Create(*ctx, "while.exit");

        builder->CreateBr(condBB);
        builder->SetInsertPoint(condBB);

        auto* cv = compileExpr(s->condition.get());
        llvm::Value* cb = cv ? builder->CreateFCmpONE(cv, llvm::ConstantFP::get(doubleTy(), 0.0), "wc")
                              : llvm::ConstantInt::getFalse(*ctx);
        builder->CreateCondBr(cb, bodyBB, exitBB);

        fn->insert(fn->end(), bodyBB);
        builder->SetInsertPoint(bodyBB);
        pushScope();
        for (auto& st : s->body) compileStmt(st.get());
        popScope();
        if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(condBB);

        fn->insert(fn->end(), exitBB);
        builder->SetInsertPoint(exitBB);
    }

    void compileForIn(HIRForIn* s) {
        auto* fn = builder->GetInsertBlock()->getParent();

        // Detect range(N) calls → compile as numeric 0..N-1 loop directly
        if (auto* call = dynamic_cast<HIRCall*>(s->iterable.get())) {
            std::string calleeName;
            if (auto* ref = dynamic_cast<HIRVarRef*>(call->callee.get()))
                calleeName = ref->name;
            if (calleeName == "range" && call->args.size() == 1) {
                auto* limit = compileExpr(call->args[0].get());
                if (limit && limit->getType() == doubleTy()) {
                    auto* iVar = createEntryAlloca(fn, s->varName, doubleTy());
                    builder->CreateStore(llvm::ConstantFP::get(doubleTy(), 0.0), iVar);

                    auto* condBB = llvm::BasicBlock::Create(*ctx, "range.cond", fn);
                    auto* bodyBB = llvm::BasicBlock::Create(*ctx, "range.body");
                    auto* exitBB = llvm::BasicBlock::Create(*ctx, "range.exit");

                    builder->CreateBr(condBB);
                    builder->SetInsertPoint(condBB);
                    auto* cur = builder->CreateLoad(doubleTy(), iVar, "i");
                    builder->CreateCondBr(
                        builder->CreateFCmpOLT(cur, limit, "lt"), bodyBB, exitBB);

                    fn->insert(fn->end(), bodyBB);
                    builder->SetInsertPoint(bodyBB);
                    pushScope();
                    declareVar(s->varName, iVar, HIRType::make(HIRType::Float));
                    for (auto& st : s->body) compileStmt(st.get());
                    popScope();

                    auto* next = builder->CreateFAdd(
                        builder->CreateLoad(doubleTy(), iVar, "i"),
                        llvm::ConstantFP::get(doubleTy(), 1.0), "inc");
                    builder->CreateStore(next, iVar);
                    if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(condBB);

                    fn->insert(fn->end(), exitBB);
                    builder->SetInsertPoint(exitBB);
                    return;
                }
            }
        }

        auto* iter = compileExpr(s->iterable.get());

        // Numeric range loop: for x in N -> 0..N-1
        if (iter && iter->getType() == doubleTy()) {
            auto* iVar = createEntryAlloca(fn, s->varName, doubleTy());
            builder->CreateStore(llvm::ConstantFP::get(doubleTy(), 0.0), iVar);

            auto* condBB = llvm::BasicBlock::Create(*ctx, "for.cond", fn);
            auto* bodyBB = llvm::BasicBlock::Create(*ctx, "for.body");
            auto* exitBB = llvm::BasicBlock::Create(*ctx, "for.exit");

            builder->CreateBr(condBB);
            builder->SetInsertPoint(condBB);
            auto* cur = builder->CreateLoad(doubleTy(), iVar, "i");
            builder->CreateCondBr(
                builder->CreateFCmpOLT(cur, iter, "lt"), bodyBB, exitBB);

            fn->insert(fn->end(), bodyBB);
            builder->SetInsertPoint(bodyBB);
            pushScope();
            declareVar(s->varName, iVar, HIRType::make(HIRType::Float));
            for (auto& st : s->body) compileStmt(st.get());
            popScope();

            auto* next = builder->CreateFAdd(
                builder->CreateLoad(doubleTy(), iVar, "i"),
                llvm::ConstantFP::get(doubleTy(), 1.0), "inc");
            builder->CreateStore(next, iVar);
            if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(condBB);

            fn->insert(fn->end(), exitBB);
            builder->SetInsertPoint(exitBB);
            return;
        }

        // Array iteration
        if (iter && iter->getType() == i8PtrTy()) {
            auto* lenFn = module->getFunction("flux_array_len");
            auto* getFn = module->getFunction("flux_array_get");
            if (!lenFn || !getFn) return;

            auto* len = builder->CreateCall(lenFn, {iter}, "len");
            auto* idxVar = createEntryAlloca(fn, "__idx", i64Ty());
            builder->CreateStore(llvm::ConstantInt::get(i64Ty(), 0), idxVar);
            auto* elemVar = createEntryAlloca(fn, s->varName, doubleTy());

            auto* condBB = llvm::BasicBlock::Create(*ctx, "for.cond", fn);
            auto* bodyBB = llvm::BasicBlock::Create(*ctx, "for.body");
            auto* exitBB = llvm::BasicBlock::Create(*ctx, "for.exit");

            builder->CreateBr(condBB);
            builder->SetInsertPoint(condBB);
            auto* idx = builder->CreateLoad(i64Ty(), idxVar, "idx");
            builder->CreateCondBr(builder->CreateICmpSLT(idx, len, "lt"), bodyBB, exitBB);

            fn->insert(fn->end(), bodyBB);
            builder->SetInsertPoint(bodyBB);
            pushScope();
            auto* elem = builder->CreateCall(getFn, {iter, idx}, "elem");
            builder->CreateStore(elem, elemVar);
            declareVar(s->varName, elemVar, HIRType::make(HIRType::Float));
            for (auto& st : s->body) compileStmt(st.get());
            popScope();

            auto* nextIdx = builder->CreateAdd(
                builder->CreateLoad(i64Ty(), idxVar, "idx"),
                llvm::ConstantInt::get(i64Ty(), 1), "ni");
            builder->CreateStore(nextIdx, idxVar);
            if (!builder->GetInsertBlock()->getTerminator()) builder->CreateBr(condBB);

            fn->insert(fn->end(), exitBB);
            builder->SetInsertPoint(exitBB);
        }
    }

    void compilePersistentBlock(HIRPersistentBlock* pb) {
        auto* initFn = module->getFunction("flux_state_init");
        if (!initFn) return;
        for (auto& f : pb->fields) {
            double defaultVal = 0.0;
            if (f.defaultValue) {
                if (auto* lit = dynamic_cast<HIRLiteral*>(f.defaultValue.get())) {
                    if (lit->litKind == HIRLiteral::Float || lit->litKind == HIRLiteral::Int)
                        defaultVal = lit->numVal;
                    else if (lit->litKind == HIRLiteral::Bool)
                        defaultVal = lit->boolVal ? 1.0 : 0.0;
                }
            }
            auto* nameStr = builder->CreateGlobalString(f.name, "pfield");
            builder->CreateCall(initFn, {nameStr, llvm::ConstantFP::get(doubleTy(), defaultVal)});
        }
    }

    void compileStateAssign(HIRStateAssign* sa) {
        auto* fn = module->getFunction("flux_store_state");
        if (!fn) return;
        auto* val = compileExpr(sa->value.get());
        if (!val || val->getType() != doubleTy())
            val = llvm::ConstantFP::get(doubleTy(), 0.0);
        auto* nameStr = builder->CreateGlobalString(sa->field, "state_field");
        builder->CreateCall(fn, {nameStr, val});
    }

    void compileBlock(HIRBlock* b) {
        pushScope();
        for (auto& s : b->stmts) compileStmt(s.get());
        popScope();
    }

    // ── Function compilation ──────────────────────────────
    // 检查函数是否为纯数值函数（所有参数类型已知且非 Any/Unknown/String）
    bool isNumericFn(HIRFnDecl* fn) {
        for (auto& p : fn->params) {
            if (p.type.kind == HIRType::Any || p.type.kind == HIRType::Unknown ||
                p.type.kind == HIRType::String)
                return false;
        }
        return true;
    }

    llvm::Function* compileFnDecl(HIRFnDecl* fn) {
        // 含有 Any/Unknown/String 参数的函数回退到解释器桥接
        // 因为 LLVM JIT 只支持纯数值运算
        if (!isNumericFn(fn)) return nullptr;

        // 根据参数实际类型生成函数签名
        std::vector<llvm::Type*> pts;
        for (auto& p : fn->params) pts.push_back(mapType(p.type));
        auto* ft = llvm::FunctionType::get(doubleTy(), pts, false);
        auto* llvmFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                                fn->name, module.get());
        functions[fn->name] = llvmFn;

        auto savedIP = builder->GetInsertBlock();
        auto savedScopes = varScopes;

        auto* entry = llvm::BasicBlock::Create(*ctx, "entry", llvmFn);
        builder->SetInsertPoint(entry);
        pushScope();

        int pi = 0;
        for (auto& arg : llvmFn->args()) {
            if (pi < (int)fn->params.size()) {
                arg.setName(fn->params[pi].name);
                auto* paramTy = mapType(fn->params[pi].type);
                auto* a = createEntryAlloca(llvmFn, fn->params[pi].name, paramTy);
                builder->CreateStore(&arg, a);
                declareVar(fn->params[pi].name, a, fn->params[pi].type);
            }
            pi++;
        }

        for (auto& s : fn->body) {
            compileStmt(s.get());
            if (builder->GetInsertBlock()->getTerminator()) break;
        }

        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateRet(llvm::ConstantFP::get(doubleTy(), 0.0));

        popScope();

        if (llvm::verifyFunction(*llvmFn, &llvm::errs())) {
            std::cerr << "[LLVM JIT] Warning: verify failed for '" << fn->name << "'\n";
            llvmFn->eraseFromParent();
            functions.erase(fn->name);
            varScopes = savedScopes;
            if (savedIP) builder->SetInsertPoint(savedIP);
            return nullptr;
        }

        stats.functionsCompiled++;
        varScopes = savedScopes;
        if (savedIP) builder->SetInsertPoint(savedIP);
        return llvmFn;
    }

    // ── Optimization ──────────────────────────────────────
    void optimizeModule() {
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;

        llvm::PassBuilder PB;
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        auto MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        MPM.run(*module, MAM);
    }

    // ── JIT setup ─────────────────────────────────────────
    bool initJIT() {
        auto jitE = llvm::orc::LLJITBuilder().create();
        if (!jitE) {
            llvm::errs() << "[LLVM JIT] Create failed: " << llvm::toString(jitE.takeError()) << "\n";
            return false;
        }
        jit = std::move(*jitE);

        auto& ES = jit->getExecutionSession();
        auto& JD = jit->getMainJITDylib();

        llvm::orc::SymbolMap syms;
        auto add = [&](const char* n, void* p) {
            syms[ES.intern(n)] = {
                llvm::orc::ExecutorAddr::fromPtr(p),
                llvm::JITSymbolFlags::Exported
            };
        };

        add("flux_string_concat",     (void*)(const char*(*)(const char*,const char*))&flux_string_concat);
        add("flux_string_from_double", (void*)(const char*(*)(double))&flux_string_from_double);
        add("flux_print",             (void*)(void(*)(const char*))&flux_print);
        add("flux_print_double",      (void*)(void(*)(double))&flux_print_double);
        add("flux_print_bool",        (void*)(void(*)(int))&flux_print_bool);
        add("flux_array_new",         (void*)(void*(*)())&flux_array_new);
        add("flux_array_push",        (void*)(void(*)(void*,double))&flux_array_push);
        add("flux_array_get",         (void*)(double(*)(void*,int64_t))&flux_array_get);
        add("flux_array_set",         (void*)(void(*)(void*,int64_t,double))&flux_array_set);
        add("flux_array_len",         (void*)(int64_t(*)(void*))&flux_array_len);
        add("flux_map_new",           (void*)(void*(*)())&flux_map_new);
        add("flux_map_set",           (void*)(void(*)(void*,const char*,double))&flux_map_set);
        add("flux_map_get",           (void*)(double(*)(void*,const char*))&flux_map_get);
        add("flux_call_function",     (void*)(double(*)(const char*,double*,int))&flux_call_function);
        add("flux_call_function_mixed", (void*)(double(*)(const char*,double*,const char**,int))&flux_call_function_mixed);
        add("flux_call_module",       (void*)(double(*)(const char*,const char*,double*,int))&flux_call_module);
        add("flux_load_state",        (void*)(double(*)(const char*))&flux_load_state);
        add("flux_store_state",       (void*)(void(*)(const char*,double))&flux_store_state);
        add("flux_state_has",         (void*)(int(*)(const char*))&flux_state_has);
        add("flux_state_init",        (void*)(void(*)(const char*,double))&flux_state_init);

        if (auto err = JD.define(llvm::orc::absoluteSymbols(std::move(syms)))) {
            llvm::errs() << "[LLVM JIT] Symbol def failed: " << llvm::toString(std::move(err)) << "\n";
            return false;
        }
        return true;
    }

    // ── Main entry: compile and run ───────────────────────
    double compileAndRun(const HIRProgram& program) {
        g_llvm_interp = interp;

        auto t0 = std::chrono::high_resolution_clock::now();

        declareRuntimeHelpers();

        // Create __flux_main__
        auto* mainFT = llvm::FunctionType::get(doubleTy(), {}, false);
        auto* mainFn = llvm::Function::Create(mainFT, llvm::Function::ExternalLinkage,
                                               "__flux_main__", module.get());
        auto* entry = llvm::BasicBlock::Create(*ctx, "entry", mainFn);
        builder->SetInsertPoint(entry);
        pushScope();

        // Pass 1: compile function declarations
        for (auto& d : program.decls) {
            if (auto* fn = dynamic_cast<HIRFnDecl*>(d.get())) {
                compileFnDecl(fn);
                builder->SetInsertPoint(&mainFn->back());
            }
        }

        // Ensure we're in the main function
        if (builder->GetInsertBlock()->getParent() != mainFn)
            builder->SetInsertPoint(&mainFn->back());

        // Pass 2: compile top-level statements
        for (auto& d : program.decls) {
            if (dynamic_cast<HIRFnDecl*>(d.get())) continue;
            if (dynamic_cast<HIRModuleDecl*>(d.get())) continue;
            compileStmt(d.get());
            if (builder->GetInsertBlock()->getTerminator()) break;
        }

        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateRet(llvm::ConstantFP::get(doubleTy(), 0.0));
        popScope();

        if (llvm::verifyFunction(*mainFn, &llvm::errs())) {
            std::cerr << "[LLVM JIT] __flux_main__ verification failed\n";
            module->print(llvm::errs(), nullptr);
            return -1.0;
        }

        // Count IR instructions
        for (auto& f : *module)
            for (auto& bb : f)
                stats.totalIRInstructions += (int)bb.size();

        // Dump IR if FLUX_DUMP_IR env var is set
        if (std::getenv("FLUX_DUMP_IR"))
            module->print(llvm::errs(), nullptr);

        optimizeModule();

        auto t1 = std::chrono::high_resolution_clock::now();
        stats.compilationTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!jitInitialized) {
            if (!initJIT()) return -1.0;
            jitInitialized = true;
        }

        // Create a resource tracker so we can remove this module on reload
        resourceTracker = jit->getMainJITDylib().createResourceTracker();

        auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx));
        if (auto err = jit->addIRModule(resourceTracker, std::move(tsm))) {
            llvm::errs() << "[LLVM JIT] addIRModule failed: "
                          << llvm::toString(std::move(err)) << "\n";
            return -1.0;
        }

        auto mainSym = jit->lookup("__flux_main__");
        if (!mainSym) {
            llvm::errs() << "[LLVM JIT] lookup failed: "
                          << llvm::toString(mainSym.takeError()) << "\n";
            return -1.0;
        }

        using MainFn = double(*)();
        auto mainPtr = mainSym->toPtr<MainFn>();

        auto t2 = std::chrono::high_resolution_clock::now();
        double result = mainPtr();
        auto t3 = std::chrono::high_resolution_clock::now();

        stats.executionTimeMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
        g_string_pool.clear();
        return result;
    }

    // ── Recompile for hot reload ───────────────────────────
    double recompile(const HIRProgram& program) {
        g_llvm_interp = interp;

        // Remove old module from JIT
        if (resourceTracker) {
            if (auto err = resourceTracker->remove()) {
                llvm::errs() << "[LLVM JIT] Resource removal failed: "
                              << llvm::toString(std::move(err)) << "\n";
                return -1.0;
            }
            resourceTracker = nullptr;
        }

        // Reset compilation state
        functions.clear();
        varScopes.clear();
        stats = LLVMJITCompiler::Stats{};
        g_string_pool.clear();

        // Create fresh context and module
        ctx = std::make_unique<llvm::LLVMContext>();
        module = std::make_unique<llvm::Module>("flux_module", *ctx);
        builder = std::make_unique<llvm::IRBuilder<>>(*ctx);

        auto t0 = std::chrono::high_resolution_clock::now();

        declareRuntimeHelpers();

        // Create __flux_main__
        auto* mainFT = llvm::FunctionType::get(doubleTy(), {}, false);
        auto* mainFn = llvm::Function::Create(mainFT, llvm::Function::ExternalLinkage,
                                               "__flux_main__", module.get());
        auto* entry = llvm::BasicBlock::Create(*ctx, "entry", mainFn);
        builder->SetInsertPoint(entry);
        pushScope();

        // Pass 1: compile function declarations
        for (auto& d : program.decls) {
            if (auto* fn = dynamic_cast<HIRFnDecl*>(d.get())) {
                compileFnDecl(fn);
                builder->SetInsertPoint(&mainFn->back());
            }
        }

        if (builder->GetInsertBlock()->getParent() != mainFn)
            builder->SetInsertPoint(&mainFn->back());

        // Pass 2: compile top-level statements
        for (auto& d : program.decls) {
            if (dynamic_cast<HIRFnDecl*>(d.get())) continue;
            if (dynamic_cast<HIRModuleDecl*>(d.get())) continue;
            compileStmt(d.get());
            if (builder->GetInsertBlock()->getTerminator()) break;
        }

        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateRet(llvm::ConstantFP::get(doubleTy(), 0.0));
        popScope();

        if (llvm::verifyFunction(*mainFn, &llvm::errs())) {
            std::cerr << "[LLVM JIT] __flux_main__ verification failed on reload\n";
            return -1.0;
        }

        for (auto& f : *module)
            for (auto& bb : f)
                stats.totalIRInstructions += (int)bb.size();

        if (std::getenv("FLUX_DUMP_IR"))
            module->print(llvm::errs(), nullptr);

        optimizeModule();

        auto t1 = std::chrono::high_resolution_clock::now();
        stats.compilationTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Add new module with a fresh resource tracker
        resourceTracker = jit->getMainJITDylib().createResourceTracker();

        auto tsm = llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx));
        if (auto err = jit->addIRModule(resourceTracker, std::move(tsm))) {
            llvm::errs() << "[LLVM JIT] addIRModule failed on reload: "
                          << llvm::toString(std::move(err)) << "\n";
            return -1.0;
        }

        auto mainSym = jit->lookup("__flux_main__");
        if (!mainSym) {
            llvm::errs() << "[LLVM JIT] lookup failed on reload: "
                          << llvm::toString(mainSym.takeError()) << "\n";
            return -1.0;
        }

        using MainFn = double(*)();
        auto mainPtr = mainSym->toPtr<MainFn>();

        auto t2 = std::chrono::high_resolution_clock::now();
        double result = mainPtr();
        auto t3 = std::chrono::high_resolution_clock::now();

        stats.executionTimeMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
        g_string_pool.clear();
        return result;
    }

    void dumpIR() const {
        if (module) module->print(llvm::errs(), nullptr);
    }
};

// =====================================================================
// Public interface forwarding to Impl (LLVM-enabled build)
// =====================================================================

LLVMJITCompiler::LLVMJITCompiler()  : impl_(std::make_unique<Impl>()) {}
LLVMJITCompiler::~LLVMJITCompiler() = default;
LLVMJITCompiler::LLVMJITCompiler(LLVMJITCompiler&&) noexcept = default;
LLVMJITCompiler& LLVMJITCompiler::operator=(LLVMJITCompiler&&) noexcept = default;

void LLVMJITCompiler::setInterpreter(Interpreter* i) { impl_->interp = i; }

double LLVMJITCompiler::compileAndRun(const HIRProgram& p) { return impl_->compileAndRun(p); }
double LLVMJITCompiler::recompile(const HIRProgram& p) { return impl_->recompile(p); }

bool LLVMJITCompiler::compileFunction(HIRFnDecl* fn) {
    return impl_->compileFnDecl(fn) != nullptr;
}

void LLVMJITCompiler::dumpIR() const { impl_->dumpIR(); }
bool LLVMJITCompiler::isAvailable() const { return true; }

LLVMJITCompiler::Stats LLVMJITCompiler::getStats() const { return impl_->stats; }

void LLVMJITCompiler::dumpStats() const {
    auto& s = impl_->stats;
    std::cerr << "\033[36m[LLVM JIT] Functions compiled: " << s.functionsCompiled
              << ", IR instructions: " << s.totalIRInstructions
              << "\n  Compilation: " << s.compilationTimeMs << "ms"
              << ", Execution: " << s.executionTimeMs << "ms\033[0m\n";
}

#else  // !FLUX_HAS_LLVM

// =====================================================================
// Stub implementation when LLVM is not available
// =====================================================================

struct LLVMJITCompiler::Impl {};

LLVMJITCompiler::LLVMJITCompiler()  : impl_(nullptr) {}
LLVMJITCompiler::~LLVMJITCompiler() = default;
LLVMJITCompiler::LLVMJITCompiler(LLVMJITCompiler&&) noexcept = default;
LLVMJITCompiler& LLVMJITCompiler::operator=(LLVMJITCompiler&&) noexcept = default;

void LLVMJITCompiler::setInterpreter(Interpreter*) {}

double LLVMJITCompiler::compileAndRun(const HIRProgram&) {
    std::cerr << "\033[33m[LLVM JIT] Not available — rebuild with LLVM to enable.\033[0m\n";
    return -1.0;
}

double LLVMJITCompiler::recompile(const HIRProgram&) {
    std::cerr << "\033[33m[LLVM JIT] Not available — rebuild with LLVM to enable.\033[0m\n";
    return -1.0;
}

bool LLVMJITCompiler::compileFunction(HIRFnDecl*) { return false; }
void LLVMJITCompiler::dumpIR() const { std::cerr << "[LLVM JIT] Not available.\n"; }
bool LLVMJITCompiler::isAvailable() const { return false; }

LLVMJITCompiler::Stats LLVMJITCompiler::getStats() const { return {}; }

void LLVMJITCompiler::dumpStats() const {
    std::cerr << "\033[33m[LLVM JIT] Not available — rebuild with LLVM to enable.\033[0m\n";
}

#endif // FLUX_HAS_LLVM
