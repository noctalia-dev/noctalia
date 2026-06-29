#pragma once

#include "app/poll_source.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class FileWatcher {
public:
  using WatchId = std::uint64_t;
  using Callback = std::function<void()>;
  enum class WatchTrigger : std::uint8_t {
    Modified,
    WriteCompleted,
  };

  FileWatcher();
  ~FileWatcher();

  FileWatcher(const FileWatcher&) = delete;
  FileWatcher& operator=(const FileWatcher&) = delete;

  WatchId
  watch(const std::filesystem::path& filePath, Callback callback, WatchTrigger trigger = WatchTrigger::Modified);
  void unwatch(WatchId id);

  [[nodiscard]] int fd() const noexcept { return m_inotifyFd; }
  void dispatch();

private:
  struct WatchEntry {
    std::string filename;
    Callback callback;
    int dirWd;
    WatchTrigger trigger = WatchTrigger::Modified;
    std::chrono::steady_clock::time_point lastFired;
  };

  int m_inotifyFd = -1;
  WatchId m_nextId = 1;
  std::unordered_map<WatchId, WatchEntry> m_watches;
  std::unordered_map<int, int> m_dirWdRefCount;
  std::unordered_map<std::string, int> m_dirToWd;
};

class FileWatchPollSource final : public PollSource {
public:
  explicit FileWatchPollSource(FileWatcher& watcher) : m_watcher(watcher) {}

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (m_watcher.fd() >= 0 && (fds[startIdx].revents & POLLIN) != 0)
      m_watcher.dispatch();
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (m_watcher.fd() >= 0)
      fds.push_back({.fd = m_watcher.fd(), .events = POLLIN, .revents = 0});
  }

private:
  FileWatcher& m_watcher;
};
