#include "shell/panel/persistent_panel_host.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_surface_style.h"
#include "shell/screen_position.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

  constexpr Logger kLog("persistent-panel");

  // Gap between the panel and the screen edge it is pinned to.
  [[nodiscard]] std::int32_t screenPadding() { return static_cast<std::int32_t>(Style::spaceSm); }

} // namespace

PersistentPanelHost::PersistentPanelHost() = default;

PersistentPanelHost::~PersistentPanelHost() { closeAll(); }

void PersistentPanelHost::initialize(
    CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext
) {
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;
}

void PersistentPanelHost::registerPanel(const std::string& id, std::unique_ptr<Panel> content) {
  m_panels[id] = std::move(content);
}

void PersistentPanelHost::unregisterPanel(const std::string& id) {
  auto it = m_panels.find(id);
  if (it == m_panels.end()) {
    return;
  }
  close(id);
  m_panels.erase(it);
}

bool PersistentPanelHost::hasPanel(std::string_view id) const noexcept { return m_panels.contains(std::string(id)); }

void PersistentPanelHost::appendPanelIds(std::vector<std::string>& out) const {
  for (const auto& entry : m_panels) {
    out.push_back(entry.first);
  }
}

bool PersistentPanelHost::isOpen(std::string_view id) const noexcept {
  return std::ranges::any_of(m_instances, [&](const auto& instance) { return instance->id == id; });
}

PersistentPanelHost::Instance* PersistentPanelHost::findInstance(std::string_view id) noexcept {
  for (auto& instance : m_instances) {
    if (instance->id == id) {
      return instance.get();
    }
  }
  return nullptr;
}

PersistentPanelHost::Instance* PersistentPanelHost::findInstanceForSurface(wl_surface* surface) noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  for (auto& instance : m_instances) {
    if (instance->surface != nullptr && instance->surface->wlSurface() == surface) {
      return instance.get();
    }
  }
  return nullptr;
}

void PersistentPanelHost::open(const std::string& id, wl_output* output, std::string_view context) {
  if (m_platform == nullptr || m_renderContext == nullptr || m_config == nullptr) {
    return;
  }
  auto panelIt = m_panels.find(id);
  if (panelIt == m_panels.end()) {
    return;
  }
  if (isOpen(id)) {
    return;
  }
  if (output == nullptr) {
    kLog.warn("no output for \"{}\"", id);
    return;
  }

  Panel* panel = panelIt->second.get();
  panel->setContentScale(shell::panel_surface::contentScale(m_config));
  panel->setPendingOpenContext(context);

  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto bleed = shell::panel_surface::bleed(panel->hasDecoration(), shadowConfig);

  const auto* wlOutput = m_platform->findOutputByWl(output);
  const auto outputWidth = wlOutput != nullptr ? static_cast<std::int32_t>(wlOutput->logicalWidth) : 0;
  const auto outputHeight = wlOutput != nullptr ? static_cast<std::int32_t>(wlOutput->logicalHeight) : 0;

  const bool fillWidth = panel->fillsWidth();
  const bool fillHeight = panel->fillsHeight();
  auto panelWidth = static_cast<std::uint32_t>(std::max(1.0f, std::round(panel->preferredWidth())));
  auto panelHeight = static_cast<std::uint32_t>(std::max(1.0f, std::round(panel->preferredHeight())));
  const auto padding = screenPadding();
  if (outputWidth > 0) {
    panelWidth = std::min(panelWidth, static_cast<std::uint32_t>(std::max(1, outputWidth - padding * 2)));
  }
  if (outputHeight > 0) {
    panelHeight = std::min(panelHeight, static_cast<std::uint32_t>(std::max(1, outputHeight - padding * 2)));
  }

  // Screen-position placement only: a persistent panel has no bar to anchor to.
  const std::string position = panel->panelScreenPosition();
  const bool centered = position == "center" || position == "auto";
  std::uint32_t anchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
  std::int32_t marginTop = 0;
  std::int32_t marginRight = 0;
  std::int32_t marginBottom = 0;
  std::int32_t marginLeft = 0;
  if (centered) {
    marginLeft = (outputWidth - static_cast<std::int32_t>(panelWidth)) / 2 - bleed.left;
    marginTop = (outputHeight - static_cast<std::int32_t>(panelHeight)) / 2 - bleed.up;
  } else {
    const auto placed = shell::screenPositionAnchor(position, padding);
    anchor = placed.anchor;
    marginTop = placed.marginTop;
    marginRight = placed.marginRight;
    marginBottom = placed.marginBottom;
    marginLeft = placed.marginLeft;
    if ((anchor & LayerShellAnchor::Left) != 0) {
      marginLeft -= bleed.left;
    } else if ((anchor & LayerShellAnchor::Right) != 0) {
      marginRight -= bleed.right;
    }
    if ((anchor & LayerShellAnchor::Top) != 0) {
      marginTop -= bleed.up;
    } else if ((anchor & LayerShellAnchor::Bottom) != 0) {
      marginBottom -= bleed.down;
    }
  }

  std::uint32_t requestedWidth = shell::panel_surface::surfaceExtent(panelWidth, bleed.left, bleed.right);
  std::uint32_t requestedHeight = shell::panel_surface::surfaceExtent(panelHeight, bleed.up, bleed.down);
  std::uint32_t fallbackWidth = requestedWidth;
  std::uint32_t fallbackHeight = requestedHeight;
  // A filled axis is dual-anchored with a requested size of 0 so the compositor
  // assigns it, subtracting every exclusive zone on the output.
  if (fillWidth) {
    anchor |= LayerShellAnchor::Left | LayerShellAnchor::Right;
    marginLeft = padding - bleed.left;
    marginRight = padding - bleed.right;
    requestedWidth = 0;
    fallbackWidth = static_cast<std::uint32_t>(std::max(1, outputWidth - marginLeft - marginRight));
  }
  if (fillHeight) {
    anchor |= LayerShellAnchor::Top | LayerShellAnchor::Bottom;
    marginTop = padding - bleed.up;
    marginBottom = padding - bleed.down;
    requestedHeight = 0;
    fallbackHeight = static_cast<std::uint32_t>(std::max(1, outputHeight - marginTop - marginBottom));
  }

  auto instance = std::make_unique<Instance>();
  instance->id = id;
  instance->panel = panel;
  instance->output = output;
  instance->insetX = bleed.left;
  instance->insetY = bleed.up;
  instance->bleedRight = bleed.right;
  instance->bleedBottom = bleed.down;
  instance->visualWidth = panelWidth;
  instance->visualHeight = panelHeight;
  instance->fillWidth = fillWidth;
  instance->fillHeight = fillHeight;

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-panel",
      .layer = panel->layer(),
      .anchor = anchor,
      .width = requestedWidth,
      .height = requestedHeight,
      // Never reserve space: a persistent panel overlays the workspace.
      .exclusiveZone = 0,
      .marginTop = marginTop,
      .marginRight = marginRight,
      .marginBottom = marginBottom,
      .marginLeft = marginLeft,
      .keyboard = panel->keyboardMode(),
      .defaultWidth = fallbackWidth,
      .defaultHeight = fallbackHeight,
      .prewarmBlur = true,
  };

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  auto* raw = instance.get();
  instance->surface->setRenderContext(m_renderContext);
  instance->surface->setAnimationManager(&instance->animations);
  instance->surface->setConfigureCallback([raw](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    if (raw->surface != nullptr) {
      raw->surface->requestLayout();
    }
  });
  instance->surface->setPrepareFrameCallback([this, raw](bool needsUpdate, bool needsLayout) {
    prepareFrame(*raw, needsUpdate, needsLayout);
  });
  instance->surface->setFrameTickCallback([raw](float deltaMs) {
    if (raw->panel != nullptr) {
      raw->panel->onFrameTick(deltaMs);
    }
  });

  if (!instance->surface->initialize(output)) {
    kLog.warn("failed to initialize surface for \"{}\"", id);
    return;
  }

  instance->surface->setInputRegion(
      {InputRect{instance->insetX, instance->insetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
  );
  instance->surface->setBlurRegion({});
  instance->surface->requestRedraw();

  m_instances.push_back(std::move(instance));
  kLog.debug("opened \"{}\"", id);
}

void PersistentPanelHost::close(const std::string& id) {
  auto it = std::ranges::find_if(m_instances, [&](const auto& instance) { return instance->id == id; });
  if (it == m_instances.end()) {
    return;
  }
  destroyInstance(it);
}

void PersistentPanelHost::closeAll() {
  while (!m_instances.empty()) {
    destroyInstance(std::prev(m_instances.end()));
  }
}

void PersistentPanelHost::destroyInstance(std::vector<std::unique_ptr<Instance>>::iterator it) {
  auto instance = std::move(*it);
  m_instances.erase(it);

  instance->animations.cancelAll();
  instance->inputDispatcher.setSceneRoot(nullptr);
  // Tooltips are xdg_popups parented to this surface; they must die before it.
  TooltipManager::instance().forceDestroy();
  if (instance->panel != nullptr) {
    instance->panel->onClose();
  }
  instance->contentNode = nullptr;
  instance->bgNode = nullptr;
  instance->shadowNode = nullptr;
  instance->sceneRoot.reset();
  instance->surface.reset();
  if (m_platform != nullptr) {
    m_platform->stopKeyRepeat();
  }
  kLog.debug("closed \"{}\"", instance->id);
}

void PersistentPanelHost::toggle(const std::string& id, wl_output* output, std::string_view context) {
  if (isOpen(id)) {
    close(id);
    return;
  }
  open(id, output, context);
}

void PersistentPanelHost::buildScene(Instance& instance, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("PersistentPanelHost::buildScene");
  if (m_renderContext == nullptr || instance.panel == nullptr || instance.sceneRoot != nullptr) {
    return;
  }

  const bool hasDecoration = instance.panel->hasDecoration();
  instance.sceneRoot = std::make_unique<Node>();
  instance.sceneRoot->setAnimationManager(&instance.animations);
  instance.sceneRoot->setSize(static_cast<float>(width), static_cast<float>(height));

  const auto& shadowConfig = m_config->config().shell.shadow;
  if (hasDecoration && shell::surface_shadow::enabled(true, shadowConfig)) {
    auto shadow = std::make_unique<Box>();
    instance.shadowNode = static_cast<Box*>(instance.sceneRoot->addChild(std::move(shadow)));
    instance.shadowNode->setZIndex(-1);
    instance.shadowNode->setVisible(m_config->config().shell.panel.shadow);
  }

  const float backgroundOpacity = shell::panel_surface::backgroundOpacity(m_config);
  if (hasDecoration) {
    auto bg = std::make_unique<Box>();
    bg->setPanelStyle(m_config->config().shell.panel.borders);
    bg->setFill(colorSpecFromRole(ColorRole::Surface, backgroundOpacity));
    instance.bgNode = static_cast<Box*>(instance.sceneRoot->addChild(std::move(bg)));
  }

  auto contentWrapper = std::make_unique<Node>();
  instance.contentNode = contentWrapper.get();
  instance.panel->setAnimationManager(&instance.animations);
  instance.panel->setPanelCardOpacity(shell::panel_surface::cardOpacity(m_config, backgroundOpacity));
  instance.panel->setPanelBordersEnabled(m_config->config().shell.panel.borders);
  instance.panel->create();
  instance.panel->onOpen(instance.panel->pendingOpenContext());
  instance.panel->setPendingOpenContext({});
  if (instance.panel->root() != nullptr) {
    contentWrapper->addChild(instance.panel->releaseRoot());
  }
  instance.sceneRoot->addChild(std::move(contentWrapper));

  auto* raw = &instance;
  instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
  instance.inputDispatcher.setTextInputContext(instance.surface->wlSurface(), m_platform->wayland().textInputService());
  instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_platform->setCursorShape(serial, shape);
  });
  instance.inputDispatcher.setHoverChangeCallback([raw](InputArea* /*old*/, InputArea* next) {
    if (raw->surface != nullptr) {
      TooltipManager::instance().onHoverChange(next, raw->surface->layerSurface(), raw->output);
    }
  });
  instance.inputDispatcher.setFocusChangeCallback([raw](InputArea* /*old*/, InputArea* next) {
    if (raw->panel != nullptr && next != nullptr) {
      raw->panel->scrollFocusedInputIntoView(next);
    }
  });

  instance.surface->setSceneRoot(instance.sceneRoot.get());

  if (auto* focusArea = instance.panel->initialFocusArea(); focusArea != nullptr) {
    instance.inputDispatcher.setFocus(focusArea);
  }
}

void PersistentPanelHost::layoutScene(Instance& instance, std::uint32_t width, std::uint32_t height) {
  if (instance.sceneRoot == nullptr || instance.panel == nullptr) {
    return;
  }
  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  instance.sceneRoot->setSize(w, h);

  // Honor the compositor-configured surface size: a filled axis derives its visual
  // size from the configure, and a fixed axis configured smaller than requested
  // lays out at the configured size instead of overflowing the buffer.
  const std::int32_t availW = static_cast<std::int32_t>(width) - instance.insetX - instance.bleedRight;
  const std::int32_t availH = static_cast<std::int32_t>(height) - instance.insetY - instance.bleedBottom;
  if (availW > 0 && (instance.fillWidth || instance.visualWidth > static_cast<std::uint32_t>(availW))) {
    instance.visualWidth = static_cast<std::uint32_t>(availW);
  }
  if (availH > 0 && (instance.fillHeight || instance.visualHeight > static_cast<std::uint32_t>(availH))) {
    instance.visualHeight = static_cast<std::uint32_t>(availH);
  }
  instance.surface->setInputRegion({InputRect{
      instance.insetX, instance.insetY, static_cast<int>(instance.visualWidth), static_cast<int>(instance.visualHeight)
  }});

  const auto panelX = static_cast<float>(instance.insetX);
  const auto panelY = static_cast<float>(instance.insetY);
  const float panelW = instance.visualWidth > 0 ? static_cast<float>(instance.visualWidth) : w;
  const float panelH = instance.visualHeight > 0 ? static_cast<float>(instance.visualHeight) : h;

  if (instance.shadowNode != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    instance.shadowNode->setVisible(panelShadow);
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    instance.shadowNode->setPosition(
        panelX + static_cast<float>(shadowOff.x), panelY + static_cast<float>(shadowOff.y)
    );
    instance.shadowNode->setSize(panelW, panelH);
    if (panelShadow) {
      const float radius = Style::scaledRadiusXl(instance.panel->contentScale());
      instance.shadowNode->setStyle(
          shell::surface_shadow::style(
              shadowConfig, shell::panel_surface::backgroundOpacity(m_config),
              shell::surface_shadow::Shape{.radius = Radii{radius, radius, radius, radius}}
          )
      );
    }
  }

  if (instance.bgNode != nullptr) {
    instance.bgNode->setPosition(panelX, panelY);
    instance.bgNode->setSize(panelW, panelH);
  }

  const float padding = instance.panel->hasDecoration() ? instance.panel->contentScale() * Style::panelPadding : 0.0f;
  const float contentWidth = panelW - padding * 2.0f;
  const float contentHeight = panelH - padding * 2.0f;
  {
    UiPhaseScope updatePhase(UiPhase::Update);
    instance.panel->update(*m_renderContext);
  }
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    instance.panel->layout(*m_renderContext, contentWidth, contentHeight);
  }
  if (instance.contentNode != nullptr) {
    instance.contentNode->setPosition(panelX + padding, panelY + padding);
    instance.contentNode->setSize(contentWidth, contentHeight);
  }
  if (auto* area = instance.panel->takePendingFocusArea(); area != nullptr) {
    instance.inputDispatcher.setFocus(area);
  }
  if (instance.pointerInside) {
    instance.inputDispatcher.syncPointerHover();
  }
}

void PersistentPanelHost::prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || instance.surface == nullptr || instance.panel == nullptr) {
    return;
  }
  m_renderContext->makeCurrent(instance.surface->renderTarget());

  const auto width = instance.surface->width();
  const auto height = instance.surface->height();
  const bool needsSceneBuild = instance.sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(instance.sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(instance.sceneRoot->height())) != height;

  if (needsSceneBuild) {
    buildScene(instance, width, height);
    layoutScene(instance, width, height);
    return;
  }

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    instance.panel->update(*m_renderContext);
  }
  if (needsLayout) {
    layoutScene(instance, width, height);
    return;
  }
  if (needsUpdate) {
    if (auto* area = instance.panel->takePendingFocusArea(); area != nullptr) {
      instance.inputDispatcher.setFocus(area);
    }
  }
}

bool PersistentPanelHost::onPointerEvent(const PointerEvent& event) {
  if (m_instances.empty()) {
    return false;
  }
  Instance* instance = findInstanceForSurface(event.surface);
  if (instance == nullptr) {
    // Motion/axis carry no surface on some compositors; route to whichever
    // instance the pointer last entered.
    for (auto& candidate : m_instances) {
      if (candidate->pointerInside) {
        instance = candidate.get();
        break;
      }
    }
  }
  if (instance == nullptr) {
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    instance->pointerInside = true;
    instance->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Leave:
    instance->pointerInside = false;
    instance->inputDispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    if (!instance->pointerInside) {
      return false;
    }
    instance->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  case PointerEvent::Type::Button:
    if (!instance->pointerInside) {
      return false;
    }
    instance->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed
    );
    break;
  case PointerEvent::Type::Axis:
    if (!instance->pointerInside) {
      return false;
    }
    instance->inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }

  if (instance->sceneRoot != nullptr && (instance->sceneRoot->paintDirty() || instance->sceneRoot->layoutDirty())) {
    if (instance->sceneRoot->layoutDirty() && !instance->panel->deferPointerRelayout()) {
      instance->surface->requestLayout();
    } else {
      instance->surface->requestRedraw();
    }
  }
  return true;
}

bool PersistentPanelHost::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_instances.empty() || m_platform == nullptr) {
    return false;
  }
  // Route only to the surface the compositor reports as keyboard-focused. A panel
  // with keyboard_interactivity None never matches, so its keys stay with the app.
  Instance* instance = findInstanceForSurface(m_platform->lastKeyboardSurface());
  if (instance == nullptr || instance->panel == nullptr) {
    return false;
  }

  if (instance->panel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
    if (instance->sceneRoot != nullptr && (instance->sceneRoot->paintDirty() || instance->sceneRoot->layoutDirty())) {
      if (instance->sceneRoot->layoutDirty()) {
        instance->surface->requestLayout();
      } else {
        instance->surface->requestRedraw();
      }
    }
    return true;
  }

  instance->inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (instance->sceneRoot != nullptr && (instance->sceneRoot->paintDirty() || instance->sceneRoot->layoutDirty())) {
    if (instance->sceneRoot->layoutDirty()) {
      instance->surface->requestLayout();
    } else {
      instance->surface->requestRedraw();
    }
  }
  return true;
}

void PersistentPanelHost::refreshPanel(std::string_view id) {
  Instance* instance = findInstance(id);
  if (instance == nullptr || instance->surface == nullptr || instance->panel == nullptr) {
    return;
  }
  if (instance->panel->deferExternalRefresh()) {
    return;
  }
  instance->surface->requestUpdate();
}

void PersistentPanelHost::onConfigReloaded() {
  for (auto& instance : m_instances) {
    if (instance->panel == nullptr) {
      continue;
    }
    const float backgroundOpacity = shell::panel_surface::backgroundOpacity(m_config);
    instance->panel->setContentScale(shell::panel_surface::contentScale(m_config));
    instance->panel->setPanelCardOpacity(shell::panel_surface::cardOpacity(m_config, backgroundOpacity));
    instance->panel->setPanelBordersEnabled(m_config->config().shell.panel.borders);
    if (instance->bgNode != nullptr) {
      instance->bgNode->setPanelStyle(m_config->config().shell.panel.borders);
      instance->bgNode->setFill(colorSpecFromRole(ColorRole::Surface, backgroundOpacity));
    }
    if (instance->surface != nullptr) {
      instance->surface->requestLayout();
    }
  }
}

void PersistentPanelHost::onIconThemeChanged() {
  for (auto& instance : m_instances) {
    if (instance->panel != nullptr) {
      instance->panel->onIconThemeChanged();
    }
    if (instance->surface != nullptr) {
      instance->surface->requestLayout();
    }
  }
}

void PersistentPanelHost::refresh() {
  for (auto& instance : m_instances) {
    if (instance->surface != nullptr) {
      instance->surface->requestUpdate();
    }
  }
}
