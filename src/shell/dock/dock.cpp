#include "shell/dock/dock.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "ipc/ipc_service.h"
#include "render/scene/node.h"
#include "shell/dock/dock_context_menu.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "shell/dock/dock_items.h"
#include "shell/dock/dock_model.h"
#include "shell/dock/pinned_apps.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "ui/app_icon_colorization.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_toplevels.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <format>
#include <optional>
#include <wayland-client-core.h>

namespace {

  constexpr Logger kLog("dock");

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
  dockLaunchOptions(const CompositorPlatform& platform, const ConfigService& config) {
    std::string token;
    if (platform.hasXdgActivation()) {
      // No layer-shell surface — binding it can misplace first launches on Hyprland.
      token = platform.requestActivationToken(nullptr);
    }
    return desktop_entry_launch::LaunchOptions{
        .activationToken = std::move(token),
        .runAsSystemdService = config.config().shell.launchAppsAsSystemdServices,
        .customCommand = config.config().shell.launchAppsCustomCommand,
    };
  }

  template <typename T> void appendOptionalStackPart(std::string& out, const std::optional<T>& value) {
    out += value.has_value() ? std::format("{}", *value) : "-";
    out.push_back('\x1f');
  }

  std::vector<std::string> barLayerStackSignature(const Config& config) {
    std::vector<std::string> signature;
    signature.reserve(config.bars.size());

    for (const auto& bar : config.bars) {
      std::string item = std::format(
          "{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}", bar.name, bar.position, bar.enabled, bar.autoHide,
          bar.reserveSpace, bar.layer, bar.thickness, bar.marginEnds, bar.marginEdge, bar.shadow
      );

      item.push_back('\x1e');
      for (const auto& override : bar.monitorOverrides) {
        item += override.match;
        item.push_back('\x1f');
        appendOptionalStackPart(item, override.enabled);
        appendOptionalStackPart(item, override.autoHide);
        appendOptionalStackPart(item, override.reserveSpace);
        appendOptionalStackPart(item, override.layer);
        appendOptionalStackPart(item, override.thickness);
        appendOptionalStackPart(item, override.marginEnds);
        appendOptionalStackPart(item, override.marginEdge);
        appendOptionalStackPart(item, override.shadow);
        item.push_back('\x1e');
      }

      signature.push_back(std::move(item));
    }

    return signature;
  }

  [[nodiscard]] bool canActivateWindow(const ToplevelInfo& window) {
    return window.handle != nullptr
        || !window.identifier.empty()
        || (compositors::isKde() && (!window.title.empty() || !window.appId.empty()));
  }

  [[nodiscard]] const ToplevelInfo* newestActivatableWindow(const std::vector<ToplevelInfo>& windows) {
    const ToplevelInfo* best = nullptr;
    for (const auto& window : windows) {
      if (!canActivateWindow(window)) {
        continue;
      }
      best = &window;
    }
    return best;
  }

  [[nodiscard]] bool matchesActiveWindow(
      const ToplevelInfo& window, const ActiveToplevel& active, const std::vector<ToplevelInfo>& windows
  ) {
    if (active.handle != nullptr && window.handle == active.handle) {
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
      std::string_view preferredIdentifier
  ) {
    if (windows.empty()) {
      return nullptr;
    }

    if (active.has_value()) {
      for (std::size_t i = 0; i < windows.size(); ++i) {
        if (!matchesActiveWindow(windows[i], *active, windows)) {
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

  zwlr_foreign_toplevel_handle_v1* nextActivatableWindowHandle(
      const std::vector<ToplevelInfo>& windows, zwlr_foreign_toplevel_handle_v1* activeHandle,
      zwlr_foreign_toplevel_handle_v1* preferredHandle
  ) {
    for (std::size_t i = 0; i < windows.size(); ++i) {
      if (windows[i].handle != nullptr && windows[i].handle == activeHandle) {
        for (std::size_t offset = 1; offset <= windows.size(); ++offset) {
          auto* nextHandle = windows[(i + offset) % windows.size()].handle;
          if (nextHandle != nullptr) {
            return nextHandle;
          }
        }
        return nullptr;
      }
    }

    for (const auto& window : windows) {
      if (window.handle != nullptr && window.handle == preferredHandle) {
        return window.handle;
      }
    }

    for (const auto& window : windows) {
      if (window.handle != nullptr) {
        return window.handle;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool dockPointerHideAllowed(const DockConfig& cfg, const shell::dock::DockInstance& instance) noexcept {
    if (cfg.smartAutoHide) {
      return !instance.smartAutoHidePinnedVisible;
    }
    return cfg.autoHide;
  }

  [[nodiscard]] bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
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

  [[nodiscard]] bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
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
      if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!assignments.empty()) {
      return false;
    }
    return active->occupied;
  }

  [[nodiscard]] bool dockSmartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
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
      [this]() {
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

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this]() {
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
  m_lastPinnedConfig = cfg.pinned;
  m_lastBarLayerStack = barLayerStackSignature(m_config->config());

  if (!cfg.enabled) {
    kLog.info("dock disabled in config");
    return true;
  }

  refreshPinnedAppsIfNeeded();
  return true;
}

void Dock::reload() {
  kLog.info("reloading config");
  const auto& cfg = m_config->config().dock;
  m_lastDockConfig = cfg;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastBarLayerStack = barLayerStackSignature(m_config->config());

  if (!cfg.enabled) {
    closeAllInstances();
    return;
  }

  refreshPinnedAppsIfNeeded();

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

void Dock::show() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  if (m_config == nullptr || !m_config->config().dock.enabled) {
    return;
  }

  pruneCachedToplevelHandles();
  refreshPinnedAppsIfNeeded();
  if (m_instances.empty()) {
    syncInstances();
    return;
  }

  refresh();
}

void Dock::closeAllInstances() {
  m_itemMenu.reset();
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
}

void Dock::onOutputChange() {
  if (!m_config->config().dock.enabled) {
    return;
  }
  syncInstances();
}

void Dock::refresh() {
  if (m_config == nullptr || m_platform == nullptr || !m_config->config().dock.enabled) {
    return;
  }

  pruneCachedToplevelHandles();
  refreshPinnedAppsIfNeeded();

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
    // Auto-hide: show the dock when the pointer enters.
    if (dockPointerHideAllowed(m_config->config().dock, *m_hoveredInstance)
        && m_hoveredInstance->sceneRoot != nullptr) {
      if (m_hoveredInstance->hideAnimId != 0) {
        m_hoveredInstance->animations.cancel(m_hoveredInstance->hideAnimId);
        m_hoveredInstance->hideAnimId = 0;
      }
      const float current = m_hoveredInstance->hideOpacity;
      m_hoveredInstance->hideAnimId = m_hoveredInstance->animations.animate(
          current, 1.0f, Style::animNormal, Easing::EaseOutCubic,
          [inst = m_hoveredInstance, this](float v) {
            inst->hideOpacity = v;
            const auto& cfg = m_config->config().dock;
            shell::dock::syncDockSlideLayerTransform(*inst, cfg);
            shell::dock::applyDockCompositorBlur(*inst, cfg);
          },
          [inst = m_hoveredInstance]() { inst->hideAnimId = 0; }
      );
      // Restore full input region (full surface so shadow-margin edges don't
      // cause an immediate Leave when triggered from the edge of the strip).
      if (m_hoveredInstance->surface != nullptr) {
        const int sw = static_cast<int>(m_hoveredInstance->surface->width());
        const int sh = static_cast<int>(m_hoveredInstance->surface->height());
        m_hoveredInstance->surface->setInputRegion({InputRect{0, 0, sw, sh}});
      }
      m_hoveredInstance->surface->requestRedraw();
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (m_hoveredInstance != nullptr) {
      if (m_hoveredInstance->drag.active || m_hoveredInstance->drag.armed) {
        endDrag(*m_hoveredInstance, false);
      }
      clearHoverZoomPointer(*m_hoveredInstance);
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();

      if (dockPointerHideAllowed(m_config->config().dock, *m_hoveredInstance) && m_popupOwnerInstance == nullptr) {
        shell::dock::startHideFadeOut(*m_hoveredInstance, *m_config);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr)
      break;
    shell::dock::DockInstance* const hovered = m_hoveredInstance;
    hovered->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    // pointerMotion can re-enter the Wayland event loop (tooltip popup creation),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != hovered)
      break;
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

    if (m_hoveredInstance == nullptr)
      break;
    const bool pressed = event.pressed;
    m_hoveredInstance->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
    );
    break;
  }
  case PointerEvent::Type::Axis:
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

bool Dock::refreshPinnedAppsIfNeeded() {
  return shell::dock::refreshPinnedAppsIfNeeded(
      m_config->config().dock, m_lastPinnedConfig, m_pinnedEntries, m_modelSerial, m_entriesVersion
  );
}

void Dock::syncInstances() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  const auto& outputs = m_platform->outputs();
  const auto& cfg = m_config->config().dock;
  const auto& selectedMonitors = cfg.monitors;
  const bool hasStaticContent = !cfg.pinned.empty() || shell::dock::dockLauncherButtonCount(cfg) > 0;
  // When activeMonitorOnly is off, the running-apps check is identical for every output, so hoist it.
  const bool anyRunningGlobal = (!hasStaticContent && cfg.showRunning && !cfg.activeMonitorOnly)
      ? !m_platform->runningAppIds(nullptr).empty()
      : false;
  const auto outputAllowed = [&](const WaylandOutput& output) {
    if (!output.done || !output.hasUsableGeometry()) {
      return false;
    }
    if (!selectedMonitors.empty() && std::ranges::none_of(selectedMonitors, [&output](const std::string& m) {
          return outputMatchesSelector(m, output);
        })) {
      return false;
    }
    if (hasStaticContent) {
      return true;
    }
    if (!cfg.showRunning) {
      return false;
    }
    if (cfg.activeMonitorOnly) {
      return !m_platform->runningAppIds(output.output).empty();
    }
    return anyRunningGlobal;
  };

  // Remove instances for dead outputs or outputs no longer selected.
  std::erase_if(m_instances, [this, &outputs, &outputAllowed](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool drop = (it == outputs.end()) || !outputAllowed(*it);
    if (drop) {
      detachInstanceState(*inst);
    }
    return drop;
  });

  for (const auto& output : outputs) {
    if (!outputAllowed(output))
      continue;
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
  DeferredCall::callLater([this]() {
    m_smartAutoHideReevalQueued = false;
    reevaluateSmartAutoHide();
  });
}

void Dock::reevaluateSmartAutoHide() {
  if (m_platform == nullptr || m_config == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  const auto& cfg = m_config->config().dock;
  if (!cfg.enabled || !cfg.smartAutoHide) {
    return;
  }

  for (const auto& instanceUp : m_instances) {
    shell::dock::DockInstance* instance = instanceUp.get();
    if (instance == nullptr || instance->surface == nullptr) {
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
      if (instance->hideOpacity < 1.0f || pinnedChanged) {
        shell::dock::revealAutoHideDock(*instance, *m_config);
        needsRedraw = true;
      }
    } else if (!instance->pointerInside && m_popupOwnerInstance == nullptr) {
      if (instance->hideOpacity > 0.0f || pinnedChanged) {
        shell::dock::startHideFadeOut(*instance, *m_config);
        needsRedraw = true;
      }
    }

    if (needsRedraw && instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

void Dock::createInstance(const WaylandOutput& output) {
  assertDockInitialized(m_platform, m_config, m_renderContext);

  const auto& cfg = m_config->config().dock;
  kLog.info(
      "creating dock on {} ({}) icon_size={} position={}", output.connectorName, output.description, cfg.iconSize,
      enumToKey(kDockEdges, cfg.position)
  );

  auto instance = std::make_unique<shell::dock::DockInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;

  const auto& shadowConfig = m_config->config().shell.shadow;
  LayerSurfaceConfig lsCfg = shell::dock::makeLayerSurfaceConfig(
      cfg, shadowConfig, cfg.pinned.size() + shell::dock::dockLauncherButtonCount(cfg)
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
    if (m_config == nullptr || m_renderContext == nullptr || !m_config->config().dock.magnification) {
      return;
    }
    const shell::dock::DockItemSceneDependencies deps{
        .model = {.config = *m_config},
        .renderContext = *m_renderContext,
        .iconResolver = m_iconResolver,
    };
    if (shell::dock::updateHoverZoom(*inst, deps, inst->snapshot, deltaMs) && inst->surface != nullptr) {
      inst->surface->requestFrameTick();
      inst->surface->requestRedraw();
    }
  });
  instance->surface->setAnimationManager(&instance->animations);

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to init dock surface for output {}", output.name);
    return;
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
  }

  auto next = shell::dock::buildDockSnapshot({
      .platform = *m_platform,
      .config = m_config->config().dock,
      .output = instance.output,
      .globalActiveIdLower = activeIdLower,
      .pinnedEntries = m_pinnedEntries,
      .sourceSerial = m_modelSerial,
  });

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
  if (!m_config->config().dock.magnification || instance.row == nullptr) {
    return;
  }

  const shell::dock::DockItemSceneDependencies deps{
      .model = {.config = *m_config},
      .renderContext = *m_renderContext,
      .iconResolver = m_iconResolver,
  };

  if (!shell::dock::syncHoverPointerFromScene(instance, m_config->config().dock, sceneX, sceneY)) {
    shell::dock::clearHoverZoom(instance, deps, instance.snapshot);
    return;
  }

  if (instance.surface == nullptr) {
    return;
  }
  instance.surface->requestFrameTick();
  instance.surface->requestRedraw();
}

void Dock::clearHoverZoomPointer(shell::dock::DockInstance& instance) {
  if (m_config == nullptr || m_renderContext == nullptr || !m_config->config().dock.magnification) {
    instance.hoverPointerValid = false;
    return;
  }

  const shell::dock::DockItemSceneDependencies deps{
      .model = {.config = *m_config},
      .renderContext = *m_renderContext,
      .iconResolver = m_iconResolver,
  };
  shell::dock::clearHoverZoom(instance, deps, instance.snapshot);
}

// ── Private: item context menu (right-click) ──────────────────────────────────

void Dock::beginDrag(shell::dock::DockInstance& instance, std::size_t index, float mainPos) {
  if (m_config == nullptr || instance.drag.active) {
    return;
  }

  const auto& cfg = m_config->config().dock;
  if (index >= cfg.pinned.size()) {
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

  const auto& cfg = m_config->config().dock;
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
  const std::size_t sourceIndex = instance.drag.sourceIndex;
  const std::size_t targetIndex = instance.drag.targetIndex;
  instance.drag = {};

  if (wasActive || wasArmed) {
    instance.suppressItemClick = true;
  }

  if (m_config == nullptr) {
    return;
  }

  const auto& cfg = m_config->config().dock;
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
  DeferredCall::callLater([config, pinnedList = std::move(pinnedList)]() mutable {
    if (config != nullptr) {
      (void)config->setOverride({"dock", "pinned"}, std::move(pinnedList));
    }
  });
}

void Dock::closeItemMenu() {
  shell::dock::DockInstance* owner = m_popupOwnerInstance;
  m_popupOwnerInstance = nullptr;
  m_itemMenu.reset();
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
      || owner->hideOpacity <= 0.0f
      || !dockPointerHideAllowed(m_config->config().dock, *owner)) {
    return;
  }
  shell::dock::startHideFadeOut(*owner, *m_config);
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
  const ToplevelInfo* window = newestActivatableWindow(windowsOnTarget);
  if (window == nullptr) {
    auto windows =
        shell::dock::windowsForDockItem(*m_platform, pending.idLower, pending.wmClassLower, pending.outputFilter);
    if (windows.empty() && pending.outputFilter != nullptr) {
      windows = shell::dock::windowsForDockItem(*m_platform, pending.idLower, pending.wmClassLower, nullptr);
    }
    window = newestActivatableWindow(windows);
    if (window == nullptr) {
      return;
    }
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

  pruneCachedToplevelHandles();

  auto windows = shell::dock::windowsForDockItem(
      *m_platform, action.windowLookupIdLower, action.windowLookupWmClassLower,
      shell::dock::dockFilterOutput(m_config->config().dock, instance.output)
  );

  if (windows.empty()) {
    m_pendingLaunchFocus = PendingLaunchFocus{
        .idLower = action.windowLookupIdLower,
        .wmClassLower = action.windowLookupWmClassLower,
        .outputFilter = shell::dock::dockFilterOutput(m_config->config().dock, instance.output),
        .targetOutput = instance.output,
        .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8),
    };
    m_platform->prepareAppLaunchOnOutput(instance.output);
    (void)desktop_entry_launch::launchEntry(action.entry, dockLaunchOptions(*m_platform, *m_config));
    return;
  }

  if (windows.size() == 1) {
    m_platform->activateToplevelInfo(windows[0]);
    return;
  }

  const auto active = m_platform->activeToplevel();
  std::string preferredIdentifier;
  if (const auto it = m_lastActiveIdentifierByAppIdLower.find(action.idLower);
      it != m_lastActiveIdentifierByAppIdLower.end()) {
    preferredIdentifier = it->second;
  }

  if (const ToplevelInfo* nextWindow = nextActivatableWindow(windows, active, preferredIdentifier);
      nextWindow != nullptr) {
    m_platform->activateToplevelInfo(*nextWindow);
    return;
  }

  zwlr_foreign_toplevel_handle_v1* activeHandle = nullptr;
  if (active.has_value()) {
    activeHandle = active->handle;
  }

  auto* preferredHandle = [&]() -> zwlr_foreign_toplevel_handle_v1* {
    const auto it = m_lastActiveHandleByAppIdLower.find(action.idLower);
    return it != m_lastActiveHandleByAppIdLower.end() ? it->second : nullptr;
  }();
  if (auto* nextHandle = nextActivatableWindowHandle(windows, activeHandle, preferredHandle); nextHandle != nullptr) {
    m_platform->activateToplevel(nextHandle);
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
      shell::dock::dockFilterOutput(m_config->config().dock, instance.output)
  );
  const std::string entryId = action.entry.id;
  const std::string entryWorkingDir = action.entry.workingDir;
  const bool entryTerminal = action.entry.terminal;
  const DesktopEntry entryForPin = action.entry;

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
      .launchAction =
          [this, entryId, entryWorkingDir, entryTerminal,
           output = instance.output](const DesktopAction& desktopAction) {
            m_platform->prepareAppLaunchOnOutput(output);
            (void)desktop_entry_launch::launchAction(
                desktopAction, entryId, entryWorkingDir, entryTerminal, dockLaunchOptions(*m_platform, *m_config)
            );
          },
      .setEntryPinned =
          [this, entryForPin](bool pinned) {
            if (m_config == nullptr) {
              return;
            }
            std::vector<std::string> pinnedList = m_config->config().dock.pinned;
            if (pinned) {
              if (shell::dock::pinned_apps::containsEntry(pinnedList, entryForPin)) {
                return;
              }
              pinnedList.push_back(entryForPin.id);
            } else {
              shell::dock::pinned_apps::removeEntry(pinnedList, entryForPin);
            }
            (void)m_config->setOverride({"dock", "pinned"}, std::move(pinnedList));
          },
      .closeMenu = [this]() { closeItemMenu(); },
  };

  auto* layerSurface =
      instance.surface != nullptr ? m_platform->layerSurfaceFor(instance.surface->wlSurface()) : nullptr;
  m_itemMenu = shell::dock::createItemMenu(
      *m_platform, *m_config, *m_renderContext, layerSurface, instance.output, m_config->config().dock, action.entry,
      windows, callbacks
  );
  if (m_itemMenu == nullptr) {
    m_popupOwnerInstance = nullptr;
  }
}

void Dock::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "dock-show",
      [this](const std::string&) -> std::string {
        if (m_config)
          m_config->setDockEnabled(true);
        return "ok\n";
      },
      "dock-show", "Show the dock (persists override)"
  );

  ipc.registerHandler(
      "dock-hide",
      [this](const std::string&) -> std::string {
        if (m_config)
          m_config->setDockEnabled(false);
        return "ok\n";
      },
      "dock-hide", "Hide the dock (persists override)"
  );

  ipc.registerHandler(
      "dock-toggle",
      [this](const std::string&) -> std::string {
        if (m_config)
          m_config->setDockEnabled(!m_config->config().dock.enabled);
        return "ok\n";
      },
      "dock-toggle", "Toggle dock visibility (persists override)"
  );

  ipc.registerHandler(
      "dock-reload",
      [this](const std::string&) -> std::string {
        reload();
        return "ok\n";
      },
      "dock-reload", "Reload dock configuration"
  );
}
