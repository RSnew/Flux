// profiler.h — Flux @profile 性能分析
// 在函数执行前后记录耗时、调用次数，输出性能报告
#pragma once
#include <string>
#include <unordered_map>
#include <chrono>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <mutex>

// ═══════════════════════════════════════════════════════════
// 函数性能数据
// ═══════════════════════════════════════════════════════════
struct ProfileEntry {
    std::string name;
    int64_t     totalUs   = 0;    // 总耗时（微秒）
    int64_t     minUs     = INT64_MAX;
    int64_t     maxUs     = 0;
    int         callCount = 0;

    double avgUs() const { return callCount > 0 ? (double)totalUs / callCount : 0; }
};

// ═══════════════════════════════════════════════════════════
// RAII 计时器 — 自动在析构时记录耗时
// ═══════════════════════════════════════════════════════════
class Profiler;

class ProfileScope {
public:
    ProfileScope(Profiler& p, const std::string& name);
    ~ProfileScope();
private:
    Profiler& profiler_;
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_;
};

// ═══════════════════════════════════════════════════════════
// 全局 Profiler
// ═══════════════════════════════════════════════════════════
class Profiler {
public:
    static Profiler& instance() {
        static Profiler p;
        return p;
    }

    // 标记某个函数需要 profile
    void markFunction(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        profiledFunctions_.insert(name);
    }

    bool isProfiled(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_);
        return profiledFunctions_.count(name) > 0;
    }

    void record(const std::string& name, int64_t us) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& e = entries_[name];
        e.name = name;
        e.callCount++;
        e.totalUs += us;
        if (us < e.minUs) e.minUs = us;
        if (us > e.maxUs) e.maxUs = us;
    }

    // 输出性能报告
    void report() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (entries_.empty()) return;

        std::vector<const ProfileEntry*> sorted;
        for (auto& [_, e] : entries_) sorted.push_back(&e);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b) { return a->totalUs > b->totalUs; });

        std::cout << "\n\033[1m\033[36m"
                  << "╔════════════════════════════════════════════════════════════╗\n"
                  << "║                   @profile Report                         ║\n"
                  << "╚════════════════════════════════════════════════════════════╝\n"
                  << "\033[0m";

        std::cout << std::left << std::setw(30) << "  Function"
                  << std::right << std::setw(8) << "Calls"
                  << std::setw(12) << "Total(ms)"
                  << std::setw(12) << "Avg(μs)"
                  << std::setw(12) << "Min(μs)"
                  << std::setw(12) << "Max(μs)"
                  << "\n";
        std::cout << "  " << std::string(84, '-') << "\n";

        for (auto* e : sorted) {
            std::cout << "  " << std::left << std::setw(28) << e->name
                      << std::right << std::setw(8) << e->callCount
                      << std::setw(12) << std::fixed << std::setprecision(2)
                      << (e->totalUs / 1000.0)
                      << std::setw(12) << std::setprecision(1) << e->avgUs()
                      << std::setw(12) << e->minUs
                      << std::setw(12) << e->maxUs
                      << "\n";
        }
        std::cout << "\n";
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.clear();
        profiledFunctions_.clear();
    }

private:
    Profiler() = default;
    mutable std::mutex mu_;
    std::unordered_map<std::string, ProfileEntry> entries_;
    std::unordered_set<std::string> profiledFunctions_;
};

// ProfileScope 实现
inline ProfileScope::ProfileScope(Profiler& p, const std::string& name)
    : profiler_(p), name_(name)
    , start_(std::chrono::high_resolution_clock::now()) {}

inline ProfileScope::~ProfileScope() {
    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
    profiler_.record(name_, us);
}
