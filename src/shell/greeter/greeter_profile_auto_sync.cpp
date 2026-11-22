#include "shell/greeter/greeter_profile_auto_sync.h"

#include "core/log.h"
#include "shell/greeter/greeter_profile_store.h"

#include <chrono>
#include <system_error>
#include <utility>

namespace {
  constexpr Logger kLog("greeter-auto-sync");
}

GreeterProfileAutoSync::GreeterProfileAutoSync(
    FileWatcher& watcher, std::filesystem::path path, std::function<greeter::GreeterSyncLaunch()> sync
)
    : m_watcher(watcher), m_path(std::move(path)), m_sync(std::move(sync)) {}

GreeterProfileAutoSync::~GreeterProfileAutoSync() { setEnabled(false); }

void GreeterProfileAutoSync::setEnabled(bool enabled) {
  if (!enabled) {
    m_timer.stop();
    if (m_watch) {
      m_watcher.unwatch(m_watch);
      m_watch = 0;
    }
    m_observed.reset();
    return;
  }
  if (m_watch) {
    return;
  }
  std::error_code ec;
  const auto parent = m_path.parent_path();
  if (std::filesystem::create_directories(parent, ec)) {
    std::filesystem::permissions(parent, std::filesystem::perms::owner_all, ec);
  }
  if (ec) {
    kLog.warn("cannot watch saved greeter profile: {}", ec.message());
    return;
  }
  acknowledgeSavedProfile();
  m_watch = m_watcher.watch(m_path, [this] { schedule(); }, FileWatcher::WatchTrigger::WriteCompleted);
  if (!m_watch) {
    kLog.warn("cannot watch saved greeter profile: {}", m_path.string());
  }
}

void GreeterProfileAutoSync::acknowledgeSavedProfile() {
  m_timer.stop();
  auto saved = greeter::readLocalProfile(m_path);
  m_observed = saved ? std::optional<GreeterProfile>(std::move(*saved)) : std::nullopt;
}

void GreeterProfileAutoSync::schedule() {
  m_timer.start(std::chrono::seconds(1), [this] {
    // Read after the debounce so bursts of atomic replacements use the latest file.
    auto saved = greeter::readLocalProfile(m_path);
    if (!saved) {
      m_observed.reset();
      kLog.warn("{}", saved.error());
      return;
    }
    if (m_observed && *m_observed == *saved) {
      return;
    }
    // Only Busy retries. Failure/cancellation needs a new edit or explicit Apply.
    if (m_sync() == greeter::GreeterSyncLaunch::Busy) {
      schedule();
    } else {
      m_observed = std::move(*saved);
    }
  });
}
