// threadpool.h — Flux 线程池（Feature K: 并发模型 v2）
// 设计原则：用户声明并控制线程池，语言保证并发安全
#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <vector>
#include <string>
#include <stdexcept>
#include <atomic>

// ── 队列满时的溢出策略 ────────────────────────────────────
enum class OverflowPolicy {
    Block,  // 默认：阻塞发送方，不丢失任务（安全优先）
    Drop,   // 静默丢弃（适合日志/采样场景）
    Error,  // 抛出运行时异常（适合金融/关键路径）
};

// ── 线程池：固定 worker 数量，可配置任务队列容量和溢出策略 ──
class ThreadPool {
public:
    explicit ThreadPool(std::string name, size_t workers,
                        size_t queueCap   = 100,
                        OverflowPolicy ov = OverflowPolicy::Block)
        : name_(std::move(name)), cap_(queueCap), overflow_(ov), stop_(false)
    {
        for (size_t i = 0; i < workers; i++)
            workers_.emplace_back([this] { run(); });
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_work_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    // 提交任务（void() lambda/function）
    // 返回 true 表示已入队，false 表示被 Drop 策略丢弃
    // 队满时根据 overflow_ 策略决定行为
    bool submit(std::function<void()> task) {
        std::unique_lock<std::mutex> lk(mu_);

        // 有界队列满时，根据策略决定行为
        if (cap_ > 0 && queue_.size() >= cap_) {
            switch (overflow_) {
            case OverflowPolicy::Block:
                // 阻塞直到有空位（安全优先，不丢数据）
                cv_send_.wait(lk, [this] { return queue_.size() < cap_ || stop_; });
                break;
            case OverflowPolicy::Drop:
                return false;   // 静默丢弃
            case OverflowPolicy::Error:
                throw std::runtime_error(
                    "thread pool '" + name_ + "' queue overflow (" +
                    std::to_string(cap_) + " tasks)");
            }
        }

        queue_.push_back(std::move(task));
        cv_work_.notify_one();
        return true;
    }

    const std::string& name()     const { return name_; }
    OverflowPolicy     overflow() const { return overflow_; }
    size_t             queueLen() const {
        std::unique_lock<std::mutex> lk(mu_);
        return queue_.size();
    }

private:
    void run() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_work_.wait(lk, [this] { return !queue_.empty() || stop_; });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
                cv_send_.notify_one();
            }
            task();
        }
    }

    std::string    name_;
    size_t         cap_;
    OverflowPolicy overflow_;
    bool           stop_;

    std::vector<std::thread>          workers_;
    mutable std::mutex                mu_;
    std::condition_variable           cv_work_;
    std::condition_variable           cv_send_;
    std::deque<std::function<void()>> queue_;
};
