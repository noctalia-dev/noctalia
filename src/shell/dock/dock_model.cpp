#include "shell/dock/dock_model.h"

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "core/log.h"
#include "shell/dock/pinned_apps.h"
#include "shell/wayland/wayland_toplevels.h"
#include "system/app_identity.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

  constexpr Logger kLog("dock");

  std::string taskbarStyleWmClassLower(std::string_view startupWmClass, std::string_view fallbackIdLower) {
    if (!startupWmClass.empty()) {
      return StringUtils::toLower(startupWmClass);
    }
    return std::string(fallbackIdLower);
  }

  // Desktop-entry resolution ranks every entry per app id. Docks rebuild their snapshot on every
  // frame, almost always with the same running apps, so results are kept per input (one slot per dock).
  constexpr std::size_t kResolveCacheSlots = 4;

  const std::vector<app_identity::ResolvedRunningApp>&
  resolveRunningAppsCached(std::span<const std::string> runningIds, std::span<const DesktopEntry> pinnedEntries) {
    struct Key {
      std::uint64_t entriesVersion = 0;
      std::vector<std::string> runningIds;
      std::vector<std::string> pinnedIds;

      bool operator==(const Key&) const = default;
    };
    struct Slot {
      Key key;
      std::vector<app_identity::ResolvedRunningApp> resolved;
    };
    static std::vector<Slot> cache;

    Key key{.entriesVersion = desktopEntriesVersion(), .runningIds = {runningIds.begin(), runningIds.end()}};
    key.pinnedIds.reserve(pinnedEntries.size());
    for (const auto& entry : pinnedEntries) {
      key.pinnedIds.push_back(entry.id);
    }
    for (const Slot& slot : cache) {
      if (slot.key == key) {
        return slot.resolved;
      }
    }
    if (cache.size() >= kResolveCacheSlots) {
      cache.erase(cache.begin());
    }
    auto resolved = app_identity::resolveRunningApps(runningIds, desktopEntries(), pinnedEntries);
    cache.push_back(Slot{.key = std::move(key), .resolved = std::move(resolved)});
    return cache.back().resolved;
  }

  std::string resolveRunningEntryIdLowerCached(std::string_view appId) {
    static std::uint64_t cachedVersion = 0;
    static std::string cachedAppId;
    static std::string cachedIdLower;
    static bool cached = false;
    const std::uint64_t version = desktopEntriesVersion();
    if (!cached || version != cachedVersion || appId != cachedAppId) {
      cachedIdLower = StringUtils::toLower(app_identity::resolveRunningDesktopEntry(appId, desktopEntries()).id);
      cachedAppId = std::string(appId);
      cachedVersion = version;
      cached = true;
    }
    return cachedIdLower;
  }

} // namespace

namespace shell::dock {

  wl_output* dockFilterOutput(const DockConfig& cfg, wl_output* instanceOutput) {
    if (!cfg.activeMonitorOnly) {
      return nullptr;
    }
    return instanceOutput;
  }

  std::string currentActiveEntryIdLower(const CompositorPlatform& platform) {
    if (const auto active = platform.activeToplevel(); active.has_value()) {
      return resolveRunningEntryIdLowerCached(active->appId);
    }
    return {};
  }

  bool refreshPinnedAppsIfNeeded(
      const DockConfig& cfg, std::vector<std::string>& lastPinnedConfig, std::vector<DesktopEntry>& pinnedEntries,
      std::uint64_t& modelSerial, std::uint64_t& entriesVersion
  ) {
    if (desktopEntriesVersion() == entriesVersion && cfg.pinned == lastPinnedConfig) {
      return false;
    }

    lastPinnedConfig = cfg.pinned;
    entriesVersion = desktopEntriesVersion();
    pinnedEntries = pinned_apps::resolveEntries(cfg.pinned);

    ++modelSerial;
    kLog.debug("pinned app list: {} entries", pinnedEntries.size());
    return true;
  }

  namespace {

    bool
    alreadyListsResolvedEntry(const std::vector<DesktopEntry>& entries, const app_identity::ResolvedRunningApp& run) {
      const std::string resolvedIdLower = StringUtils::toLower(run.entry.id);
      return std::ranges::any_of(entries, [&](const auto& entry) {
        return StringUtils::toLower(entry.id) == resolvedIdLower;
      });
    }

  } // namespace

  std::vector<ToplevelInfo>
  windowsForDockItem(CompositorPlatform& platform, const DockItemModel& item, wl_output* outputFilter) {
    return windowsForDockItem(platform, item.windowLookupIdLower, item.windowLookupWmClassLower, outputFilter);
  }

  std::vector<ToplevelInfo> windowsForDockItem(
      CompositorPlatform& platform, std::string_view idLower, std::string_view wmClassLower, wl_output* outputFilter
  ) {
    return platform.enrichedWindowsForApp(std::string(idLower), std::string(wmClassLower), outputFilter);
  }

  DockSnapshot buildDockSnapshot(DockModelDependencies deps) {
    DockSnapshot snapshot;
    snapshot.output = deps.output;
    snapshot.filterOutput = dockFilterOutput(deps.config, deps.output);
    snapshot.sourceSerial = deps.sourceSerial;

    wl_output* const activeOutput = deps.platform.activeToplevelOutput();
    snapshot.activeAppIdLower =
        (deps.config.activeMonitorOnly && activeOutput != deps.output) ? std::string{} : deps.globalActiveIdLower;

    const auto runningIds =
        deps.config.showRunning ? deps.platform.runningAppIds(snapshot.filterOutput) : std::vector<std::string>{};
    const auto& resolvedRunning = resolveRunningAppsCached(runningIds, deps.pinnedEntries);

    std::unordered_map<std::string, std::string> compositorIdByEntryId;
    compositorIdByEntryId.reserve(resolvedRunning.size());
    for (const auto& run : resolvedRunning) {
      compositorIdByEntryId.emplace(StringUtils::toLower(run.entry.id), run.runningLower);
    }

    std::vector<DesktopEntry> itemEntries = deps.pinnedEntries;
    if (deps.config.showRunning) {
      for (const auto& run : resolvedRunning) {
        if (!alreadyListsResolvedEntry(itemEntries, run)) {
          itemEntries.push_back(run.entry);
        }
      }
    }

    snapshot.items.reserve(itemEntries.size());
    for (const auto& entry : itemEntries) {
      DockItemModel dockItem;
      dockItem.entry = entry;
      dockItem.idLower = StringUtils::toLower(entry.id);
      dockItem.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);

      if (const auto it = compositorIdByEntryId.find(dockItem.idLower); it != compositorIdByEntryId.end()) {
        dockItem.running = true;
        dockItem.windowLookupIdLower = it->second;
        dockItem.windowLookupWmClassLower = taskbarStyleWmClassLower(entry.startupWmClass, it->second);
      } else {
        dockItem.running = false;
        dockItem.windowLookupIdLower = dockItem.idLower;
        dockItem.windowLookupWmClassLower = taskbarStyleWmClassLower(entry.startupWmClass, dockItem.idLower);
      }

      dockItem.active = !snapshot.activeAppIdLower.empty() && snapshot.activeAppIdLower == dockItem.idLower;
      const auto windows = windowsForDockItem(deps.platform, dockItem, snapshot.filterOutput);
      dockItem.running = dockItem.running || !windows.empty();
      dockItem.instanceCount = windows.size();
      for (const auto& window : windows) {
        if (window.minimizeState.has_value() && window.minimizeState->minimized) {
          ++dockItem.minimizedCount;
        }
      }
      dockItem.allMinimized = !windows.empty() && dockItem.minimizedCount == windows.size();
      if (dockItem.allMinimized) {
        dockItem.active = false;
      }
      snapshot.items.push_back(std::move(dockItem));
    }

    return snapshot;
  }

  void keepRunningItemOrder(const DockSnapshot& previous, DockSnapshot& next, std::size_t pinnedCount) {
    std::unordered_map<std::string_view, std::size_t> previousPositions;
    previousPositions.reserve(previous.items.size());
    for (std::size_t i = 0; i < previous.items.size(); ++i) {
      previousPositions.emplace(previous.items[i].idLower, i);
    }
    const auto position = [&](const DockItemModel& item) {
      const auto it = previousPositions.find(item.idLower);
      return it != previousPositions.end() ? it->second : previous.items.size();
    };
    auto runningItems = std::span(next.items).subspan(pinnedCount);
    std::ranges::stable_sort(runningItems, [&](const auto& a, const auto& b) { return position(a) < position(b); });
  }

  bool sameDockItemSet(const DockSnapshot& a, const DockSnapshot& b) {
    if (a.items.size() != b.items.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.items.size(); ++i) {
      if (a.items[i].entry.id != b.items[i].entry.id) {
        return false;
      }
      if (a.items[i].idLower != b.items[i].idLower) {
        return false;
      }
      if (a.items[i].startupWmClassLower != b.items[i].startupWmClassLower) {
        return false;
      }
    }
    return true;
  }

} // namespace shell::dock
