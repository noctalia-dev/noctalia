#include "shell/workspace_map/workspace_map_window.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/surface_haze.h"
#include "render/render_context.h"
#include "ui/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <xkbcommon/xkbcommon-keysyms.h>

WorkspaceMapWindow::WorkspaceMapWindow(
    CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext
)
    : m_platform(platform), m_config(config), m_renderContext(renderContext) {}

WorkspaceMapWindow::~WorkspaceMapWindow() { destroy(); }

void WorkspaceMapWindow::open(const std::string& outputName) {
  if (m_surface && m_surface->isRunning() && m_outputName == outputName) {
    m_platform.wayland().activateSurface(m_surface->wlSurface());
    return;
  }
  destroy();
  m_outputName = outputName;
  auto& wayland = m_platform.wayland();
  if (!wayland.hasXdgShell())
    return;
  const auto output = std::ranges::find_if(m_platform.outputs(), [&](const WaylandOutput& info) {
    return info.connectorName == outputName;
  });
  if (output == m_platform.outputs().end())
    return;
  TooltipManager::instance().forceDestroy();
  m_map = std::make_unique<WorkspaceMapPanel>(m_platform, &m_config, true);
  m_map->setContentScale(std::max(0.1F, m_config.config().accessibility.uiScale));
  m_map->setPendingOpenContext(outputName);
  m_map->setAnimationManager(&m_animations);
  m_map->setWindowCallbacks(
      [this] { close(); }, [this] { refresh(); },
      [this](std::uint32_t serial) {
        if (m_surface)
          m_surface->beginResize(serial);
      }
  );
  m_map->onOpen(outputName);
  m_map->create();
  m_surface = std::make_unique<ToplevelSurface>(wayland);
  m_surface->setRenderContext(&m_renderContext);
  m_surface->setHazeProfile("workspace-map-window");
  m_surface->setAnimationManager(&m_animations);
  m_surface->setClosedCallback([this] { close(); });
  m_surface->setSuspendedCallback([this](bool suspended) {
    if (m_map)
      m_map->setPreviewSuspended(suspended);
  });
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { refresh(); });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  m_surface->setUpdateCallback([] {});
  ToplevelSurfaceConfig surfaceConfig{
      .width = static_cast<std::uint32_t>(std::lround(m_map->preferredWidth())),
      .height = static_cast<std::uint32_t>(std::lround(m_map->preferredHeight())),
      .minWidth = static_cast<std::uint32_t>(std::min(400, output->effectiveLogicalWidth())),
      .minHeight = static_cast<std::uint32_t>(std::min(280, output->effectiveLogicalHeight())),
      .title = i18n::tr("workspace-map.title") + " · " + outputName,
      .appId = "dev.nythera.Nythera",
  };
  if (!m_surface->initialize(output->output, surfaceConfig))
    destroy();
}

void WorkspaceMapWindow::close() {
  const auto generation = ++m_generation;
  DeferredCall::callLater([this, generation] {
    if (m_generation == generation)
      destroy();
  });
}

void WorkspaceMapWindow::destroy() {
  ++m_generation;
  if (m_map && m_surface && m_scene) {
    m_map->onResizeFinished(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
    m_map->onClose();
  }
  if (m_surface)
    TooltipManager::instance().forceDestroy();
  m_input.setSceneRoot(nullptr);
  if (m_surface)
    m_surface->setSceneRoot(nullptr);
  m_scene.reset();
  m_map.reset();
  m_surface.reset();
  m_background = nullptr;
  m_animations.cancelAll();
}

void WorkspaceMapWindow::refresh() {
  if (m_surface)
    m_surface->requestUpdate();
}

void WorkspaceMapWindow::prepareFrame(bool update, bool layout) {
  if (!m_surface || !m_map)
    return;
  m_renderContext.makeCurrent(m_surface->renderTarget());
  auto& renderer = m_surface->renderTarget().renderer();
  if (!m_scene) {
    UiPhaseScope phase(UiPhase::Layout);
    m_scene = ui::node({});
    m_scene->setAnimationManager(&m_animations);
    auto background = ui::box({});
    m_background = background.get();
    background->setPanelStyle();
    background->setRadius(0);
    background->clearBorder();
    m_scene->addChild(std::move(background));
    m_scene->addChild(m_map->releaseRoot());
    m_input.setSceneRoot(m_scene.get());
    m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform.wayland().setCursorShape(serial, shape);
    });
    m_input.setHoverChangeCallback([this](InputArea*, InputArea* next) {
      if (m_surface)
        TooltipManager::instance().onHoverChange(
            next, m_surface->xdgSurface(), m_platform.wayland().outputForSurface(m_surface->wlSurface())
        );
    });
    m_surface->setSceneRoot(m_scene.get());
    layout = update = true;
  }
  float backingOpacity = m_config.config().shell.settingsWindowTranslucent ? 0.8F : 1.0F;
  const auto haze = SurfaceHaze::instance().style("workspace-map-window", 0, 0);
  if (haze.enabled && haze.transparent) {
    // Retain the map prototype's Soft/Glass transparency baseline.
    backingOpacity = std::min(
        0.8F, detachedPanelBackgroundOpacityForTransparencyMode(m_config.config().shell.panel.transparencyMode)
    );
  }
  m_background->setFill(colorSpecFromRole(ColorRole::Surface, backingOpacity));
  if (update)
    m_map->update(renderer);
  if (layout || m_scene->layoutDirty()) {
    UiPhaseScope phase(UiPhase::Layout);
    const float scale = std::max(0.1F, m_config.config().accessibility.uiScale);
    const float padding = Style::spaceLg * scale;
    const float width = static_cast<float>(m_surface->width());
    const float height = static_cast<float>(m_surface->height());
    m_scene->setFrameSize(width, height);
    m_background->setFrameSize(width, height);
    m_map->setContentScale(scale);
    m_map->root()->setPosition(padding, padding);
    m_map->layout(renderer, std::max(1.0F, width - padding * 2), std::max(1.0F, height - padding * 2));
  }
}

bool WorkspaceMapWindow::onPointerEvent(const PointerEvent& event) {
  if (!m_surface || event.surface != m_surface->wlSurface())
    return false;
  const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_input.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_input.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_input.pointerMotion(x, y, m_platform.wayland().lastInputSerial());
    break;
  case PointerEvent::Type::Button:
    m_input.pointerMotion(x, y, event.serial);
    if (event.pressed
        && event.button == BTN_LEFT
        && m_input.inputAreaAt(x, y) == nullptr
        && y < (Style::spaceLg + Style::controlHeightSm) * m_map->contentScale()) {
      m_surface->beginMove(event.serial);
    } else {
      m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    }
    break;
  case PointerEvent::Type::Axis:
    m_input.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  if (m_scene) {
    if (m_scene->layoutDirty())
      m_surface->requestLayout();
    else if (m_scene->paintDirty())
      m_surface->requestRedraw();
  }
  return true;
}

bool WorkspaceMapWindow::onKeyboardEvent(const KeyboardEvent& event) {
  if (!m_surface || m_platform.wayland().lastKeyboardSurface() != m_surface->wlSurface())
    return false;
  if (event.pressed && event.sym == XKB_KEY_Escape)
    close();
  else
    m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  return true;
}
