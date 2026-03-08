#include "watcher.h"
#include <unistd.h>
#include <stdexcept>
#include <iostream>
#include <cstring>

#ifdef __linux__
#include <sys/inotify.h>
#include <libgen.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

FileWatcher::FileWatcher(const std::string& filepath, Callback cb)
    : filepath_(filepath), callback_(std::move(cb)) {}

FileWatcher::~FileWatcher() { stop(); }

void FileWatcher::start() {
    running_ = true;
    thread_ = std::thread(&FileWatcher::watchLoop, this);
}

void FileWatcher::stop() {
    running_ = false;
    if (watchFd_ >= 0) { close(watchFd_); watchFd_ = -1; }
    if (thread_.joinable()) thread_.join();
}

#ifdef __linux__

void FileWatcher::watchLoop() {
    watchFd_ = inotify_init();
    if (watchFd_ < 0) {
        std::cerr << "[flux] inotify_init failed\n";
        return;
    }

    // 监听文件所在目录（编辑器通常是原子替换文件，监听目录更可靠）
    char pathCopy[4096];
    strncpy(pathCopy, filepath_.c_str(), sizeof(pathCopy)-1);
    pathCopy[sizeof(pathCopy)-1] = '\0';
    std::string dir = dirname(pathCopy);

    int wd = inotify_add_watch(watchFd_, dir.c_str(),
                                IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd < 0) {
        std::cerr << "[flux] inotify_add_watch failed\n";
        return;
    }

    char buf[4096];
    while (running_) {
        int len = read(watchFd_, buf, sizeof(buf));
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
    close(watchFd_);
    watchFd_ = -1;
}

#elif defined(__APPLE__)

void FileWatcher::watchLoop() {
    watchFd_ = kqueue();
    if (watchFd_ < 0) {
        std::cerr << "[flux] kqueue failed\n";
        return;
    }

    int fileFd = open(filepath_.c_str(), O_RDONLY);
    if (fileFd < 0) {
        std::cerr << "[flux] cannot open file for watching: " << filepath_ << "\n";
        close(watchFd_);
        watchFd_ = -1;
        return;
    }

    struct kevent change;
    EV_SET(&change, fileFd, EVFILT_VNODE,
           EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, nullptr);

    struct timespec timeout;
    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;

    while (running_) {
        struct kevent event;
        int nev = kevent(watchFd_, &change, 1, &event, 1, &timeout);
        if (nev < 0) break;
        if (nev == 0) continue; // timeout, check running_ again

        if (event.fflags & (NOTE_WRITE | NOTE_RENAME | NOTE_DELETE)) {
            callback_(filepath_);

            // 文件被删除或重命名时（编辑器原子保存），重新打开
            if (event.fflags & (NOTE_DELETE | NOTE_RENAME)) {
                close(fileFd);
                // 等待新文件出现
                for (int retry = 0; retry < 50 && running_; ++retry) {
                    usleep(100000); // 100ms
                    fileFd = open(filepath_.c_str(), O_RDONLY);
                    if (fileFd >= 0) break;
                }
                if (fileFd < 0) break;
                EV_SET(&change, fileFd, EVFILT_VNODE,
                       EV_ADD | EV_ENABLE | EV_CLEAR,
                       NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, nullptr);
            }
        }
    }
    close(fileFd);
    close(watchFd_);
    watchFd_ = -1;
}

#else

// 回退：轮询方式检测文件修改
void FileWatcher::watchLoop() {
    struct stat st;
    if (stat(filepath_.c_str(), &st) != 0) {
        std::cerr << "[flux] cannot stat file: " << filepath_ << "\n";
        return;
    }
    auto lastMod = st.st_mtime;

    while (running_) {
        usleep(500000); // 500ms
        if (stat(filepath_.c_str(), &st) == 0 && st.st_mtime != lastMod) {
            lastMod = st.st_mtime;
            callback_(filepath_);
        }
    }
}

#endif
