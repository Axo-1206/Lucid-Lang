/// @file cli/FileWatcher.hpp
/// @brief File watcher for hot‑reload.

#pragma once

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <filesystem>
#include <chrono>
#include <mutex>

namespace cli {

using FileChangeCallback = std::function<void(const std::string& filePath)>;

/**
 * @brief Simple file watcher using polling.
 *
 * In a production implementation, use platform‑specific APIs:
 *   - Windows: ReadDirectoryChangesW
 *   - Linux: inotify
 *   - macOS: FSEvents
 *
 * Polling is used here for simplicity and portability.
 */
class FileWatcher {
public:
    FileWatcher(const std::filesystem::path& rootDir, FileChangeCallback callback)
        : rootDir_(rootDir)
        , callback_(std::move(callback))
        , running_(false) {}

    ~FileWatcher() { stop(); }

    /// @brief Start watching (runs in a separate thread).
    void start() {
        if (running_) return;
        running_ = true;
        worker_ = std::thread(&FileWatcher::run, this);
    }

    /// @brief Stop watching.
    void stop() {
        if (!running_) return;
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    /// @brief Add a file to the watch list (only .luc files).
    void watchFile(const std::string& filePath) {
        std::lock_guard<std::mutex> lock(mutex_);
        watchedFiles_[filePath] = getLastWriteTime(filePath);
    }

    /// @brief Add multiple files to the watch list.
    void watchFiles(const std::vector<std::string>& filePaths) {
        for (const auto& path : filePaths) {
            watchFile(path);
        }
    }

    /// @brief Remove a file from the watch list.
    void unwatchFile(const std::string& filePath) {
        std::lock_guard<std::mutex> lock(mutex_);
        watchedFiles_.erase(filePath);
    }

    /// @brief Check if the watcher is running.
    bool isRunning() const { return running_; }

private:
    void run() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            std::lock_guard<std::mutex> lock(mutex_);

            for (auto& [filePath, lastTime] : watchedFiles_) {
                auto currentTime = getLastWriteTime(filePath);
                if (currentTime > lastTime) {
                    lastTime = currentTime;
                    if (callback_) {
                        // Convert absolute path to relative path
                        std::string relativePath = std::filesystem::relative(
                            filePath, rootDir_
                        ).string();
                        callback_(relativePath);
                    }
                }
            }
        }
    }

    std::time_t getLastWriteTime(const std::string& filePath) const {
        try {
            auto ftime = std::filesystem::last_write_time(filePath);
            return std::chrono::duration_cast<std::chrono::seconds>(
                ftime.time_since_epoch()
            ).count();
        } catch (...) {
            return 0;
        }
    }

    std::filesystem::path rootDir_;
    FileChangeCallback callback_;
    std::thread worker_;
    std::atomic<bool> running_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::time_t> watchedFiles_;
};

} // namespace cli