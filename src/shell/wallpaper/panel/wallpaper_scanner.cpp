#include "shell/wallpaper/panel/wallpaper_scanner.h"

#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("wp-scan");

  bool hasImageExtension(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    for (char& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp" || ext == ".gif";
  }

  // Pull the entry's modification time from the cached directory_entry metadata
  // so date sorting later never has to stat again.
  void cacheMtime(const std::filesystem::directory_entry& entry, WallpaperEntry& out) {
    std::error_code ec;
    const auto mtime = entry.last_write_time(ec);
    if (!ec) {
      out.mtime = mtime;
      out.hasMtime = true;
    }
  }

  void collectFlat(const std::filesystem::path& dir, std::vector<WallpaperEntry>& out) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec
         );
         !ec && it != std::filesystem::end(it); it.increment(ec)) {
      if (ec) {
        break;
      }
      const auto& entry = *it;
      if (entry.path().filename().string().starts_with('.')) {
        if (entry.is_directory()) {
          it.disable_recursion_pending();
        }
        continue;
      }

      std::error_code typeEc;
      if (!entry.is_regular_file(typeEc) || typeEc) {
        continue;
      }
      if (!hasImageExtension(entry.path())) {
        continue;
      }
      WallpaperEntry e;
      e.name = entry.path().filename().string();
      e.absPath = entry.path();
      e.isDir = false;
      cacheMtime(entry, e);
      out.push_back(std::move(e));
    }
  }

  void collectShallow(const std::filesystem::path& dir, std::vector<WallpaperEntry>& out) {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        break;
      }
      if (entry.path().filename().string().starts_with('.')) {
        continue;
      }

      std::error_code typeEc;
      if (entry.is_directory(typeEc) && !typeEc) {
        WallpaperEntry e;
        e.name = entry.path().filename().string();
        e.absPath = entry.path();
        e.isDir = true;
        out.push_back(std::move(e));
        continue;
      }
      if (entry.is_regular_file(typeEc) && !typeEc && hasImageExtension(entry.path())) {
        WallpaperEntry e;
        e.name = entry.path().filename().string();
        e.absPath = entry.path();
        e.isDir = false;
        cacheMtime(entry, e);
        out.push_back(std::move(e));
      }
    }
  }

  void sortEntries(std::vector<WallpaperEntry>& entries) {
    // Directories first, then files; natural case-insensitive name order.
    std::ranges::sort(entries, [](const WallpaperEntry& a, const WallpaperEntry& b) {
      if (a.isDir != b.isDir) {
        return a.isDir;
      }
      return StringUtils::naturalCaseInsensitiveLess(a.name, b.name);
    });
  }

  WallpaperScanResult scanDirectory(const std::string& dir, bool flatten, std::filesystem::file_time_type dirMtime) {
    WallpaperScanResult result;
    result.dir = dir;
    result.flatten = flatten;
    result.dirMtime = dirMtime;
    if (flatten) {
      collectFlat(dir, result.entries);
    } else {
      collectShallow(dir, result.entries);
    }
    sortEntries(result.entries);
    return result;
  }

} // namespace

WallpaperScanner::WallpaperScanner() {
  m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_eventFd < 0) {
    kLog.warn("failed to create eventfd; wallpaper scans will not wake the loop");
  }
  m_worker = std::thread([this]() { workerLoop(); });
}

WallpaperScanner::~WallpaperScanner() {
  {
    std::scoped_lock lock(m_queueMutex);
    m_shutdown.store(true);
  }
  m_queueCv.notify_all();
  if (m_worker.joinable()) {
    m_worker.join();
  }
  if (m_eventFd >= 0) {
    ::close(m_eventFd);
    m_eventFd = -1;
  }
}

bool WallpaperScanner::requestScan(const std::filesystem::path& dir, bool flatten) {
  CacheKey key{dir.string(), flatten};

  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) {
    // Cache an empty result so cached() reports "scanned, nothing there".
    WallpaperScanResult empty;
    empty.dir = key.dir;
    empty.flatten = flatten;
    m_cache.insert_or_assign(key, std::move(empty));
    m_pending.erase(key);
    return true;
  }

  const auto mtime = std::filesystem::last_write_time(dir, ec);
  if (ec) {
    return true;
  }

  if (const auto it = m_cache.find(key); it != m_cache.end() && it->second.dirMtime == mtime) {
    return true; // fresh
  }

  if (!m_pending.insert(key).second) {
    return false; // already queued/in flight
  }

  {
    std::scoped_lock lock(m_queueMutex);
    m_jobQueue.push_back(Job{.dir = key.dir, .flatten = flatten, .dirMtime = mtime});
  }
  m_queueCv.notify_one();
  return false;
}

const WallpaperScanResult* WallpaperScanner::cached(const std::filesystem::path& dir, bool flatten) const {
  const auto it = m_cache.find(CacheKey{dir.string(), flatten});
  return it == m_cache.end() ? nullptr : &it->second;
}

bool WallpaperScanner::scanning(const std::filesystem::path& dir, bool flatten) const {
  return m_pending.contains(CacheKey{dir.string(), flatten});
}

void WallpaperScanner::invalidate() { m_cache.clear(); }

void WallpaperScanner::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_eventFd < 0) {
    return;
  }
  fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0});
}

void WallpaperScanner::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_eventFd < 0 || startIdx >= fds.size()) {
    return;
  }
  if ((fds[startIdx].revents & POLLIN) == 0) {
    return;
  }

  std::uint64_t ignored = 0;
  while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
  }

  std::deque<WallpaperScanResult> results;
  {
    std::scoped_lock lock(m_resultMutex);
    results = std::move(m_results);
    m_results.clear();
  }
  if (results.empty()) {
    return;
  }

  for (auto& result : results) {
    CacheKey key{result.dir, result.flatten};
    m_pending.erase(key);
    m_cache.insert_or_assign(std::move(key), std::move(result));
  }

  if (m_onComplete) {
    m_onComplete();
  }
}

void WallpaperScanner::signalMain() {
  if (m_eventFd < 0) {
    return;
  }
  const std::uint64_t one = 1;
  const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
  if (written < 0 && errno != EAGAIN) {
    kLog.warn("failed to signal wallpaper scan eventfd: errno={}", errno);
  }
}

void WallpaperScanner::workerLoop() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueCv.wait(lock, [this]() { return m_shutdown.load() || !m_jobQueue.empty(); });
      if (m_shutdown.load()) {
        return;
      }
      job = std::move(m_jobQueue.front());
      m_jobQueue.pop_front();
    }

    WallpaperScanResult result = scanDirectory(job.dir, job.flatten, job.dirMtime);
    kLog.debug("scanned {} ({}): {} entries", result.dir, result.flatten ? "flat" : "shallow", result.entries.size());
    {
      std::scoped_lock lock(m_resultMutex);
      m_results.push_back(std::move(result));
    }
    signalMain();
  }
}
