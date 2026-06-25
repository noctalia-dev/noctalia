#include "shell/dock/dock_model.h"

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "core/log.h"
#include "shell/dock/pinned_apps.h"
#include "system/app_identity.h"
#include "util/string_utils.h"
#include "wayland/wayland_toplevels.h"

#include <unordered_map>

namespace {

  constexpr Logger kLog("dock");

  [[nodiscard]] std::string
  taskbarStyleWmClassLower(std::string_view startupWmClass, std::string_view fallbackIdLower) {
    if (!startupWmClass.empty()) {
      return StringUtils::toLower(std::string(startupWmClass));
    }
    return std::string(fallbackIdLower);
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
      return StringUtils::toLower(app_identity::resolveRunningDesktopEntry(active->appId, desktopEntries()).id);
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
      for (const auto& entry : entries) {
        if (StringUtils::toLower(entry.id) == resolvedIdLower) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  std::vector<ToplevelInfo>
  windowsForDockItem(CompositorPlatform& platform, const DockItemModel& item, wl_output* outputFilter) {
    return windowsForDockItem(platform, item.windowLookupIdLower, item.windowLookupWmClassLower, outputFilter);
  }

  std::vector<ToplevelInfo> windowsForDockItem(
      CompositorPlatform& platform, std::string_view idLower, std::string_view wmClassLower, wl_output* outputFilter
  ) {
    return platform.windowsForApp(std::string(idLower), std::string(wmClassLower), outputFilter);
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
    const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, desktopEntries());

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
      if (deps.config.showDots || deps.config.showInstanceCount) {
        dockItem.instanceCount = windowsForDockItem(deps.platform, dockItem, snapshot.filterOutput).size();
      }
      snapshot.items.push_back(std::move(dockItem));
    }

    return snapshot;
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
