#include "system/icon_theme_poll_source.h"

#include "core/inotify/inotify.h"
#include "system/icon_resolver.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace {
  using Clock = std::chrono::steady_clock;

  constexpr std::chrono::seconds kCheckInterval{60};
  constexpr std::chrono::milliseconds kDebounceInterval{100};
  constexpr std::uint32_t kCompletedFileChange = IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
  constexpr std::uint32_t kDirectoryChange = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;

  enum WatchRole : std::uint8_t {
    WATCH_BASE_DIR = 1U << 0U,
    WATCH_THEME_ROOT = 1U << 1U,
    WATCH_ICON_DIR = 1U << 2U,
    WATCH_SETTINGS_DIR = 1U << 3U,
  };

  struct DesiredWatch {
    std::uint8_t roles = 0;
    std::unordered_set<std::string> settingsFiles;
  };

  struct ActiveWatch {
    std::uint8_t roles = 0;
    std::unordered_set<std::string> settingsFiles;
  };

  bool hasRole(std::uint8_t roles, WatchRole role) { return (roles & static_cast<std::uint8_t>(role)) != 0; }

  bool isIconFile(std::string_view name) { return name.ends_with(".svg") || name.ends_with(".png"); }

  int millisecondsUntil(Clock::time_point deadline, Clock::time_point now) {
    if (now >= deadline) {
      return 0;
    }
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count());
  }
} // namespace

struct IconThemePollSource::Impl {
  Inotify inotify;
  std::function<void()> changeCallback;
  std::unordered_map<int, ActiveWatch> watches;
  Clock::time_point nextCheck = Clock::now() + kCheckInterval;
  Clock::time_point debounceUntil;
  bool contentPending = false;
  bool metadataPending = false;

  Impl() { resetWatches(); }

  void resetWatches() {
    for (const auto& watch : watches) {
      inotify.unwatch(watch.first);
    }
    watches.clear();

    std::unordered_map<std::string, DesiredWatch> desired;
    const auto paths = IconResolver::themeWatchPaths();
    for (const auto& path : paths.baseDirs) {
      desired[path].roles |= WATCH_BASE_DIR;
    }
    for (const auto& path : paths.themeRoots) {
      desired[path].roles |= WATCH_THEME_ROOT;
    }
    for (const auto& path : paths.iconDirs) {
      desired[path].roles |= WATCH_ICON_DIR;
    }
    for (const auto& path : paths.settingsFiles) {
      const fs::path settingsPath(path);
      auto& watch = desired[settingsPath.parent_path().string()];
      watch.roles |= WATCH_SETTINGS_DIR;
      watch.settingsFiles.emplace(settingsPath.filename().string());
    }

    constexpr std::uint32_t mask =
        IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
    for (auto& [path, watch] : desired) {
      std::error_code ec;
      if (!fs::is_directory(path, ec)) {
        continue;
      }
      const auto wd = inotify.watch(path, mask);
      if (wd.has_value()) {
        auto& active = watches[*wd];
        active.roles |= watch.roles;
        active.settingsFiles.insert(watch.settingsFiles.begin(), watch.settingsFiles.end());
      }
    }
  }

  void queueChange(bool content, bool metadata) {
    contentPending = contentPending || content;
    metadataPending = metadataPending || metadata;
    debounceUntil = Clock::now() + kDebounceInterval;
  }

  void handleEvent(const inotify_event* event) {
    if ((event->mask & IN_Q_OVERFLOW) != 0) {
      queueChange(true, true);
      return;
    }
    const auto it = watches.find(event->wd);
    if (it == watches.end()) {
      return;
    }

    const auto& watch = it->second;
    const std::string_view name = event->len > 0 ? std::string_view(event->name) : std::string_view{};
    const bool completedFileChange = (event->mask & kCompletedFileChange) != 0;
    const bool directoryChange = (event->mask & IN_ISDIR) != 0 && (event->mask & kDirectoryChange) != 0;
    bool content = false;
    bool metadata = (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0;

    if (hasRole(watch.roles, WATCH_SETTINGS_DIR)
        && completedFileChange
        && watch.settingsFiles.contains(std::string(name))) {
      metadata = true;
    }
    if (hasRole(watch.roles, WATCH_BASE_DIR) && directoryChange) {
      metadata = true;
    }
    if (hasRole(watch.roles, WATCH_THEME_ROOT) && completedFileChange) {
      metadata = metadata || name == "index.theme";
    }
    if (hasRole(watch.roles, WATCH_ICON_DIR)) {
      content = content
          || (completedFileChange && isIconFile(name))
          || (hasRole(watch.roles, WATCH_THEME_ROOT) && directoryChange);
    }
    if (content || metadata) {
      queueChange(content, metadata);
    }
  }

  void applyPendingChange() {
    const bool hadContent = std::exchange(contentPending, false);
    const bool hadMetadata = std::exchange(metadataPending, false);
    bool changed = false;
    if (hadContent) {
      IconResolver::invalidateThemeContent();
      changed = true;
    } else if (hadMetadata) {
      changed = IconResolver::checkThemePlanChanged();
    }
    resetWatches();
    if (changed && changeCallback) {
      changeCallback();
    }
  }
};

IconThemePollSource::IconThemePollSource() : m_impl(std::make_unique<Impl>()) {}

IconThemePollSource::~IconThemePollSource() = default;

void IconThemePollSource::setChangeCallback(std::function<void()> callback) {
  m_impl->changeCallback = std::move(callback);
}

int IconThemePollSource::pollTimeoutMs() const {
  const auto now = Clock::now();
  int timeout = millisecondsUntil(m_impl->nextCheck, now);
  if (m_impl->contentPending || m_impl->metadataPending) {
    timeout = std::min(timeout, millisecondsUntil(m_impl->debounceUntil, now));
  }
  return timeout;
}

void IconThemePollSource::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_impl->inotify.fd() >= 0 && startIdx < fds.size() && (fds[startIdx].revents & POLLIN) != 0) {
    m_impl->inotify.drain([this](const inotify_event* event) { m_impl->handleEvent(event); });
  }

  auto now = Clock::now();
  if ((m_impl->contentPending || m_impl->metadataPending) && now >= m_impl->debounceUntil) {
    m_impl->applyPendingChange();
    now = Clock::now();
  }
  if (now >= m_impl->nextCheck) {
    m_impl->nextCheck = now + kCheckInterval;
    const bool changed = IconResolver::checkThemeChanged();
    if (changed) {
      m_impl->resetWatches();
      if (m_impl->changeCallback) {
        m_impl->changeCallback();
      }
    }
  }
}

void IconThemePollSource::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_impl->inotify.fd() >= 0) {
    fds.push_back({.fd = m_impl->inotify.fd(), .events = POLLIN, .revents = 0});
  }
}
