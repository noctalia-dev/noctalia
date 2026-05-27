#include "shell/dock/dock.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/dock_context_menu.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "shell/dock/dock_items.h"
#include "shell/panel/panel_manager.h"
#include "shell/surface_shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_toplevels.h"

#include <cassert>
#include <cerrno>
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
  dockLaunchOptions(const CompositorPlatform& platform, const ConfigService& config, wl_surface* activationSurface) {
    std::string token;
    if (platform.hasXdgActivation()) {
      token = platform.requestActivationToken(activationSurface);
    }
    return desktop_entry_launch::LaunchOptions{
        .activationToken = std::move(token),
        .runAsSystemdService = config.config().shell.launchAppsAsSystemdServices,
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
          "{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}", bar.name, bar.position, bar.enabled, bar.autoHide,
          bar.reserveSpace, bar.thickness, bar.marginEnds, bar.marginEdge, bar.shadow
      );

      item.push_back('\x1e');
      for (const auto& override : bar.monitorOverrides) {
        item += override.match;
        item.push_back('\x1f');
        appendOptionalStackPart(item, override.enabled);
        appendOptionalStackPart(item, override.autoHide);
        appendOptionalStackPart(item, override.reserveSpace);
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

} // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────────

Dock::Dock() = default;
Dock::~Dock() = default;

bool Dock::initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext) {
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;

  const auto& cfg = m_config->config().dock;
  m_config->addReloadCallback([this]() {
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
  syncInstances();
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
  // Route to any open popup first.
  // If a pointer press is not consumed by the popup, close it and let the same
  // event continue to dock item hit-testing.
  if (m_itemMenu != nullptr) {
    const bool consumed = shell::dock::routePopupEvent(*m_itemMenu, event);
    if (consumed) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      closeItemMenu();
    }
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    auto it = m_surfaceMap.find(event.surface);
    if (it == m_surfaceMap.end()) {
      break;
    }
    m_hoveredInstance = it->second;
    m_hoveredInstance->pointerInside = true;
    m_hoveredInstance->inputDispatcher.pointerEnter(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial
    );
    // Auto-hide: show the dock when the pointer enters.
    if (m_config->config().dock.autoHide && m_hoveredInstance->sceneRoot != nullptr) {
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
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();

      // Clear item hover state.
      for (auto& item : m_hoveredInstance->items) {
        if (item.hovered) {
          item.hovered = false;
          if (item.background != nullptr) {
            item.background->setFill(clearColorSpec());
          }
          if (m_hoveredInstance->sceneRoot) {
            m_hoveredInstance->sceneRoot->markPaintDirty();
          }
        }
      }

      if (m_config->config().dock.autoHide && m_popupOwnerInstance == nullptr) {
        shell::dock::startHideFadeOut(*m_hoveredInstance, *m_config);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr) {
      break;
    }
    m_hoveredInstance->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  }
  case PointerEvent::Type::Button: {
    auto it = m_surfaceMap.find(event.surface);
    if (it != m_surfaceMap.end()) {
      shell::dock::DockInstance* targetInstance = it->second;
      if (m_hoveredInstance != targetInstance) {
        if (m_hoveredInstance != nullptr) {
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
    }

    if (m_hoveredInstance == nullptr) {
      break;
    }
    const bool pressed = (event.state == 1);
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
  const auto& outputs = m_platform->outputs();
  const auto& cfg = m_config->config().dock;
  const auto& selectedMonitors = cfg.monitors;
  const bool hasStaticContent = !cfg.pinned.empty() || shell::dock::dockLauncherButtonCount(cfg) > 0;
  // When activeMonitorOnly is off, the running-apps check is identical for every output, so hoist it.
  const bool anyRunningGlobal = (!hasStaticContent && cfg.showRunning && !cfg.activeMonitorOnly)
      ? !m_platform->runningAppIds(nullptr).empty()
      : false;
  const auto outputAllowed = [&](const WaylandOutput& output) {
    if (!selectedMonitors.empty()
        && std::none_of(selectedMonitors.begin(), selectedMonitors.end(), [&output](const std::string& m) {
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
    const auto it =
        std::find_if(outputs.begin(), outputs.end(), [&inst](const auto& o) { return o.name == inst->outputName; });
    const bool drop = (it == outputs.end()) || !outputAllowed(*it);
    if (drop) {
      detachInstanceState(*inst);
    }
    return drop;
  });

  for (const auto& output : outputs) {
    if (!output.done) {
      continue;
    }
    if (!outputAllowed(output)) {
      continue;
    }
    const bool exists = std::any_of(m_instances.begin(), m_instances.end(), [&output](const auto& inst) {
      return inst->outputName == output.name;
    });
    if (!exists) {
      createInstance(output);
    }
  }
}

void Dock::createInstance(const WaylandOutput& output) {
  assertDockInitialized(m_platform, m_config, m_renderContext);

  const auto& cfg = m_config->config().dock;
  kLog.info(
      "creating dock on {} ({}) icon_size={} position={}", output.connectorName, output.description, cfg.iconSize,
      cfg.position
  );

  auto instance = std::make_unique<shell::dock::DockInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->activeAppIdLower = shell::dock::currentActiveEntryIdLower(*m_platform);

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

  return shell::dock::syncInstanceModel(
      instance,
      {
          .platform = *m_platform,
          .config = *m_config,
          .lastActiveHandleByAppIdLower = m_lastActiveHandleByAppIdLower,
          .pinnedEntries = m_pinnedEntries,
          .modelSerial = m_modelSerial,
      }
  );
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
                  .platform = *m_platform,
                  .config = *m_config,
                  .lastActiveHandleByAppIdLower = m_lastActiveHandleByAppIdLower,
                  .pinnedEntries = m_pinnedEntries,
                  .modelSerial = m_modelSerial,
              },
          .renderContext = *m_renderContext,
          .iconResolver = m_iconResolver,
      },
      {
          .pruneCachedToplevelHandles = [this]() { pruneCachedToplevelHandles(); },
          .launchOptions = [this](
                               wl_surface* activationSurface
                           ) { return dockLaunchOptions(*m_platform, *m_config, activationSurface); },
          .toggleLauncher =
              [](shell::dock::DockInstance& inst) {
                PanelManager::instance().togglePanel("launcher", PanelOpenRequest{.output = inst.output});
              },
          .openItemMenu =
              [this](shell::dock::DockInstance& inst, shell::dock::DockItemView& item) { openItemMenu(inst, item); },
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
                  .platform = *m_platform,
                  .config = *m_config,
                  .lastActiveHandleByAppIdLower = m_lastActiveHandleByAppIdLower,
                  .pinnedEntries = m_pinnedEntries,
                  .modelSerial = m_modelSerial,
              },
          .renderContext = *m_renderContext,
          .iconResolver = m_iconResolver,
      }
  );
}

// ── Private: item context menu (right-click) ──────────────────────────────────

void Dock::closeItemMenu() {
  shell::dock::DockInstance* owner = m_popupOwnerInstance;
  m_popupOwnerInstance = nullptr;
  m_itemMenu.reset();
  // Fade the owner out — the pointer left the dock to interact with the menu,
  // whether or not the compositor sent a Leave event at that time.
  if (owner != nullptr && owner->hideOpacity > 0.0f && m_config->config().dock.autoHide) {
    owner->pointerInside = false;
    if (m_hoveredInstance == owner) {
      m_hoveredInstance = nullptr;
    }
    shell::dock::startHideFadeOut(*owner, *m_config);
  }
}

void Dock::openItemMenu(shell::dock::DockInstance& instance, shell::dock::DockItemView& item) {
  assertDockInitialized(m_platform, m_config, m_renderContext);

  closeItemMenu();

  if (!m_platform->hasXdgShell()) {
    return;
  }

  m_popupOwnerInstance = &instance;

  auto windows = m_platform->windowsForApp(
      item.idLower, item.startupWmClassLower, shell::dock::dockFilterOutput(m_config->config().dock, instance.output)
  );
  const std::string entryId = item.entry.id;
  const std::string entryWorkingDir = item.entry.workingDir;
  const bool entryTerminal = item.entry.terminal;

  shell::dock::DockMenuCallbacks callbacks{
      .activateWindow = [this](zwlr_foreign_toplevel_handle_v1* handle) { m_platform->activateToplevel(handle); },
      .closeWindow = [this](zwlr_foreign_toplevel_handle_v1* handle) { m_platform->closeToplevel(handle); },
      .launchAction =
          [this, entryId, entryWorkingDir, entryTerminal](const DesktopAction& action) {
            (void)desktop_entry_launch::launchAction(
                action, entryId, entryWorkingDir, entryTerminal, dockLaunchOptions(*m_platform, *m_config, nullptr)
            );
          },
      .closeMenu = [this]() { closeItemMenu(); },
  };

  auto* layerSurface =
      instance.surface != nullptr ? m_platform->layerSurfaceFor(instance.surface->wlSurface()) : nullptr;
  m_itemMenu = shell::dock::createItemMenu(
      *m_platform, *m_config, *m_renderContext, layerSurface, instance.output, m_config->config().dock, item.entry,
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
        if (m_config) {
          m_config->setDockEnabled(true);
        }
        return "ok\n";
      },
      "dock-show", "Show the dock (persists override)"
  );

  ipc.registerHandler(
      "dock-hide",
      [this](const std::string&) -> std::string {
        if (m_config) {
          m_config->setDockEnabled(false);
        }
        return "ok\n";
      },
      "dock-hide", "Hide the dock (persists override)"
  );

  ipc.registerHandler(
      "dock-toggle",
      [this](const std::string&) -> std::string {
        if (m_config) {
          m_config->setDockEnabled(!m_config->config().dock.enabled);
        }
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
