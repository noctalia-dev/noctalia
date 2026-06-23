#include "shell/panel/panel_manager.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/clipboard/clipboard_panel.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/box.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <malloc.h>
#include <string>

PanelManager* PanelManager::s_instance = nullptr;

namespace {

  constexpr Logger kLog("panel");
  constexpr std::int32_t kDetachedPanelShadowSafetyPadding = 2;

  struct BarVisibleRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
  };

  BarVisibleRect
  resolveBarVisibleRect(const BarConfig& barConfig, std::int32_t outputWidth, std::int32_t outputHeight) {
    const bool barIsBottom = barConfig.position == "bottom";
    const bool barIsLeft = barConfig.position == "left";
    const bool barIsRight = barConfig.position == "right";
    const bool barIsVertical = barIsLeft || barIsRight;
    const std::int32_t mEdge = std::max(0, barConfig.marginEdge);
    const std::int32_t mEnds = std::max(0, barConfig.marginEnds);
    const std::int32_t thickness = std::max(0, barConfig.thickness);

    const std::int32_t left =
        barIsRight ? std::max(0, outputWidth - mEdge - thickness) : (barIsVertical ? mEdge : mEnds);
    const std::int32_t top =
        barIsBottom ? std::max(0, outputHeight - mEdge - thickness) : (barIsVertical ? mEnds : mEdge);
    const std::int32_t right = barIsVertical ? left + thickness : std::max(left, outputWidth - mEnds);
    const std::int32_t bottom = barIsVertical ? std::max(top, outputHeight - mEnds) : top + thickness;

    return BarVisibleRect{
        .left = left,
        .top = top,
        .right = right,
        .bottom = bottom,
    };
  }

  shell::surface_shadow::Bleed
  detachedPanelSurfaceBleed(bool hasDecoration, const ShellConfig::ShadowConfig& shadow) noexcept {
    auto bleed = shell::surface_shadow::bleed(hasDecoration, shadow);
    if (shell::surface_shadow::enabled(hasDecoration, shadow)) {
      bleed.left += kDetachedPanelShadowSafetyPadding;
      bleed.right += kDetachedPanelShadowSafetyPadding;
      bleed.up += kDetachedPanelShadowSafetyPadding;
      bleed.down += kDetachedPanelShadowSafetyPadding;
    }
    return bleed;
  }

  std::uint32_t panelSurfaceExtent(std::uint32_t contentSize, std::int32_t before, std::int32_t after) noexcept {
    const auto total =
        static_cast<std::int64_t>(contentSize) + static_cast<std::int64_t>(before) + static_cast<std::int64_t>(after);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, total));
  }

  BarConfig resolvePanelBarConfig(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output, std::string_view barName = {}
  ) {
    BarConfig barConfig;
    if (configService == nullptr || configService->config().bars.empty()) {
      return barConfig;
    }

    const auto& bars = configService->config().bars;
    bool found = false;
    if (!barName.empty()) {
      for (const auto& bar : bars) {
        if (bar.name == barName) {
          barConfig = bar;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      barConfig = bars.front();
    }

    if (platform == nullptr || output == nullptr) {
      return barConfig;
    }

    if (const auto* wlOutput = platform->findOutputByWl(output); wlOutput != nullptr) {
      return ConfigService::resolveForOutput(barConfig, *wlOutput);
    }

    return barConfig;
  }

  bool hasMultipleEnabledBarsOnEdge(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output, std::string_view position
  ) {
    if (configService == nullptr || position.empty()) {
      return false;
    }

    const WaylandOutput* wlOutput = nullptr;
    if (platform != nullptr && output != nullptr) {
      wlOutput = platform->findOutputByWl(output);
    }

    std::size_t count = 0;
    for (const auto& bar : configService->config().bars) {
      const BarConfig resolved = wlOutput != nullptr ? ConfigService::resolveForOutput(bar, *wlOutput) : bar;
      if (!resolved.enabled || resolved.position != position) {
        continue;
      }
      ++count;
      if (count > 1) {
        return true;
      }
    }
    return false;
  }

  float resolvePanelContentScale(ConfigService* configService) {
    if (configService == nullptr) {
      return 1.0f;
    }
    return std::max(0.1f, configService->config().shell.uiScale);
  }

  float resolvePanelCardOpacity(ConfigService* configService, float panelBackgroundOpacity) {
    const auto mode =
        configService != nullptr ? configService->config().shell.panel.transparencyMode : PanelTransparencyMode::Solid;
    return panelCardOpacityForTransparencyMode(mode, panelBackgroundOpacity);
  }

  float resolveDetachedPanelBackgroundOpacity(ConfigService* configService) {
    const auto mode =
        configService != nullptr ? configService->config().shell.panel.transparencyMode : PanelTransparencyMode::Solid;
    return detachedPanelBackgroundOpacityForTransparencyMode(mode);
  }

  [[nodiscard]] bool openNearClickEnabledForPanel(const ConfigService* configService, std::string_view panelId) {
    if (panelId == "tray-drawer") {
      return true;
    }
    if (configService == nullptr) {
      return false;
    }
    const auto& pc = configService->config().shell.panel;
    if (panelId == "control-center") {
      if (pc.controlCenterPlacement == PanelPlacement::Centered) {
        return false;
      }
      return pc.openNearClickControlCenter;
    }
    if (panelId == "launcher") {
      if (pc.launcherPlacement == PanelPlacement::Centered) {
        return false;
      }
      return pc.openNearClickLauncher;
    }
    if (panelId == "clipboard") {
      if (pc.clipboardPlacement == PanelPlacement::Centered) {
        return false;
      }
      return pc.openNearClickClipboard;
    }
    if (panelId == "wallpaper") {
      if (pc.wallpaperPlacement == PanelPlacement::Centered) {
        return false;
      }
      return pc.openNearClickWallpaper;
    }
    if (panelId == "session") {
      if (pc.sessionPlacement == PanelPlacement::Centered) {
        return false;
      }
      return pc.openNearClickSession;
    }
    return false;
  }

} // namespace

PanelManager::PanelManager() { s_instance = this; }

PanelManager::~PanelManager() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

PanelManager& PanelManager::instance() { return *s_instance; }

PanelManager* PanelManager::current() noexcept { return s_instance; }

WaylandConnection* PanelManager::wayland() const noexcept {
  return m_platform != nullptr ? &m_platform->wayland() : nullptr;
}

void PanelManager::initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext) {
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;
  m_clickShield.initialize(platform.wayland());
}

void PanelManager::setOpenSettingsWindowCallback(std::function<void()> callback) {
  m_openSettingsWindow = std::move(callback);
}

void PanelManager::setCloseSettingsWindowCallback(std::function<void()> callback) {
  m_closeSettingsWindow = std::move(callback);
}

void PanelManager::setToggleSettingsWindowCallback(std::function<void()> callback) {
  m_toggleSettingsWindow = std::move(callback);
}

void PanelManager::setCloseDesktopWidgetsEditorCallback(std::function<void()> callback) {
  m_closeDesktopWidgetsEditor = std::move(callback);
}

void PanelManager::openSettingsWindow() {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow();
  }
}

void PanelManager::closeSettingsWindow() {
  if (m_closeSettingsWindow) {
    m_closeSettingsWindow();
  }
}

void PanelManager::toggleSettingsWindow() {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_toggleSettingsWindow) {
    m_toggleSettingsWindow();
    return;
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow();
  }
}

void PanelManager::setAttachedPanelGeometryCallback(
    std::function<void(wl_output*, std::string_view, std::optional<AttachedPanelGeometry>)> callback
) {
  m_attachedPanelGeometryCallback = std::move(callback);
}

void PanelManager::setClickShieldExcludeRectsProvider(std::function<std::vector<InputRect>(wl_output*)> provider) {
  m_clickShieldExcludeRectsProvider = std::move(provider);
}

void PanelManager::setFocusGrabBarSurfacesProvider(std::function<std::vector<wl_surface*>()> provider) {
  m_focusGrabBarSurfacesProvider = std::move(provider);
}

void PanelManager::setPanelClosedCallback(std::function<void()> callback) {
  m_panelClosedCallback = std::move(callback);
}

void PanelManager::setPanelOpenedCallback(std::function<void()> callback) {
  m_panelOpenedCallback = std::move(callback);
}

void PanelManager::setAttachedPanelAvailabilityCallback(std::function<bool(wl_output*, std::string_view)> callback) {
  m_attachedPanelAvailabilityCallback = std::move(callback);
}

void PanelManager::setAttachedPanelBarSettledCallback(std::function<bool(wl_output*, std::string_view)> callback) {
  m_attachedPanelBarSettledCallback = std::move(callback);
}

void PanelManager::onAttachedBarRevealSettled(wl_output* output, std::string_view barName) {
  if (!m_attachedOpenAnimationPending || !isAttachedOpen() || m_output != output) {
    return;
  }
  if (!m_sourceBarName.empty() && !barName.empty() && m_sourceBarName != barName) {
    return;
  }
  startAttachedOpenAnimation();
  requestFrameTick();
}

void PanelManager::registerPanel(const std::string& id, std::unique_ptr<Panel> content) {
  m_panels[id] = std::move(content);
}

void PanelManager::openPanel(const std::string& panelId, PanelOpenRequest request) {
  if (m_inTransition) {
    return;
  }

  if (m_closeDesktopWidgetsEditor) {
    m_closeDesktopWidgetsEditor();
  }

  // If a panel is open or closing, destroy it immediately with no close animation.
  // Bump the generation first so any in-flight deferred destroyPanel is a no-op.
  if (isOpen() || m_closing) {
    ++m_destroyGeneration;
    m_closing = false;
    destroyPanel();
  }

  auto it = m_panels.find(panelId);
  if (it == m_panels.end()) {
    kLog.warn("panel manager: unknown panel \"{}\"", panelId);
    return;
  }

  m_activePanel = it->second.get();
  m_activePanelId = panelId;
  m_activePanel->setContentScale(resolvePanelContentScale(m_config));
  m_pendingOpenContext = std::string(request.context);
  m_activePanel->setPendingOpenContext(request.context);

  const auto panelWidth = static_cast<std::uint32_t>(m_activePanel->preferredWidth());
  const auto panelHeight = static_cast<std::uint32_t>(m_activePanel->preferredHeight());
  const auto barConfig = resolvePanelBarConfig(m_config, m_platform, request.output, request.sourceBarName);
  m_sourceBarName = request.sourceBarName.empty() ? barConfig.name : std::string(request.sourceBarName);
  const bool isBottom = barConfig.position == "bottom";
  const bool isLeft = barConfig.position == "left";
  const bool isRight = barConfig.position == "right";
  const std::int32_t panelGap = m_config->config().shell.panel.floatingOffset;
  const auto screenPadding = static_cast<std::int32_t>(Style::spaceSm);

  auto outputWidth = static_cast<std::int32_t>(panelWidth);
  auto outputHeight = static_cast<std::int32_t>(panelHeight);
  if (m_platform != nullptr) {
    const auto* wlOutput = m_platform->findOutputByWl(request.output);
    if (wlOutput != nullptr && wlOutput->width > 0) {
      outputWidth =
          wlOutput->logicalWidth > 0 ? wlOutput->logicalWidth : wlOutput->width / std::max(1, wlOutput->scale);
    }
    if (wlOutput != nullptr && wlOutput->height > 0) {
      outputHeight =
          wlOutput->logicalHeight > 0 ? wlOutput->logicalHeight : wlOutput->height / std::max(1, wlOutput->scale);
    }
  }

  const auto clampMargin = [](float desired, std::int32_t panelSize, std::int32_t outputSize,
                              std::int32_t padding) -> std::int32_t {
    const std::int32_t maxValue = std::max(padding, outputSize - panelSize - padding);
    return static_cast<std::int32_t>(std::clamp(desired, static_cast<float>(padding), static_cast<float>(maxValue)));
  };

  const PanelPlacement activePlacement = m_activePanel->panelPlacement();
  const bool useCenteredPlacement = activePlacement == PanelPlacement::Centered
      || (activePlacement == PanelPlacement::Attached
          && m_attachedPanelAvailabilityCallback != nullptr
          && !m_attachedPanelAvailabilityCallback(request.output, m_sourceBarName));
  const bool useFloatingAnchor =
      !useCenteredPlacement && request.hasAnchorPosition && openNearClickEnabledForPanel(m_config, m_activePanelId);
  const auto detachedShadowBleed =
      detachedPanelSurfaceBleed(m_activePanel->hasDecoration(), m_config->config().shell.shadow);
  const std::uint32_t detachedSurfaceWidth =
      panelSurfaceExtent(panelWidth, detachedShadowBleed.left, detachedShadowBleed.right);
  const std::uint32_t detachedSurfaceHeight =
      panelSurfaceExtent(panelHeight, detachedShadowBleed.up, detachedShadowBleed.down);
  const auto barRect = resolveBarVisibleRect(barConfig, outputWidth, outputHeight);
  const bool multipleBarsOnEdge =
      hasMultipleEnabledBarsOnEdge(m_config, m_platform, request.output, barConfig.position);
  const bool useReservedEdgePlacement =
      !useCenteredPlacement && multipleBarsOnEdge && barConfig.reserveSpace && barConfig.thickness > 0;
  const auto marginLeftFromAnchor = clampMargin(
      request.anchorX - static_cast<float>(panelWidth) * 0.5f, static_cast<std::int32_t>(panelWidth), outputWidth,
      screenPadding
  );
  const auto marginTopFromAnchor = clampMargin(
      request.anchorY - static_cast<float>(panelHeight) * 0.5f, static_cast<std::int32_t>(panelHeight), outputHeight,
      screenPadding
  );

  std::uint32_t standaloneAnchor = 0;
  std::int32_t standaloneMarginTop = 0;
  std::int32_t standaloneMarginRight = 0;
  std::int32_t standaloneMarginBottom = 0;
  std::int32_t standaloneMarginLeft = 0;
  if (!useCenteredPlacement) {
    const std::int32_t barWidth = std::max(0, barRect.right - barRect.left);
    const std::int32_t barHeight = std::max(0, barRect.bottom - barRect.top);
    const auto centeredAlongBarX = clampMargin(
        static_cast<float>(barRect.left) + (static_cast<float>(barWidth) - static_cast<float>(panelWidth)) * 0.5f,
        static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
    );
    const auto centeredAlongBarY = clampMargin(
        static_cast<float>(barRect.top) + (static_cast<float>(barHeight) - static_cast<float>(panelHeight)) * 0.5f,
        static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
    );

    if (useReservedEdgePlacement) {
      if (isLeft) {
        standaloneAnchor = LayerShellAnchor::Left | LayerShellAnchor::Top;
        standaloneMarginLeft = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneAnchor = LayerShellAnchor::Right | LayerShellAnchor::Top;
        standaloneMarginRight = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
        standaloneMarginBottom = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
        standaloneMarginTop = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    } else {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
      if (isLeft) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.right + panelGap), static_cast<std::int32_t>(panelWidth), outputWidth,
            screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.left - static_cast<std::int32_t>(panelWidth) - panelGap),
            static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.top - static_cast<std::int32_t>(panelHeight) - panelGap),
            static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.bottom + panelGap), static_cast<std::int32_t>(panelHeight), outputHeight,
            screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    }
  }

  if (useCenteredPlacement) {
    standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    standaloneMarginLeft = (outputWidth - static_cast<std::int32_t>(panelWidth)) / 2 - detachedShadowBleed.left;
    standaloneMarginTop = (outputHeight - static_cast<std::int32_t>(panelHeight)) / 2 - detachedShadowBleed.up;
  } else {
    if ((standaloneAnchor & LayerShellAnchor::Left) != 0) {
      standaloneMarginLeft -= detachedShadowBleed.left;
    } else if ((standaloneAnchor & LayerShellAnchor::Right) != 0) {
      standaloneMarginRight -= detachedShadowBleed.right;
    }
    if ((standaloneAnchor & LayerShellAnchor::Top) != 0) {
      standaloneMarginTop -= detachedShadowBleed.up;
    } else if ((standaloneAnchor & LayerShellAnchor::Bottom) != 0) {
      standaloneMarginBottom -= detachedShadowBleed.down;
    }
  }

  // Single-bar detached panels are placed relative to the bar's config edge. Honor
  // other surfaces' exclusive zones (exclusive_zone = 0 below) and anchor to the
  // bar's reserved edge so the panel tracks the bar's real on-screen position;
  // subtract the bar's own reservation on the main axis to avoid double-counting.
  // Reproduces the prior absolute placement when nothing else reserves space.
  const bool useBarRelativeDetached = !useCenteredPlacement && !useReservedEdgePlacement;
  if (useBarRelativeDetached) {
    const std::int32_t barReserved =
        barConfig.reserveSpace ? reservedBarExclusiveZone(barConfig, m_config->config().shell.shadow) : 0;
    const auto sw = static_cast<std::int32_t>(detachedSurfaceWidth);
    const auto sh = static_cast<std::int32_t>(detachedSurfaceHeight);
    if (isBottom) {
      standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
      standaloneMarginBottom = outputHeight - sh - standaloneMarginTop - barReserved;
      standaloneMarginTop = 0;
    } else if (isRight) {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Right;
      standaloneMarginRight = outputWidth - sw - standaloneMarginLeft - barReserved;
      standaloneMarginLeft = 0;
    } else if (isLeft) {
      standaloneMarginLeft -= barReserved;
    } else {
      standaloneMarginTop -= barReserved;
    }
  }

  const bool useAttachedPlacement = activePlacement == PanelPlacement::Attached
      && !multipleBarsOnEdge
      && (m_attachedPanelAvailabilityCallback == nullptr
          || m_attachedPanelAvailabilityCallback(request.output, m_sourceBarName))
      && barConfig.thickness > 0
      && outputWidth > 0
      && outputHeight > 0;
  const LayerShellLayer panelLayer =
      useAttachedPlacement ? layerShellLayerFromConfig(barConfig.layer) : m_activePanel->layer();

  // Map shields BEFORE the panel surface is created or committed.
  // Within a single layer, wlroots stacks surfaces by mapping order.
  activateClickShield(panelLayer);

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-panel",
      .layer = m_activePanel->layer(),
      .anchor = standaloneAnchor,
      .width = detachedSurfaceWidth,
      .height = detachedSurfaceHeight,
      .exclusiveZone = useCenteredPlacement ? -1 : 0,
      .marginTop = standaloneMarginTop,
      .marginRight = standaloneMarginRight,
      .marginBottom = standaloneMarginBottom,
      .marginLeft = standaloneMarginLeft,
      .keyboard = m_activePanel->keyboardMode(),
      .defaultWidth = detachedSurfaceWidth,
      .defaultHeight = detachedSurfaceHeight,
  };

  const auto configureSurfaceCallbacks = [this](Surface& surface) {
    surface.setRenderContext(m_renderContext);
    surface.setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) {
      if (m_surface != nullptr) {
        m_surface->requestLayout();
      }
    });
    surface.setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
      prepareFrame(needsUpdate, needsLayout);
    });
    surface.setFrameTickCallback([this](float deltaMs) {
      startAttachedOpenAnimation();
      if (m_activePanel != nullptr) {
        m_activePanel->onFrameTick(deltaMs);
      }
    });
    surface.setAnimationManager(&m_animations);
  };

  const auto resetPanelOpenState = [this]() {
    deactivateOutsideClickHandlers();
    m_surface.reset();
    m_layerSurface = nullptr;
    m_output = nullptr;
    m_wlSurface = nullptr;
    m_activePanel = nullptr;
    m_activePanelId.clear();
    m_pendingOpenContext.clear();
    m_panelInsetX = 0;
    m_panelInsetY = 0;
    m_panelVisualWidth = 0;
    m_panelVisualHeight = 0;
    m_attachedBackgroundOpacity = 1.0f;
    m_attachedContactShadow = false;
    m_attachedRevealProgress = 1.0f;
    m_attachedRevealDirection = AttachedRevealDirection::Down;
    m_keyboardRelaxTimer.stop();
    m_attachedBarPosition.clear();
    m_sourceBarName.clear();
    m_attachedPanelGeometry.reset();
    m_attachedToBar = false;
    m_attachedOpenAnimationPending = false;
  };

  if (useAttachedPlacement) {
    const std::string_view barPosition = barConfig.position;
    const bool barIsBottom = barPosition == "bottom";
    const bool barIsLeft = barPosition == "left";
    const bool barIsRight = barPosition == "right";
    const bool barIsVertical = barIsLeft || barIsRight;

    const float scale = m_activePanel->contentScale();
    const float cornerRadius = Style::scaledRadiusXl(scale);
    const auto& shadowConfig = m_config->config().shell.shadow;
    const auto shadowBleed = shell::surface_shadow::bleed(m_activePanel->hasDecoration(), shadowConfig);
    const auto cornerOutset = static_cast<std::int32_t>(std::ceil(cornerRadius));

    // Cross-axis outset wraps the concave-corner overhang and shadow bleed.
    // Main-axis bleed extends only away from the bar edge.
    std::int32_t crossOutsetStart = 0;
    std::int32_t crossOutsetEnd = 0;
    std::int32_t mainBleedAway = 0;
    if (barIsVertical) {
      crossOutsetStart = std::max(shadowBleed.up, shadowBleed.down) + cornerOutset + 2;
      crossOutsetEnd = crossOutsetStart;
      mainBleedAway = (barIsLeft ? shadowBleed.right : shadowBleed.left) + 2;
    } else {
      crossOutsetStart = std::max(shadowBleed.left, shadowBleed.right) + cornerOutset + 2;
      crossOutsetEnd = crossOutsetStart;
      mainBleedAway = (barIsBottom ? shadowBleed.up : shadowBleed.down) + 2;
    }

    const auto crossPad = static_cast<std::uint32_t>(std::max(0, crossOutsetStart + crossOutsetEnd));
    const auto mainPad = static_cast<std::uint32_t>(std::max(0, mainBleedAway));
    const std::uint32_t surfaceWidth = barIsVertical ? (panelWidth + mainPad) : (panelWidth + crossPad);
    const std::uint32_t surfaceHeight = barIsVertical ? (panelHeight + crossPad) : (panelHeight + mainPad);

    // Bar visible rect in screen coords, derived from BarConfig + output dimensions.
    const std::int32_t mEnds = std::max(0, barConfig.marginEnds);
    const std::int32_t barLeft = barRect.left;
    const std::int32_t barTop = barRect.top;
    const std::int32_t barRight = barRect.right;
    const std::int32_t barBottom = barRect.bottom;

    // Place panel along bar main axis using click anchor or center fallback.
    // Inset from bar end equals barR plus panelR for concave cutout nesting.
    const auto computeTotalInset = [&](float barR) -> std::int32_t {
      return static_cast<std::int32_t>(std::ceil(barR + cornerRadius));
    };
    // Bar corner radii at the attachment edge.
    const auto barRStart = static_cast<float>(
        barIsVertical ? (barIsLeft ? barConfig.radiusTopRight : barConfig.radiusTopLeft)
                      : (barIsBottom ? barConfig.radiusTopLeft : barConfig.radiusBottomLeft)
    );
    const auto barREnd = static_cast<float>(
        barIsVertical ? (barIsLeft ? barConfig.radiusBottomRight : barConfig.radiusBottomLeft)
                      : (barIsBottom ? barConfig.radiusTopRight : barConfig.radiusBottomRight)
    );
    const auto totalStartInset = computeTotalInset(barRStart);
    const auto totalEndInset = computeTotalInset(barREnd);
    // Logical px the attached panel overlaps the bar edge to hide the seam (per-bar/per-monitor tunable).
    const std::int32_t panelOverlap = barConfig.panelOverlap;
    std::int32_t visualX = 0;
    std::int32_t visualY = 0;
    const bool useAnchorForAttached =
        request.hasAnchorPosition && openNearClickEnabledForPanel(m_config, m_activePanelId);
    if (barIsVertical) {
      const auto minY = barTop + totalStartInset;
      const auto maxY = std::max(minY, barBottom - static_cast<std::int32_t>(panelHeight) - totalEndInset);
      const auto centeredY = barTop + (barBottom - barTop - static_cast<std::int32_t>(panelHeight)) / 2;
      const auto desiredY =
          static_cast<std::int32_t>(std::lround(request.anchorY - static_cast<float>(panelHeight) * 0.5f));
      visualY = useAnchorForAttached ? std::clamp(desiredY, minY, maxY) : centeredY;
      visualX = barIsLeft ? barRight - panelOverlap : barLeft - static_cast<std::int32_t>(panelWidth) + panelOverlap;
    } else {
      const auto minX = barLeft + totalStartInset;
      const auto maxX = std::max(minX, barRight - static_cast<std::int32_t>(panelWidth) - totalEndInset);
      const auto centeredX = barLeft + (barRight - barLeft - static_cast<std::int32_t>(panelWidth)) / 2;
      const auto desiredX =
          static_cast<std::int32_t>(std::lround(request.anchorX - static_cast<float>(panelWidth) * 0.5f));
      visualX = useAnchorForAttached ? std::clamp(desiredX, minX, maxX) : centeredX;
      visualY = barIsBottom ? barTop - static_cast<std::int32_t>(panelHeight) + panelOverlap : barBottom - panelOverlap;
    }

    // Surface origin: cross-axis outset on each side, main-axis bleed on the side opposite the bar.
    std::int32_t surfaceX = 0;
    std::int32_t surfaceY = 0;
    if (barIsVertical) {
      surfaceY = visualY - crossOutsetStart;
      surfaceX = barIsLeft ? visualX : visualX - mainBleedAway;
    } else {
      surfaceX = visualX - crossOutsetStart;
      surfaceY = barIsBottom ? visualY - mainBleedAway : visualY;
    }

    m_panelInsetX = visualX - surfaceX;
    m_panelInsetY = visualY - surfaceY;
    m_panelVisualWidth = panelWidth;
    m_panelVisualHeight = panelHeight;
    m_attachedBackgroundOpacity = m_activePanel->inheritsBarBackgroundOpacity()
        ? barConfig.backgroundOpacity
        : m_activePanel->attachedBackgroundOpacityOverride();
    m_attachedContactShadow = barConfig.contactShadow;
    m_attachedRevealProgress = 0.0f;
    m_attachedRevealDirection = attached_panel::revealDirection(barPosition);
    m_keyboardRelaxTimer.stop();
    m_attachedBarPosition = std::string(barPosition);
    m_attachedToBar = true;

    // Convert panel screen coords to bar-surface-local coords for shadow exclusion.
    // Bar surface origin sits one shadow bleed inset from the visible bar top-left.
    const auto barShadowBleed = shell::surface_shadow::bleed(barConfig.shadow, shadowConfig);
    std::int32_t barSurfaceLocalVisualX;
    std::int32_t barSurfaceLocalVisualY;
    if (barIsVertical) {
      barSurfaceLocalVisualY = visualY - (barTop - std::min(mEnds, barShadowBleed.up));
      const std::int32_t barSurfaceOriginX =
          barIsLeft ? std::max(0, barLeft - barShadowBleed.left) : barLeft - barShadowBleed.left;
      barSurfaceLocalVisualX = visualX - barSurfaceOriginX;
    } else {
      barSurfaceLocalVisualX = visualX - (barLeft - std::min(mEnds, barShadowBleed.left));
      const std::int32_t barSurfaceOriginY =
          barIsBottom ? barTop - barShadowBleed.up : std::max(0, barTop - barShadowBleed.up);
      barSurfaceLocalVisualY = visualY - barSurfaceOriginY;
    }

    // Geometry passed to the bar for shadow exclusion in bar-surface-local coords.
    // Visible rect extends past the body by cornerRadius on the cross axis.
    AttachedPanelGeometry attachedGeometry;
    attachedGeometry.cornerRadius = cornerRadius;
    attachedGeometry.bulgeRadius = cornerRadius;
    if (barIsVertical) {
      attachedGeometry.x = static_cast<float>(barSurfaceLocalVisualX);
      attachedGeometry.y = static_cast<float>(barSurfaceLocalVisualY) - cornerRadius;
      attachedGeometry.width = static_cast<float>(panelWidth);
      attachedGeometry.height = static_cast<float>(panelHeight) + cornerRadius * 2.0f;
    } else {
      attachedGeometry.x = static_cast<float>(barSurfaceLocalVisualX) - cornerRadius;
      attachedGeometry.y = static_cast<float>(barSurfaceLocalVisualY);
      attachedGeometry.width = static_cast<float>(panelWidth) + cornerRadius * 2.0f;
      attachedGeometry.height = static_cast<float>(panelHeight);
    }
    m_attachedPanelGeometry = attachedGeometry;

    // Anchor against the bar's reserved edge and honor other surfaces' exclusive
    // zones (exclusive_zone = 0). The compositor stacks the panel past any external
    // reservation on that edge exactly as it does the bar, so the panel tracks the
    // bar's real on-screen position. surfaceX/surfaceY are computed from the bar's
    // config edge; subtracting the bar's own reservation on the main axis avoids
    // double-counting it. With no other reservation this matches the old absolute
    // placement; it self-corrects by the external reservation when one exists.
    const std::int32_t barReserved = barConfig.reserveSpace ? reservedBarExclusiveZone(barConfig, shadowConfig) : 0;
    std::uint32_t attachedAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    std::int32_t attachedMarginTop = surfaceY;
    std::int32_t attachedMarginRight = 0;
    std::int32_t attachedMarginBottom = 0;
    std::int32_t attachedMarginLeft = surfaceX;
    if (barIsBottom) {
      attachedAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
      attachedMarginTop = 0;
      attachedMarginBottom = outputHeight - static_cast<std::int32_t>(surfaceHeight) - surfaceY - barReserved;
    } else if (barIsRight) {
      attachedAnchor = LayerShellAnchor::Top | LayerShellAnchor::Right;
      attachedMarginLeft = 0;
      attachedMarginRight = outputWidth - static_cast<std::int32_t>(surfaceWidth) - surfaceX - barReserved;
    } else if (barIsLeft) {
      attachedMarginLeft = surfaceX - barReserved;
    } else {
      attachedMarginTop = surfaceY - barReserved;
    }

    auto attachedConfig = LayerSurfaceConfig{
        .nameSpace = "noctalia-attached-panel",
        .layer = panelLayer,
        .anchor = attachedAnchor,
        .width = surfaceWidth,
        .height = surfaceHeight,
        .exclusiveZone = 0,
        .marginTop = attachedMarginTop,
        .marginRight = attachedMarginRight,
        .marginBottom = attachedMarginBottom,
        .marginLeft = attachedMarginLeft,
        .keyboard = (m_platform != nullptr
                     && m_platform->focusGrabService() != nullptr
                     && m_platform->focusGrabService()->available())
            ? LayerShellKeyboard::Exclusive
            : LayerShellKeyboard::None,
        .defaultWidth = surfaceWidth,
        .defaultHeight = surfaceHeight,
    };

    auto layerSurfaceUnique = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(attachedConfig));
    m_layerSurface = layerSurfaceUnique.get();
    m_surface = std::move(layerSurfaceUnique);
    configureSurfaceCallbacks(*m_surface);

    m_inTransition = true;
    const bool ok = m_layerSurface->initialize(request.output);
    m_inTransition = false;

    if (ok) {
      m_output = request.output;
      m_wlSurface = m_surface->wlSurface();
      m_surface->setInputRegion(
          {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
      );
      applyPanelCompositorBlur();
      publishAttachedPanelGeometry(m_attachedRevealProgress);
      m_surface->requestRedraw();
      const bool hasFocusGrab = m_platform != nullptr
          && m_platform->focusGrabService() != nullptr
          && m_platform->focusGrabService()->available();
      const std::uint64_t gen = m_destroyGeneration;
      if (hasFocusGrab) {
        activateFocusGrab();
        m_keyboardRelaxTimer.start(std::chrono::milliseconds(100), [this, gen]() {
          if (m_destroyGeneration != gen || !isAttachedOpen() || m_layerSurface == nullptr || m_closing) {
            return;
          }
          m_layerSurface->setKeyboardInteractivity(LayerShellKeyboard::OnDemand);
        });
      } else {
        m_keyboardRelaxTimer.start(std::chrono::milliseconds(100), [this, gen]() {
          if (m_destroyGeneration != gen || !isAttachedOpen() || m_layerSurface == nullptr || m_closing) {
            return;
          }
          m_layerSurface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
        });
      }
      kLog.debug("panel manager: opened \"{}\" as attached layer-shell", panelId);
      if (m_panelOpenedCallback) {
        m_panelOpenedCallback();
      }
      return;
    }

    if (m_attachedPanelGeometryCallback) {
      m_attachedPanelGeometryCallback(request.output, m_sourceBarName, std::nullopt);
    }
    m_surface.reset();
    m_layerSurface = nullptr;
    m_attachedToBar = false;
    m_panelInsetX = 0;
    m_panelInsetY = 0;
    m_panelVisualWidth = 0;
    m_panelVisualHeight = 0;
    m_attachedBackgroundOpacity = 1.0f;
    m_attachedContactShadow = false;
    m_attachedRevealProgress = 1.0f;
    m_attachedRevealDirection = AttachedRevealDirection::Down;
    m_keyboardRelaxTimer.stop();
    m_attachedBarPosition.clear();
    m_attachedPanelGeometry.reset();
    m_attachedOpenAnimationPending = false;
    kLog.warn("panel manager: attached layer-shell failed for \"{}\", falling back to standalone", panelId);
  }

  auto layerSurface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  m_layerSurface = layerSurface.get();
  m_surface = std::move(layerSurface);
  m_panelInsetX = detachedShadowBleed.left;
  m_panelInsetY = detachedShadowBleed.up;
  m_panelVisualWidth = panelWidth;
  m_panelVisualHeight = panelHeight;
  m_attachedBackgroundOpacity = 1.0f;
  m_attachedContactShadow = false;
  m_attachedRevealProgress = 1.0f;
  m_attachedRevealDirection = AttachedRevealDirection::Down;
  m_attachedPanelGeometry.reset();
  m_attachedToBar = false;
  configureSurfaceCallbacks(*m_surface);

  // Guard against re-entrancy: initialize can process queued Wayland events.
  m_inTransition = true;
  bool ok = m_layerSurface->initialize(request.output);
  m_inTransition = false;

  if (!ok) {
    kLog.warn("panel manager: failed to initialize surface for panel \"{}\"", panelId);
    resetPanelOpenState();
    return;
  }

  m_output = request.output;
  m_wlSurface = m_surface->wlSurface();
  m_surface->setInputRegion(
      {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
  );
  applyPanelCompositorBlur();
  // Defer the focus grab to the next tick. See attached-path comment above.
  const std::uint64_t gen = m_destroyGeneration;
  DeferredCall::callLater([this, gen]() {
    if (m_destroyGeneration == gen) {
      activateFocusGrab();
    }
  });
  kLog.debug("panel manager: opened \"{}\"", panelId);
  if (m_panelOpenedCallback) {
    m_panelOpenedCallback();
  }
}

void PanelManager::activateClickShield(LayerShellLayer layer) {
  if (m_activePanel == nullptr || m_platform == nullptr) {
    return;
  }
  // Hyprland: prefer the native focus-grab path. Skip the shield and let
  // activateFocusGrab handle it later.
  auto* grabService = m_platform->focusGrabService();
  if (grabService != nullptr && grabService->available()) {
    return;
  }
  std::vector<wl_output*> outputs;
  outputs.reserve(m_platform->outputs().size());
  for (const auto& wlOutput : m_platform->outputs()) {
    if (wlOutput.output != nullptr) {
      outputs.push_back(wlOutput.output);
    }
  }
  m_clickShield.activate(outputs, layer, m_clickShieldExcludeRectsProvider);
}

void PanelManager::activateFocusGrab() {
  if (m_platform == nullptr || m_wlSurface == nullptr) {
    return;
  }
  auto* grabService = m_platform->focusGrabService();
  if (grabService == nullptr || !grabService->available()) {
    return;
  }
  // Whitelist the panel and every bar surface. Clicks on whitelisted surfaces
  // pass through normally. Clicks anywhere else clear the grab and close the panel.
  m_focusGrab = grabService->createGrab();
  if (m_focusGrab == nullptr) {
    return;
  }
  m_focusGrab->setOnCleared([this]() {
    if (isOpen() && !m_closing) {
      closePanel();
    }
  });
  grabService->setPopupGrabHost(this);
  m_focusGrab->addSurface(m_wlSurface);
  if (m_focusGrabBarSurfacesProvider) {
    auto bars = m_focusGrabBarSurfacesProvider();
    for (auto* surface : bars) {
      m_focusGrab->addSurface(surface);
    }
  }
  m_focusGrab->commit();
}

void PanelManager::deactivateOutsideClickHandlers() {
  m_clickShield.deactivate();
  if (m_platform != nullptr) {
    if (auto* svc = m_platform->focusGrabService(); svc != nullptr && svc->popupGrabHost() == this) {
      svc->setPopupGrabHost(nullptr);
    }
  }
  m_focusGrab.reset();
}

void PanelManager::closePanel(bool animateClose) {
  if (!isOpen() || m_inTransition || m_closing) {
    return;
  }

  kLog.debug("panel manager: closing \"{}\"", m_activePanelId);

  // Drop the outside-click handlers as soon as close starts.
  // During the close animation we want clicks on apps to behave normally.
  deactivateOutsideClickHandlers();

  // Disable input during close animation
  m_inputDispatcher.setSceneRoot(nullptr);
  m_closing = true;
  m_attachedOpenAnimationPending = false;

  if (animateClose && m_sceneRoot != nullptr && m_activePanel != nullptr && m_activePanel->wantsCloseAnimation()) {
    const std::uint64_t gen = ++m_destroyGeneration;
    if (m_attachedToBar && m_attachedRevealClipNode != nullptr) {
      m_animations.cancelForOwner(m_attachedRevealClipNode);
      m_animations.animate(
          m_attachedRevealProgress, 0.0f, Style::animNormal, Easing::EaseInOutQuad,
          [this](float v) { applyAttachedReveal(v); },
          [this, gen]() {
            DeferredCall::callLater([this, gen]() {
              if (m_destroyGeneration == gen) {
                destroyPanel();
              }
            });
          },
          m_attachedRevealClipNode
      );
    } else {
      m_animations.cancelForOwner(m_sceneRoot.get());
      m_animations.animate(
          m_detachedRevealProgress, 0.0f, Style::animNormal, Easing::EaseInOutCubic,
          [this](float v) { applyDetachedReveal(v); },
          [this, gen]() {
            DeferredCall::callLater([this, gen]() {
              if (m_destroyGeneration == gen) {
                destroyPanel();
              }
            });
          },
          m_sceneRoot.get()
      );
    }
    m_surface->requestRedraw();
  } else {
    destroyPanel();
  }
}

void PanelManager::destroyPanel() {
  if (m_attachedToBar && m_attachedPanelGeometryCallback && m_output != nullptr) {
    m_attachedPanelGeometryCallback(m_output, m_sourceBarName, std::nullopt);
  }
  // Defensive: closePanel deactivates first, but destroyPanel can also be
  // reached directly when openPanel preempts an open panel.
  deactivateOutsideClickHandlers();
  m_animations.cancelAll();
  m_closing = false;
  m_pointerInside = false;
  m_attachedPopupCount = 0;
  m_inputDispatcher.setSceneRoot(nullptr);
  if (m_activePanel != nullptr) {
    m_activePanel->onClose();
  }
  m_bgNode = nullptr;
  m_contentNode = nullptr;
  m_attachedRevealClipNode = nullptr;
  m_attachedRevealContentNode = nullptr;
  m_panelShadowNode = nullptr;
  m_panelContactShadowNode = nullptr;
  m_selectPopup.reset();
  m_sceneRoot.reset();
  m_surface.reset();
  m_layerSurface = nullptr;
  m_output = nullptr;
  m_wlSurface = nullptr;
  m_activePanel = nullptr;
  m_activePanelId.clear();
  m_pendingOpenContext.clear();
  m_panelInsetX = 0;
  m_panelInsetY = 0;
  m_panelVisualWidth = 0;
  m_panelVisualHeight = 0;
  m_attachedBackgroundOpacity = 1.0f;
  m_attachedContactShadow = false;
  m_attachedRevealProgress = 1.0f;
  m_attachedRevealDirection = AttachedRevealDirection::Down;
  m_keyboardRelaxTimer.stop();
  m_attachedBarPosition.clear();
  m_sourceBarName.clear();
  m_attachedPanelGeometry.reset();
  m_attachedToBar = false;
  m_attachedOpenAnimationPending = false;
  if (m_platform != nullptr) {
    m_platform->stopKeyRepeat();
  }
  if (m_panelClosedCallback) {
    m_panelClosedCallback();
  }
}

void PanelManager::togglePanel(const std::string& panelId, PanelOpenRequest request) {
  // Treat a closing panel as closed: re-clicking while it animates out reopens it immediately.
  if (isOpen() && !m_closing && m_activePanelId == panelId) {
    if (!request.context.empty() && m_activePanel != nullptr) {
      if (m_activePanel->isContextActive(request.context)) {
        closePanel();
        return;
      }
      // Panels placed near the clicked widget must fully reopen so geometry
      // and bar decoration track the new anchor.
      if (request.hasAnchorPosition
          && m_activePanel->panelPlacement() != PanelPlacement::Centered
          && openNearClickEnabledForPanel(m_config, panelId)) {
        openPanel(panelId, request);
        return;
      }
      m_activePanel->onOpen(request.context);
      refresh();
      return;
    }
    closePanel();
  } else {
    openPanel(panelId, request);
  }
}

void PanelManager::togglePanel(const std::string& panelId) {
  if (isOpen() && !m_closing && m_activePanelId == panelId) {
    closePanel();
    return;
  }
  wl_output* output =
      m_platform != nullptr ? m_platform->preferredInteractiveOutput(std::chrono::milliseconds(1200)) : nullptr;
  openPanel(panelId, PanelOpenRequest{.output = output});
}

void PanelManager::clearClipboardHistory() {
  const auto it = m_panels.find("clipboard");
  if (it == m_panels.end()) {
    return;
  }
  if (auto* clipboardPanel = dynamic_cast<ClipboardPanel*>(it->second.get())) {
    clipboardPanel->clearHistoryFromIpc();
  }
}

bool PanelManager::onPointerEvent(const PointerEvent& event) {
  if (!isOpen() || m_inTransition) {
    return false;
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }

  if (m_activePopup != nullptr) {
    if (m_activePopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      m_activePopup->close();
      return true;
    }
  }

  if (m_attachedPopupCount > 0) {
    if (event.surface == m_wlSurface) {
      if (event.type == PointerEvent::Type::Enter) {
        m_pointerInside = true;
      } else if (event.type == PointerEvent::Type::Leave) {
        m_pointerInside = false;
      }
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = false;
      m_inputDispatcher.pointerLeave();
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (!m_pointerInside) {
      return false;
    }
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  }
  case PointerEvent::Type::Button: {
    bool pressed = (event.state == 1);

    // Click outside panel closes it.
    if (pressed && !m_pointerInside) {
      closePanel();
      return false;
    }

    if (m_pointerInside) {
      if (pressed
          && event.surface == m_wlSurface
          && m_activePanelId == "control-center"
          && m_inputDispatcher.hoveredArea() == nullptr) {
        if (auto* controlCenter = dynamic_cast<ControlCenterPanel*>(m_activePanel);
            controlCenter != nullptr && controlCenter->dismissTransientUi()) {
          refresh();
          return true;
        }
      }
      m_inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
    }
    break;
  }
  case PointerEvent::Type::Axis: {
    if (!m_pointerInside) {
      return false;
    }
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  }

  // Pointer interactions often only affect visual state.
  // Relayout only when the scene explicitly accumulated layout invalidation.
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty() && m_activePanel != nullptr && !m_activePanel->deferPointerRelayout()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }

  return m_pointerInside;
}

bool PanelManager::isOpen() const noexcept { return m_surface != nullptr && m_activePanel != nullptr; }

bool PanelManager::isOpenPanel(std::string_view panelId) const noexcept {
  return isOpen() && m_activePanelId == panelId;
}

bool PanelManager::isPanelTransitionActive() const noexcept {
  if (!isOpen() && !m_closing) {
    return false;
  }
  if (m_closing || m_attachedOpenAnimationPending) {
    return true;
  }
  if (m_attachedToBar) {
    return m_attachedRevealProgress < 0.999f;
  }
  return m_detachedRevealProgress < 0.999f;
}

bool PanelManager::isAttachedOpen() const noexcept { return isOpen() && m_attachedToBar; }

wl_output* PanelManager::attachedPanelOutput() const noexcept { return m_output; }

std::string_view PanelManager::attachedSourceBarName() const noexcept { return m_sourceBarName; }

const std::string& PanelManager::activePanelId() const noexcept { return m_activePanelId; }

bool PanelManager::isActivePanelContext(std::string_view context) const noexcept {
  if (!isOpen() || m_activePanel == nullptr) {
    return false;
  }
  return m_activePanel->isContextActive(context);
}

void PanelManager::refresh() {
  if (!isOpen() || m_renderContext == nullptr || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel->deferExternalRefresh()) {
    return;
  }

  m_surface->requestUpdate();
}

void PanelManager::onIconThemeChanged() {
  if (!isOpen() || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }

  m_activePanel->onIconThemeChanged();
  m_surface->requestUpdate();
}

void PanelManager::focusArea(InputArea* area) {
  if (!isOpen() || m_sceneRoot == nullptr) {
    return;
  }
  m_inputDispatcher.setFocus(area);
}

void PanelManager::requestUpdateOnly() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestUpdateOnly();
}

void PanelManager::requestLayout() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestLayout();
}

void PanelManager::requestRedraw() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestRedraw();
}

void PanelManager::requestFrameTick() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestFrameTick();
}

void PanelManager::close() { closePanel(); }

void PanelManager::setActivePopup(ContextMenuPopup* popup) { m_activePopup = popup; }

void PanelManager::clearActivePopup() { m_activePopup = nullptr; }

void PanelManager::registerPopupSurface(wl_surface* surface) {
  if (m_focusGrab == nullptr || surface == nullptr) {
    return;
  }
  m_focusGrab->addSurface(surface);
  m_focusGrab->commit();
}

void PanelManager::unregisterPopupSurface(wl_surface* surface) {
  if (m_focusGrab == nullptr || surface == nullptr) {
    return;
  }
  m_focusGrab->removeSurface(surface);
  m_focusGrab->commit();
}

void PanelManager::beginAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  ++m_attachedPopupCount;
}

void PanelManager::endAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  if (m_attachedPopupCount > 0) {
    --m_attachedPopupCount;
  }
  if (m_platform != nullptr) {
    m_pointerInside = (m_platform->lastPointerSurface() == m_wlSurface);
  }
}

std::optional<LayerPopupParentContext> PanelManager::popupParentContextForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr || surface != m_wlSurface) {
    return std::nullopt;
  }
  return fallbackPopupParentContext();
}

std::optional<LayerPopupParentContext> PanelManager::fallbackPopupParentContext() const noexcept {
  if (!isOpen() || m_surface == nullptr || m_wlSurface == nullptr || m_layerSurface == nullptr) {
    return std::nullopt;
  }

  LayerPopupParentContext context;
  context.surface = m_wlSurface;
  context.layerSurface = m_layerSurface->layerSurface();
  context.output = m_output;
  context.width = m_surface->width();
  context.height = m_surface->height();
  if (context.layerSurface == nullptr || context.width == 0 || context.height == 0) {
    return std::nullopt;
  }
  return context;
}

void PanelManager::onKeyboardEvent(const KeyboardEvent& event) {
  // m_inTransition means the surface is still initializing.
  // Keyboard events during this window must be ignored.
  if (!isOpen() || m_inTransition) {
    return;
  }

  // Gate on compositor focus: route keys only when the surface owning this panel
  // input is the one the compositor reports as keyboard-focused.
  if (m_platform != nullptr) {
    wl_surface* const kbSurface = m_platform->lastKeyboardSurface();
    const bool onPanel = (m_wlSurface != nullptr && kbSurface == m_wlSurface);
    const bool onSelectPopup =
        (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && kbSurface == m_selectPopup->wlSurface());
    if (!onPanel && !onSelectPopup) {
      return;
    }
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }

  if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    if (m_activePanel != nullptr
        && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
      if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
    closePanel();
    return;
  }

  if (m_activePanel != nullptr
      && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
    if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
      if (m_sceneRoot->layoutDirty()) {
        m_surface->requestLayout();
      } else {
        m_surface->requestRedraw();
      }
    }
    return;
  }

  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }
}

void PanelManager::applyAttachedReveal(float progress) {
  m_attachedRevealProgress = std::clamp(progress, 0.0f, 1.0f);
  if (!m_attachedToBar || m_attachedRevealClipNode == nullptr || m_sceneRoot == nullptr) {
    return;
  }

  const float w = m_sceneRoot->width();
  const float h = m_sceneRoot->height();
  const float panelW = m_panelVisualWidth > 0 ? static_cast<float>(m_panelVisualWidth) : w;
  const float panelH = m_panelVisualHeight > 0 ? static_cast<float>(m_panelVisualHeight) : h;
  const float travelX = (m_attachedRevealDirection == AttachedRevealDirection::Left
                         || m_attachedRevealDirection == AttachedRevealDirection::Right)
      ? panelW * (1.0f - m_attachedRevealProgress)
      : 0.0f;
  const float travelY = (m_attachedRevealDirection == AttachedRevealDirection::Up
                         || m_attachedRevealDirection == AttachedRevealDirection::Down)
      ? panelH * (1.0f - m_attachedRevealProgress)
      : 0.0f;

  float contentX = 0.0f;
  float contentY = 0.0f;
  switch (m_attachedRevealDirection) {
  case AttachedRevealDirection::Down:
    contentY = -travelY;
    break;
  case AttachedRevealDirection::Up:
    contentY = travelY;
    break;
  case AttachedRevealDirection::Right:
    contentX = -travelX;
    break;
  case AttachedRevealDirection::Left:
    contentX = travelX;
    break;
  }

  m_attachedRevealClipNode->setPosition(0.0f, 0.0f);
  m_attachedRevealClipNode->setFrameSize(w, h);

  if (m_attachedRevealContentNode != nullptr) {
    m_attachedRevealContentNode->setPosition(contentX, contentY);
    m_attachedRevealContentNode->setFrameSize(w, h);
  }
  if (m_panelShadowNode != nullptr) {
    m_panelShadowNode->setOpacity(m_attachedRevealProgress);
  }
  if (m_panelContactShadowNode != nullptr) {
    m_panelContactShadowNode->setOpacity(m_attachedRevealProgress);
  }

  publishAttachedPanelGeometry(m_attachedRevealProgress);
  applyPanelCompositorBlur();
}

void PanelManager::applyDetachedReveal(float progress) {
  m_detachedRevealProgress = std::clamp(progress, 0.0f, 1.0f);
  if (m_attachedToBar || m_sceneRoot == nullptr) {
    return;
  }
  // Scale the entire scene from 0.95 to 1.0 around the surface center.
  const float s = 1.0f - 0.05f * (1.0f - m_detachedRevealProgress);
  m_sceneRoot->setScale(s);
  // Fade the whole panel (background + content) uniformly. The compositor blur region
  // is not opacity-aware, so we gate it in applyPanelCompositorBlur() to stay cleared
  // while the panel is mostly translucent.
  m_sceneRoot->setOpacity(m_detachedRevealProgress);
  applyPanelCompositorBlur();
}

void PanelManager::startAttachedOpenAnimation() {
  if (!m_attachedOpenAnimationPending || !m_attachedToBar || m_attachedRevealClipNode == nullptr || m_closing) {
    return;
  }
  if (m_attachedPanelBarSettledCallback != nullptr
      && m_output != nullptr
      && !m_attachedPanelBarSettledCallback(m_output, m_sourceBarName)) {
    return;
  }

  m_attachedOpenAnimationPending = false;
  m_animations.animate(
      m_attachedRevealProgress, 1.0f, Style::animNormal, Easing::EaseOutCubic,
      [this](float v) { applyAttachedReveal(v); }, {}, m_attachedRevealClipNode
  );
}

void PanelManager::publishAttachedPanelGeometry(float revealProgress) {
  if (!m_attachedToBar || !m_attachedPanelGeometryCallback || m_output == nullptr || !m_attachedPanelGeometry) {
    return;
  }

  const float progress = std::clamp(revealProgress, 0.0f, 1.0f);
  if (progress <= 0.001f) {
    m_attachedPanelGeometryCallback(m_output, m_sourceBarName, std::nullopt);
    return;
  }

  auto geometry = *m_attachedPanelGeometry;

  // The bar-side concave bulges only enter the visible clip during the last
  // portion of the animation. Until then the silhouette is a sharp-edged rectangle.
  const float originalRadius = geometry.cornerRadius;
  const bool vertical =
      (m_attachedRevealDirection == AttachedRevealDirection::Right
       || m_attachedRevealDirection == AttachedRevealDirection::Left);
  const float panelMainDim = vertical ? geometry.width : geometry.height;
  const float bulgeRevealAmount = std::clamp(originalRadius - panelMainDim * (1.0f - progress), 0.0f, originalRadius);
  const float crossDelta = originalRadius - bulgeRevealAmount;
  geometry.bulgeRadius = bulgeRevealAmount;

  // The away-side convex corners are visible at full radius throughout the animation.
  // Extend the body main-axis dimension toward the bar so it is at least 2*cornerRadius.
  const float minMainDim = 2.0f * originalRadius;

  switch (m_attachedRevealDirection) {
  case AttachedRevealDirection::Down: {
    const float visibleHeight = geometry.height * progress;
    const float effectiveHeight = std::max(visibleHeight, minMainDim);
    const float extension = effectiveHeight - visibleHeight;
    geometry.y -= extension;
    geometry.height = effectiveHeight;
    geometry.x += crossDelta;
    geometry.width -= 2.0f * crossDelta;
    break;
  }
  case AttachedRevealDirection::Up: {
    const float originalHeight = geometry.height;
    const float visibleHeight = originalHeight * progress;
    const float effectiveHeight = std::max(visibleHeight, minMainDim);
    geometry.y += originalHeight - visibleHeight;
    geometry.height = effectiveHeight;
    geometry.x += crossDelta;
    geometry.width -= 2.0f * crossDelta;
    break;
  }
  case AttachedRevealDirection::Right: {
    const float visibleWidth = geometry.width * progress;
    const float effectiveWidth = std::max(visibleWidth, minMainDim);
    const float extension = effectiveWidth - visibleWidth;
    geometry.x -= extension;
    geometry.width = effectiveWidth;
    geometry.y += crossDelta;
    geometry.height -= 2.0f * crossDelta;
    break;
  }
  case AttachedRevealDirection::Left: {
    const float originalWidth = geometry.width;
    const float visibleWidth = originalWidth * progress;
    const float effectiveWidth = std::max(visibleWidth, minMainDim);
    geometry.x += originalWidth - visibleWidth;
    geometry.width = effectiveWidth;
    geometry.y += crossDelta;
    geometry.height -= 2.0f * crossDelta;
    break;
  }
  }

  m_attachedPanelGeometryCallback(m_output, m_sourceBarName, geometry);
}

void PanelManager::applyPanelCompositorBlur() {
  // The blur region is submitted on every panel surface.
  // As of niri 26.04, subsurfaces are ignored for ext-background-effect-v1.
  if (m_surface == nullptr || m_activePanel == nullptr) {
    return;
  }

  int bx = m_panelInsetX;
  int by = m_panelInsetY;
  int bw = static_cast<int>(m_panelVisualWidth);
  int bh = static_cast<int>(m_panelVisualHeight);
  if (bw <= 0 || bh <= 0) {
    m_surface->clearBlurRegion();
    return;
  }

  if (!m_attachedToBar) {
    // The blur region is not opacity-aware: while the detached panel is still mostly
    // translucent a submitted region would show through as a blurred blob, so keep it
    // cleared until the panel is opaque enough. This works the same during both fade in/out.
    const float kBlurRevealThreshold = 0.6f;
    if (m_detachedRevealProgress < kBlurRevealThreshold) {
      m_surface->clearBlurRegion();
      return;
    }

    const float progress = std::clamp(m_detachedRevealProgress, 0.0f, 1.0f);
    const float s = 1.0f - 0.05f * (1.0f - progress);
    const int scaledW = static_cast<int>(std::lround(static_cast<float>(bw) * s));
    const int scaledH = static_cast<int>(std::lround(static_cast<float>(bh) * s));
    bx += (bw - scaledW) / 2;
    by += (bh - scaledH) / 2;
    bw = scaledW;
    bh = scaledH;
  } else {
    // Mirror the slide that the visible content node performs in applyAttachedReveal.
    // This keeps the blur region in lockstep with what is actually on screen.
    const float progress = std::clamp(m_attachedRevealProgress, 0.0f, 1.0f);
    if (progress < 0.001f) {
      m_surface->clearBlurRegion();
      return;
    }
    const auto panelW = static_cast<float>(m_panelVisualWidth);
    const auto panelH = static_cast<float>(m_panelVisualHeight);
    switch (m_attachedRevealDirection) {
    case AttachedRevealDirection::Down:
      by -= static_cast<int>(std::lround(panelH * (1.0f - progress)));
      break;
    case AttachedRevealDirection::Up:
      by += static_cast<int>(std::lround(panelH * (1.0f - progress)));
      break;
    case AttachedRevealDirection::Right:
      bx -= static_cast<int>(std::lround(panelW * (1.0f - progress)));
      break;
    case AttachedRevealDirection::Left:
      bx += static_cast<int>(std::lround(panelW * (1.0f - progress)));
      break;
    }
  }

  const float radius = Style::scaledRadiusXl(m_activePanel->contentScale());
  const CornerShapes corners = m_attachedToBar ? attached_panel::cornerShapes(m_attachedBarPosition) : CornerShapes{};
  const RectInsets logicalInset =
      m_attachedToBar ? attached_panel::logicalInset(m_attachedBarPosition, radius) : RectInsets{};
  const Radii radii = Radii{radius, radius, radius, radius};
  auto strips = Surface::tessellateShape(bx, by, bw, bh, corners, logicalInset, radii);
  if (strips.empty()) {
    m_surface->clearBlurRegion();
    return;
  }

  if (m_attachedToBar && m_sceneRoot != nullptr) {
    const int clipMaxX = static_cast<int>(std::lround(m_sceneRoot->width()));
    const int clipMaxY = static_cast<int>(std::lround(m_sceneRoot->height()));
    std::vector<InputRect> clipped;
    clipped.reserve(strips.size());
    for (const auto& s : strips) {
      const int sxLeft = std::max(s.x, 0);
      const int sxRight = std::min(s.x + s.width, clipMaxX);
      const int syTop = std::max(s.y, 0);
      const int syBot = std::min(s.y + s.height, clipMaxY);
      if (sxRight > sxLeft && syBot > syTop) {
        clipped.push_back({sxLeft, syTop, sxRight - sxLeft, syBot - syTop});
      }
    }
    if (clipped.empty()) {
      m_surface->clearBlurRegion();
      return;
    }
    m_surface->setBlurRegion(clipped);
    return;
  }

  m_surface->setBlurRegion(strips);
}

void PanelManager::applyAttachedDecorationStyle() {
  if (!m_attachedToBar || m_activePanel == nullptr) {
    return;
  }
  const float scale = m_activePanel->contentScale();
  const float radius = Style::scaledRadiusXl(scale);

  if (m_bgNode != nullptr) {
    auto* bg = static_cast<Box*>(m_bgNode);
    bg->setFill(colorSpecFromRole(ColorRole::Surface, m_attachedBackgroundOpacity));
  }

  if (m_panelShadowNode != nullptr && m_config != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    if (panelShadow) {
      const RoundedRectStyle shadowStyle = shell::surface_shadow::style(
          shadowConfig, m_attachedBackgroundOpacity,
          shell::surface_shadow::Shape{
              .corners = attached_panel::cornerShapes(m_attachedBarPosition),
              .logicalInset = attached_panel::logicalInset(m_attachedBarPosition, radius),
              .radius = Radii{radius, radius, radius, radius},
          }
      );
      m_panelShadowNode->setStyle(shadowStyle);
    }
  }

  if (m_panelContactShadowNode != nullptr) {
    const float contactAlpha = 0.16f * std::clamp(m_attachedBackgroundOpacity, 0.0f, 1.0f);
    const bool barIsBottom = m_attachedBarPosition == "bottom";
    const bool barIsRight = m_attachedBarPosition == "right";
    const bool barIsVertical = m_attachedBarPosition == "left" || m_attachedBarPosition == "right";
    // Gradient runs perpendicular to the bar edge, dark next to the bar, transparent toward
    // the panel interior. For top/left: dark at start. For bottom/right: dark at end.
    const bool darkAtStart = !(barIsBottom || barIsRight);
    const Color darkColor = rgba(0.0f, 0.0f, 0.0f, contactAlpha);
    const Color clearGradient = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    const Color startColor = darkAtStart ? darkColor : clearGradient;
    const Color endColor = darkAtStart ? clearGradient : darkColor;
    const RoundedRectStyle contactStyle{
        .fill = startColor,
        .border = clearColor(),
        .fillMode = FillMode::LinearGradient,
        .gradientDirection = barIsVertical ? GradientDirection::Horizontal : GradientDirection::Vertical,
        .gradientStops =
            {GradientStop{0.0f, startColor}, GradientStop{0.0f, startColor}, GradientStop{1.0f, endColor},
             GradientStop{1.0f, endColor}},
        .corners = attached_panel::cornerShapes(m_attachedBarPosition),
        .logicalInset = attached_panel::logicalInset(m_attachedBarPosition, radius),
        .radius = Radii{radius, radius, radius, radius},
        .softness = 1.0f,
        .borderWidth = 0.0f,
    };
    m_panelContactShadowNode->setStyle(contactStyle);
  }
}

void PanelManager::onConfigReloaded() {
  if (!isOpen() || m_config == nullptr || m_activePanel == nullptr) {
    return;
  }

  applyPanelCompositorBlur();
  const float panelBackgroundOpacity =
      m_attachedToBar ? m_attachedBackgroundOpacity : resolveDetachedPanelBackgroundOpacity(m_config);
  m_activePanel->setPanelCardOpacity(resolvePanelCardOpacity(m_config, panelBackgroundOpacity));
  m_activePanel->setPanelBordersEnabled(m_config->config().shell.panel.borders);
  if (!m_attachedToBar && m_bgNode != nullptr) {
    auto* bg = static_cast<Box*>(m_bgNode);
    bg->setPanelStyle(m_config->config().shell.panel.borders);
    bg->setFill(colorSpecFromRole(ColorRole::Surface, panelBackgroundOpacity));
  }
  if (m_panelShadowNode != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    if (!m_attachedToBar && panelShadow) {
      const float shadowRadius = Style::scaledRadiusXl(m_activePanel->contentScale());
      m_panelShadowNode->setStyle(
          shell::surface_shadow::style(
              shadowConfig, panelBackgroundOpacity,
              shell::surface_shadow::Shape{.radius = Radii{shadowRadius, shadowRadius, shadowRadius, shadowRadius}}
          )
      );
    }
  }
  if (m_surface != nullptr) {
    m_surface->requestUpdate();
  }

  // The remaining work is bar-config-driven and only applies to attached panels.
  if (!isAttachedOpen() || m_output == nullptr) {
    return;
  }

  const auto barConfig = resolvePanelBarConfig(m_config, m_platform, m_output, m_sourceBarName);
  bool changed = false;
  if (m_activePanel->inheritsBarBackgroundOpacity()) {
    const float newOpacity = barConfig.backgroundOpacity;
    if (std::abs(newOpacity - m_attachedBackgroundOpacity) >= 0.001f) {
      m_attachedBackgroundOpacity = newOpacity;
      m_activePanel->setPanelCardOpacity(resolvePanelCardOpacity(m_config, m_attachedBackgroundOpacity));
      changed = true;
    }
  }
  if (!changed) {
    return;
  }

  applyAttachedDecorationStyle();
  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
}

void PanelManager::buildScene(std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("PanelManager::buildScene");
  if (m_renderContext == nullptr || m_activePanel == nullptr) {
    return;
  }
  auto* renderer = m_renderContext;
  const bool hasDecoration = m_activePanel->hasDecoration();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  if (m_sceneRoot == nullptr) {
    m_sceneRoot = std::make_unique<Node>();
    m_sceneRoot->setAnimationManager(&m_animations);
    if (m_layerSurface != nullptr && m_renderContext != nullptr) {
      m_selectPopup = std::make_unique<SelectDropdownPopup>(m_platform->wayland(), *m_renderContext);
      if (m_config != nullptr) {
        m_selectPopup->setShadowConfig(m_config->config().shell.shadow);
      }
      m_selectPopup->setParent(m_layerSurface->layerSurface(), m_wlSurface, m_output);
      m_sceneRoot->setPopupContext(m_selectPopup.get());
    }
    m_sceneRoot->setSize(w, h);

    Node* sceneParent = m_sceneRoot.get();
    if (m_attachedToBar) {
      auto revealClip = std::make_unique<Node>();
      revealClip->setClipChildren(true);
      m_attachedRevealClipNode = m_sceneRoot->addChild(std::move(revealClip));

      auto revealContent = std::make_unique<Node>();
      m_attachedRevealContentNode = m_attachedRevealClipNode->addChild(std::move(revealContent));
      sceneParent = m_attachedRevealContentNode;
    }

    if (hasDecoration && m_config != nullptr && shell::surface_shadow::enabled(true, m_config->config().shell.shadow)) {
      auto shadow = std::make_unique<Box>();
      m_panelShadowNode = static_cast<Box*>(sceneParent->addChild(std::move(shadow)));
      m_panelShadowNode->setZIndex(-1);
      m_panelShadowNode->setVisible(m_config->config().shell.panel.shadow);
    }

    if (hasDecoration) {
      auto bg = std::make_unique<Box>();
      const bool panelBorders = m_config != nullptr && m_config->config().shell.panel.borders;
      bg->setPanelStyle(panelBorders);
      if (m_attachedToBar) {
        const float radius = Style::scaledRadiusXl(m_activePanel->contentScale());
        bg->clearBorder();
        bg->setCornerShapes(attached_panel::cornerShapes(m_attachedBarPosition));
        bg->setLogicalInset(attached_panel::logicalInset(m_attachedBarPosition, radius));
        bg->setRadii(Radii{radius, radius, radius, radius});
        // Fill (opacity-dependent) is applied via applyAttachedDecorationStyle() below.
      } else {
        bg->setFill(colorSpecFromRole(ColorRole::Surface, resolveDetachedPanelBackgroundOpacity(m_config)));
      }
      m_bgNode = sceneParent->addChild(std::move(bg));
    }

    if (hasDecoration && m_attachedToBar && m_attachedContactShadow) {
      auto contactShadow = std::make_unique<Box>();
      m_panelContactShadowNode = static_cast<Box*>(sceneParent->addChild(std::move(contactShadow)));
    }

    // Create panel content inside a wrapper node for staggered fade-in
    auto contentWrapper = std::make_unique<Node>();
    m_contentNode = contentWrapper.get();
    m_activePanel->setAnimationManager(&m_animations);
    const float panelBackgroundOpacity =
        m_attachedToBar ? m_attachedBackgroundOpacity : resolveDetachedPanelBackgroundOpacity(m_config);
    m_activePanel->setPanelCardOpacity(resolvePanelCardOpacity(m_config, panelBackgroundOpacity));
    m_activePanel->setPanelBordersEnabled(m_config->config().shell.panel.borders);
    m_activePanel->create();
    m_activePanel->onOpen(m_pendingOpenContext);
    m_pendingOpenContext.clear();
    if (m_activePanel->root() != nullptr) {
      contentWrapper->addChild(m_activePanel->releaseRoot());
    }
    sceneParent->addChild(std::move(contentWrapper));

    m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
    m_inputDispatcher.setTextInputContext(m_wlSurface, m_platform->wayland().textInputService());
    m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    m_inputDispatcher.setHoverChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (m_layerSurface != nullptr) {
        TooltipManager::instance().onHoverChange(next, m_layerSurface->layerSurface(), m_output);
      }
    });

    if (m_attachedToBar && m_attachedRevealClipNode != nullptr) {
      m_sceneRoot->setOpacity(1.0f);
      applyAttachedReveal(0.0f);
      m_attachedOpenAnimationPending = true;
    } else {
      applyDetachedReveal(0.0f);
      m_animations.animate(
          0.0f, 1.0f, Style::animNormal, Easing::EaseOutCubic, [this](float v) { applyDetachedReveal(v); }, {},
          m_sceneRoot.get()
      );
    }

    m_surface->setSceneRoot(m_sceneRoot.get());

    // Set initial keyboard focus if the panel requests it
    if (m_activePanel != nullptr) {
      if (auto* focusArea = m_activePanel->initialFocusArea(); focusArea != nullptr) {
        m_inputDispatcher.setFocus(focusArea);
      }
    }
  }

  m_sceneRoot->setSize(w, h);
  if (m_attachedRevealContentNode != nullptr) {
    m_attachedRevealContentNode->setFrameSize(w, h);
  }
  applyAttachedReveal(m_attachedRevealProgress);

  const auto panelX = static_cast<float>(m_panelInsetX);
  const auto panelY = static_cast<float>(m_panelInsetY);
  const float panelW = m_panelVisualWidth > 0 ? static_cast<float>(m_panelVisualWidth) : w;
  const float panelH = m_panelVisualHeight > 0 ? static_cast<float>(m_panelVisualHeight) : h;
  const float attachedRadius = m_attachedToBar ? Style::scaledRadiusXl(m_activePanel->contentScale()) : 0.0f;
  const bool barIsVertical = m_attachedToBar && (m_attachedBarPosition == "left" || m_attachedBarPosition == "right");
  // The bg extends past the body along the bar cross axis for concave-corner notches.
  const float bgX = barIsVertical ? panelX : panelX - attachedRadius;
  const float bgY = barIsVertical ? panelY - attachedRadius : panelY;
  const float bgW = barIsVertical ? panelW : panelW + attachedRadius * 2.0f;
  const float bgH = barIsVertical ? panelH + attachedRadius * 2.0f : panelH;

  if (m_panelShadowNode != nullptr && m_config != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    const auto shadowOffsetX = static_cast<float>(shadowOff.x);
    const auto shadowOffsetY = static_cast<float>(shadowOff.y);
    m_panelShadowNode->setPosition(bgX + shadowOffsetX, bgY + shadowOffsetY);
    m_panelShadowNode->setSize(bgW, bgH);
    if (!m_attachedToBar && panelShadow) {
      const float shadowRadius = Style::scaledRadiusXl(m_activePanel->contentScale());
      const float panelBackgroundOpacity = resolveDetachedPanelBackgroundOpacity(m_config);
      m_panelShadowNode->setStyle(
          shell::surface_shadow::style(
              shadowConfig, panelBackgroundOpacity,
              shell::surface_shadow::Shape{.radius = Radii{shadowRadius, shadowRadius, shadowRadius, shadowRadius}}
          )
      );
    }
  }

  if (m_bgNode != nullptr) {
    m_bgNode->setPosition(bgX, bgY);
    m_bgNode->setSize(bgW, bgH);
  }

  if (m_panelContactShadowNode != nullptr) {
    constexpr float kContactShadowBaseThickness = 16.0f;
    const float scale = m_activePanel->contentScale();
    const float contactThickness =
        std::min(std::max(kContactShadowBaseThickness * scale, attachedRadius * 2.0f), barIsVertical ? bgW : bgH);
    const bool barIsBottom = m_attachedBarPosition == "bottom";
    const bool barIsRight = m_attachedBarPosition == "right";
    float contactX = bgX;
    float contactY = bgY;
    float contactW = bgW;
    float contactH = bgH;
    if (barIsVertical) {
      contactW = contactThickness;
      if (barIsRight) {
        contactX = bgX + bgW - contactThickness;
      }
    } else {
      contactH = contactThickness;
      if (barIsBottom) {
        contactY = bgY + bgH - contactThickness;
      }
    }
    m_panelContactShadowNode->setPosition(contactX, contactY);
    m_panelContactShadowNode->setSize(contactW, contactH);
  }

  // Re-apply opacity-dependent styling for bg, shadow, and contact-shadow.
  // Ensures these stay in sync if the bar config changed.
  if (m_attachedToBar) {
    applyAttachedDecorationStyle();
  }

  const float kPadding = hasDecoration ? m_activePanel->contentScale() * Style::panelPadding : 0.0f;
  m_contentWidth = panelW - kPadding * 2.0f;
  m_contentHeight = panelH - kPadding * 2.0f;
  {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(*renderer);
  }
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_activePanel->layout(*renderer, m_contentWidth, m_contentHeight);
  }
  if (m_contentNode != nullptr) {
    m_contentNode->setPosition(panelX + kPadding, panelY + kPadding);
    m_contentNode->setSize(panelW - kPadding * 2.0f, panelH - kPadding * 2.0f);
  }
  if (m_pointerInside) {
    m_inputDispatcher.syncPointerHover();
  }
}

void PanelManager::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());

  const auto width = m_surface->width();
  const auto height = m_surface->height();

  const bool needsSceneBuild = m_sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->height())) != height;
  if (needsSceneBuild) {
    buildScene(width, height);
  }

  if (!needsSceneBuild && needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(*m_renderContext);
  }
  if (!needsSceneBuild && needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    if (m_activePanel != nullptr) {
      m_activePanel->layout(*m_renderContext, m_contentWidth, m_contentHeight);
    }
    if (m_pointerInside) {
      m_inputDispatcher.syncPointerHover();
    }
  }
}

void PanelManager::registerIpc(IpcService& ipc) {
  auto parseOpenArgs = [](std::string_view rawArgs, std::string_view command, std::string& panelId,
                          std::string& context) -> std::optional<std::string> {
    const std::string_view args = StringUtils::trimLeftView(rawArgs);
    if (args.empty()) {
      return "error: " + std::string(command) + " requires a panel id\n";
    }

    const auto sep = args.find_first_of(" \t\n\r\f\v");
    if (sep == std::string_view::npos) {
      panelId = std::string(args);
      context.clear();
      return std::nullopt;
    }

    panelId = std::string(args.substr(0, sep));
    // Preserve the context verbatim (only strip the separator's leading
    // whitespace) — trailing whitespace can be significant, e.g. a command.
    context = std::string(StringUtils::trimLeftView(args.substr(sep + 1)));
    return std::nullopt;
  };

  auto unknownPanelError = [this](std::string_view panelId) -> std::string {
    std::vector<std::string> ids;
    ids.reserve(m_panels.size());
    for (const auto& entry : m_panels) {
      ids.push_back(entry.first);
    }
    std::ranges::sort(ids);

    std::string error = "error: unknown panel \"" + std::string(panelId) + "\"";
    if (!ids.empty()) {
      error += " (available: " + StringUtils::join(ids, ", ") + ")";
    }
    error += '\n';
    return error;
  };

  auto preferredOutput = [this]() -> wl_output* {
    return m_platform != nullptr ? m_platform->preferredInteractiveOutput(std::chrono::milliseconds(1200)) : nullptr;
  };

  ipc.registerHandler(
      "panel-toggle",
      [this, parseOpenArgs, unknownPanelError, preferredOutput](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-toggle", panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId)) {
          return unknownPanelError(panelId);
        }
        if (context.empty()) {
          togglePanel(panelId);
        } else {
          togglePanel(panelId, PanelOpenRequest{.output = preferredOutput(), .context = context});
        }
        return "ok\n";
      },
      "panel-toggle <id> [context]",
      "Toggle a panel by id, optionally with context (e.g. launcher /emo, control-center audio)"
  );

  ipc.registerHandler(
      "panel-open",
      [this, parseOpenArgs, unknownPanelError, preferredOutput](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-open", panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId)) {
          return unknownPanelError(panelId);
        }

        if (isOpen() && !m_closing && m_activePanelId == panelId) {
          if (!context.empty() && m_activePanel != nullptr) {
            m_activePanel->onOpen(context);
            refresh();
          }
          return "ok\n";
        }

        openPanel(panelId, PanelOpenRequest{.output = preferredOutput(), .context = context});
        return "ok\n";
      },
      "panel-open <id> [context]",
      "Open a panel by id, optionally with context (e.g. launcher /emo, control-center audio)"
  );

  ipc.registerHandler(
      "panel-close",
      [this, unknownPanelError](const std::string& args) -> std::string {
        const std::string panelId = StringUtils::trim(args);
        if (!panelId.empty() && StringUtils::splitWhitespace(panelId).size() != 1) {
          return "error: panel-close accepts at most one panel id\n";
        }
        if (!panelId.empty() && !m_panels.contains(panelId)) {
          return unknownPanelError(panelId);
        }

        if (panelId.empty() || isOpenPanel(panelId)) {
          closePanel();
        }
        return "ok\n";
      },
      "panel-close [id]", "Close the active panel, or close the named panel if it is active"
  );

  const auto rejectSettingsArgs = [](const std::string& args, std::string_view command) -> std::optional<std::string> {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return std::format("error: {} accepts no arguments\n", command);
  };

  ipc.registerHandler(
      "settings-open",
      [this, rejectSettingsArgs](const std::string& args) -> std::string {
        if (auto error = rejectSettingsArgs(args, "settings-open")) {
          return *error;
        }
        openSettingsWindow();
        return "ok\n";
      },
      "settings-open", "Open the settings window, or focus it if already open"
  );

  ipc.registerHandler(
      "settings-close",
      [this, rejectSettingsArgs](const std::string& args) -> std::string {
        if (auto error = rejectSettingsArgs(args, "settings-close")) {
          return *error;
        }
        closeSettingsWindow();
        return "ok\n";
      },
      "settings-close", "Close the settings window"
  );

  ipc.registerHandler(
      "settings-toggle",
      [this, rejectSettingsArgs](const std::string& args) -> std::string {
        if (auto error = rejectSettingsArgs(args, "settings-toggle")) {
          return *error;
        }
        toggleSettingsWindow();
        return "ok\n";
      },
      "settings-toggle", "Toggle the settings window"
  );
}
