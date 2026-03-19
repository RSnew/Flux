// hotswap.h — Flux AOT 热替换运行时
// 编译 .flux → .so，监听文件变更，dlopen 热替换
#pragma once

#include "lexer.h"
#include "parser.h"
#include "typechecker.h"
#include "hir.h"
#include "mir.h"
#include "codegen.h"
#include "watcher.h"

#include <dlfcn.h>
#include <string>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <thread>
#include <iostream>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════
// 热替换引擎
//
// 工作流程:
//   1. 首次编译 .flux → .so (v0)
//   2. dlopen 加载 .so，查找并执行 __main__
//   3. FileWatcher 监听源文件变更
//   4. 变更时: 重新编译 → .so (v1)，dlclose(v0)，dlopen(v1)
//   5. 执行新版本的 __main__，持久状态通过共享内存保留
// ═══════════════════════════════════════════════════════════

class HotSwapEngine {
public:
    using MainFn = void(*)();                     // __main__ 函数指针
    using NumericFn = double(*)(double*, int);     // 数值函数指针

    explicit HotSwapEngine(bool verbose = false)
        : verbose_(verbose) {}

    ~HotSwapEngine() {
        stop();
        unloadCurrent();
    }

    // 编译并加载一个 .flux 文件，返回是否成功
    bool loadFromSource(const std::string& filepath) {
        filepath_ = filepath;

        if (!compileToSharedLib()) return false;
        if (!loadSharedLib()) return false;

        return true;
    }

    // 执行当前加载的 __main__
    bool executeMain() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!mainFn_) {
            std::cerr << "\033[31m[hotswap] No __main__ loaded\033[0m\n";
            return false;
        }
        mainFn_();
        return true;
    }

    // 查找已加载的函数
    void* findSymbol(const std::string& name) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!handle_) return nullptr;
        return dlsym(handle_, name.c_str());
    }

    // 启动文件监听 + 自动热替换
    void startWatching() {
        if (watching_.exchange(true)) return;

        watcher_ = std::make_unique<FileWatcher>(filepath_, [this](const std::string& path) {
            (void)path;
            // 防抖: 忽略 100ms 内的重复事件
            auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastReload_).count();
                if (elapsed < 100) return;
                lastReload_ = now;
            }

            std::cerr << "\033[33m[hotswap] Source changed, recompiling...\033[0m\n";

            // 保存旧 .so 路径，再编译新版
            std::string oldSoPath;
            {
                std::lock_guard<std::mutex> lock(mu_);
                oldSoPath = soPath_;
            }

            // 重新编译
            if (!compileToSharedLib()) {
                std::cerr << "\033[31m[hotswap] Recompilation failed, keeping old version\033[0m\n";
                return;
            }

            // 热替换: 卸载旧的，加载新的
            {
                std::lock_guard<std::mutex> lock(mu_);
                // 关闭旧 handle
                if (handle_) {
                    dlclose(handle_);
                    handle_ = nullptr;
                    mainFn_ = nullptr;
                }
                // 删除旧 .so
                if (!oldSoPath.empty()) {
                    std::remove(oldSoPath.c_str());
                }
            }

            if (!loadSharedLib()) {
                std::cerr << "\033[31m[hotswap] Failed to load new version\033[0m\n";
                return;
            }

            reloadCount_++;
            std::cerr << "\033[32m[hotswap] Hot-swapped v" << reloadCount_
                      << " successfully\033[0m\n";

            // 执行新版本
            executeMain();
        });

        watcher_->start();
    }

    void stop() {
        if (watching_.exchange(false)) {
            if (watcher_) {
                watcher_->stop();
                watcher_.reset();
            }
        }
    }

    int reloadCount() const { return reloadCount_.load(); }

private:
    std::string filepath_;           // 源 .flux 文件路径
    std::string soPath_;             // 当前 .so 路径
    void*       handle_ = nullptr;   // dlopen 句柄
    MainFn      mainFn_ = nullptr;   // __main__ 指针
    int         version_ = 0;        // .so 版本号（用于文件名）

    std::mutex mu_;
    bool verbose_ = false;
    std::atomic<bool> watching_{false};
    std::atomic<int>  reloadCount_{0};
    std::unique_ptr<FileWatcher> watcher_;
    std::chrono::steady_clock::time_point lastReload_{};

    // 编译 .flux → .so
    bool compileToSharedLib() {
        try {
            // 读取源文件
            std::ifstream file(filepath_);
            if (!file) {
                std::cerr << "[hotswap] Cannot read: " << filepath_ << "\n";
                return false;
            }
            std::string src((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

            // Pipeline: Source → AST → TypeCheck → HIR → MIR → optimize
            Lexer  lexer(src);
            auto   tokens = lexer.tokenize();
            Parser parser(std::move(tokens));
            auto   program = parser.parse();

            TypeChecker checker;
            auto typeErrors = checker.check(program.get());
            if (!typeErrors.empty()) {
                std::cerr << "\033[31m[hotswap] Type errors:\033[0m\n";
                for (auto& e : typeErrors)
                    std::cerr << "  " << e.message << "\n";
                return false;
            }

            HIRLowering lowering;
            auto hir = lowering.lower(program.get());
            MIRBuilder builder;
            auto mir = builder.build(hir);
            mirOptimize(mir);

            // 编译到 .so（每次用新版本号，避免 dlopen 缓存）
            version_++;
            soPath_ = filepath_ + ".v" + std::to_string(version_) + ".so";

            CompileOptions opts;
            opts.arch = detectHostArch();
            opts.output = soPath_;
            opts.sharedLib = true;
            opts.verbose = verbose_;

            if (!compileToBinary(mir, opts)) {
                return false;
            }

            if (verbose_) {
                std::cerr << "[hotswap] Compiled → " << soPath_ << "\n";
            }
            return true;

        } catch (const std::exception& e) {
            std::cerr << "\033[31m[hotswap] Compile error: " << e.what() << "\033[0m\n";
            return false;
        }
    }

    // dlopen 加载 .so
    bool loadSharedLib() {
        std::lock_guard<std::mutex> lock(mu_);

        handle_ = dlopen(soPath_.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            std::cerr << "\033[31m[hotswap] dlopen failed: " << dlerror() << "\033[0m\n";
            return false;
        }

        // 查找 __main__
        mainFn_ = (MainFn)dlsym(handle_, "__main__");
        if (!mainFn_) {
            if (verbose_) {
                std::cerr << "[hotswap] Warning: no __main__ symbol found\n";
            }
        }

        if (verbose_) {
            std::cerr << "[hotswap] Loaded " << soPath_ << "\n";
        }
        return true;
    }

    void unloadCurrent() {
        std::lock_guard<std::mutex> lock(mu_);
        unloadCurrentLocked();
    }

    void unloadCurrentLocked() {
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
            mainFn_ = nullptr;
        }
        // 删除旧的 .so 文件
        if (!soPath_.empty()) {
            std::remove(soPath_.c_str());
        }
    }
};
