// debugger.h — Flux DAP (Debug Adapter Protocol) 调试器
// 支持断点、单步执行、变量检查
#pragma once
#include "interpreter.h"
#include "ast.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <iostream>

// ═══════════════════════════════════════════════════════════
// 断点
// ═══════════════════════════════════════════════════════════
struct Breakpoint {
    int id;
    std::string file;
    int line;
    std::string condition;  // 条件断点表达式（可为空）
    bool enabled = true;
};

// ═══════════════════════════════════════════════════════════
// 变量信息（用于 Variables 请求）
// ═══════════════════════════════════════════════════════════
struct VariableInfo {
    std::string name;
    std::string value;
    std::string type;
    int variablesReference = 0;  // > 0 表示有子变量
};

// ═══════════════════════════════════════════════════════════
// 调试步进模式
// ═══════════════════════════════════════════════════════════
enum class StepMode {
    Continue,    // 运行到下一个断点
    StepOver,    // 执行当前行，不进入函数
    StepInto,    // 执行当前行，进入函数调用
    StepOut,     // 运行到当前函数返回
    Pause,       // 暂停执行
};

// ═══════════════════════════════════════════════════════════
// 栈帧信息
// ═══════════════════════════════════════════════════════════
struct StackFrame {
    int id;
    std::string name;
    std::string file;
    int line;
    int column;
};

// ═══════════════════════════════════════════════════════════
// Debugger — 调试器核心
// ═══════════════════════════════════════════════════════════
class Debugger {
public:
    Debugger() : nextBreakpointId_(1), nextFrameId_(1) {}

    // ── 断点管理 ─────────────────────────────────────────────
    int addBreakpoint(const std::string& file, int line,
                      const std::string& condition = "") {
        int id = nextBreakpointId_++;
        breakpoints_.push_back({id, file, line, condition, true});
        return id;
    }

    bool removeBreakpoint(int id) {
        auto it = std::find_if(breakpoints_.begin(), breakpoints_.end(),
                               [id](const Breakpoint& bp) { return bp.id == id; });
        if (it != breakpoints_.end()) {
            breakpoints_.erase(it);
            return true;
        }
        return false;
    }

    void enableBreakpoint(int id, bool enabled) {
        for (auto& bp : breakpoints_)
            if (bp.id == id) { bp.enabled = enabled; break; }
    }

    std::vector<Breakpoint> getBreakpoints() const { return breakpoints_; }

    // ── 执行控制 ─────────────────────────────────────────────
    void setStepMode(StepMode mode) {
        std::lock_guard<std::mutex> lock(mu_);
        stepMode_ = mode;
        cv_.notify_all();
    }

    StepMode getStepMode() const { return stepMode_; }

    // 在 AST 节点执行前调用的钩子（由解释器回调）
    // 返回 true 表示应该暂停执行
    bool shouldPause(const std::string& file, int line, int depth) {
        std::lock_guard<std::mutex> lock(mu_);

        switch (stepMode_) {
        case StepMode::Continue:
            // 检查断点
            return isBreakpointAt(file, line);

        case StepMode::StepOver:
            // 在同一深度或更浅处暂停
            if (depth <= stepDepth_) {
                stepMode_ = StepMode::Continue;
                return true;
            }
            return isBreakpointAt(file, line);

        case StepMode::StepInto:
            // 在下一个可执行行暂停
            stepMode_ = StepMode::Continue;
            return true;

        case StepMode::StepOut:
            // 在比当前更浅的深度暂停
            if (depth < stepDepth_) {
                stepMode_ = StepMode::Continue;
                return true;
            }
            return isBreakpointAt(file, line);

        case StepMode::Pause:
            return true;
        }
        return false;
    }

    // 暂停并等待调试命令
    void waitForCommand() {
        std::unique_lock<std::mutex> lock(mu_);
        paused_ = true;
        cv_.wait(lock, [this] { return stepMode_ != StepMode::Pause; });
        paused_ = false;
    }

    bool isPaused() const { return paused_; }

    // ── 变量检查 ─────────────────────────────────────────────
    std::vector<VariableInfo> getLocals(std::shared_ptr<Environment> env) {
        std::vector<VariableInfo> vars;
        if (!env) return vars;
        // Environment doesn't expose iteration, so this is a conceptual implementation
        // In production, Environment would need a method to list all variables
        return vars;
    }

    std::vector<VariableInfo> inspectValue(const Value& v, const std::string& name) {
        std::vector<VariableInfo> vars;

        switch (v.type) {
        case Value::Type::Array:
            if (v.array) {
                for (size_t i = 0; i < v.array->size(); i++) {
                    vars.push_back({
                        "[" + std::to_string(i) + "]",
                        (*v.array)[i].toString(),
                        typeName((*v.array)[i]),
                        hasChildren((*v.array)[i]) ? (int)i + 1 : 0
                    });
                }
            }
            break;
        case Value::Type::Map:
            if (v.map) {
                for (auto& [k, val] : *v.map) {
                    vars.push_back({k, val.toString(), typeName(val),
                                   hasChildren(val) ? (int)vars.size() + 1 : 0});
                }
            }
            break;
        case Value::Type::StructInst:
            if (v.structInst) {
                for (auto& [fname, fval] : v.structInst->fields) {
                    vars.push_back({fname, fval.toString(), typeName(fval),
                                   hasChildren(fval) ? (int)vars.size() + 1 : 0});
                }
            }
            break;
        default:
            vars.push_back({name, v.toString(), typeName(v), 0});
            break;
        }
        return vars;
    }

    // ── 栈帧 ─────────────────────────────────────────────────
    void pushFrame(const std::string& name, const std::string& file, int line) {
        callStack_.push_back({nextFrameId_++, name, file, line, 0});
    }

    void popFrame() {
        if (!callStack_.empty()) callStack_.pop_back();
    }

    std::vector<StackFrame> getCallStack() const { return callStack_; }
    int currentDepth() const { return (int)callStack_.size(); }

    void setStepDepth(int depth) { stepDepth_ = depth; }

    // ── 持久状态检查 ─────────────────────────────────────────
    std::vector<VariableInfo> getPersistentState(
        const std::unordered_map<std::string, Value>& store) {
        std::vector<VariableInfo> vars;
        for (auto& [name, val] : store)
            vars.push_back({"state." + name, val.toString(), typeName(val), 0});
        return vars;
    }

private:
    std::vector<Breakpoint>    breakpoints_;
    std::vector<StackFrame>    callStack_;
    int                        nextBreakpointId_;
    int                        nextFrameId_;
    StepMode                   stepMode_ = StepMode::Continue;
    int                        stepDepth_ = 0;
    bool                       paused_ = false;
    std::mutex                 mu_;
    std::condition_variable    cv_;

    bool isBreakpointAt(const std::string& file, int line) {
        for (auto& bp : breakpoints_) {
            if (bp.enabled && bp.line == line &&
                (bp.file.empty() || bp.file == file))
                return true;
        }
        return false;
    }

    std::string typeName(const Value& v) {
        switch (v.type) {
        case Value::Type::Nil:        return "Nil";
        case Value::Type::Number:     return "Number";
        case Value::Type::String:     return "String";
        case Value::Type::Bool:       return "Bool";
        case Value::Type::Array:      return "Array";
        case Value::Type::Map:        return "Map";
        case Value::Type::Future:     return "Future";
        case Value::Type::Chan:       return "Chan";
        case Value::Type::StructType: return "StructType";
        case Value::Type::StructInst: return "StructInst";
        case Value::Type::Interface:  return "Interface";
        case Value::Type::Function:   return "Function";
        case Value::Type::Addr:       return "Addr";
        case Value::Type::Specify:   return "Specify";
        }
        return "Unknown";
    }

    bool hasChildren(const Value& v) {
        return v.type == Value::Type::Array ||
               v.type == Value::Type::Map ||
               v.type == Value::Type::StructInst;
    }
};
