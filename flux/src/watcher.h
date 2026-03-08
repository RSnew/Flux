#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>

// 跨平台文件监听：Linux 使用 inotify，macOS 使用 kqueue
// 回调在独立线程触发，调用方负责线程安全
class FileWatcher {
public:
    using Callback = std::function<void(const std::string& path)>;

    FileWatcher(const std::string& filepath, Callback cb);
    ~FileWatcher();

    void start();
    void stop();

private:
    std::string       filepath_;
    Callback          callback_;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    int               watchFd_ = -1;

    void watchLoop();
};
