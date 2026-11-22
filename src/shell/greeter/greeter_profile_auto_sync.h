#pragma once

#include "core/files/file_watcher.h"
#include "core/timer_manager.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/greeter/greeter_profile.h"

#include <filesystem>
#include <functional>
#include <optional>

// Watches saved profiles only; drafts never enter the automatic Apply path.
class GreeterProfileAutoSync {
public:
  GreeterProfileAutoSync(
      FileWatcher& watcher, std::filesystem::path path, std::function<greeter::GreeterSyncLaunch()> sync
  );
  ~GreeterProfileAutoSync();
  void setEnabled(bool enabled);
  // An explicit Apply already handles these saved bytes, even if their watch event is pending.
  void acknowledgeSavedProfile();

private:
  void schedule();

  FileWatcher& m_watcher;
  std::filesystem::path m_path;
  std::function<greeter::GreeterSyncLaunch()> m_sync;
  FileWatcher::WatchId m_watch = 0;
  std::optional<GreeterProfile> m_observed;
  Timer m_timer;
};
