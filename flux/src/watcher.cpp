#include "watcher.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <stdexcept>
#include <iostream>
#include <libgen.h>
#include <cstring>

FileWatcher::FileWatcher(const std::string& filepath, Callback cb)
    : filepath_(filepath), callback_(std::move(cb)) {}

FileWatcher::~FileWatcher() { stop(); }

void FileWatcher::start() {
    running_ = true;
    thread_ = std::thread(&FileWatcher::watchLoop, this);
}

void FileWatcher::stop() {
    running_ = false;
    if (inotifyFd_ >= 0) { close(inotifyFd_); inotifyFd_ = -1; }
    if (thread_.joinable()) thread_.join();
}

void FileWatcher::watchLoop() {
    inotifyFd_ = inotify_init();
    if (inotifyFd_ < 0) {
        std::cerr << "[flux] inotify_init failed\n";
        return;
    }

    // 监听文件所在目录（编辑器通常是原子替换文件，监听目录更可靠）
    char pathCopy[4096];
    strncpy(pathCopy, filepath_.c_str(), sizeof(pathCopy)-1);
    std::string dir = dirname(pathCopy);

    int wd = inotify_add_watch(inotifyFd_, dir.c_str(),
                                IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0) {
        std::cerr << "[flux] inotify_add_watch failed\n";
        return;
    }

    char buf[4096];
    while (running_) {
        int len = read(inotifyFd_, buf, sizeof(buf));
        if (len <= 0) break;

        int i = 0;
        while (i < len) {
            auto* event = reinterpret_cast<inotify_event*>(buf + i);
            if (event->len > 0) {
                std::string changed = dir + "/" + event->name;
                if (changed == filepath_) {
                    callback_(filepath_);
                }
            }
            i += sizeof(inotify_event) + event->len;
        }
    }
    close(inotifyFd_);
    inotifyFd_ = -1;
}
