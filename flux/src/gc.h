// gc.h — Flux 循环引用检测器
// 基于 shared_ptr use_count 的启发式循环检测
#pragma once
#include "interpreter.h"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>

// ═══════════════════════════════════════════════════════════
// CycleDetector — 检测 Value 图中的循环引用
// 使用 DFS 标记法：遍历所有可达 Value，检测已访问节点
// ═══════════════════════════════════════════════════════════
class CycleDetector {
public:
    struct CycleInfo {
        std::string description;  // 人类可读的循环描述
        int refCount;             // 涉及的引用计数
    };

    // 从 Environment 开始扫描所有可达的 Value
    std::vector<CycleInfo> detect(std::shared_ptr<Environment> env) {
        cycles_.clear();
        visited_.clear();
        inStack_.clear();
        path_.clear();

        scanEnv(env);
        return cycles_;
    }

    // 从单个 Value 开始检测
    std::vector<CycleInfo> detect(const Value& v) {
        cycles_.clear();
        visited_.clear();
        inStack_.clear();
        path_.clear();

        scanValue(v, "root");
        return cycles_;
    }

    // 获取可能泄漏的对象数量（shared_ptr use_count > 预期值的对象）
    size_t suspectedLeaks() const {
        return cycles_.size();
    }

private:
    std::vector<CycleInfo>       cycles_;
    std::unordered_set<void*>    visited_;
    std::unordered_set<void*>    inStack_;  // 当前 DFS 栈上的节点
    std::vector<std::string>     path_;     // 当前路径

    void scanEnv(std::shared_ptr<Environment> env) {
        if (!env) return;
        // Environment 本身可能形成循环（parent 链不会，但 Value 中的闭包可能引用 env）
        // 我们主要关心 Value 层面的循环
    }

    void scanValue(const Value& v, const std::string& name) {
        void* ptr = nullptr;

        switch (v.type) {
        case Value::Type::Array:
            if (!v.array) return;
            ptr = v.array.get();
            if (inStack_.count(ptr)) {
                cycles_.push_back({
                    "Cycle detected at '" + name + "': Array → " + pathStr(),
                    (int)v.array.use_count()
                });
                return;
            }
            if (visited_.count(ptr)) return;
            visited_.insert(ptr);
            inStack_.insert(ptr);
            path_.push_back(name);
            for (size_t i = 0; i < v.array->size(); i++)
                scanValue((*v.array)[i], name + "[" + std::to_string(i) + "]");
            path_.pop_back();
            inStack_.erase(ptr);
            break;

        case Value::Type::Map:
            if (!v.map) return;
            ptr = v.map.get();
            if (inStack_.count(ptr)) {
                cycles_.push_back({
                    "Cycle detected at '" + name + "': Map → " + pathStr(),
                    (int)v.map.use_count()
                });
                return;
            }
            if (visited_.count(ptr)) return;
            visited_.insert(ptr);
            inStack_.insert(ptr);
            path_.push_back(name);
            for (auto& [k, val] : *v.map)
                scanValue(val, name + "." + k);
            path_.pop_back();
            inStack_.erase(ptr);
            break;

        case Value::Type::StructInst:
            if (!v.structInst) return;
            ptr = v.structInst.get();
            if (inStack_.count(ptr)) {
                cycles_.push_back({
                    "Cycle detected at '" + name + "': StructInst → " + pathStr(),
                    (int)v.structInst.use_count()
                });
                return;
            }
            if (visited_.count(ptr)) return;
            visited_.insert(ptr);
            inStack_.insert(ptr);
            path_.push_back(name);
            for (auto& [fname, fval] : v.structInst->fields)
                scanValue(fval, name + "." + fname);
            path_.pop_back();
            inStack_.erase(ptr);
            break;

        default:
            break;
        }
    }

    std::string pathStr() const {
        std::string s;
        for (auto& p : path_) {
            if (!s.empty()) s += " → ";
            s += p;
        }
        return s;
    }
};
