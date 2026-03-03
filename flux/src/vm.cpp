// vm.cpp — Flux 字节码虚拟机实现
#include "vm.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cmath>

// ── 构造 ─────────────────────────────────────────────────
VM::VM(Interpreter& interp) : interp_(interp) {
    stack_.reserve(256);
}

// ═══════════════════════════════════════════════════════════
// 主执行循环
// ═══════════════════════════════════════════════════════════
Value VM::run(Chunk& chunk,
              std::shared_ptr<Environment> env,
              ModuleRuntime* mod) {

    // 当前 environment 指针（scope push/pop 会改变它）
    std::shared_ptr<Environment> curEnv = env;

    size_t ip = 0;
    const size_t codeSize = chunk.code.size();

    while (ip < codeSize) {
        const Instruction& inst = chunk.code[ip++];

        switch (inst.op) {

        // ── 常量 ─────────────────────────────────────────
        case OpCode::PUSH_NIL:   push(Value::Nil());  break;
        case OpCode::PUSH_TRUE:  push(Value::Bool(true));  break;
        case OpCode::PUSH_FALSE: push(Value::Bool(false)); break;
        case OpCode::PUSH_CONST: push(chunk.constants[inst.a]); break;

        // ── 变量 ─────────────────────────────────────────
        case OpCode::LOAD: {
            const std::string& name = chunk.names[inst.a];
            push(curEnv->get(name));
            break;
        }
        case OpCode::STORE: {
            const std::string& name = chunk.names[inst.a];
            Value v = pop();
            if (!curEnv->assign(name, v))
                throw std::runtime_error("assignment to undefined variable: " + name);
            push(v);   // store 的返回值
            break;
        }
        case OpCode::DEFINE: {
            const std::string& name = chunk.names[inst.a];
            Value v = pop();
            // 特殊：如果是 __iter_N，需要把 iterable 转为 Array
            if (name.rfind("__iter_", 0) == 0) {
                if (v.type == Value::Type::String) {
                    auto arr = std::make_shared<std::vector<Value>>();
                    for (char c : v.string)
                        arr->push_back(Value::Str(std::string(1, c)));
                    v = Value::Arr(arr);
                } else if (v.type == Value::Type::Map && v.map) {
                    auto arr = std::make_shared<std::vector<Value>>();
                    for (auto& kv : *v.map)
                        arr->push_back(Value::Str(kv.first));
                    v = Value::Arr(arr);
                }
                // Array stays as-is
            }
            curEnv->set(name, v);
            break;
        }

        // ── 持久状态 ─────────────────────────────────────
        case OpCode::LOAD_STATE: {
            const std::string& field = chunk.names[inst.a];
            auto& store = mod ? mod->persistentStore : interp_.persistentStore_;
            auto it = store.find(field);
            if (it == store.end())
                throw std::runtime_error("undefined persistent field: state." + field);
            push(it->second);
            break;
        }
        case OpCode::STORE_STATE: {
            const std::string& field = chunk.names[inst.a];
            auto& store = mod ? mod->persistentStore : interp_.persistentStore_;
            auto it = store.find(field);
            if (it == store.end())
                throw std::runtime_error("undefined persistent field: state." + field
                    + " (declare in persistent { })");
            Value v = pop();
            it->second = v;
            push(v);
            break;
        }

        // ── 算术 ─────────────────────────────────────────
        case OpCode::ADD: {
            Value r = pop(), l = pop();
            if (l.type == Value::Type::String || r.type == Value::Type::String) {
                push(Value::Str(l.toString() + r.toString()));
            } else if (l.type == Value::Type::Number && r.type == Value::Type::Number) {
                push(Value::Num(l.number + r.number));
            } else {
                throw std::runtime_error("VM ADD: incompatible types");
            }
            break;
        }
        case OpCode::SUB: { Value r=pop(),l=pop(); push(Value::Num(l.number-r.number)); break; }
        case OpCode::MUL: { Value r=pop(),l=pop(); push(Value::Num(l.number*r.number)); break; }
        case OpCode::DIV: {
            Value r=pop(),l=pop();
            if (r.number == 0) throw std::runtime_error("division by zero");
            push(Value::Num(l.number/r.number));
            break;
        }
        case OpCode::MOD: {
            Value r=pop(),l=pop();
            push(Value::Num(std::fmod(l.number, r.number)));
            break;
        }
        case OpCode::NEG: { Value v=pop(); push(Value::Num(-v.number)); break; }
        case OpCode::NOT: { Value v=pop(); push(Value::Bool(!v.isTruthy())); break; }

        // ── 比较 ─────────────────────────────────────────
        case OpCode::EQ:  {
            Value r=pop(),l=pop();
            if (l.type==Value::Type::Number && r.type==Value::Type::Number)
                push(Value::Bool(l.number == r.number));
            else
                push(Value::Bool(l.toString() == r.toString()));
            break;
        }
        case OpCode::NEQ: {
            Value r=pop(),l=pop();
            if (l.type==Value::Type::Number && r.type==Value::Type::Number)
                push(Value::Bool(l.number != r.number));
            else
                push(Value::Bool(l.toString() != r.toString()));
            break;
        }
        case OpCode::LT:  { Value r=pop(),l=pop(); push(Value::Bool(l.number <  r.number)); break; }
        case OpCode::GT:  { Value r=pop(),l=pop(); push(Value::Bool(l.number >  r.number)); break; }
        case OpCode::LEQ: { Value r=pop(),l=pop(); push(Value::Bool(l.number <= r.number)); break; }
        case OpCode::GEQ: { Value r=pop(),l=pop(); push(Value::Bool(l.number >= r.number)); break; }

        // ── 作用域 ───────────────────────────────────────
        case OpCode::PUSH_SCOPE:
            curEnv = std::make_shared<Environment>(curEnv);
            break;
        case OpCode::POP_SCOPE:
            if (curEnv->parent()) curEnv = curEnv->parent();
            break;
        case OpCode::POP:
            pop();
            break;

        // ── 控制流 ───────────────────────────────────────
        case OpCode::JUMP:
            ip = (size_t)inst.a;
            break;
        case OpCode::JUMP_IF_FALSE: {
            Value cond = pop();
            if (!cond.isTruthy()) ip = (size_t)inst.a;
            break;
        }
        case OpCode::JUMP_IF_TRUE: {
            Value cond = pop();
            if (cond.isTruthy()) ip = (size_t)inst.a;
            break;
        }

        // ── 函数调用 ─────────────────────────────────────
        case OpCode::CALL: {
            const std::string& name = chunk.names[inst.a];
            int argc = inst.b;
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();
            Value result = interp_.callFunction(name, std::move(args), mod);
            push(result);
            break;
        }

        // ── 模块调用 Module.fn(args) ─────────────────────
        case OpCode::CALL_MODULE: {
            const std::string& combined = chunk.names[inst.a];
            int argc = inst.b;
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();

            // 拆分 "Module.fn"
            auto dot = combined.find('.');
            if (dot == std::string::npos)
                throw std::runtime_error("VM CALL_MODULE: bad name: " + combined);
            std::string modName = combined.substr(0, dot);
            std::string fnName  = combined.substr(dot + 1);

            // 优先检查标准库模块和用户模块；否则回退为变量方法调用
            bool isKnownModule = interp_.stdlibModules_.count(modName) > 0
                              || interp_.modules_.count(modName) > 0;
            Value result;
            if (isKnownModule) {
                result = interp_.callModuleFunction(modName, fnName, std::move(args));
            } else {
                // 变量方法调用（如 arr.push(), str.upper(), map.keys()）
                Value obj = curEnv->get(modName);
                result = dispatchMethod(obj, fnName, std::move(args), mod);
            }
            push(result);
            break;
        }

        // ── 方法调用 obj.method(args) ────────────────────
        case OpCode::CALL_METHOD: {
            const std::string& method = chunk.names[inst.a];
            int argc = inst.b;
            std::vector<Value> args(argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();
            Value obj = pop();
            Value result = dispatchMethod(obj, method, std::move(args), mod);
            push(result);
            break;
        }

        // ── 返回 ─────────────────────────────────────────
        case OpCode::RETURN:
            return pop();
        case OpCode::RETURN_NIL:
            return Value::Nil();

        // ── 集合 ─────────────────────────────────────────
        case OpCode::MAKE_ARRAY: {
            int count = inst.b;
            auto arr = std::make_shared<std::vector<Value>>(count);
            for (int i = count - 1; i >= 0; --i) (*arr)[i] = pop();
            push(Value::Arr(arr));
            break;
        }
        case OpCode::MAKE_MAP: {
            int pairs = inst.b;
            auto map = std::make_shared<std::unordered_map<std::string, Value>>();
            // 每对：先 key 后 val（编译时按 key0,val0,key1,val1... 顺序入栈）
            std::vector<std::pair<std::string,Value>> entries(pairs);
            for (int i = pairs - 1; i >= 0; --i) {
                entries[i].second = pop();
                entries[i].first  = pop().toString();
            }
            for (auto& e : entries) (*map)[e.first] = e.second;
            push(Value::MapOf(map));
            break;
        }
        case OpCode::INDEX_GET: {
            Value idx = pop();
            Value obj = pop();
            if (obj.type == Value::Type::Array) {
                if (!obj.array) throw std::runtime_error("null array");
                int i = (int)idx.number;
                if (i < 0) i = (int)obj.array->size() + i;
                if (i < 0 || i >= (int)obj.array->size())
                    throw std::runtime_error("array index out of bounds: " + std::to_string(i));
                push((*obj.array)[i]);
            } else if (obj.type == Value::Type::String) {
                int i = (int)idx.number;
                if (i < 0 || i >= (int)obj.string.size())
                    throw std::runtime_error("string index out of bounds");
                push(Value::Str(std::string(1, obj.string[i])));
            } else if (obj.type == Value::Type::Map) {
                if (!obj.map) { push(Value::Nil()); break; }
                auto it = obj.map->find(idx.toString());
                push(it != obj.map->end() ? it->second : Value::Nil());
            } else {
                throw std::runtime_error("cannot index into " + obj.toString());
            }
            break;
        }
        case OpCode::INDEX_SET: {
            Value val = pop();
            Value idx = pop();
            Value obj = pop();
            if (obj.type == Value::Type::Map) {
                if (!obj.map) throw std::runtime_error("null map");
                (*obj.map)[idx.toString()] = val;
            } else if (obj.type == Value::Type::Array) {
                if (!obj.array) throw std::runtime_error("null array");
                int i = (int)idx.number;
                if (i < 0) i = (int)obj.array->size() + i;
                if (i < 0 || i >= (int)obj.array->size())
                    throw std::runtime_error("array index out of bounds: " + std::to_string(i));
                (*obj.array)[i] = val;
            } else {
                throw std::runtime_error("cannot index-assign into " + obj.toString());
            }
            push(val);
            break;
        }

        default:
            throw std::runtime_error("VM: unhandled opcode " +
                                     std::to_string((int)inst.op));
        } // switch
    } // while

    return stack_.empty() ? Value::Nil() : pop();
}

// ═══════════════════════════════════════════════════════════
// 方法分发（Array / String / Map 方法，复用解释器逻辑）
// ═══════════════════════════════════════════════════════════
Value VM::dispatchMethod(Value& obj, const std::string& m,
                         std::vector<Value> args, ModuleRuntime* /*mod*/) {
    // ── Array ──────────────────────────────────────────────
    if (obj.type == Value::Type::Array && obj.array) {
        if (m == "push" || m == "append") {
            for (auto& v : args) obj.array->push_back(v);
            return Value::Num((double)obj.array->size());
        }
        if (m == "pop") {
            if (obj.array->empty()) throw std::runtime_error("pop from empty array");
            Value last = obj.array->back();
            obj.array->pop_back();
            return last;
        }
        if (m == "len"  || m == "size")  return Value::Num((double)obj.array->size());
        if (m == "first") return obj.array->empty() ? Value::Nil() : obj.array->front();
        if (m == "last")  return obj.array->empty() ? Value::Nil() : obj.array->back();
        if (m == "contains") {
            if (args.empty()) return Value::Bool(false);
            std::string needle = args[0].toString();
            for (auto& v : *obj.array)
                if (v.toString() == needle) return Value::Bool(true);
            return Value::Bool(false);
        }
        if (m == "join") {
            std::string sep = args.empty() ? "" : args[0].toString();
            std::string result;
            for (size_t i = 0; i < obj.array->size(); ++i) {
                if (i > 0) result += sep;
                result += (*obj.array)[i].toString();
            }
            return Value::Str(result);
        }
        if (m == "reverse") {
            std::reverse(obj.array->begin(), obj.array->end());
            return obj;
        }
    }
    // ── String ─────────────────────────────────────────────
    if (obj.type == Value::Type::String) {
        if (m == "len" || m == "size") return Value::Num((double)obj.string.size());
        if (m == "upper") {
            std::string s = obj.string;
            for (auto& c : s) c = (char)toupper((unsigned char)c);
            return Value::Str(s);
        }
        if (m == "lower") {
            std::string s = obj.string;
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            return Value::Str(s);
        }
        if (m == "split") {
            std::string sep = args.empty() ? " " : args[0].toString();
            auto arr = std::make_shared<std::vector<Value>>();
            size_t start = 0, pos = 0;
            while ((pos = obj.string.find(sep, start)) != std::string::npos) {
                arr->push_back(Value::Str(obj.string.substr(start, pos - start)));
                start = pos + sep.size();
            }
            arr->push_back(Value::Str(obj.string.substr(start)));
            return Value::Arr(arr);
        }
        if (m == "contains") {
            if (args.empty()) return Value::Bool(false);
            return Value::Bool(obj.string.find(args[0].toString()) != std::string::npos);
        }
        if (m == "trim") {
            std::string s = obj.string;
            size_t l = s.find_first_not_of(" \t\n\r");
            size_t r = s.find_last_not_of(" \t\n\r");
            return Value::Str(l == std::string::npos ? "" : s.substr(l, r - l + 1));
        }
    }
    // ── Map ────────────────────────────────────────────────
    if (obj.type == Value::Type::Map && obj.map) {
        if (m == "len"  || m == "size") return Value::Num((double)obj.map->size());
        if (m == "has")  return Value::Bool(!args.empty() && obj.map->count(args[0].toString()) > 0);
        if (m == "get") {
            if (args.empty()) return Value::Nil();
            auto it = obj.map->find(args[0].toString());
            return it != obj.map->end() ? it->second : (args.size()>1 ? args[1] : Value::Nil());
        }
        if (m == "set") {
            if (args.size() < 2) return Value::Nil();
            (*obj.map)[args[0].toString()] = args[1];
            return args[1];
        }
        if (m == "delete" || m == "remove")
            return Value::Bool(!args.empty() && obj.map->erase(args[0].toString()) > 0);
        if (m == "keys") {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& kv : *obj.map) arr->push_back(Value::Str(kv.first));
            return Value::Arr(arr);
        }
        if (m == "values") {
            auto arr = std::make_shared<std::vector<Value>>();
            for (auto& kv : *obj.map) arr->push_back(kv.second);
            return Value::Arr(arr);
        }
        if (m == "entries") {
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
    throw std::runtime_error("VM: unknown method '" + m + "' on " + obj.toString());
}

// ═══════════════════════════════════════════════════════════
// 调试：反汇编
// ═══════════════════════════════════════════════════════════
void Chunk::dump(const std::string& title) const {
    std::cout << "=== Bytecode: " << title << " ===\n";
    for (size_t i = 0; i < code.size(); ++i) {
        const auto& inst = code[i];
        std::string name;
        switch (inst.op) {
#define CASE(X) case OpCode::X: name = #X; break;
            CASE(PUSH_NIL) CASE(PUSH_TRUE) CASE(PUSH_FALSE) CASE(PUSH_CONST)
            CASE(LOAD) CASE(STORE) CASE(DEFINE)
            CASE(LOAD_STATE) CASE(STORE_STATE)
            CASE(ADD) CASE(SUB) CASE(MUL) CASE(DIV) CASE(MOD) CASE(NEG) CASE(NOT)
            CASE(EQ) CASE(NEQ) CASE(LT) CASE(GT) CASE(LEQ) CASE(GEQ)
            CASE(PUSH_SCOPE) CASE(POP_SCOPE) CASE(POP)
            CASE(JUMP) CASE(JUMP_IF_FALSE) CASE(JUMP_IF_TRUE)
            CASE(CALL) CASE(CALL_MODULE) CASE(CALL_METHOD) CASE(RETURN) CASE(RETURN_NIL)
            CASE(MAKE_ARRAY) CASE(MAKE_MAP) CASE(INDEX_GET) CASE(INDEX_SET)
#undef CASE
            default: name = "?"; break;
        }
        std::printf("  %4zu  %-20s  a=%-4d b=%-4d", i, name.c_str(), inst.a, inst.b);
        // 附加注释
        if (inst.op == OpCode::PUSH_CONST && inst.a < (int)constants.size())
            std::cout << "  ; " << constants[inst.a].toString();
        else if ((inst.op==OpCode::LOAD||inst.op==OpCode::STORE||inst.op==OpCode::DEFINE||
                  inst.op==OpCode::LOAD_STATE||inst.op==OpCode::STORE_STATE||
                  inst.op==OpCode::CALL||inst.op==OpCode::CALL_MODULE||
                  inst.op==OpCode::CALL_METHOD) && inst.a < (int)names.size())
            std::cout << "  ; " << names[inst.a];
        std::cout << "\n";
    }
    std::cout << "  (" << code.size() << " instructions, "
              << constants.size() << " constants, "
              << names.size() << " names)\n";
}
