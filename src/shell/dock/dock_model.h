#pragma once

#include "system/desktop_entry.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CompositorPlatform;
struct DockConfig;
struct ToplevelInfo;
struct wl_output;
struct zwlr_foreign_toplevel_handle_v1;

namespace shell::dock {

  struct DockItemModel {
    DesktopEntry entry;
    std::string idLower;
    std::string startupWmClassLower;
    // Compositor app id + WM class used to find windows (matches taskbar policy).
    std::string windowLookupIdLower;
    std::string windowLookupWmClassLower;
    bool running = false;
    bool active = false;
    std::size_t instanceCount = 0;
    std::size_t minimizedCount = 0;
    bool allMinimized = false;
    // The item's windows, which get its icon's rectangle as their minimise target.
    std::vector<zwlr_foreign_toplevel_handle_v1*> handles;
  };

  struct DockSnapshot {
    wl_output* output = nullptr;
    wl_output* filterOutput = nullptr;
    std::string activeAppIdLower;
    std::vector<DockItemModel> items;
    std::uint64_t sourceSerial = 0;
  };

  struct DockModelDependencies {
    CompositorPlatform& platform;
    const DockConfig& config;
    wl_output* output = nullptr;
    const std::string& globalActiveIdLower;
    const std::vector<DesktopEntry>& pinnedEntries;
    std::uint64_t sourceSerial = 0;
  };

  wl_output* dockFilterOutput(const DockConfig& cfg, wl_output* instanceOutput);
  std::string currentActiveEntryIdLower(const CompositorPlatform& platform);
  bool refreshPinnedAppsIfNeeded(
      const DockConfig& cfg, std::vector<std::string>& lastPinnedConfig, std::vector<DesktopEntry>& pinnedEntries,
      std::uint64_t& modelSerial, std::uint64_t& entriesVersion
  );
  DockSnapshot buildDockSnapshot(DockModelDependencies deps);
  // Keeps unpinned running items in their previous order and appends new ones; pinned items stay in config order.
  void keepRunningItemOrder(const DockSnapshot& previous, DockSnapshot& next, std::size_t pinnedCount);
  bool sameDockItemSet(const DockSnapshot& a, const DockSnapshot& b);
  std::vector<ToplevelInfo>
  windowsForDockItem(CompositorPlatform& platform, const DockItemModel& item, wl_output* outputFilter);
  std::vector<ToplevelInfo> windowsForDockItem(
      CompositorPlatform& platform, std::string_view idLower, std::string_view wmClassLower, wl_output* outputFilter
  );

} // namespace shell::dock
