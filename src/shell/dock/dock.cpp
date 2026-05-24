#include "shell/dock/dock.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "shell/surface_shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/palette.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/popup_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_toplevels.h"
#include "xdg-shell-client-protocol.h"

#include <cerrno>
#include <format>
#include <optional>
#include <wayland-client-core.h>

namespace {

  constexpr Logger kLog("dock");

  // Instance-count badge geometry — scales with icon size.
  constexpr float kBadgeSizeRatio = 0.30f; // fraction of icon size
  constexpr float kBadgeMinSize = 16.0f;   // minimum diameter in px
  constexpr float kBadgeFontRatio = 0.72f; // font size relative to badge diameter
  constexpr float kDotSizeRatio = 0.09f;
  constexpr float kDotMinSize = 4.0f;
  constexpr float kDotGap = 3.0f;

  // Thin strip (px) kept in the input region when auto-hide is in the hidden
  // state, so the pointer can re-trigger show on approach to the screen edge.
  constexpr std::int32_t kAutoHideTriggerPx = 2;
  constexpr float kAutoHideSlideExtraPx = 16.0f;

  // Slide the whole dock chrome (panel ± shadow) completely past the layer buffer edge, same
  // idea as notification toasts driving `reveal` until the full card width has cleared the surface.
  std::pair<float, float> computeAutoHideHiddenDelta(
      bool vert, bool isBottom, bool isRight, float w, float h, float contentLeft, float contentTop, float contentRight,
      float contentBottom
  ) {
    const float k = kAutoHideSlideExtraPx;
    if (!vert) {
      if (isBottom) {
        // After slide: contentTop + dy >= h (+ slack) so nothing remains inside the buffer.
        return {0.0f, (h - contentTop) + k};
      }
      // Top-anchored: move up until contentBottom + dy <= 0.
      return {0.0f, -(contentBottom + k)};
    }
    if (isRight) {
      return {(w - contentLeft) + k, 0.0f};
    }
    return {-(contentRight + k), 0.0f};
  }

  std::string currentActiveEntryIdLower(const CompositorPlatform& platform) {
    if (const auto active = platform.activeToplevel(); active.has_value()) {
      return StringUtils::toLower(app_identity::resolveRunningDesktopEntry(active->appId, desktopEntries()).id);
    }
    return {};
  }

  wl_output* currentDockFilterOutput(const DockConfig& cfg, wl_output* instanceOutput) {
    if (!cfg.activeMonitorOnly) {
      return nullptr;
    }
    return instanceOutput;
  }

  popup_chrome::Attachment popupAttachmentForDockPosition(bool isBottom, bool isTop, bool isRight) {
    if (isBottom) {
      return popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Bottom
      };
    }
    if (isTop) {
      return popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
      };
    }
    if (isRight) {
      return popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Right, .vertical = popup_chrome::VerticalAttachment::Center
      };
    }
    return popup_chrome::Attachment{
        .horizontal = popup_chrome::HorizontalAttachment::Left, .vertical = popup_chrome::VerticalAttachment::Center
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

  // Returns an anchor bitmask for the given position string.
  std::uint32_t positionToAnchor(const std::string& pos) {
    if (pos == "top")
      return LayerShellAnchor::Top;
    if (pos == "left")
      return LayerShellAnchor::Left;
    if (pos == "right")
      return LayerShellAnchor::Right;
    return LayerShellAnchor::Bottom; // default
  }

  std::size_t dockLauncherButtonCount(const DockConfig& cfg) {
    return (cfg.launcherPosition == "start" || cfg.launcherPosition == "end") ? 1U : 0U;
  }

  std::string_view dockLauncherIconGlyph(const DockConfig& cfg) {
    return cfg.launcherIcon.empty() ? "grid-dots" : std::string_view{cfg.launcherIcon};
  }

  Radii dockCornerRadii(const DockConfig& cfg) {
    return Radii{
        static_cast<float>(cfg.radiusTopLeft),
        static_cast<float>(cfg.radiusTopRight),
        static_cast<float>(cfg.radiusBottomRight),
        static_cast<float>(cfg.radiusBottomLeft),
    };
  }

  std::unique_ptr<Flex> makeDockItemRow(const DockConfig& cfg, bool vertical) {
    return ui::makeFlex(
        vertical ? FlexDirection::Vertical : FlexDirection::Horizontal, {
                                                                            .align = FlexAlign::Center,
                                                                            .gap = static_cast<float>(cfg.itemSpacing),
                                                                            .padding = static_cast<float>(cfg.padding),
                                                                        }
    );
  }

} // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────────────

Dock::Dock() = default;

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

  refreshPinnedAppsIfNeeded();
  if (m_instances.empty()) {
    syncInstances();
    return;
  }

  refresh();
}

void Dock::closeAllInstances() {
  m_windowMenu.reset();
  m_itemMenu.reset();
  m_surfaceMap.clear();
  m_hoveredInstance = nullptr;
  m_popupOwnerInstance = nullptr;
  m_instances.clear();
}

void Dock::detachInstanceState(DockInstance& inst) {
  if (inst.surface != nullptr) {
    if (wl_surface* const wls = inst.surface->wlSurface()) {
      m_surfaceMap.erase(wls);
    }
  }
  if (m_hoveredInstance == &inst) {
    m_hoveredInstance = nullptr;
  }
  if (m_popupOwnerInstance == &inst) {
    m_windowMenu.reset();
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
  // Route to any open popup first (item menu takes priority over window picker).
  // If a pointer press is not consumed by the popup, close it and let the same
  // event continue to dock item hit-testing.
  if (m_itemMenu != nullptr) {
    const bool consumed = routePopupEvent(m_itemMenu.get(), event);
    if (consumed) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      closeItemMenu();
    }
  }
  if (m_windowMenu != nullptr) {
    const bool consumed = routePopupEvent(m_windowMenu.get(), event);
    if (consumed) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      closeWindowPicker();
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
            syncDockSlideLayerTransform(*inst);
            applyDockCompositorBlur(*inst);
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
        startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  }
  case PointerEvent::Type::Button: {
    auto it = m_surfaceMap.find(event.surface);
    if (it != m_surfaceMap.end()) {
      DockInstance* targetInstance = it->second;
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

    if (m_hoveredInstance == nullptr)
      break;
    const bool pressed = (event.state == 1);
    m_hoveredInstance->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
    );
    break;
  }
  case PointerEvent::Type::Axis:
    break;
  }

  if (m_hoveredInstance != nullptr && m_hoveredInstance->sceneRoot != nullptr &&
      (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
    if (m_hoveredInstance->sceneRoot->layoutDirty()) {
      m_hoveredInstance->surface->requestLayout();
    } else {
      m_hoveredInstance->surface->requestRedraw();
    }
  }

  return m_hoveredInstance != nullptr;
}

// ── Private: geometry helpers ─────────────────────────────────────────────────

bool Dock::isVertical() const {
  const auto& pos = m_config->config().dock.position;
  return pos == "left" || pos == "right";
}

std::int32_t Dock::dockThickness() const {
  const auto& cfg = m_config->config().dock;
  constexpr std::int32_t kCellPad = 6;
  return cfg.iconSize + kCellPad * 2 + cfg.padding * 2;
}

std::int32_t Dock::dockContentSize(std::size_t itemCount) const {
  const auto& cfg = m_config->config().dock;
  const auto n = static_cast<std::int32_t>(itemCount);
  constexpr std::int32_t kCellPad = 6;
  const std::int32_t cellSize = cfg.iconSize + kCellPad * 2;
  if (n == 0)
    return cellSize + cfg.padding * 2;
  return n * cellSize + std::max(0, n - 1) * cfg.itemSpacing + cfg.padding * 2;
}

// ── Private: instance management ─────────────────────────────────────────────

bool Dock::refreshPinnedAppsIfNeeded() {
  if (desktopEntriesVersion() == m_entriesVersion && m_config->config().dock.pinned == m_lastPinnedConfig) {
    return false;
  }

  m_lastPinnedConfig = m_config->config().dock.pinned;
  m_entriesVersion = desktopEntriesVersion();
  m_pinnedEntries.clear();

  const auto& entries = desktopEntries();

  for (const auto& pinnedId : m_config->config().dock.pinned) {
    const auto pinnedLower = StringUtils::toLower(pinnedId);
    bool found = false;

    for (const auto& entry : entries) {
      if (entry.hidden || entry.noDisplay) {
        continue;
      }
      // Match by entry ID (stem of the desktop file path, e.g. "firefox"),
      // by StartupWMClass (lower), or by Name (lower).
      const auto stemLower = StringUtils::toLower([&] {
        // Extract stem: "org.mozilla.firefox.desktop" → "firefox" (last component, no ext)
        const auto slash = entry.id.rfind('/');
        const auto base = (slash == std::string::npos) ? entry.id : entry.id.substr(slash + 1);
        const auto dot = base.rfind('.');
        return (dot == std::string::npos) ? base : base.substr(0, dot);
      }());

      if (stemLower == pinnedLower || app_identity::desktopEntryMatchesLower(entry, pinnedLower)) {
        m_pinnedEntries.push_back(entry);
        found = true;
        break;
      }
    }

    if (!found) {
      kLog.debug("pinned app not found: {}", pinnedId);
      // Add placeholder so the pinned slot is visible even when app is not installed.
      DesktopEntry placeholder;
      placeholder.id = pinnedId;
      placeholder.name = pinnedId;
      placeholder.nameLower = pinnedLower;
      m_pinnedEntries.push_back(std::move(placeholder));
    }
  }

  ++m_modelSerial;
  kLog.debug("pinned app list: {} entries", m_pinnedEntries.size());
  return true;
}

void Dock::syncInstances() {
  const auto& outputs = m_platform->outputs();
  const auto& cfg = m_config->config().dock;
  const auto& selectedMonitors = cfg.monitors;
  const bool hasStaticContent = !cfg.pinned.empty() || dockLauncherButtonCount(cfg) > 0;
  // When activeMonitorOnly is off, the running-apps check is identical for every output, so hoist it.
  const bool anyRunningGlobal = (!hasStaticContent && cfg.showRunning && !cfg.activeMonitorOnly)
                                    ? !m_platform->runningAppIds(nullptr).empty()
                                    : false;
  const auto outputAllowed = [&](const WaylandOutput& output) {
    if (!selectedMonitors.empty() &&
        std::none_of(selectedMonitors.begin(), selectedMonitors.end(), [&output](const std::string& m) {
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
    if (!output.done)
      continue;
    if (!outputAllowed(output))
      continue;
    const bool exists = std::any_of(m_instances.begin(), m_instances.end(), [&output](const auto& inst) {
      return inst->outputName == output.name;
    });
    if (!exists) {
      createInstance(output);
    }
  }
}

void Dock::createInstance(const WaylandOutput& output) {
  const auto& cfg = m_config->config().dock;
  kLog.info(
      "creating dock on {} ({}) icon_size={} position={}", output.connectorName, output.description, cfg.iconSize,
      cfg.position
  );

  auto instance = std::make_unique<DockInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->activeAppIdLower = currentActiveEntryIdLower(*m_platform);

  const bool vert = isVertical();
  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto sb = shell::surface_shadow::bleed(cfg.shadow, shadowConfig);
  const bool hiddenOverlayMode = cfg.autoHide && !cfg.reserveSpace;
  const auto panelW = dockContentSize(cfg.pinned.size() + dockLauncherButtonCount(cfg));
  const auto panelH = dockThickness();
  const auto anchor = positionToAnchor(cfg.position);
  const bool isBottom = (cfg.position == "bottom");
  const bool isRight = (cfg.position == "right");

  // Surface dimensions incorporate shadow bleed + margin.
  std::uint32_t surfW, surfH;
  std::int32_t mL = 0, mR = 0, mT = 0, mB = 0;
  std::int32_t exclusiveZone = 0;

  if (!vert) {
    // Horizontal dock (top / bottom): ends inset = left/right, edge gap = top/bottom.
    surfW = static_cast<std::uint32_t>(panelW + sb.left + sb.right);
    surfH = static_cast<std::uint32_t>(sb.up + panelH + std::min(cfg.marginEdge, sb.down));
    if (isBottom) {
      mB = std::max(0, cfg.marginEdge - sb.down);
      surfH = static_cast<std::uint32_t>(sb.up + panelH + std::min(cfg.marginEdge, sb.down));
      exclusiveZone = hiddenOverlayMode ? 0 : (panelH + std::min(cfg.marginEdge, sb.down));
    } else {
      mT = std::max(0, cfg.marginEdge - sb.up);
      surfH = static_cast<std::uint32_t>(std::min(cfg.marginEdge, sb.up) + panelH + sb.down);
      exclusiveZone = hiddenOverlayMode ? 0 : (std::min(cfg.marginEdge, sb.up) + panelH);
    }
    // marginEnds is applied symmetrically as compositor side margins.
    mL = cfg.marginEnds;
    mR = cfg.marginEnds;
  } else {
    // Vertical dock (left / right): ends inset = top/bottom, edge gap = left/right.
    surfH = static_cast<std::uint32_t>(panelW + sb.up + sb.down);
    if (isRight) {
      mR = std::max(0, cfg.marginEdge - sb.right);
      surfW = static_cast<std::uint32_t>(sb.left + panelH + std::min(cfg.marginEdge, sb.right));
      exclusiveZone = hiddenOverlayMode ? 0 : (panelH + std::min(cfg.marginEdge, sb.right));
    } else {
      mL = std::max(0, cfg.marginEdge - sb.left);
      surfW = static_cast<std::uint32_t>(std::min(cfg.marginEdge, sb.left) + panelH + sb.right);
      exclusiveZone = hiddenOverlayMode ? 0 : (std::min(cfg.marginEdge, sb.left) + panelH);
    }
    mT = cfg.marginEnds;
    mB = cfg.marginEnds;
  }

  LayerSurfaceConfig lsCfg{
      .nameSpace = "noctalia-dock",
      .layer = LayerShellLayer::Top,
      .anchor = anchor,
      .width = surfW,
      .height = surfH,
      .exclusiveZone = exclusiveZone,
      .marginTop = mT,
      .marginRight = mR,
      .marginBottom = mB,
      .marginLeft = mL,
      .defaultWidth = surfW,
      .defaultHeight = surfH,
  };

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(lsCfg));
  instance->surface->setRenderContext(m_renderContext);

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([inst](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    inst->surface->requestLayout();
  });
  instance->surface->setPrepareFrameCallback([this, inst](bool needsUpdate, bool needsLayout) {
    prepareFrame(*inst, needsUpdate, needsLayout);
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

void Dock::prepareFrame(DockInstance& instance, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  const auto width = instance.surface->width();
  const auto height = instance.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());

  bool needsModelRebuild = false;
  if (needsUpdate || instance.sceneRoot == nullptr) {
    UiPhaseScope updatePhase(UiPhase::Update);
    needsModelRebuild = syncInstanceModel(instance);
  }

  const bool needsSceneBuild = instance.sceneRoot == nullptr ||
                               static_cast<std::uint32_t>(std::round(instance.sceneRoot->width())) != width ||
                               static_cast<std::uint32_t>(std::round(instance.sceneRoot->height())) != height;
  if (needsSceneBuild || needsLayout || needsModelRebuild) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    if (needsModelRebuild && instance.sceneRoot != nullptr) {
      rebuildItems(instance);
    }
    buildScene(instance);
  } else if (needsUpdate) {
    updateVisuals(instance);
  }
}

bool Dock::syncInstanceModel(DockInstance& instance) {
  if (m_platform == nullptr || m_config == nullptr) {
    return false;
  }

  const auto& cfg = m_config->config().dock;
  const std::string globalActiveIdLower = currentActiveEntryIdLower(*m_platform);
  wl_output* const activeOutput = m_platform->activeToplevelOutput();
  wl_output* filterOutput = currentDockFilterOutput(cfg, instance.output);
  const bool filterOutputChanged = (filterOutput != instance.lastFilterOutput);
  instance.lastFilterOutput = filterOutput;

  // When filtering by active monitor, inactive monitors' docks should not
  // highlight the globally-active app — it isn't on them.
  const std::string activeIdLower =
      (cfg.activeMonitorOnly && activeOutput != instance.output) ? std::string{} : globalActiveIdLower;
  instance.activeAppIdLower = activeIdLower;

  const auto runningIds = cfg.showRunning ? m_platform->runningAppIds(filterOutput) : std::vector<std::string>{};
  const auto& allEntries = desktopEntries();
  const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, allEntries);
  std::vector<std::string> runningLower;
  runningLower.reserve(resolvedRunning.size());
  for (const auto& run : resolvedRunning) {
    runningLower.push_back(StringUtils::toLower(run.entry.id));
  }

  bool needRebuild = (instance.modelSerial != m_modelSerial) || filterOutputChanged;
  if (!needRebuild && cfg.showRunning) {
    const std::size_t expectedTotal = [&] {
      std::vector<DesktopEntry> entries = m_pinnedEntries;
      for (const auto& run : resolvedRunning) {
        bool present = false;
        for (const auto& entry : entries) {
          if (app_identity::desktopEntryMatchesLower(entry, run.runningLower)) {
            present = true;
            break;
          }
        }
        if (!present) {
          entries.push_back(run.entry);
        }
      }
      return entries.size();
    }();
    if (expectedTotal != instance.items.size()) {
      needRebuild = true;
    }
  }

  for (auto& item : instance.items) {
    item.running = matchesRunningApp(item, runningLower);
    item.active = matchesActiveApp(item, activeIdLower);
  }

  return needRebuild;
}

void Dock::syncDockSlideLayerTransform(DockInstance& instance) {
  if (instance.slideRoot == nullptr) {
    return;
  }
  const auto& cfg = m_config->config().dock;
  if (cfg.autoHide) {
    const float t = 1.0f - instance.hideOpacity;
    instance.slideRoot->setPosition(instance.slideHiddenDx * t, instance.slideHiddenDy * t);
  } else {
    instance.slideRoot->setPosition(0.0f, 0.0f);
  }
}

void Dock::applyDockCompositorBlur(DockInstance& instance) {
  const auto& cfg = m_config->config().dock;
  if (instance.surface == nullptr) {
    return;
  }
  // Compositor blur is independent of scene opacity — clear it while auto-hide
  // has faded the dock out so a transparent buffer does not leave a blur halo.
  constexpr float kBlurVisibleOpacity = 0.02f;
  if (cfg.autoHide && instance.hideOpacity < kBlurVisibleOpacity) {
    instance.surface->clearBlurRegion();
    return;
  }
  if (instance.panel == nullptr) {
    return;
  }
  float absX = 0.0f;
  float absY = 0.0f;
  Node::absolutePosition(instance.panel, absX, absY);
  const int px = static_cast<int>(std::lround(absX));
  const int py = static_cast<int>(std::lround(absY));
  const int pw = static_cast<int>(std::lround(instance.panel->width()));
  const int ph = static_cast<int>(std::lround(instance.panel->height()));
  const Radii radii = dockCornerRadii(cfg);
  auto blurStrips = Surface::tessellateRoundedRect(px, py, pw, ph, radii.tl, radii.tr, radii.br, radii.bl);
  instance.surface->setBlurRegion(blurStrips);
}

void Dock::buildScene(DockInstance& instance) {
  uiAssertNotRendering("Dock::buildScene");
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  const auto& cfg = m_config->config().dock;
  const bool vert = isVertical();

  const float w = static_cast<float>(instance.surface->width());
  const float h = static_cast<float>(instance.surface->height());

  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto sb = shell::surface_shadow::bleed(cfg.shadow, shadowConfig);
  const float bleedL = static_cast<float>(sb.left);
  const float bleedR = static_cast<float>(sb.right);
  const float bleedU = static_cast<float>(sb.up);
  const float bleedD = static_cast<float>(sb.down);
  const float mEdge = static_cast<float>(cfg.marginEdge);
  const bool isBottom = (cfg.position == "bottom");
  const bool isRight = (cfg.position == "right");

  // Panel visual area within the surface.
  float panelX, panelY, panelW, panelH;
  if (!vert) {
    panelX = bleedL;
    panelY = isBottom ? bleedU : std::min(mEdge, bleedU);
    panelW = w - bleedL - bleedR;
    panelH = static_cast<float>(dockThickness());
  } else {
    panelX = isRight ? bleedL : std::min(mEdge, bleedL);
    panelY = bleedU;
    panelW = static_cast<float>(dockThickness());
    panelH = h - bleedU - bleedD;
  }

  const Radii radii = dockCornerRadii(cfg);

  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = std::make_unique<Node>();
    instance.sceneRoot->setAnimationManager(&instance.animations);
    instance.sceneRoot->setSize(w, h);
    instance.sceneRoot->setOpacity(1.0f);

    auto slide = std::make_unique<Node>();
    slide->setParticipatesInLayout(false);
    instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

    // Shadow
    if (shell::surface_shadow::enabled(cfg.shadow, shadowConfig)) {
      instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
    }

    // Panel background
    instance.panel = static_cast<Box*>(instance.slideRoot->addChild(
        ui::box({
            .configure = [radii](Box& box) { box.setRadii(radii); },
        })
    ));

    // Item row
    instance.row = static_cast<Flex*>(instance.panel->addChild(makeDockItemRow(cfg, vert)));

    // Wire up InputDispatcher.
    instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
    instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    instance.inputDispatcher.setHoverChangeCallback([inst = &instance](InputArea* /*old*/, InputArea* next) {
      TooltipManager::instance().onHoverChange(next, inst->surface->layerSurface(), inst->output);
    });

    // Populate items and wire up palette reactivity.
    rebuildItems(instance);

    if (cfg.autoHide) {
      // Start off-screen (slide); opacity stays at 1 so the compositor blur matches the panel.
      instance.slideRoot->setOpacity(1.0f);
      instance.hideOpacity = 0.0f;
    } else {
      instance.slideRoot->setOpacity(0.0f);
      instance.hideOpacity = 1.0f;
      instance.animations.animate(
          0.0f, 1.0f, Style::animSlow, Easing::EaseOutCubic,
          [slide = instance.slideRoot](float v) { slide->setOpacity(v); }, {}, instance.slideRoot
      );
    }

    instance.surface->setSceneRoot(instance.sceneRoot.get());
  }

  // Update root size on reconfigure.
  instance.sceneRoot->setSize(w, h);
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }

  // Shadow
  if (instance.shadow != nullptr) {
    const float shadowOffsetX = static_cast<float>(shadowConfig.offsetX);
    const float shadowOffsetY = static_cast<float>(shadowConfig.offsetY);
    const RoundedRectStyle shadowStyle = shell::surface_shadow::style(
        shadowConfig, cfg.backgroundOpacity, shell::surface_shadow::Shape{.radius = radii}
    );
    instance.shadow->setStyle(shadowStyle);
    instance.shadow->setZIndex(-1);
    instance.shadow->setPosition(panelX + shadowOffsetX, panelY + shadowOffsetY);
    instance.shadow->setSize(panelW, panelH);
  }

  // Panel
  applyPanelPalette(instance);
  instance.panel->setPosition(panelX, panelY);
  instance.panel->setSize(panelW, panelH);

  // Row fills panel (padding already applied via Flex::setPadding).
  instance.row->setPosition(0.0f, 0.0f);
  instance.row->setSize(panelW, panelH);
  instance.row->layout(*m_renderContext);

  if (cfg.autoHide) {
    float contentLeft = panelX;
    float contentTop = panelY;
    float contentRight = panelX + panelW;
    float contentBottom = panelY + panelH;
    if (instance.shadow != nullptr) {
      const float sx = panelX + static_cast<float>(shadowConfig.offsetX);
      const float sy = panelY + static_cast<float>(shadowConfig.offsetY);
      contentLeft = std::min(contentLeft, sx);
      contentTop = std::min(contentTop, sy);
      contentRight = std::max(contentRight, sx + panelW);
      contentBottom = std::max(contentBottom, sy + panelH);
    }
    const auto hiddenDelta =
        computeAutoHideHiddenDelta(vert, isBottom, isRight, w, h, contentLeft, contentTop, contentRight, contentBottom);
    instance.slideHiddenDx = hiddenDelta.first;
    instance.slideHiddenDy = hiddenDelta.second;
  } else {
    instance.slideHiddenDx = 0.0f;
    instance.slideHiddenDy = 0.0f;
  }
  syncDockSlideLayerTransform(instance);

  // Input region: trigger strip when hidden (autoHide), full panel otherwise.
  if (cfg.autoHide && instance.hideOpacity < 0.5f) {
    const int surfW = static_cast<int>(w);
    const int surfH = static_cast<int>(h);
    if (!vert) {
      instance.surface->setInputRegion({InputRect{0, surfH - kAutoHideTriggerPx, surfW, kAutoHideTriggerPx}});
    } else if (cfg.position == "left") {
      instance.surface->setInputRegion({InputRect{surfW - kAutoHideTriggerPx, 0, kAutoHideTriggerPx, surfH}});
    } else {
      instance.surface->setInputRegion({InputRect{0, 0, kAutoHideTriggerPx, surfH}});
    }
  } else {
    float absX = 0.0f;
    float absY = 0.0f;
    Node::absolutePosition(instance.panel, absX, absY);
    instance.surface->setInputRegion({InputRect{
        static_cast<int>(std::lround(absX)),
        static_cast<int>(std::lround(absY)),
        static_cast<int>(std::lround(instance.panel->width())),
        static_cast<int>(std::lround(instance.panel->height())),
    }});
  }

  applyDockCompositorBlur(instance);

  // Palette reactivity.
  instance.paletteConn = paletteChanged().connect([inst = &instance, this] {
    applyPanelPalette(*inst);
    if (inst->surface)
      inst->surface->requestRedraw();
  });

  updateVisuals(instance);
}

void Dock::applyPanelPalette(DockInstance& instance) {
  if (instance.panel == nullptr)
    return;
  const float opacity = m_config->config().dock.backgroundOpacity;
  instance.panel->setFill(colorSpecFromRole(ColorRole::Surface, opacity));
  instance.panel->setBorder(colorSpecFromRole(ColorRole::Outline), 0.0f);
}

// ── Private: item population ──────────────────────────────────────────────────

void Dock::rebuildItems(DockInstance& instance) {
  uiAssertNotRendering("Dock::rebuildItems");
  if (instance.row == nullptr || m_renderContext == nullptr) {
    return;
  }

  const auto& cfg = m_config->config().dock;
  const bool vert = isVertical();
  const float iSize = static_cast<float>(cfg.iconSize);

  for (auto& item : instance.items) {
    if (item.scaleAnimId != 0) {
      instance.animations.cancel(item.scaleAnimId);
      item.scaleAnimId = 0;
    }
    if (item.opacityAnimId != 0) {
      instance.animations.cancel(item.opacityAnimId);
      item.opacityAnimId = 0;
    }
  }

  // Clear previous items by recreating the row.
  if (instance.row != nullptr && instance.panel != nullptr) {
    instance.panel->removeChild(instance.row);
    instance.row = nullptr;
  }
  instance.items.clear();

  // Create a fresh row.
  auto freshRow = makeDockItemRow(cfg, vert);
  instance.row = static_cast<Flex*>(
      instance.panel != nullptr ? instance.panel->addChild(std::move(freshRow))
                                : instance.sceneRoot->addChild(std::move(freshRow))
  );

  // Determine items: pinned + (optionally) running-only apps not in pinned.
  std::vector<DesktopEntry> itemEntries = m_pinnedEntries;
  wl_output* filterOutput = currentDockFilterOutput(cfg, instance.output);
  instance.lastFilterOutput = filterOutput;

  if (cfg.showRunning) {
    const auto runningIds = m_platform->runningAppIds(filterOutput);
    const auto& allEntries = desktopEntries();
    const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, allEntries);

    for (const auto& run : resolvedRunning) {
      // Skip if already in itemEntries (covers pinned entries).
      bool alreadyPresent = false;
      for (const auto& itm : itemEntries) {
        if (app_identity::desktopEntryMatchesLower(itm, run.runningLower)) {
          alreadyPresent = true;
          break;
        }
      }
      if (alreadyPresent)
        continue;

      itemEntries.push_back(run.entry);
    }
  }

  const auto activeIdLower = instance.activeAppIdLower;
  const auto runningIds = m_platform->runningAppIds(filterOutput);
  const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, desktopEntries());
  std::vector<std::string> runningLower;
  runningLower.reserve(resolvedRunning.size());
  for (const auto& run : resolvedRunning) {
    runningLower.push_back(StringUtils::toLower(run.entry.id));
  }

  if (cfg.launcherPosition == "start") {
    instance.row->addChild(createLauncherButton(instance));
  }

  // Reserve up-front so emplace_back never reallocates while lambdas hold raw pointers.
  instance.items.reserve(itemEntries.size());

  for (const auto& entry : itemEntries) {
    auto& item = instance.items.emplace_back();
    item.entry = entry;
    item.idLower = StringUtils::toLower(entry.id);
    item.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);
    item.active = matchesActiveApp(item, activeIdLower);
    item.running = matchesRunningApp(item, runningLower);

    // Cell is icon + kCellPad on each side; hover bg fills the full cell.
    constexpr float kCellPad = 6.0f; // px extra on each side
    const float cellMain = iSize + 2.0f * kCellPad;
    const float cellCross = iSize + 2.0f * kCellPad;
    auto areaNode = std::make_unique<InputArea>();
    if (!vert) {
      areaNode->setSize(cellMain, cellCross);
    } else {
      areaNode->setSize(cellCross, cellMain);
    }

    // Hover background — fills cell, radius matches dock panel.
    areaNode->addChild(
        ui::box({
            .out = &item.background,
            .fill = clearColorSpec(),
            .radius = static_cast<float>(cfg.radius),
            .width = cellMain,
            .height = cellMain, // square — excludes indicator strip
            .configure = [](Box& box) { box.setPosition(0.0f, 0.0f); },
        })
    );

    // Icon centred inside the padded cell.
    const std::string& iconPath = [&]() -> const std::string& {
      if (!entry.icon.empty()) {
        const std::string& primary = m_iconResolver.resolve(entry.icon, cfg.iconSize);
        if (!primary.empty()) {
          return primary;
        }
      }
      return m_iconResolver.resolve("application-x-executable", cfg.iconSize);
    }();
    auto iconImg = ui::image({
        .width = iSize,
        .height = iSize,
        .configure = [this, &iconPath, &cfg, kCellPad](Image& image) {
          if (!iconPath.empty() && m_renderContext != nullptr) {
            image.setSourceFile(*m_renderContext, iconPath, cfg.iconSize, true);
          }
          image.setPosition(kCellPad, kCellPad);
        },
    });

    if (iconImg->hasImage()) {
      item.iconImage = static_cast<Image*>(areaNode->addChild(std::move(iconImg)));
    } else {
      // Fallback: Tabler app-window glyph (matches launcher when theme icons are unavailable).
      item.iconGlyph = static_cast<Glyph*>(areaNode->addChild(
          ui::glyph({
              .glyph = "app-window",
              .glyphSize = iSize,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .width = iSize,
              .height = iSize,
              .configure = [kCellPad](Glyph& glyph) { glyph.setPosition(kCellPad, kCellPad); },
          })
      ));
    }

    if (cfg.showDots) {
      const float dot = std::max(kDotMinSize, std::round(iSize * kDotSizeRatio));
      const bool verticalDots = cfg.position == "left" || cfg.position == "right";

      for (std::size_t dotIndex = 0; dotIndex < item.dotIndicators.size(); ++dotIndex) {
        item.dotIndicators[dotIndex] = static_cast<Box*>(areaNode->addChild(
            ui::box({
                .fill = colorSpecFromRole(ColorRole::Secondary),
                .radius = dot * 0.5f,
                .width = dot,
                .height = dot,
                .visible = false,
                .configure = [verticalDots, position = cfg.position, cellMain, dot](Box& box) {
                  if (verticalDots) {
                    const float x = position == "left" ? std::round(cellMain - dot - 1.0f) : 1.0f;
                    box.setPosition(x, std::round((cellMain - dot) * 0.5f));
                  } else {
                    const float y = position == "bottom" ? 1.0f : std::round(cellMain - dot - 1.0f);
                    box.setPosition(std::round((cellMain - dot) * 0.5f), y);
                  }
                },
            })
        ));
      }
    }

    // Instance-count badge — top-right corner of the icon, initially hidden.
    if (cfg.showInstanceCount) {
      const float bd = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);
      const float badgeX = kCellPad + iSize - bd * 0.55f;
      const float badgeY = kCellPad - bd * 0.45f;

      areaNode->addChild(
          ui::box({
              .out = &item.badge,
              .radius = bd * 0.5f,
              .width = bd,
              .height = bd,
              .visible = false,
              .configure = [badgeX, badgeY](Box& box) { box.setPosition(badgeX, badgeY); },
          })
      );

      item.badge->addChild(
          ui::label({
              .out = &item.badgeLabel,
              .fontSize = bd * kBadgeFontRatio,
              .maxLines = 1,
              .fontWeight = FontWeight::Bold,
              .visible = false,
          })
      );
    }

    // Pointer callbacks.
    auto* itemPtr = &item;
    auto* instPtr = &instance;

    areaNode->setOnEnter([itemPtr, instPtr](const InputArea::PointerData&) {
      if (!itemPtr->hovered) {
        itemPtr->hovered = true;
        if (itemPtr->background) {
          itemPtr->background->setFill(colorSpecFromRole(ColorRole::Hover, 0.8f));
        }
        if (instPtr->sceneRoot)
          instPtr->sceneRoot->markPaintDirty();
      }
    });
    areaNode->setOnLeave([itemPtr, instPtr]() {
      if (itemPtr->hovered) {
        itemPtr->hovered = false;
        if (itemPtr->background) {
          itemPtr->background->setFill(clearColorSpec());
        }
        if (instPtr->sceneRoot)
          instPtr->sceneRoot->markPaintDirty();
      }
    });
    areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
    areaNode->setOnClick([itemPtr, instPtr, this](const InputArea::PointerData& d) {
      if (d.button == BTN_LEFT) {
        handleItemClick(*instPtr, *itemPtr);
      } else if (d.button == BTN_RIGHT) {
        openItemMenu(*instPtr, *itemPtr);
      }
    });

    item.area = static_cast<InputArea*>(instance.row->addChild(std::move(areaNode)));
  }

  if (cfg.launcherPosition == "end") {
    instance.row->addChild(createLauncherButton(instance));
  }

  instance.modelSerial = m_modelSerial;

  // Force surface resize when item count changes.
  resizeSurface(instance);
}

void Dock::resizeSurface(DockInstance& instance) {
  if (instance.surface == nullptr) {
    return;
  }

  const auto& cfg = m_config->config().dock;
  const bool vert = isVertical();
  const auto sb = shell::surface_shadow::bleed(cfg.shadow, m_config->config().shell.shadow);
  const auto panelW = dockContentSize(instance.items.size() + dockLauncherButtonCount(cfg));
  const auto panelH = dockThickness();
  const bool isBottom = (cfg.position == "bottom");

  std::uint32_t surfW, surfH;
  if (!vert) {
    surfW = static_cast<std::uint32_t>(panelW + sb.left + sb.right);
    surfH = isBottom ? static_cast<std::uint32_t>(sb.up + panelH + std::min(cfg.marginEdge, sb.down))
                     : static_cast<std::uint32_t>(std::min(cfg.marginEdge, sb.up) + panelH + sb.down);
  } else {
    const bool isRight = (cfg.position == "right");
    surfW = isRight ? static_cast<std::uint32_t>(sb.left + panelH + std::min(cfg.marginEdge, sb.right))
                    : static_cast<std::uint32_t>(std::min(cfg.marginEdge, sb.left) + panelH + sb.right);
    surfH = static_cast<std::uint32_t>(panelW + sb.up + sb.down);
  }

  if (instance.surface->width() != surfW || instance.surface->height() != surfH) {
    instance.surface->requestSize(surfW, surfH);
  }
}

// ── Private: visual update ────────────────────────────────────────────────────

void Dock::updateVisuals(DockInstance& instance) {
  const auto& cfg = m_config->config().dock;

  for (auto& item : instance.items) {
    const float iconScale = item.active ? cfg.activeScale : cfg.inactiveScale;
    const float iconOpacity = item.active ? cfg.activeOpacity : cfg.inactiveOpacity;
    Node* iconNode =
        item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);

    if (iconNode != nullptr) {
      if (item.visualScale < 0.0f) {
        item.visualScale = iconScale;
        iconNode->setScale(iconScale);
      } else if (std::abs(item.visualScale - iconScale) > 0.001f) {
        if (item.scaleAnimId != 0) {
          instance.animations.cancel(item.scaleAnimId);
        }
        item.scaleAnimId = instance.animations.animate(
            item.visualScale, iconScale, Style::animNormal, Easing::EaseOutCubic,
            [node = iconNode, itemPtr = &item](float value) {
              itemPtr->visualScale = value;
              node->setScale(value);
            },
            [itemPtr = &item] { itemPtr->scaleAnimId = 0; }
        );
      }

      if (item.visualOpacity < 0.0f) {
        item.visualOpacity = iconOpacity;
        iconNode->setOpacity(iconOpacity);
      } else if (std::abs(item.visualOpacity - iconOpacity) > 0.001f) {
        if (item.opacityAnimId != 0) {
          instance.animations.cancel(item.opacityAnimId);
        }
        item.opacityAnimId = instance.animations.animate(
            item.visualOpacity, iconOpacity, Style::animNormal, Easing::EaseOutCubic,
            [node = iconNode, itemPtr = &item](float value) {
              itemPtr->visualOpacity = value;
              node->setOpacity(value);
            },
            [itemPtr = &item] { itemPtr->opacityAnimId = 0; }
        );
      }
    }

    const bool needsWindowCount = cfg.showDots || item.badge != nullptr;
    std::size_t count = 0;
    if (needsWindowCount) {
      const auto windows = m_platform->windowsForApp(
          item.idLower, item.startupWmClassLower, currentDockFilterOutput(cfg, instance.output)
      );
      count = windows.size();
      item.instanceCount = count;
    }

    if (cfg.showDots) {
      const std::size_t dotCount = std::min<std::size_t>(count, 3);
      const float iSize = static_cast<float>(cfg.iconSize);
      constexpr float kCellPad = 6.0f;
      const float cellMain = iSize + 2.0f * kCellPad;
      const float dot = std::max(kDotMinSize, std::round(iSize * kDotSizeRatio));
      const float groupLength =
          dotCount == 0 ? dot : dot * static_cast<float>(dotCount) + kDotGap * static_cast<float>(dotCount - 1);
      const float groupStart = std::round((cellMain - groupLength) * 0.5f);
      const bool verticalDots = cfg.position == "left" || cfg.position == "right";

      for (std::size_t dotIndex = 0; dotIndex < item.dotIndicators.size(); ++dotIndex) {
        if (item.dotIndicators[dotIndex] == nullptr) {
          continue;
        }
        Box* dotNode = item.dotIndicators[dotIndex];
        const bool visible = dotIndex < dotCount;
        dotNode->setVisible(visible);
        dotNode->setFill(colorSpecFromRole(ColorRole::Secondary));
        if (visible) {
          const float main = groupStart + static_cast<float>(dotIndex) * (dot + kDotGap);
          if (verticalDots) {
            const float x = cfg.position == "left" ? std::round(cellMain - dot - 1.0f) : 1.0f;
            dotNode->setPosition(x, main);
          } else {
            const float y = cfg.position == "bottom" ? 1.0f : std::round(cellMain - dot - 1.0f);
            dotNode->setPosition(main, y);
          }
        }
      }
    }

    // Instance-count badge.
    if (item.badge != nullptr && item.badgeLabel != nullptr) {
      const bool show = count >= 2;
      item.badge->setVisible(show);
      item.badgeLabel->setVisible(show);
      if (show) {
        const std::string label = (count > 9) ? "9+" : std::to_string(count);
        item.badgeLabel->setText(label);
        item.badgeLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary));
        item.badge->setFill(colorSpecFromRole(ColorRole::Primary));
        if (m_renderContext != nullptr) {
          const float bd = std::max(kBadgeMinSize, static_cast<float>(cfg.iconSize) * kBadgeSizeRatio);
          item.badgeLabel->measure(*m_renderContext);
          item.badgeLabel->setPosition(
              std::round((bd - item.badgeLabel->width()) * 0.5f), std::round((bd - item.badgeLabel->height()) * 0.5f)
          );
        }
      }
    }
  }
}

// ── Private: helpers ──────────────────────────────────────────────────────────

bool Dock::matchesActiveApp(const DockItemView& item, std::string_view activeIdLower) const {
  return !activeIdLower.empty() && activeIdLower == item.idLower;
}

bool Dock::matchesRunningApp(const DockItemView& item, const std::vector<std::string>& runningLower) const {
  for (const auto& rid : runningLower) {
    if (!rid.empty() && rid == item.idLower) {
      return true;
    }
  }
  return false;
}

std::unique_ptr<InputArea> Dock::createLauncherButton(DockInstance& instance) {
  const auto& cfg = m_config->config().dock;
  const bool vert = isVertical();
  const float iSize = static_cast<float>(cfg.iconSize);
  constexpr float kCellPad = 6.0f;
  const float cellMain = iSize + 2.0f * kCellPad;
  const float cellCross = iSize + 2.0f * kCellPad;
  const float glyphSize = iSize * 0.8f;
  const float glyphOffsetY = kCellPad + (iSize - glyphSize) * 0.5f;

  auto areaNode = std::make_unique<InputArea>();
  if (!vert) {
    areaNode->setSize(cellMain, cellCross);
  } else {
    areaNode->setSize(cellCross, cellMain);
  }

  Box* bgPtr = nullptr;
  areaNode->addChild(
      ui::box({
          .out = &bgPtr,
          .fill = clearColorSpec(),
          .radius = static_cast<float>(cfg.radius),
          .width = cellMain,
          .height = cellMain,
          .configure = [](Box& box) { box.setPosition(0.0f, 0.0f); },
      })
  );

  areaNode->addChild(
      ui::glyph({
          .glyphSize = glyphSize,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .width = iSize,
          .height = iSize,
          .configure = [&cfg, kCellPad, glyphOffsetY](Glyph& glyph) {
            if (!glyph.setGlyph(dockLauncherIconGlyph(cfg))) {
              glyph.setGlyph("grid-dots");
            }
            glyph.setPosition(kCellPad, glyphOffsetY);
          },
      })
  );

  auto* instPtr = &instance;
  areaNode->setOnEnter([bgPtr, instPtr](const InputArea::PointerData&) {
    bgPtr->setFill(colorSpecFromRole(ColorRole::Hover, 0.8f));
    if (instPtr->sceneRoot != nullptr) {
      instPtr->sceneRoot->markPaintDirty();
    }
  });
  areaNode->setOnLeave([bgPtr, instPtr]() {
    bgPtr->setFill(clearColorSpec());
    if (instPtr->sceneRoot != nullptr) {
      instPtr->sceneRoot->markPaintDirty();
    }
  });
  areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
  areaNode->setOnClick([instPtr](const InputArea::PointerData& d) {
    if (d.button == BTN_LEFT) {
      PanelManager::instance().togglePanel("launcher", PanelOpenRequest{.output = instPtr->output});
    }
  });

  return areaNode;
}

void Dock::launchEntry(const DesktopEntry& entry) {
  if (entry.exec.empty()) {
    kLog.warn("no exec for {}", entry.name);
    return;
  }
  // Strip desktop-entry field codes (%u, %f, %F, %U, …).
  std::string cmd;
  cmd.reserve(entry.exec.size());
  for (std::size_t i = 0; i < entry.exec.size(); ++i) {
    if (entry.exec[i] == '%' && i + 1 < entry.exec.size()) {
      ++i; // skip field code char
      continue;
    }
    cmd += entry.exec[i];
  }
  while (!cmd.empty() && std::isspace(static_cast<unsigned char>(cmd.back()))) {
    cmd.pop_back();
  }
  kLog.info("launching: {}", cmd);
  (void)process::runAsync(cmd);
}

// ── Private: click handling ───────────────────────────────────────────────────

void Dock::handleItemClick(DockInstance& instance, DockItemView& item) {
  // Find all windows matching this item's app.
  auto windows = m_platform->windowsForApp(
      item.idLower, item.startupWmClassLower, currentDockFilterOutput(m_config->config().dock, instance.output)
  );

  if (windows.empty()) {
    // Nothing running — launch the app.
    launchEntry(item.entry);
    return;
  }

  if (windows.size() == 1) {
    // Exactly one window: activate it directly.
    m_platform->activateToplevel(windows[0].handle);
    return;
  }

  // Multiple windows: show a picker popup.
  openWindowPicker(instance, item, std::move(windows));
}

// ── Private: window picker popup ─────────────────────────────────────────────

static constexpr float kMenuWidth = 240.0f;

void Dock::openWindowPicker(DockInstance& instance, DockItemView& item, std::vector<ToplevelInfo> windows) {
  if (!m_platform->hasXdgShell()) {
    // Fallback: activate the first window if we can't show a popup.
    if (!windows.empty()) {
      m_platform->activateToplevel(windows[0].handle);
    }
    return;
  }

  closeWindowPicker();

  m_popupOwnerInstance = &instance;
  auto menu = std::make_unique<DockPopup>();

  // Build context menu entries (window titles).
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(windows.size());
  for (std::size_t i = 0; i < windows.size(); ++i) {
    const auto& title = windows[i].title.empty() ? item.entry.name : windows[i].title;
    entries.push_back(
        ContextMenuControlEntry{
            .id = static_cast<std::int32_t>(i),
            .label = title,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
    menu->handles.push_back(windows[i].handle);
  }

  // Compute popup height.
  const float menuHeight = ContextMenuControl::preferredHeight(entries, entries.size());

  // Determine anchor / gravity + gap based on dock position.
  const auto& cfg = m_config->config().dock;
  const bool isBottom = (cfg.position == "bottom");
  const bool isTop = (cfg.position == "top");
  const bool isRight = (cfg.position == "right");

  std::uint32_t anchor = XDG_POSITIONER_ANCHOR_NONE;
  std::uint32_t gravity = XDG_POSITIONER_GRAVITY_NONE;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  const std::int32_t kGapBottom = std::max(2, static_cast<std::int32_t>(Style::spaceLg));
  const std::int32_t kGap = std::max(2, static_cast<std::int32_t>(Style::spaceMd));

  if (isBottom) {
    anchor = XDG_POSITIONER_ANCHOR_TOP;
    gravity = XDG_POSITIONER_GRAVITY_TOP;
    offsetY = -kGapBottom;
  } else if (isTop) {
    anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
    gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
    offsetY = kGap;
  } else if (isRight) {
    anchor = XDG_POSITIONER_ANCHOR_LEFT;
    gravity = XDG_POSITIONER_GRAVITY_LEFT;
    offsetX = -kGap;
  } else { // left
    anchor = XDG_POSITIONER_ANCHOR_RIGHT;
    gravity = XDG_POSITIONER_GRAVITY_RIGHT;
    offsetX = kGap;
  }

  const auto sb = shell::surface_shadow::bleed(cfg.shadow, m_config->config().shell.shadow);
  const std::int32_t panelThk = dockThickness();
  const std::int32_t ptrX = static_cast<std::int32_t>(m_platform->lastPointerX());
  const std::int32_t ptrY = static_cast<std::int32_t>(m_platform->lastPointerY());
  const std::int32_t halfCell = cfg.iconSize / 2;

  // Anchor rect: pointer-centred on main axis × panel face on cross axis.
  std::int32_t aX, aY, aW, aH;
  if (isBottom) {
    // Panel top face is at sb.up.
    aX = ptrX - halfCell;
    aY = sb.up;
    aW = halfCell * 2;
    aH = panelThk;
  } else if (isTop) {
    const std::int32_t panelFace = std::min(cfg.marginEdge, sb.up) + panelThk;
    aX = ptrX - halfCell;
    aY = 0;
    aW = halfCell * 2;
    aH = panelFace;
  } else if (isRight) {
    aX = sb.left;
    aY = ptrY - halfCell;
    aW = panelThk;
    aH = halfCell * 2;
  } else { // left
    const std::int32_t panelFace = std::min(cfg.marginEdge, sb.left) + panelThk;
    aX = 0;
    aY = ptrY - halfCell;
    aW = panelFace;
    aH = halfCell * 2;
  }

  const auto menuChrome = popup_chrome::computeGeometry(kMenuWidth, menuHeight, m_config->config().shell.shadow);
  PopupSurfaceConfig popupCfg{
      .anchorX = aX,
      .anchorY = aY,
      .anchorWidth = std::max(1, aW),
      .anchorHeight = std::max(1, aH),
      .width = menuChrome.surfaceWidth,
      .height = menuChrome.surfaceHeight,
      .anchor = anchor,
      .gravity = gravity,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = offsetX,
      .offsetY = offsetY,
      .serial = m_platform->lastInputSerial(),
      .grab = true,
  };
  popup_chrome::applyToConfig(popupCfg, menuChrome, popupAttachmentForDockPosition(isBottom, isTop, isRight));

  menu->surface = std::make_unique<PopupSurface>(m_platform->wayland());
  menu->surface->setRenderContext(m_renderContext);
  menu->chrome = menuChrome;

  auto* menuPtr = menu.get();

  menu->surface->setConfigureCallback([menuPtr](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    menuPtr->surface->requestLayout();
  });
  menu->surface->setPrepareFrameCallback([this, menuPtr, entries](bool /*needsUpdate*/, bool needsLayout) {
    if (m_renderContext == nullptr || menuPtr->surface == nullptr) {
      return;
    }

    const auto width = menuPtr->surface->width();
    const auto height = menuPtr->surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    m_renderContext->makeCurrent(menuPtr->surface->renderTarget());

    const bool needsSceneBuild = menuPtr->sceneRoot == nullptr ||
                                 static_cast<std::uint32_t>(std::round(menuPtr->sceneRoot->width())) != width ||
                                 static_cast<std::uint32_t>(std::round(menuPtr->sceneRoot->height())) != height;
    if (!needsSceneBuild && !needsLayout) {
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);

    menuPtr->sceneRoot = std::make_unique<Node>();
    menuPtr->sceneRoot->setSize(fw, fh);
    (void)popup_chrome::addShadow(
        *menuPtr->sceneRoot, menuPtr->chrome, m_config->config().shell.shadow, Style::scaledRadiusLg()
    );

    auto ctrl = std::make_unique<ContextMenuControl>();
    ctrl->setMenuWidth(menuPtr->chrome.contentWidth);
    ctrl->setMaxVisible(entries.size());
    ctrl->setEntries(entries);
    ctrl->setRedrawCallback([menuPtr]() {
      if (menuPtr->surface)
        menuPtr->surface->requestRedraw();
    });
    ctrl->setOnActivate([this, menuPtr](const ContextMenuControlEntry& e) {
      const auto idx = static_cast<std::size_t>(e.id);
      auto* handle = (idx < menuPtr->handles.size()) ? menuPtr->handles[idx] : nullptr;
      DeferredCall::callLater([this, handle]() {
        if (handle != nullptr) {
          m_platform->activateToplevel(handle);
        }
        closeWindowPicker();
      });
    });
    ctrl->setPosition(menuPtr->chrome.contentX(), menuPtr->chrome.contentY());
    ctrl->setSize(menuPtr->chrome.contentWidth, menuPtr->chrome.contentHeight);
    ctrl->layout(*m_renderContext);

    menuPtr->sceneRoot->addChild(std::move(ctrl));
    menuPtr->inputDispatcher.setSceneRoot(menuPtr->sceneRoot.get());
    menuPtr->inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    menuPtr->surface->setSceneRoot(menuPtr->sceneRoot.get());
  });

  menu->surface->setDismissedCallback([this]() { closeWindowPicker(); });

  auto* layerSurface = m_platform->layerSurfaceFor(instance.surface->wlSurface());
  if (layerSurface == nullptr || !menu->surface->initialize(layerSurface, instance.output, popupCfg)) {
    kLog.warn("dock: failed to create window-picker popup");
    return;
  }

  popup_chrome::setContentInputRegion(*menu->surface, menu->chrome);
  menu->wlSurface = menu->surface->wlSurface();
  m_windowMenu = std::move(menu);
}

void Dock::closeWindowPicker() {
  DockInstance* owner = m_popupOwnerInstance;
  m_popupOwnerInstance = nullptr;
  m_windowMenu.reset();
  if (m_config->config().dock.autoHide && owner != nullptr && owner->hideOpacity > 0.0f) {
    owner->pointerInside = false;
    if (m_hoveredInstance == owner) {
      m_hoveredInstance = nullptr;
    }
    startHideFadeOut(*owner);
  }
}

// ── Private: generic popup routing ───────────────────────────────────────────

bool Dock::routePopupEvent(DockPopup* popup, const PointerEvent& event) {
  const bool onPopup = (event.surface != nullptr && event.surface == popup->wlSurface);
  bool consumed = false;

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onPopup) {
      popup->pointerInside = true;
      popup->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    break;
  case PointerEvent::Type::Leave:
    if (onPopup) {
      popup->pointerInside = false;
      popup->inputDispatcher.pointerLeave();
    }
    break;
  case PointerEvent::Type::Motion:
    if (onPopup || popup->pointerInside) {
      if (onPopup) {
        popup->pointerInside = true;
      }
      popup->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
      consumed = true;
    }
    break;
  case PointerEvent::Type::Button:
    if (onPopup || popup->pointerInside) {
      if (onPopup) {
        popup->pointerInside = true;
      }
      // Keep hover state synced before click dispatch so stationary pointers can
      // still activate rows even if Enter/Motion ordering is flaky.
      popup->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      const bool pressed = (event.state == 1);
      popup->inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
      consumed = true;
    }
    break;
  case PointerEvent::Type::Axis:
    break;
  }

  if (popup->surface != nullptr && popup->sceneRoot != nullptr &&
      (popup->sceneRoot->paintDirty() || popup->sceneRoot->layoutDirty())) {
    if (popup->sceneRoot->layoutDirty()) {
      popup->surface->requestLayout();
    } else {
      popup->surface->requestRedraw();
    }
  }

  return consumed;
}

// ── Private: item context menu (right-click) ──────────────────────────────────

void Dock::startHideFadeOut(DockInstance& inst) {
  if (inst.hideAnimId != 0) {
    inst.animations.cancel(inst.hideAnimId);
    inst.hideAnimId = 0;
  }
  const float current = inst.hideOpacity;
  inst.hideAnimId = inst.animations.animate(
      current, 0.0f, Style::animSlow, Easing::EaseInQuad,
      [&inst, this](float v) {
        inst.hideOpacity = v;
        syncDockSlideLayerTransform(inst);
        applyDockCompositorBlur(inst);
      },
      [&inst, this]() {
        inst.hideAnimId = 0;
        if (inst.surface == nullptr)
          return;
        const auto& cfg = m_config->config().dock;
        const bool vert = isVertical();
        const auto sb = shell::surface_shadow::bleed(cfg.shadow, m_config->config().shell.shadow);
        const auto panelW = dockContentSize(inst.items.size());
        const auto panelH = dockThickness();
        const auto surfW = static_cast<int>(vert ? (sb.left + panelH + sb.right) : (panelW + sb.left + sb.right));
        const auto surfH = static_cast<int>(vert ? (panelW + sb.up + sb.down) : (sb.up + panelH + cfg.marginEdge));
        if (!vert) {
          inst.surface->setInputRegion({InputRect{0, surfH - kAutoHideTriggerPx, surfW, kAutoHideTriggerPx}});
        } else if (cfg.position == "left") {
          inst.surface->setInputRegion({InputRect{surfW - kAutoHideTriggerPx, 0, kAutoHideTriggerPx, surfH}});
        } else {
          inst.surface->setInputRegion({InputRect{0, 0, kAutoHideTriggerPx, surfH}});
        }
      }
  );
  if (inst.surface)
    inst.surface->requestRedraw();
}

void Dock::closeItemMenu() {
  DockInstance* owner = m_popupOwnerInstance;
  m_popupOwnerInstance = nullptr;
  m_itemMenu.reset();
  // Fade the owner out — the pointer left the dock to interact with the menu,
  // whether or not the compositor sent a Leave event at that time.
  if (m_config->config().dock.autoHide && owner != nullptr && owner->hideOpacity > 0.0f) {
    owner->pointerInside = false;
    if (m_hoveredInstance == owner) {
      m_hoveredInstance = nullptr;
    }
    startHideFadeOut(*owner);
  }
}

void Dock::launchAction(const DesktopAction& action) {
  if (action.exec.empty()) {
    kLog.warn("no exec for action {}", action.name);
    return;
  }
  // Strip desktop-entry field codes (%u, %f, %F, %U, …).
  std::string cmd;
  cmd.reserve(action.exec.size());
  for (std::size_t i = 0; i < action.exec.size(); ++i) {
    if (action.exec[i] == '%' && i + 1 < action.exec.size()) {
      ++i;
      continue;
    }
    cmd += action.exec[i];
  }
  while (!cmd.empty() && std::isspace(static_cast<unsigned char>(cmd.back()))) {
    cmd.pop_back();
  }
  kLog.info("launching action: {}", cmd);
  (void)process::runAsync(cmd);
}

void Dock::openItemMenu(DockInstance& instance, DockItemView& item) {
  if (!m_platform->hasXdgShell())
    return;

  // Close existing popups before opening the new one.
  closeWindowPicker();
  closeItemMenu();

  m_popupOwnerInstance = &instance;
  auto menu = std::make_unique<DockPopup>();

  // Collect running windows for "Close" / "Close All" entries.
  auto windows = m_platform->windowsForApp(
      item.idLower, item.startupWmClassLower, currentDockFilterOutput(m_config->config().dock, instance.output)
  );
  for (const auto& w : windows) {
    menu->handles.push_back(w.handle);
  }

  // IDs 0..N-1 → desktop actions; -1 → Close; -2 → Close All.
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(item.entry.actions.size() + 3);

  for (std::int32_t i = 0; i < static_cast<std::int32_t>(item.entry.actions.size()); ++i) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = i,
            .label = item.entry.actions[static_cast<std::size_t>(i)].name,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }

  const std::size_t runCount = menu->handles.size();
  if (runCount > 0) {
    if (!entries.empty()) {
      // Separator between app actions and window-management entries.
      entries.push_back(
          ContextMenuControlEntry{.id = -3, .label = {}, .enabled = false, .separator = true, .hasSubmenu = false}
      );
    }
    if (runCount == 1) {
      entries.push_back(
          ContextMenuControlEntry{
              .id = -1,
              .label = i18n::tr("dock.actions.close"),
              .enabled = true,
              .separator = false,
              .hasSubmenu = false
          }
      );
    } else {
      entries.push_back(
          ContextMenuControlEntry{
              .id = -2,
              .label = i18n::tr("dock.actions.close-all"),
              .enabled = true,
              .separator = false,
              .hasSubmenu = false
          }
      );
    }
  }

  if (entries.empty())
    return; // nothing to show

  // Compute popup height.
  const float menuHeight = ContextMenuControl::preferredHeight(entries, entries.size());

  // Determine anchor / gravity + gap based on dock position.
  const auto& cfg = m_config->config().dock;
  const bool isBottom = (cfg.position == "bottom");
  const bool isTop = (cfg.position == "top");
  const bool isRight = (cfg.position == "right");

  std::uint32_t anchor = XDG_POSITIONER_ANCHOR_NONE;
  std::uint32_t gravity = XDG_POSITIONER_GRAVITY_NONE;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  const std::int32_t kGapBottom = std::max(2, static_cast<std::int32_t>(Style::spaceLg));
  const std::int32_t kGap = std::max(2, static_cast<std::int32_t>(Style::spaceMd));

  if (isBottom) {
    anchor = XDG_POSITIONER_ANCHOR_TOP;
    gravity = XDG_POSITIONER_GRAVITY_TOP;
    offsetY = -kGapBottom;
  } else if (isTop) {
    anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
    gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
    offsetY = kGap;
  } else if (isRight) {
    anchor = XDG_POSITIONER_ANCHOR_LEFT;
    gravity = XDG_POSITIONER_GRAVITY_LEFT;
    offsetX = -kGap;
  } else { // left
    anchor = XDG_POSITIONER_ANCHOR_RIGHT;
    gravity = XDG_POSITIONER_GRAVITY_RIGHT;
    offsetX = kGap;
  }

  const auto sb = shell::surface_shadow::bleed(cfg.shadow, m_config->config().shell.shadow);
  const std::int32_t panelThk = dockThickness();
  const std::int32_t ptrX = static_cast<std::int32_t>(m_platform->lastPointerX());
  const std::int32_t ptrY = static_cast<std::int32_t>(m_platform->lastPointerY());
  const std::int32_t halfCell = cfg.iconSize / 2;

  // Anchor rect: pointer-centred on main axis × panel face on cross axis.
  std::int32_t aX, aY, aW, aH;
  if (isBottom) {
    // Panel top face is at sb.up.
    aX = ptrX - halfCell;
    aY = sb.up;
    aW = halfCell * 2;
    aH = panelThk;
  } else if (isTop) {
    const std::int32_t panelFace = std::min(cfg.marginEdge, sb.up) + panelThk;
    aX = ptrX - halfCell;
    aY = 0;
    aW = halfCell * 2;
    aH = panelFace;
  } else if (isRight) {
    aX = sb.left;
    aY = ptrY - halfCell;
    aW = panelThk;
    aH = halfCell * 2;
  } else { // left
    const std::int32_t panelFace = std::min(cfg.marginEdge, sb.left) + panelThk;
    aX = 0;
    aY = ptrY - halfCell;
    aW = panelFace;
    aH = halfCell * 2;
  }

  const auto menuChrome = popup_chrome::computeGeometry(kMenuWidth, menuHeight, m_config->config().shell.shadow);
  PopupSurfaceConfig popupCfg{
      .anchorX = aX,
      .anchorY = aY,
      .anchorWidth = std::max(1, aW),
      .anchorHeight = std::max(1, aH),
      .width = menuChrome.surfaceWidth,
      .height = menuChrome.surfaceHeight,
      .anchor = anchor,
      .gravity = gravity,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = offsetX,
      .offsetY = offsetY,
      .serial = m_platform->lastInputSerial(),
      .grab = true,
  };
  popup_chrome::applyToConfig(popupCfg, menuChrome, popupAttachmentForDockPosition(isBottom, isTop, isRight));

  menu->surface = std::make_unique<PopupSurface>(m_platform->wayland());
  menu->surface->setRenderContext(m_renderContext);
  menu->chrome = menuChrome;

  auto* menuPtr = menu.get();

  // Capture actions by value — item may be rebuilt before the callback fires.
  auto entryActions = item.entry.actions;

  menu->surface->setConfigureCallback([menuPtr](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    menuPtr->surface->requestLayout();
  });
  menu->surface->setPrepareFrameCallback([this, menuPtr, entries,
                                          entryActions](bool /*needsUpdate*/, bool needsLayout) {
    if (m_renderContext == nullptr || menuPtr->surface == nullptr) {
      return;
    }

    const auto width = menuPtr->surface->width();
    const auto height = menuPtr->surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    m_renderContext->makeCurrent(menuPtr->surface->renderTarget());

    const bool needsSceneBuild = menuPtr->sceneRoot == nullptr ||
                                 static_cast<std::uint32_t>(std::round(menuPtr->sceneRoot->width())) != width ||
                                 static_cast<std::uint32_t>(std::round(menuPtr->sceneRoot->height())) != height;
    if (!needsSceneBuild && !needsLayout) {
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);

    menuPtr->sceneRoot = std::make_unique<Node>();
    menuPtr->sceneRoot->setSize(fw, fh);
    (void)popup_chrome::addShadow(
        *menuPtr->sceneRoot, menuPtr->chrome, m_config->config().shell.shadow, Style::scaledRadiusLg()
    );

    auto ctrl = std::make_unique<ContextMenuControl>();
    ctrl->setMenuWidth(menuPtr->chrome.contentWidth);
    ctrl->setMaxVisible(entries.size());
    ctrl->setEntries(entries);
    ctrl->setRedrawCallback([menuPtr]() {
      if (menuPtr->surface)
        menuPtr->surface->requestRedraw();
    });
    ctrl->setOnActivate([this, menuPtr, entryActions](const ContextMenuControlEntry& e) {
      const std::int32_t id = e.id;
      auto closingHandles = menuPtr->handles;
      DeferredCall::callLater([this, id, entryActions, closingHandles = std::move(closingHandles)]() mutable {
        if (id >= 0) {
          const auto idx = static_cast<std::size_t>(id);
          if (idx < entryActions.size()) {
            launchAction(entryActions[idx]);
          }
        } else if (id == -1 && !closingHandles.empty()) {
          m_platform->closeToplevel(closingHandles[0]);
        } else if (id == -2) {
          for (auto* handle : closingHandles) {
            m_platform->closeToplevel(handle);
          }
        }
        closeItemMenu();
      });
    });
    ctrl->setPosition(menuPtr->chrome.contentX(), menuPtr->chrome.contentY());
    ctrl->setSize(menuPtr->chrome.contentWidth, menuPtr->chrome.contentHeight);
    ctrl->layout(*m_renderContext);

    menuPtr->sceneRoot->addChild(std::move(ctrl));
    menuPtr->inputDispatcher.setSceneRoot(menuPtr->sceneRoot.get());
    menuPtr->inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    menuPtr->surface->setSceneRoot(menuPtr->sceneRoot.get());
  });

  menu->surface->setDismissedCallback([this]() { closeItemMenu(); });

  auto* layerSurface = m_platform->layerSurfaceFor(instance.surface->wlSurface());
  if (layerSurface == nullptr || !menu->surface->initialize(layerSurface, instance.output, popupCfg)) {
    kLog.warn("dock: failed to create item-menu popup");
    return;
  }

  popup_chrome::setContentInputRegion(*menu->surface, menu->chrome);
  menu->wlSurface = menu->surface->wlSurface();
  m_itemMenu = std::move(menu);
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
