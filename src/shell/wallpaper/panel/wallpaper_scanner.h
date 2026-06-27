#pragma once

#include "app/poll_source.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct WallpaperEntry {
  std::string name;
  std::filesystem::path absPath;
  bool isDir = false;
  // Cached on the scan worker so date sorting never stats per comparison.
  std::filesystem::file_time_type mtime;
  bool hasMtime = false;
};

struct WallpaperScanResult {
  std::string dir;
  std::vector<WallpaperEntry> entries;
  std::filesystem::file_time_type dirMtime;
  bool flatten = false;
};

// Walks wallpaper directories on a background worker thread. The directory walk
// (and its per-entry stat calls) used to run synchronously on the UI thread,
// freezing the shell on large folders. Results land in a main-thread cache and
// fire onComplete() via the poll loop. Mirrors ThumbnailService's worker +
// eventfd pattern.
class WallpaperScanner : public PollSource {
public:
  // Fired on the main thread after one or more scans land in the cache.
  using CompletionCallback = std::function<void()>;

  WallpaperScanner();
  ~WallpaperScanner() override;

  WallpaperScanner(const WallpaperScanner&) = delete;
  WallpaperScanner& operator=(const WallpaperScanner&) = delete;

  void setOnComplete(CompletionCallback callback) { m_onComplete = std::move(callback); }

  // Ensures (dir, flatten) is cached and fresh against the directory's mtime.
  // Returns true when a fresh cached result already exists (nothing queued);
  // returns false when a worker scan was queued — onComplete() fires later.
  // A missing/empty directory caches an empty result and returns true.
  bool requestScan(const std::filesystem::path& dir, bool flatten);

  // Cached result for (dir, flatten), or nullptr. Pure main-thread lookup.
  [[nodiscard]] const WallpaperScanResult* cached(const std::filesystem::path& dir, bool flatten) const;

  // True if a scan for (dir, flatten) is queued or in flight.
  [[nodiscard]] bool scanning(const std::filesystem::path& dir, bool flatten) const;

  // Drops all cached results so the next requestScan() re-walks.
  void invalidate();

  [[nodiscard]] int pollTimeoutMs() const override { return -1; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct CacheKey {
    std::string dir;
    bool flatten = false;
    bool operator==(const CacheKey& other) const noexcept { return flatten == other.flatten && dir == other.dir; }
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
      return std::hash<std::string>{}(k.dir) ^ (k.flatten ? 0x9e3779b9u : 0u);
    }
  };

  struct Job {
    std::string dir;
    bool flatten = false;
    std::filesystem::file_time_type dirMtime;
  };

  void workerLoop();
  void signalMain();

  int m_eventFd = -1;
  std::thread m_worker;
  std::atomic<bool> m_shutdown{false};

  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::deque<Job> m_jobQueue;

  std::mutex m_resultMutex;
  std::deque<WallpaperScanResult> m_results;

  // Main-thread state.
  std::unordered_map<CacheKey, WallpaperScanResult, CacheKeyHash> m_cache;
  std::unordered_set<CacheKey, CacheKeyHash> m_pending;
  CompletionCallback m_onComplete;
};
