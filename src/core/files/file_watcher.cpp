#include "core/files/file_watcher.h"

#include "core/log.h"

#include <algorithm>
#include <unistd.h>

namespace {
  constexpr Logger kLog("file-watcher");

  bool eventMatchesTrigger(FileWatcher::WatchTrigger trigger, std::uint32_t mask) {
    switch (trigger) {
    case FileWatcher::WatchTrigger::Modified:
      return (mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO)) != 0;
    case FileWatcher::WatchTrigger::WriteCompleted:
      return (mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) != 0;
    }
    return false;
  }
} // namespace

FileWatcher::WatchId
FileWatcher::watch(const std::filesystem::path& filePath, Callback callback, WatchTrigger trigger) {
  if (inotify.fd() < 0)
    return 0;

  auto dir = filePath.parent_path().string();
  auto filename = filePath.filename().string();

  int wd;
  auto it = m_dirToWd.find(dir);
  if (it != m_dirToWd.end()) {
    wd = it->second;
    m_dirWdRefCount[wd]++;
  } else {
    auto maybe_wd = inotify.watch(dir.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
    if (!maybe_wd.has_value()) {
      kLog.warn("failed to watch directory '{}'", dir);
      return 0;
    }
    wd = maybe_wd.value();
    m_dirToWd[dir] = wd;
    m_dirWdRefCount[wd] = 1;
  }

  auto id = m_nextId++;
  m_watches[id] = {std::move(filename), std::move(callback), wd, trigger};
  kLog.debug("watching '{}' (id {})", filePath.string(), id);
  return id;
}

void FileWatcher::unwatch(WatchId id) {
  auto it = m_watches.find(id);
  if (it == m_watches.end())
    return;

  int wd = it->second.dirWd;
  kLog.debug("unwatching id {}", id);
  m_watches.erase(it);

  auto refIt = m_dirWdRefCount.find(wd);
  if (refIt != m_dirWdRefCount.end() && --refIt->second <= 0) {
    inotify.unwatch(wd);
    m_dirWdRefCount.erase(refIt);
    std::erase_if(m_dirToWd, [wd](const auto& pair) { return pair.second == wd; });
  }
}

void FileWatcher::dispatch() {
  std::vector<WatchId> triggered;

  inotify.drain([this, &triggered](const inotify_event* event) {
    if (event->len > 0) {
      std::string_view name(event->name);
      for (auto& [id, entry] : m_watches) {
        if (entry.dirWd == event->wd
            && entry.filename == name
            && eventMatchesTrigger(entry.trigger, event->mask)
            && !std::ranges::contains(triggered, id))
          triggered.push_back(id);
      }
    }
  });

  auto now = std::chrono::steady_clock::now();
  for (auto id : triggered) {
    auto it = m_watches.find(id);
    if (it == m_watches.end())
      continue;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.lastFired);
    if (elapsed.count() < 100)
      continue;
    it->second.lastFired = now;
    it->second.callback();
  }
}
