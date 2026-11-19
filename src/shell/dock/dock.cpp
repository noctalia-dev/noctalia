#include "shell/dock/dock.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/dock/dock_context_menu.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "shell/dock/dock_items.h"
#include "shell/dock/dock_model.h"
#include "shell/dock/dock_stack.h"
#include "shell/dock/pinned_apps.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/wayland/file_drag.h"
#include "shell/wayland/layer_surface.h"
#include "shell/wayland/wayland_toplevels.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/process.h"
#include "ui/style.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <gio/gio.h>
#include <optional>
#include <unordered_map>
#include <wayland-client-core.h>

namespace {

  constexpr Logger kLog("dock");

  // Quotes one argument for a desktop-entry Exec line.
  std::string execQuote(std::string_view arg) {
    std::string quoted = "\"";
    for (const char c : arg) {
      if (c == '"' || c == '`' || c == '$' || c == '\\') {
        quoted.push_back('\\');
      }
      quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
  }

  bool isBuiltInFolderItem(const DesktopEntry& entry, const DockConfig& cfg) {
    return shell::dock::pinned_apps::isTrash(entry)
        || (shell::dock::pinned_apps::isStack(entry) && !shell::dock::pinned_apps::containsEntry(cfg.pinned, entry));
  }

  // trash:/// when an app handles it, so the trash opens with restore; otherwise the trash's files folder.
  std::string trashLocation(const DesktopEntry& trash) {
    GAppInfo* handler = g_app_info_get_default_for_uri_scheme("trash");
    if (handler == nullptr) {
      return trash.workingDir;
    }
    g_object_unref(handler);
    return "trash:///";
  }

  void assertDockInitialized(
      [[maybe_unused]] const CompositorPlatform* platform, [[maybe_unused]] const ConfigService* config,
      [[maybe_unused]] const RenderContext* renderContext
  ) {
    assert(platform != nullptr);
    assert(config != nullptr);
    assert(renderContext != nullptr);
  }

  void assertDockCoreInitialized(
      [[maybe_unused]] const CompositorPlatform* platform, [[maybe_unused]] const ConfigService* config
  ) {
    assert(platform != nullptr);
    assert(config != nullptr);
  }

  desktop_entry_launch::LaunchOptions
  dockLaunchOptions(const CompositorPlatform& platform, const ConfigService& config, const DesktopEntry& entry = {}) {
    std::string token;
    if (platform.hasXdgActivation()) {
      // No layer-shell surface — binding it can misplace first launches on Hyprland.
      token = platform.requestActivationToken(nullptr);
    }
    return desktop_entry_launch::LaunchOptions{
        .activationToken = std::move(token),
        .runAsSystemdService = config.config().shell.launchAppsAsSystemdServices,
        .customCommand = config.config().shell.launchAppsCustomCommand,
        .dbusActivatable = entry.dbusActivatable,
        .dbusAppId = entry.id,
    };
  }

  template <typename T> void appendOptionalStackPart(std::string& out, const std::optional<T>& value) {
    out += value.has_value() ? std::format("{}", *value) : "-";
    out.push_back('\x1F');
  }

  std::vector<std::string> barLayerStackSignature(const Config& config) {
    std::vector<std::string> signature;
    signature.reserve(config.bars.size());

    for (const auto& bar : config.bars) {
      std::string item = std::format(
          "{}\x1F{}\x1F{}\x1F{}\x1F{}\x1F{}\x1F{}\x1F{}\x1F{}\x1F{}", bar.name, bar.position, bar.enabled, bar.autoHide,
          bar.reserveSpace, bar.layer, bar.thickness, formatLength(bar.marginEnds), bar.marginEdge, bar.shadow
      );

      item.push_back('\x1e');
      for (const auto& override : bar.monitorOverrides) {
        item += override.match;
        item.push_back('\x1F');
        appendOptionalStackPart(item, override.enabled);
        appendOptionalStackPart(item, override.autoHide);
        appendOptionalStackPart(item, override.reserveSpace);
        appendOptionalStackPart(item, override.layer);
        appendOptionalStackPart(item, override.thickness);
        appendOptionalStackPart(item, override.marginEnds.transform(formatLength));
        appendOptionalStackPart(item, override.marginEdge);
        appendOptionalStackPart(item, override.shadow);
        item.push_back('\x1e');
      }

      signature.push_back(std::move(item));
    }

    return signature;
  }

  bool canActivateWindow(const ToplevelInfo& window) {
    return window.handle != nullptr
        || !window.identifier.empty()
        || (compositors::isKde() && (!window.title.empty() || !window.appId.empty()));
  }

  const ToplevelInfo* newestActivatableWindow(const std::vector<ToplevelInfo>& windows) {
    const ToplevelInfo* best = nullptr;
    for (const auto& window : windows) {
      if (!canActivateWindow(window)) {
        continue;
      }
      best = &window;
    }
    return best;
  }

  bool matchesActiveWindow(
      const ToplevelInfo& window, const ActiveToplevel& active, std::string_view focusedCompositorWindowId,
      const std::vector<ToplevelInfo>& windows
  ) {
    if (active.handle != nullptr && window.handle == active.handle) {
      return true;
    }

    // Exact-identity ext toplevels have no wlr handle; match them by the compositor's focused
    // window id, since `active.identifier` is an appId+title synthetic key for wlr toplevels.
    if (window.exactIdentity
        && !window.identifier.empty()
        && !focusedCompositorWindowId.empty()
        && window.identifier == focusedCompositorWindowId) {
      return true;
    }

    if (active.identifier.empty() || window.identifier.empty() || active.identifier != window.identifier) {
      return false;
    }

    int count = 0;
    for (const auto& w : windows) {
      if (w.identifier == active.identifier && ++count > 1) {
        return false;
      }
    }
    return true;
  }

  const ToplevelInfo* nextActivatableWindow(
      const std::vector<ToplevelInfo>& windows, const std::optional<ActiveToplevel>& active,
      std::string_view focusedCompositorWindowId, std::string_view preferredIdentifier,
      zwlr_foreign_toplevel_handle_v1* preferredHandle
  ) {
    if (windows.empty()) {
      return nullptr;
    }

    if (active.has_value()) {
      for (std::size_t i = 0; i < windows.size(); ++i) {
        if (!matchesActiveWindow(windows[i], *active, focusedCompositorWindowId, windows)) {
          continue;
        }
        for (std::size_t offset = 1; offset <= windows.size(); ++offset) {
          const auto& candidate = windows[(i + offset) % windows.size()];
          if (canActivateWindow(candidate)) {
            return &candidate;
          }
        }
        return nullptr;
      }
    }

    // The app's last active window by handle. preferredIdentifier can be a compositor window id
    // (Hyprland), which matches no wlr window.
    if (preferredHandle != nullptr) {
      for (const auto& window : windows) {
        if (window.handle == preferredHandle && canActivateWindow(window)) {
          return &window;
        }
      }
    }

    if (!preferredIdentifier.empty()) {
      for (const auto& window : windows) {
        if (window.identifier == preferredIdentifier && canActivateWindow(window)) {
          return &window;
        }
      }
    }

    for (const auto& window : windows) {
      if (canActivateWindow(window)) {
        return &window;
      }
    }
    return nullptr;
  }

  bool dockPointerHideAllowed(const DockConfig& cfg, const shell::dock::DockInstance& instance) noexcept {
    if (cfg.smartAutoHide) {
      return !instance.smartAutoHidePinnedVisible;
    }
    return cfg.autoHide;
  }

  const DockMonitorOverride* matchingDockMonitorOverride(const DockConfig& cfg, const WaylandOutput& output) {
    const auto it = std::ranges::find_if(cfg.monitorOverrides, [&output](const DockMonitorOverride& override) {
      return outputMatchesSelector(override.match, output);
    });
    return it != cfg.monitorOverrides.end() ? &*it : nullptr;
  }

  bool dockMayBeEnabled(const DockConfig& cfg) {
    return cfg.enabled || std::ranges::any_of(cfg.monitorOverrides, [](const DockMonitorOverride& override) {
             return override.enabled.value_or(false);
           });
  }

  std::vector<std::string> dockPinnedOverridePath(const shell::dock::DockInstance& instance) {
    if (instance.pinnedOverridden && !instance.monitorOverrideTable.empty()) {
      return {"dock", "monitor", instance.monitorOverrideTable, "pinned"};
    }
    return {"dock", "pinned"};
  }

  bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!workspace.id.empty() && assignmentKey == workspace.id) {
      return true;
    }
    if (!workspace.name.empty() && assignmentKey == workspace.name) {
      return true;
    }
    if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
      return true;
    }
    return false;
  }

  bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
    const auto workspaces = platform.workspaces(output);
    const Workspace* active = nullptr;
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }
    if (active == nullptr) {
      return false;
    }

    const auto assignments = platform.workspaceWindowAssignments(output);
    for (const auto& assignment : assignments) {
      if (assignment.minimizeState.has_value() && assignment.minimizeState->minimized) {
        continue;
      }
      if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!assignments.empty()) {
      return false;
    }
    if (platform.hasUnminimizedToplevels(output) == false) {
      return false;
    }
    return active->occupied;
  }

  bool dockSmartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
    if (platform.hasOverviewState() && platform.isOverviewOpen()) {
      return true;
    }
    return !activeWorkspaceHasWindows(platform, output);
  }

} // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────────

Dock::Dock() = default;
Dock::~Dock() = default;

bool Dock::initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext) {
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;

  const auto& cfg = m_config->config().dock;
  m_config->addReloadCallback(
      [this] {
        const auto& newCfg = m_config->config().dock;
        const auto& newShadow = m_config->config().shell.shadow;
        const auto newBarLayerStack = barLayerStackSignature(m_config->config());
        if (newCfg == m_lastDockConfig && newShadow == m_lastShadow && newBarLayerStack == m_lastBarLayerStack) {
          return;
        }
        if (newBarLayerStack != m_lastBarLayerStack && newCfg == m_lastDockConfig && newShadow == m_lastShadow) {
          kLog.info("bar layer stack changed; recreating dock surfaces");
        }
        reload();
      },
      "dock"
  );

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this] {
    if (m_config == nullptr) {
      return;
    }
    for (const auto& instance : m_instances) {
      if (instance != nullptr) {
        updateVisuals(*instance);
      }
    }
    requestRedraw();
  });

  m_lastDockConfig = cfg;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastBarLayerStack = barLayerStackSignature(m_config->config());

  m_stackPollTimer.startRepeating(std::chrono::seconds(2), [this] { checkStackFolders(); });
  m_platform->wayland().extension<FileDrag>().setDropHandler({
      .accepts = [this](wl_surface* surface, double x, double y) { return dropTargetAt(surface, x, y) != nullptr; },
      .drop =
          [this](wl_surface* surface, double x, double y, std::vector<std::string> paths) {
            if (const auto* target = dropTargetAt(surface, x, y); target != nullptr) {
              dropFiles(*target, std::move(paths));
            }
          },
  });

  if (!dockMayBeEnabled(cfg)) {
    kLog.info("dock disabled in config");
    return true;
  }
  return true;
}

const shell::dock::DockItemModel* Dock::dropTargetAt(wl_surface* surface, double x, double y) const {
  const auto it = m_surfaceMap.find(surface);
  if (it == m_surfaceMap.end() || it->second == nullptr) {
    return nullptr;
  }
  const shell::dock::DockInstance& instance = *it->second;
  const std::size_t itemCount = std::min(instance.items.size(), instance.snapshot.items.size());
  for (std::size_t i = 0; i < itemCount; ++i) {
    const auto& model = instance.snapshot.items[i];
    if (instance.items[i].area == nullptr
        || !(shell::dock::pinned_apps::isStack(model.entry) || shell::dock::pinned_apps::isTrash(model.entry))) {
      continue;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(instance.items[i].area, left, top, right, bottom);
    if (x >= left && x < right && y >= top && y < bottom) {
      return &model;
    }
  }
  return nullptr;
}

void Dock::dropFiles(const shell::dock::DockItemModel& target, std::vector<std::string> paths) {
  const std::filesystem::path folder = target.entry.workingDir;
  std::erase_if(paths, [&folder](const std::string& path) {
    return std::filesystem::path(path).parent_path() == folder;
  });
  if (paths.empty()) {
    return;
  }
  // Trashed files keep their restore info; files dropped on a folder are moved without overwriting.
  std::vector<std::string> command = shell::dock::pinned_apps::isTrash(target.entry)
      ? std::vector<std::string>{"gio", "trash", "--"}
      : std::vector<std::string>{"mv", "-n", "--"};
  command.insert(command.end(), paths.begin(), paths.end());
  if (!shell::dock::pinned_apps::isTrash(target.entry)) {
    command.push_back(folder.string());
  }
  if (!process::runAsync(command)) {
    kLog.warn("dock: failed to move dropped files into {}", folder.string());
    return;
  }
  // Pick up the change sooner than the next poll so the icon reacts to the drop.
  m_stackRefreshTimer.start(std::chrono::milliseconds(300), [this] { checkStackFolders(); });
}

void Dock::checkStackFolders() {
  bool changed = false;
  bool trashFilled = false;
  std::unordered_map<std::string, std::filesystem::file_time_type> seen;
  for (const auto& instance : m_instances) {
    if (instance == nullptr) {
      continue;
    }
    for (const auto& item : instance->snapshot.items) {
      const bool folderItem =
          shell::dock::pinned_apps::isStack(item.entry) || shell::dock::pinned_apps::isTrash(item.entry);
      if (!folderItem || seen.contains(item.entry.workingDir)) {
        continue;
      }
      std::error_code ec;
      const auto mtime = std::filesystem::last_write_time(item.entry.workingDir, ec);
      seen.emplace(item.entry.workingDir, mtime);
      const auto previous = m_stackFolderTimes.find(item.entry.workingDir);
      const bool folderChanged = previous != m_stackFolderTimes.end() && previous->second != mtime;
      changed = changed || folderChanged;
      std::error_code emptyEc;
      if (folderChanged
          && shell::dock::pinned_apps::isTrash(item.entry)
          && !std::filesystem::is_empty(item.entry.workingDir, emptyEc)) {
        trashFilled = true;
      }
    }
  }
  m_stackFolderTimes = std::move(seen);
  if (trashFilled) {
    for (auto& instance : m_instances) {
      if (instance != nullptr) {
        instance->trashBouncePending = true;
      }
    }
  }
  if (changed) {
    onIconThemeChanged();
  }
}

void Dock::reload() {
  kLog.info("reloading config");
  const auto& cfg = m_config->config().dock;
  m_lastDockConfig = cfg;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastBarLayerStack = barLayerStackSignature(m_config->config());

  if (!dockMayBeEnabled(cfg)) {
    closeAllInstances();
    return;
  }

  closeAllInstances();

  if (wl_display_roundtrip(m_platform->display()) < 0) {
    const int roundtripErrno = errno;
    kLog.error(
        "Wayland roundtrip failed while reloading dock surfaces: {}",
        m_platform->wayland().describeDisplayError(roundtripErrno)
    );
  }
  syncInstances();
}

void Dock::onIconThemeChanged() {
  for (auto& inst : m_instances) {
    // A new model serial makes the next update rebuild the items, which re-resolves icons.
    ++inst->modelSerial;
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
    }
  }
}

void Dock::show() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  if (m_config == nullptr || !dockMayBeEnabled(m_config->config().dock)) {
    return;
  }

  pruneCachedToplevelHandles();
  if (m_instances.empty()) {
    syncInstances();
    return;
  }

  refresh();
}

void Dock::closeAllInstances() {
  for (auto& inst : m_instances) {
    detachInstanceState(*inst);
  }
  m_itemMenu.reset();
  m_closingStackPopup.reset();
  m_lastActiveHandleByAppIdLower.clear();
  m_surfaceMap.clear();
  m_hoveredInstance = nullptr;
  m_popupOwnerInstance = nullptr;
  m_instances.clear();
}

void Dock::suppressDisplay() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = true;
  m_hadInstancesBeforeOverlaySuppress = !m_instances.empty();
  closeAllInstances();
}

void Dock::unsuppressDisplay() {
  if (!m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = false;
  if (m_hadInstancesBeforeOverlaySuppress) {
    show();
  }
}

void Dock::pruneCachedToplevelHandles() {
  if (m_platform == nullptr) {
    m_lastActiveHandleByAppIdLower.clear();
    return;
  }

  std::erase_if(m_lastActiveHandleByAppIdLower, [this](const auto& cached) {
    return cached.second == nullptr || !m_platform->containsWlrToplevelHandle(cached.second);
  });
}

void Dock::detachInstanceState(shell::dock::DockInstance& inst) {
  if (inst.config.showRunning) {
    m_previousItemsByOutput.insert_or_assign(inst.outputName, inst.snapshot.items);
  }

  // The instance's InputDispatcher is destroyed before its scene, so it cannot report the hovered
  // item's destruction. Drop the tooltip (and any pending show timer holding that item) now; the
  // popup is also parented to this layer surface and must die before it (#5311).
  TooltipManager::instance().forceDestroy();
  if (inst.surface != nullptr) {
    if (wl_surface* const wls = inst.surface->wlSurface()) {
      m_surfaceMap.erase(wls);
    }
  }
  if (m_hoveredInstance == &inst) {
    m_hoveredInstance = nullptr;
  }
  if (m_popupOwnerInstance == &inst) {
    m_itemMenu.reset();
    m_popupOwnerInstance = nullptr;
  }
  m_closingStackPopup.reset();
}

void Dock::onOutputChange() {
  if (m_config == nullptr || !dockMayBeEnabled(m_config->config().dock)) {
    return;
  }
  syncInstances();
}

void Dock::refresh() {
  if (m_config == nullptr || m_platform == nullptr || !dockMayBeEnabled(m_config->config().dock)) {
    return;
  }

  pruneCachedToplevelHandles();
  syncInstances();

  if (m_instances.empty()) {
    return;
  }

  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    inst->surface->requestUpdateOnly();
  }

  tryFulfillPendingLaunchFocus();
}

void Dock::toggleVisibility() {
  if (m_instances.empty()) {
    show();
  } else {
    closeAllInstances();
  }
}

void Dock::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Dock::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

// ── Input ─────────────────────────────────────────────────────────────────────

bool Dock::onPointerEvent(const PointerEvent& event) {
  // Route open popups first. Unconsumed presses dismiss the menu; only continue to
  // dock hit-testing when the press is on the dock itself.
  if (m_itemMenu != nullptr) {
    const bool consumed = shell::dock::routePopupEvent(*m_itemMenu, event);
    if (consumed) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      closeItemMenu();
      if (event.surface == nullptr || !m_surfaceMap.contains(event.surface)) {
        return true;
      }
    }
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    auto it = m_surfaceMap.find(event.surface);
    if (it == m_surfaceMap.end()) {
      break;
    }
    m_hoveredInstance = it->second;
    shell::dock::DockInstance* const entered = m_hoveredInstance;
    entered->pointerInside = true;
    entered->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    // pointerEnter can re-enter the Wayland event loop (tooltip popup creation),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != entered) {
      break;
    }
    updateHoverZoomPointer(*m_hoveredInstance, static_cast<float>(event.sx), static_cast<float>(event.sy));
    // Auto-hide: reveal once the pointer has dwelled on the edge trigger strip.
    if (dockPointerHideAllowed(m_hoveredInstance->config, *m_hoveredInstance)
        && m_hoveredInstance->sceneRoot != nullptr) {
      const auto revealDelay = std::chrono::milliseconds{entered->config.autoHideRevealDelayMs};
      if (revealDelay.count() <= 0 || (entered->hideOpacity >= 0.999F && entered->hideAnimId == 0)) {
        shell::dock::revealAutoHideDock(*entered);
      } else {
        entered->revealTimer.start(revealDelay, [entered] {
          if (entered->pointerInside && dockPointerHideAllowed(entered->config, *entered)) {
            shell::dock::revealAutoHideDock(*entered);
          }
        });
      }
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (m_hoveredInstance != nullptr) {
      m_hoveredInstance->revealTimer.stop();
      if (m_hoveredInstance->drag.active || m_hoveredInstance->drag.armed) {
        endDrag(*m_hoveredInstance, false);
      }
      clearHoverZoomPointer(*m_hoveredInstance);
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();

      if (dockPointerHideAllowed(m_hoveredInstance->config, *m_hoveredInstance) && m_popupOwnerInstance == nullptr) {
        shell::dock::startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr) {
      break;
    }
    shell::dock::DockInstance* const hovered = m_hoveredInstance;
    hovered->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    // pointerMotion can re-enter the Wayland event loop (tooltip popup creation),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != hovered) {
      break;
    }
    updateHoverZoomPointer(*m_hoveredInstance, static_cast<float>(event.sx), static_cast<float>(event.sy));
    break;
  }
  case PointerEvent::Type::Button: {
    auto it = m_surfaceMap.find(event.surface);
    if (it != m_surfaceMap.end()) {
      shell::dock::DockInstance* targetInstance = it->second;
      if (m_hoveredInstance != targetInstance) {
        if (m_hoveredInstance != nullptr) {
          clearHoverZoomPointer(*m_hoveredInstance);
          m_hoveredInstance->pointerInside = false;
          m_hoveredInstance->inputDispatcher.pointerLeave();
        }
        m_hoveredInstance = targetInstance;
        m_hoveredInstance->pointerInside = true;
        m_hoveredInstance->inputDispatcher.pointerEnter(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial
        );
      } else {
        m_hoveredInstance->inputDispatcher.pointerMotion(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial
        );
      }
      // pointerEnter/pointerMotion can re-enter the Wayland event loop (tooltip
      // popup creation), which may clear m_hoveredInstance before we use it.
      if (m_hoveredInstance != nullptr) {
        updateHoverZoomPointer(*m_hoveredInstance, static_cast<float>(event.sx), static_cast<float>(event.sy));
      }
    }

    if (m_hoveredInstance == nullptr) {
      break;
    }
    const bool pressed = event.pressed;
    m_hoveredInstance->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed, event.serial, event.time,
        event.touch
    );
    break;
  }
  case PointerEvent::Type::Axis:
    if (m_hoveredInstance != nullptr) {
      m_hoveredInstance->inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines, event.axisGestureSerial
      );
    }
    break;
  }

  if (m_hoveredInstance != nullptr
      && m_hoveredInstance->sceneRoot != nullptr
      && (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
    if (m_hoveredInstance->sceneRoot->layoutDirty()) {
      m_hoveredInstance->surface->requestLayout();
    } else {
      m_hoveredInstance->surface->requestRedraw();
    }
  }

  return m_hoveredInstance != nullptr;
}

// ── Private: instance management ─────────────────────────────────────────────

void Dock::syncInstances() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  const auto& outputs = m_platform->outputs();
  const auto& baseConfig = m_config->config().dock;
  const auto outputAllowed = [&](const WaylandOutput& output) {
    if (!output.done || !output.hasUsableGeometry()) {
      return false;
    }
    const DockConfig cfg = ConfigService::resolveForOutput(baseConfig, output);
    if (!cfg.enabled) {
      return false;
    }
    const bool hasStaticContent = !cfg.pinned.empty() || shell::dock::dockLauncherButtonCount(cfg) > 0;
    if (hasStaticContent) {
      return true;
    }
    if (!cfg.showRunning) {
      return false;
    }
    if (cfg.activeMonitorOnly) {
      return !m_platform->runningAppIds(output.output).empty();
    }
    return !m_platform->runningAppIds(nullptr).empty();
  };

  // Remove instances for dead outputs or outputs no longer selected.
  std::erase_if(m_instances, [this, &outputs, &outputAllowed](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool geometryChanged = it != outputs.end()
        && (inst->outputLogicalX != it->logicalX
            || inst->outputLogicalY != it->logicalY
            || inst->outputLogicalWidth != it->effectiveLogicalWidth()
            || inst->outputLogicalHeight != it->effectiveLogicalHeight());
    const bool drop = (it == outputs.end()) || !outputAllowed(*it) || geometryChanged;
    if (drop) {
      detachInstanceState(*inst);
    }
    return drop;
  });

  // Forget saved order for outputs that are no longer connected.
  std::erase_if(m_previousItemsByOutput, [&outputs](const auto& cached) {
    return std::ranges::find(outputs, cached.first, &WaylandOutput::name) == outputs.end();
  });

  for (const auto& output : outputs) {
    if (!outputAllowed(output)) {
      continue;
    }
    const bool exists =
        std::ranges::any_of(m_instances, [&output](const auto& inst) { return inst->outputName == output.name; });
    if (!exists) {
      createInstance(output);
    }
  }
  scheduleSmartAutoHideReevaluation();
}

void Dock::onWorkspaceChanged() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }
  scheduleSmartAutoHideReevaluation();
}

void Dock::scheduleSmartAutoHideReevaluation() {
  if (m_smartAutoHideReevalQueued) {
    return;
  }
  m_smartAutoHideReevalQueued = true;
  DeferredCall::callLater([this] {
    m_smartAutoHideReevalQueued = false;
    reevaluateSmartAutoHide();
  });
}

void Dock::reevaluateSmartAutoHide() {
  if (m_platform == nullptr || m_config == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  if (!dockMayBeEnabled(m_config->config().dock)) {
    return;
  }

  for (const auto& instanceUp : m_instances) {
    shell::dock::DockInstance* instance = instanceUp.get();
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    const auto& cfg = instance->config;
    if (!cfg.smartAutoHide) {
      continue;
    }

    // Stale pointerInside blocks hide (and can leave smart-hide stuck after grabs / focus
    // changes). Resync from the compositor before deciding.
    const wl_surface* surface = instance->surface->wlSurface();
    const bool pointerOnDock =
        m_platform->hasPointerPosition() && surface != nullptr && m_platform->lastPointerSurface() == surface;
    if (instance->pointerInside && !pointerOnDock) {
      clearHoverZoomPointer(*instance);
      instance->pointerInside = false;
      instance->inputDispatcher.pointerLeave();
      if (m_hoveredInstance == instance) {
        m_hoveredInstance = nullptr;
      }
    } else if (!instance->pointerInside && pointerOnDock) {
      instance->pointerInside = true;
      instance->inputDispatcher.pointerEnter(
          static_cast<float>(m_platform->lastPointerX()), static_cast<float>(m_platform->lastPointerY()),
          m_platform->lastInputSerial()
      );
      m_hoveredInstance = instance;
    }

    const bool wantsPinned = dockSmartAutoHideWantsPinnedVisible(*m_platform, instance->output);
    const bool pinnedChanged = wantsPinned != instance->smartAutoHidePinnedVisible;
    instance->smartAutoHidePinnedVisible = wantsPinned;

    bool needsRedraw = pinnedChanged;
    if (wantsPinned) {
      if (instance->hideOpacity < 1.0F || pinnedChanged) {
        shell::dock::revealAutoHideDock(*instance);
        needsRedraw = true;
      }
    } else if (
        !instance->pointerInside && m_popupOwnerInstance == nullptr && (instance->hideOpacity > 0.0F || pinnedChanged)
    ) {
      shell::dock::startHideFadeOut(*instance);
      needsRedraw = true;
    }

    if (needsRedraw && instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

void Dock::createInstance(const WaylandOutput& output) {
  assertDockInitialized(m_platform, m_config, m_renderContext);

  const auto& baseConfig = m_config->config().dock;
  const DockConfig cfg = ConfigService::resolveForOutput(baseConfig, output);
  kLog.info(
      "creating dock on {} ({}) icon_size={} position={}", output.connectorName, output.description, cfg.iconSize,
      enumToKey(kDockEdges, cfg.position)
  );

  auto instance = std::make_unique<shell::dock::DockInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->fractionalScale = (output.configuredScaleNumerator % wayland::kScaleNumeratorBase) != 0;
  instance->outputLogicalX = output.logicalX;
  instance->outputLogicalY = output.logicalY;
  instance->outputLogicalWidth = output.effectiveLogicalWidth();
  instance->outputLogicalHeight = output.effectiveLogicalHeight();
  instance->config = cfg;
  if (const auto* override = matchingDockMonitorOverride(baseConfig, output); override != nullptr) {
    instance->monitorOverrideTable = override->tableName;
    instance->pinnedOverridden = override->pinned.has_value();
  }

  if (cfg.showRunning) {
    if (const auto it = m_previousItemsByOutput.find(output.name); it != m_previousItemsByOutput.end()) {
      instance->snapshot.items = it->second;
    }
  }

  const auto& shadowConfig = m_config->config().shell.shadow;
  LayerSurfaceConfig lsCfg = shell::dock::makeLayerSurfaceConfig(
      cfg, shadowConfig, instance->fractionalScale, instance->outputLogicalWidth, instance->outputLogicalHeight
  );

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(lsCfg));
  instance->surface->setRenderContext(m_renderContext);

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([inst](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    inst->surface->requestLayout();
  });
  instance->surface->setPrepareFrameCallback([this, inst](bool needsUpdate, bool needsLayout) {
    assertDockInitialized(m_platform, m_config, m_renderContext);
    shell::dock::prepareFrame(
        *inst, {.platform = *m_platform, .config = *m_config, .renderContext = *m_renderContext},
        shell::dock::DockInstanceCallbacks{
            .syncModel =
                [this](shell::dock::DockInstance& callbackInstance) { return syncInstanceModel(callbackInstance); },
            .rebuildItems = [this](shell::dock::DockInstance& callbackInstance) { rebuildItems(callbackInstance); },
            .updateVisuals = [this](shell::dock::DockInstance& callbackInstance) { updateVisuals(callbackInstance); },
        },
        needsUpdate, needsLayout
    );
  });
  instance->surface->setFrameTickCallback([this, inst](float deltaMs) {
    if (m_config == nullptr || m_renderContext == nullptr || !inst->config.magnification) {
      return;
    }
    if (shell::dock::updateHoverZoom(*inst, inst->snapshot, deltaMs) && inst->surface != nullptr) {
      inst->surface->requestFrameTick();
      inst->surface->requestRedraw();
    }
  });
  instance->surface->setAnimationManager(&instance->animations);

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to init dock surface for output {}", output.name);
    return;
  }

  if (cfg.showRunning) {
    m_previousItemsByOutput.erase(output.name);
  }
  m_surfaceMap[instance->surface->wlSurface()] = instance.get();
  m_instances.push_back(std::move(instance));
}

// ── Private: scene building ───────────────────────────────────────────────────

bool Dock::syncInstanceModel(shell::dock::DockInstance& instance) {
  if (m_platform == nullptr || m_config == nullptr) {
    return false;
  }

  // Resolve the active app once: it feeds both the last-active-handle cache and the snapshot,
  // keeping buildDockSnapshot a pure query.
  const std::string activeIdLower = shell::dock::currentActiveEntryIdLower(*m_platform);
  if (!activeIdLower.empty()) {
    if (const auto active = m_platform->activeToplevel(); active.has_value()) {
      if (active->handle != nullptr) {
        m_lastActiveHandleByAppIdLower[activeIdLower] = active->handle;
      }
      if (!active->identifier.empty()) {
        m_lastActiveIdentifierByAppIdLower[activeIdLower] = active->identifier;
      }
    }
    // Also record the compositor-exact focused window id so preferred-identifier lookups
    // match ext-only toplevels (whose identifier is the exact window id, not appId+title).
    if (const auto focusedId = m_platform->focusedCompositorWindowId(); focusedId.has_value() && !focusedId->empty()) {
      m_lastActiveIdentifierByAppIdLower[activeIdLower] = *focusedId;
    }
  }

  shell::dock::refreshPinnedAppsIfNeeded(
      instance.config, instance.lastPinnedConfig, instance.pinnedEntries, instance.modelSerial, instance.entriesVersion
  );

  auto next = shell::dock::buildDockSnapshot({
      .platform = *m_platform,
      .config = instance.config,
      .output = instance.output,
      .globalActiveIdLower = activeIdLower,
      .pinnedEntries = instance.pinnedEntries,
      .sourceSerial = instance.modelSerial,
  });
  shell::dock::keepRunningItemOrder(instance.snapshot, next);

  const bool needRebuild = instance.snapshot.sourceSerial != next.sourceSerial
      || instance.snapshot.filterOutput != next.filterOutput
      || !shell::dock::sameDockItemSet(instance.snapshot, next);
  instance.snapshot = std::move(next);

  return needRebuild;
}

// ── Private: item population ──────────────────────────────────────────────────

void Dock::rebuildItems(shell::dock::DockInstance& instance) {
  assertDockCoreInitialized(m_platform, m_config);
  if (m_renderContext == nullptr) {
    return;
  }

  shell::dock::rebuildItems(
      instance,
      {
          .model =
              {
                  .config = *m_config,
              },
          .renderContext = *m_renderContext,
          .iconResolver = m_iconResolver,
      },
      instance.snapshot,
      {
          .activateOrLaunch = [this](
                                  shell::dock::DockInstance& inst, const shell::dock::DockItemAction& action
                              ) { activateOrLaunchItem(inst, action); },
          .toggleLauncher =
              [](shell::dock::DockInstance& inst) {
                PanelManager::instance().togglePanel("launcher", PanelOpenRequest{.output = inst.output});
              },
          .openItemMenu = [this](
                              shell::dock::DockInstance& inst, const shell::dock::DockItemAction& action
                          ) { openItemMenu(inst, action); },
          .beginDrag = [this](
                           shell::dock::DockInstance& inst, std::size_t index, float mainPos
                       ) { beginDrag(inst, index, mainPos); },
          .updateDrag = [this](shell::dock::DockInstance& inst, float mainPos) { updateDrag(inst, mainPos); },
          .endDrag = [this](shell::dock::DockInstance& inst, bool commit) { endDrag(inst, commit); },
      }
  );
}

// ── Private: visual update ────────────────────────────────────────────────────

void Dock::updateVisuals(shell::dock::DockInstance& instance) {
  assertDockInitialized(m_platform, m_config, m_renderContext);

  shell::dock::updateVisuals(
      instance,
      {
          .model =
              {
                  .config = *m_config,
              },
          .renderContext = *m_renderContext,
          .iconResolver = m_iconResolver,
      },
      instance.snapshot
  );
}

void Dock::updateHoverZoomPointer(shell::dock::DockInstance& instance, float sceneX, float sceneY) {
  assertDockInitialized(m_platform, m_config, m_renderContext);
  if (!instance.config.magnification || instance.row == nullptr) {
    return;
  }

  if (!shell::dock::syncHoverPointerFromScene(instance, instance.config, sceneX, sceneY)) {
    shell::dock::clearHoverZoom(instance, instance.snapshot);
    return;
  }

  if (instance.surface == nullptr) {
    return;
  }
  instance.surface->requestFrameTick();
  instance.surface->requestRedraw();
}

void Dock::clearHoverZoomPointer(shell::dock::DockInstance& instance) {
  if (m_config == nullptr || m_renderContext == nullptr || !instance.config.magnification) {
    instance.hoverPointerValid = false;
    return;
  }

  shell::dock::clearHoverZoom(instance, instance.snapshot);
}

// ── Private: item context menu (right-click) ──────────────────────────────────

void Dock::beginDrag(shell::dock::DockInstance& instance, std::size_t index, float mainPos) {
  if (m_config == nullptr || instance.drag.active) {
    return;
  }

  const auto& cfg = instance.config;
  if (index >= instance.snapshot.items.size() || !instance.snapshot.items[index].pinnedIndex.has_value()) {
    return;
  }

  instance.drag.active = true;
  instance.drag.sourceIndex = index;
  instance.drag.currentMain = mainPos;
  instance.drag.targetIndex = shell::dock::computeDragTargetIndex(instance, cfg, mainPos);

  shell::dock::dismissDockTooltip();
  shell::dock::applyDragVisuals(instance, cfg);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Dock::updateDrag(shell::dock::DockInstance& instance, float mainPos) {
  if (m_config == nullptr || !instance.drag.active) {
    return;
  }

  const auto& cfg = instance.config;
  instance.drag.currentMain = mainPos;
  const std::size_t nextTarget = shell::dock::computeDragTargetIndex(instance, cfg, mainPos);
  if (nextTarget != instance.drag.targetIndex) {
    instance.drag.targetIndex = nextTarget;
  }

  shell::dock::applyDragVisuals(instance, cfg);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Dock::endDrag(shell::dock::DockInstance& instance, bool commit) {
  if (!instance.drag.active && !instance.drag.armed) {
    return;
  }

  instance.drag.holdTimer.stop();
  const bool wasActive = instance.drag.active;
  const bool wasArmed = instance.drag.armed;
  const std::optional<std::size_t> sourcePinned = instance.drag.sourceIndex < instance.snapshot.items.size()
      ? instance.snapshot.items[instance.drag.sourceIndex].pinnedIndex
      : std::nullopt;
  const std::size_t sourceIndex = sourcePinned.value_or(instance.config.pinned.size());
  const std::size_t targetIndex = instance.drag.targetIndex;
  instance.drag = {};

  if (wasActive || wasArmed) {
    instance.suppressItemClick = true;
  }

  if (m_config == nullptr) {
    return;
  }

  const auto& cfg = instance.config;
  if (wasActive) {
    shell::dock::clearDragVisuals(instance, cfg);
  }

  if (!commit || !wasActive || sourceIndex == targetIndex || sourceIndex >= cfg.pinned.size()) {
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
    return;
  }

  std::vector<std::string> pinnedList = cfg.pinned;
  std::size_t insertAt = std::min(targetIndex, pinnedList.size());
  std::string moved = std::move(pinnedList[sourceIndex]);
  pinnedList.erase(pinnedList.begin() + static_cast<std::ptrdiff_t>(sourceIndex));
  if (insertAt > sourceIndex) {
    --insertAt;
  }
  pinnedList.insert(pinnedList.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(moved));
  ConfigService* config = m_config;
  auto path = dockPinnedOverridePath(instance);
  DeferredCall::callLater([config, path = std::move(path), pinnedList = std::move(pinnedList)] mutable {
    if (config != nullptr) {
      config->setOverride(path, std::move(pinnedList));
    }
  });
}

void Dock::closeItemMenu() {
  shell::dock::DockInstance* owner = m_popupOwnerInstance;
  m_popupOwnerInstance = nullptr;
  if (m_itemMenu != nullptr && !m_openStackId.empty()) {
    m_closingStackPopup = std::move(m_itemMenu);
    shell::dock::closeStackPopup(*m_closingStackPopup, [this] {
      DeferredCall::callLater([this] { m_closingStackPopup.reset(); });
    });
  }
  m_itemMenu.reset();
  if (!m_openStackId.empty()) {
    m_closedStackId = std::exchange(m_openStackId, {});
    m_closedStackAt = std::chrono::steady_clock::now();
  }
  if (owner == nullptr) {
    return;
  }

  // Resync hover after grab dismiss — Leave is often missing while the menu is open.
  const wl_surface* ownerSurface = owner->surface != nullptr ? owner->surface->wlSurface() : nullptr;
  owner->pointerInside = m_platform != nullptr
      && ownerSurface != nullptr
      && m_platform->hasPointerPosition()
      && m_platform->lastPointerSurface() == ownerSurface;

  if (owner->pointerInside) {
    const auto sx = static_cast<float>(m_platform->lastPointerX());
    const auto sy = static_cast<float>(m_platform->lastPointerY());
    owner->inputDispatcher.pointerEnter(sx, sy, m_platform->lastInputSerial());
    m_hoveredInstance = owner;
    updateHoverZoomPointer(*owner, sx, sy);
  } else {
    clearHoverZoomPointer(*owner);
    owner->inputDispatcher.pointerLeave();
    if (m_hoveredInstance == owner) {
      m_hoveredInstance = nullptr;
    }
  }

  if (owner->surface != nullptr) {
    owner->surface->requestRedraw();
  }

  if (owner->pointerInside
      || m_config == nullptr
      || owner->hideOpacity <= 0.0F
      || !dockPointerHideAllowed(owner->config, *owner)) {
    return;
  }
  shell::dock::startHideFadeOut(*owner);
}

void Dock::tryFulfillPendingLaunchFocus() {
  if (m_platform == nullptr || !m_pendingLaunchFocus.has_value()) {
    return;
  }

  PendingLaunchFocus pending = *m_pendingLaunchFocus;
  if (std::chrono::steady_clock::now() > pending.deadline) {
    m_pendingLaunchFocus.reset();
    return;
  }

  auto windowsOnTarget =
      shell::dock::windowsForDockItem(*m_platform, pending.idLower, pending.wmClassLower, pending.targetOutput);
  std::optional<ToplevelInfo> window;
  if (const ToplevelInfo* candidate = newestActivatableWindow(windowsOnTarget); candidate != nullptr) {
    window = *candidate;
  }
  if (!window.has_value()) {
    auto windows =
        shell::dock::windowsForDockItem(*m_platform, pending.idLower, pending.wmClassLower, pending.outputFilter);
    if (windows.empty() && pending.outputFilter != nullptr) {
      windows = shell::dock::windowsForDockItem(*m_platform, pending.idLower, pending.wmClassLower, nullptr);
    }
    const ToplevelInfo* candidate = newestActivatableWindow(windows);
    if (candidate == nullptr) {
      return;
    }
    window = *candidate;
    // Landed off the launch monitor; relocate before activate.
    if (pending.targetOutput != nullptr) {
      m_platform->moveToplevelToOutput(*window, pending.targetOutput);
    }
  }

  m_pendingLaunchFocus.reset();
  m_platform->activateToplevelInfo(*window);
}

void Dock::activateOrLaunchItem(shell::dock::DockInstance& instance, const shell::dock::DockItemAction& action) {
  assertDockInitialized(m_platform, m_config, m_renderContext);

  if (shell::dock::pinned_apps::isTrash(action.entry)) {
    m_platform->prepareAppLaunchOnOutput(instance.output);
    if (!process::runAsync(std::vector<std::string>{"xdg-open", trashLocation(action.entry)})) {
      kLog.warn("dock: failed to open the trash");
    }
    return;
  }
  if (shell::dock::pinned_apps::isStack(action.entry)) {
    openStackPopup(instance, action);
    return;
  }

  pruneCachedToplevelHandles();

  auto windows = shell::dock::windowsForDockItem(
      *m_platform, action.windowLookupIdLower, action.windowLookupWmClassLower,
      shell::dock::dockFilterOutput(instance.config, instance.output)
  );

  if (windows.empty()) {
    m_pendingLaunchFocus = PendingLaunchFocus{
        .idLower = action.windowLookupIdLower,
        .wmClassLower = action.windowLookupWmClassLower,
        .outputFilter = shell::dock::dockFilterOutput(instance.config, instance.output),
        .targetOutput = instance.output,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8),
    };
    m_platform->prepareAppLaunchOnOutput(instance.output);
    (void)desktop_entry_launch::launchEntry(action.entry, dockLaunchOptions(*m_platform, *m_config, action.entry));
    return;
  }

  if (windows.size() == 1) {
    m_platform->activateOrMinimizeToplevelInfo(windows[0]);
    return;
  }

  const auto active = m_platform->activeToplevel();
  std::string preferredIdentifier;
  if (const auto it = m_lastActiveIdentifierByAppIdLower.find(action.idLower);
      it != m_lastActiveIdentifierByAppIdLower.end()) {
    preferredIdentifier = it->second;
  }

  auto* preferredHandle = [&] -> zwlr_foreign_toplevel_handle_v1* {
    const auto it = m_lastActiveHandleByAppIdLower.find(action.idLower);
    return it != m_lastActiveHandleByAppIdLower.end() ? it->second : nullptr;
  }();

  const auto focusedCompositorWindowId = m_platform->focusedCompositorWindowId();
  const std::string_view focusedCompositorWindowIdView =
      focusedCompositorWindowId.has_value() ? std::string_view(*focusedCompositorWindowId) : std::string_view{};
  if (const ToplevelInfo* nextWindow =
          nextActivatableWindow(windows, active, focusedCompositorWindowIdView, preferredIdentifier, preferredHandle);
      nextWindow != nullptr) {
    m_platform->activateToplevelInfo(*nextWindow);
  }
}

void Dock::openItemMenu(shell::dock::DockInstance& instance, const shell::dock::DockItemAction& action) {
  assertDockInitialized(m_platform, m_config, m_renderContext);

  closeItemMenu();

  if (!m_platform->hasXdgShell()) {
    return;
  }

  m_popupOwnerInstance = &instance;

  auto windows = shell::dock::windowsForDockItem(
      *m_platform, action.windowLookupIdLower, action.windowLookupWmClassLower,
      shell::dock::dockFilterOutput(instance.config, instance.output)
  );
  const std::string entryId = action.entry.id;
  const std::string entryWorkingDir = action.entry.workingDir;
  const bool entryTerminal = action.entry.terminal;
  const DesktopEntry entryForPin = action.entry;
  const auto pinnedPath = dockPinnedOverridePath(instance);
  const auto configuredPinned = instance.config.pinned;

  shell::dock::DockMenuCallbacks callbacks{
      .activateWindow =
          [this, windows](std::size_t windowIndex) {
            if (windowIndex < windows.size()) {
              m_platform->activateToplevelInfo(windows[windowIndex]);
            }
          },
      .closeWindow =
          [this, windows](std::size_t windowIndex) {
            if (windowIndex < windows.size()) {
              m_platform->closeToplevelInfo(windows[windowIndex]);
            }
          },
      .setWindowMinimized =
          [this, windows](std::size_t windowIndex, bool minimized) {
            if (windowIndex >= windows.size()) {
              return;
            }
            const auto& window = windows[windowIndex];
            const auto state = m_platform->windowMinimizeState(window);
            if (!state.has_value() || state->minimized == minimized) {
              return;
            }
            if (minimized) {
              (void)m_platform->minimizeToplevelInfo(window);
            } else {
              m_platform->activateToplevelInfo(window);
            }
          },
      .launchAction =
          [this, entryId, entryWorkingDir, entryTerminal, entryForPin,
           output = instance.output](const DesktopAction& desktopAction) {
            m_platform->prepareAppLaunchOnOutput(output);
            (void)desktop_entry_launch::launchAction(
                desktopAction, entryId, entryWorkingDir, entryTerminal,
                dockLaunchOptions(*m_platform, *m_config, entryForPin)
            );
          },
      .setEntryPinned =
          [this, entryForPin, pinnedPath, configuredPinned](bool pinned) {
            if (m_config == nullptr) {
              return;
            }
            std::vector<std::string> pinnedList = configuredPinned;
            if (pinned) {
              if (shell::dock::pinned_apps::containsEntry(pinnedList, entryForPin)) {
                return;
              }
              pinnedList.push_back(entryForPin.id);
            } else {
              shell::dock::pinned_apps::removeEntry(pinnedList, entryForPin);
            }
            m_config->setOverride(pinnedPath, std::move(pinnedList));
          },
      .closeMenu = [this] { closeItemMenu(); },
  };

  // Folder items offer folder actions; the built-in ones cannot be pinned.
  DesktopEntry menuEntry = action.entry;
  if (shell::dock::pinned_apps::isTrash(menuEntry)) {
    menuEntry.actions = {
        DesktopAction{
            .id = "open",
            .name = i18n::tr("dock.stack.open-trash"),
            .exec = "xdg-open " + execQuote(trashLocation(menuEntry))
        },
        DesktopAction{.id = "empty", .name = i18n::tr("dock.stack.empty-trash"), .exec = "gio trash --empty"},
    };
  } else if (shell::dock::pinned_apps::isStack(menuEntry)) {
    menuEntry.actions = {DesktopAction{
        .id = "open", .name = i18n::tr("dock.stack.open-folder"), .exec = "xdg-open " + execQuote(menuEntry.workingDir)
    }};
  }
  if (isBuiltInFolderItem(menuEntry, instance.config)) {
    callbacks.setEntryPinned = nullptr;
  }

  auto* layerSurface =
      instance.surface != nullptr ? m_platform->layerSurfaceFor(instance.surface->wlSurface()) : nullptr;
  m_closingStackPopup.reset();
  m_itemMenu = shell::dock::createItemMenu(
      *m_platform, *m_config, *m_renderContext, layerSurface, instance.output, instance.config, menuEntry, windows,
      callbacks
  );
  if (m_itemMenu == nullptr) {
    m_popupOwnerInstance = nullptr;
  }
}

void Dock::openStackPopup(shell::dock::DockInstance& instance, const shell::dock::DockItemAction& action) {
  constexpr auto kToggleWindow = std::chrono::milliseconds(500);
  const bool justClosed =
      m_closedStackId == action.entry.id && std::chrono::steady_clock::now() - m_closedStackAt < kToggleWindow;
  m_closedStackId.clear();
  if (justClosed || m_openStackId == action.entry.id) {
    closeItemMenu();
    m_closedStackId.clear();
    return;
  }
  closeItemMenu();

  if (!m_platform->hasXdgShell()) {
    return;
  }

  m_popupOwnerInstance = &instance;
  const shell::dock::DockStackCallbacks callbacks{
      .openPath =
          [this, output = instance.output](const std::string& path) {
            m_platform->prepareAppLaunchOnOutput(output);
            if (!process::runAsync(std::vector<std::string>{"xdg-open", path})) {
              kLog.warn("dock: failed to open {}", path);
            }
          },
      .closeMenu = [this] { closeItemMenu(); },
  };

  // Attach to the stack's icon rather than the pointer.
  std::optional<shell::dock::DockPopupAnchor> anchor;
  const std::size_t itemCount = std::min(instance.items.size(), instance.snapshot.items.size());
  for (std::size_t i = 0; i < itemCount; ++i) {
    if (instance.snapshot.items[i].entry.id != action.entry.id || instance.items[i].area == nullptr) {
      continue;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(instance.items[i].area, left, top, right, bottom);
    anchor = shell::dock::DockPopupAnchor{
        .x = static_cast<std::int32_t>(std::round(left)),
        .y = static_cast<std::int32_t>(std::round(top)),
        .width = std::max(1, static_cast<std::int32_t>(std::round(right - left))),
        .height = std::max(1, static_cast<std::int32_t>(std::round(bottom - top))),
    };
    break;
  }

  auto* layerSurface =
      instance.surface != nullptr ? m_platform->layerSurfaceFor(instance.surface->wlSurface()) : nullptr;
  m_closingStackPopup.reset();
  m_itemMenu = shell::dock::createStackPopup(
      *m_platform, *m_config, *m_renderContext, m_iconResolver, layerSurface, instance.output, instance.config,
      action.entry.workingDir, anchor, callbacks
  );
  if (m_itemMenu == nullptr) {
    m_popupOwnerInstance = nullptr;
  } else {
    m_openStackId = action.entry.id;
  }
}

void Dock::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::dockShow, [this](const std::string&) -> std::string {
    if (m_config) {
      m_config->setDockEnabled(true);
    }
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::dockHide, [this](const std::string&) -> std::string {
    if (m_config) {
      m_config->setDockEnabled(false);
    }
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::dockToggle, [this](const std::string&) -> std::string {
    if (m_config) {
      m_config->setDockEnabled(!m_config->config().dock.enabled);
    }
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::dockReload, [this](const std::string&) -> std::string {
    reload();
    return "ok\n";
  });
}
