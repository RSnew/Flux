// concurrency.h — Flux 全局解释器锁 (GIL) + 异步基础设施 (Feature K)
//
// 线程模型：
//   - 同一时刻只有一个线程在执行 Flux 代码（GIL 保证）
//   - async/await 允许 I/O 密集型任务真正并发：
//     I/O 阻塞期间 GIL 会被临时释放，其他 Flux 线程趁机运行
//   - Chan 的 send/recv 在等待期间也会释放 GIL
#pragma once
#include <mutex>

// ── 全局解释器锁 ──────────────────────────────────────────
// 使用普通 mutex + 每线程深度计数实现"可重入"语义：
// 同一线程可多次进入解释器（递归 evalNode），只有最外层获取锁。
inline std::mutex g_interpGIL;
thread_local inline int g_gilDepth = 0;

// ── GILGuard — 进入解释器时自动加锁 ─────────────────────
struct GILGuard {
    bool toplevel;
    explicit GILGuard() : toplevel(g_gilDepth++ == 0) {
        if (toplevel) g_interpGIL.lock();
    }
    ~GILGuard() {
        if (--g_gilDepth == 0 && toplevel) g_interpGIL.unlock();
    }
    GILGuard(const GILGuard&) = delete;
    GILGuard& operator=(const GILGuard&) = delete;
};

// ── GILRelease — 阻塞等待前释放 GIL（让其他线程运行）───
struct GILRelease {
    int savedDepth;
    GILRelease() : savedDepth(g_gilDepth) {
        g_gilDepth = 0;
        if (savedDepth > 0) g_interpGIL.unlock();
    }
    ~GILRelease() {
        if (savedDepth > 0) g_interpGIL.lock();
        g_gilDepth = savedDepth;
    }
    GILRelease(const GILRelease&) = delete;
    GILRelease& operator=(const GILRelease&) = delete;
};
