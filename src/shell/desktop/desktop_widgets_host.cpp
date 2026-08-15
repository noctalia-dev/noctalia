#include "shell/desktop/desktop_widgets_host.h"

#include "config/config_service.h"
#include "core/log.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "scripting/plugin_registry.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/widget_transform.h"
#include "shell/wallpaper/wallpaper_geometry.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace {

  constexpr Logger kLog("desktop");

  DesktopWidgetState* findStateById(DesktopWidgetsSnapshot& snapshot, const std::string& id) {
    for (auto& widget : snapshot.widgets) {
      if (widget.id == id) {
        return &widget;
      }
    }
    return nullptr;
  }

  // Per-widget layer-shell namespace so compositor rules can target individual widgets.
  // The id already carries the "desktop-widget-" prefix; strip it to avoid doubling.
  std::string desktopWidgetNamespace(const DesktopWidgetState& state) {
    constexpr std::string_view kIdPrefix = "desktop-widget-";
    std::string_view uid = state.id;
    if (uid.starts_with(kIdPrefix)) {
      uid.remove_prefix(kIdPrefix.size());
    }
    return "noctalia-desktop-widget-" + state.type + "-" + std::string(uid);
  }

} // namespace

DesktopWidgetsHost::~DesktopWidgetsHost() { releaseWallpaperMasks(); }

void DesktopWidgetsHost::initialize(const DesktopWidgetServices& services) {
  m_wayland = &services.wayland;
  m_config = services.config;
  m_renderContext = services.renderContext;
  m_textureCache = services.textureCache;
  m_factory = std::make_unique<DesktopWidgetFactory>(services.runtime);
}

void DesktopWidgetsHost::releaseWallpaperMasks() {
  if (m_textureCache != nullptr) {
    for (auto& [outputName, mask] : m_wallpaperMasks) {
      (void)outputName;
      m_textureCache->releaseAlphaMask(mask.retainedTexture, mask.descriptor.path);
    }
  }
  m_wallpaperMasks.clear();
}

void DesktopWidgetsHost::setWallpaperMasks(const OutputWallpaperMaskMap& masks) {
  for (auto it = m_wallpaperMasks.begin(); it != m_wallpaperMasks.end();) {
    const auto desired = masks.find(it->first);
    if (desired != masks.end() && desired->second == it->second.descriptor) {
      ++it;
      continue;
    }
    if (m_textureCache != nullptr) {
      m_textureCache->releaseAlphaMask(it->second.retainedTexture, it->second.descriptor.path);
    }
    it = m_wallpaperMasks.erase(it);
  }

  if (m_textureCache != nullptr) {
    for (const auto& [outputName, descriptor] : masks) {
      if (m_wallpaperMasks.contains(outputName)) {
        continue;
      }
      TextureHandle texture = m_textureCache->acquireAlphaMask(descriptor.path);
      const TextureHandle wallpaperTexture = m_textureCache->peek(descriptor.wallpaperPath);
      if (!texture.valid()
          || !wallpaperTexture.valid()
          || texture.width != wallpaperTexture.width
          || texture.height != wallpaperTexture.height) {
        kLog.warn("rejected wallpaper mask with invalid source dimensions for {}", outputName);
        m_textureCache->releaseAlphaMask(texture, descriptor.path);
        continue;
      }
      m_wallpaperMasks.emplace(outputName, LoadedWallpaperMask{.descriptor = descriptor, .retainedTexture = texture});
    }
  }
  for (auto& instance : m_instances) {
    updateWallpaperMask(*instance);
  }
}

void DesktopWidgetsHost::show(const DesktopWidgetsSnapshot& snapshot) {
  m_snapshot = snapshot;
  m_visible = true;
  syncInstances();
}

void DesktopWidgetsHost::hide() {
  m_visible = false;
  m_instances.clear();
}

void DesktopWidgetsHost::rebuild(const DesktopWidgetsSnapshot& snapshot) {
  m_snapshot = snapshot;
  if (!m_visible) {
    return;
  }
  syncInstances();
}

void DesktopWidgetsHost::reloadPluginWidgets() {
  if (!m_visible) {
    return;
  }
  const auto before = m_instances.size();
  std::erase_if(m_instances, [](const auto& instance) {
    return scripting::isPluginEntryOfKind(instance->state.type, scripting::PluginEntryKind::DesktopWidget);
  });
  if (m_instances.size() == before) {
    return;
  }
  syncInstances();
}

void DesktopWidgetsHost::onOutputChange() {
  if (!m_visible) {
    return;
  }
  syncInstances();
}

void DesktopWidgetsHost::onSecondTick() {
  if (!m_visible) {
    return;
  }

  const bool minuteBoundary = formatLocalTime("{:%S}") == "00";
  for (auto& instance : m_instances) {
    if (instance->surface == nullptr || instance->widget == nullptr) {
      continue;
    }
    if (instance->widget->wantsSecondTicks()) {
      instance->surface->requestUpdateOnly();
    } else if (minuteBoundary) {
      instance->surface->requestUpdate();
    }
  }
}

void DesktopWidgetsHost::requestUpdate() {
  for (auto& instance : m_instances) {
    if (instance->surface != nullptr) {
      instance->surface->requestUpdateOnly();
    }
  }
}

void DesktopWidgetsHost::requestLayout() {
  for (auto& instance : m_instances) {
    if (instance->surface != nullptr) {
      instance->surface->requestLayout();
    }
  }
}

void DesktopWidgetsHost::requestRedraw() {
  for (auto& instance : m_instances) {
    if (instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

DesktopWidgetsHost::DesktopWidgetInstance* DesktopWidgetsHost::findInstance(const std::string& id) {
  for (auto& instance : m_instances) {
    if (instance->state.id == id) {
      return instance.get();
    }
  }
  return nullptr;
}

void DesktopWidgetsHost::syncInstances() {
  if (!m_visible || m_wayland == nullptr || m_renderContext == nullptr || m_factory == nullptr) {
    return;
  }

  std::erase_if(m_instances, [this](const auto& instance) {
    const DesktopWidgetState* state = findStateById(m_snapshot, instance->state.id);
    return state == nullptr || !state->enabled;
  });

  for (const auto& state : m_snapshot.widgets) {
    if (!state.enabled) {
      continue;
    }

    const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, state);
    if (output == nullptr) {
      // Explicitly bound widgets are hidden while their target output is unavailable.
      std::erase_if(m_instances, [&state](const auto& instance) { return instance->state.id == state.id; });
      continue;
    }

    DesktopWidgetInstance* existing = findInstance(state.id);
    if (existing == nullptr) {
      createInstance(state, *output);
      continue;
    }

    const std::string effectiveOutputName = desktop_widgets::outputKey(*output);
    const bool widgetDefinitionChanged = existing->state.type != state.type
        || existing->state.settings != state.settings
        || existing->effectiveOutputName != effectiveOutputName;

    if (widgetDefinitionChanged) {
      std::erase_if(m_instances, [&state](const auto& instance) { return instance->state.id == state.id; });
      createInstance(state, *output);
      continue;
    }

    if (!(existing->state == state)) {
      existing->state = state;
      if (existing->surface != nullptr) {
        existing->surface->requestLayout();
      }
    }
  }
}

void DesktopWidgetsHost::createInstance(const DesktopWidgetState& state, const WaylandOutput& output) {
  if (m_factory == nullptr || m_renderContext == nullptr) {
    return;
  }

  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0F;
  auto widget = m_factory->create(state.type, state.settings, desktop_widgets::widgetContentScale(baseUiScale));
  if (widget == nullptr) {
    return;
  }

  widget->create();
  widget->setBox(state.boxWidth, state.boxHeight);
  ScaledRenderer measureRenderer(*m_renderContext, output.configuredScale());
  widget->update(measureRenderer);
  widget->layout(measureRenderer);

  const float intrinsicWidth = std::max(1.0F, widget->intrinsicWidth());
  const float intrinsicHeight = std::max(1.0F, widget->intrinsicHeight());

  DesktopWidgetState clampedState = state;
  if (m_wayland != nullptr) {
    desktop_widgets::clampStateToOutput(*m_wayland, clampedState, intrinsicWidth, intrinsicHeight);
  }

  const float outW = desktop_widgets::outputLogicalWidth(output);
  const float outH = desktop_widgets::outputLogicalHeight(output);
  const WidgetTransformClippedGeometry geometry = computeClippedWidgetSurfaceGeometry(
      clampedState.cx, clampedState.cy, intrinsicWidth, intrinsicHeight, 1.0F, clampedState.rotationRad, outW, outH
  );

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = desktopWidgetNamespace(clampedState),
      .layer = LayerShellLayer::Bottom,
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Left,
      .width = geometry.surfaceWidth,
      .height = geometry.surfaceHeight,
      .exclusiveZone = -1,
      .marginTop = geometry.marginTop,
      .marginLeft = geometry.marginLeft,
      .keyboard = LayerShellKeyboard::None,
      .defaultWidth = geometry.surfaceWidth,
      .defaultHeight = geometry.surfaceHeight,
  };

  auto instance = std::make_unique<DesktopWidgetInstance>();
  instance->state = clampedState;
  instance->effectiveOutputName = desktop_widgets::outputKey(output);
  instance->output = output.output;
  instance->widget = std::move(widget);
  instance->intrinsicWidth = intrinsicWidth;
  instance->intrinsicHeight = intrinsicHeight;

  instance->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
  instance->surface->setRenderContext(m_renderContext);
  instance->surface->setAnimationManager(&instance->animations);

  auto* rawInstance = instance.get();
  instance->widget->setAnimationManager(&instance->animations);
  instance->widget->setUpdateCallback([rawInstance]() {
    if (rawInstance->surface != nullptr) {
      rawInstance->surface->requestUpdateOnly();
    }
  });
  instance->widget->setLayoutCallback([rawInstance]() {
    if (rawInstance->surface != nullptr) {
      rawInstance->surface->requestUpdate();
    }
  });
  instance->widget->setRedrawCallback([rawInstance]() {
    if (rawInstance->surface != nullptr) {
      rawInstance->surface->requestRedraw();
    }
  });
  instance->widget->setFrameTickRequestCallback([rawInstance]() {
    if (rawInstance->surface != nullptr) {
      rawInstance->surface->requestFrameTick();
    }
  });

  instance->surface->setConfigureCallback([rawInstance](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    rawInstance->surface->requestLayout();
  });
  instance->surface->setPrepareFrameCallback([this, rawInstance](bool needsUpdate, bool needsLayout) {
    prepareFrame(*rawInstance, needsUpdate, needsLayout);
  });
  instance->surface->setFrameTickCallback([this, rawInstance](float deltaMs) {
    if (rawInstance->widget == nullptr || rawInstance->surface == nullptr || m_renderContext == nullptr) {
      return;
    }
    if (!rawInstance->widget->needsFrameTick()) {
      return;
    }
    m_renderContext->makeCurrent(rawInstance->surface->renderTarget());
    Renderer& renderer = rawInstance->surface->renderTarget().renderer();
    rawInstance->widget->onFrameTick(deltaMs, renderer);
  });

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("desktop widgets host: failed to initialize widget {} on {}", state.id, instance->effectiveOutputName);
    return;
  }

  // The pre-surface measurement above bound retained render state (owned Image
  // textures, the sticker's frame renderer) to the stack-local ScaledRenderer.
  // initialize() wired the surface's RenderTarget (renderer context + content
  // scale seeded from this output), so rebind the whole widget tree to that
  // stable view before the temporary dies.
  instance->widget->rebindRenderer(instance->surface->renderTarget().renderer());

  m_instances.push_back(std::move(instance));
}

void DesktopWidgetsHost::buildScene(DesktopWidgetInstance& instance) {
  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = ui::node({});
    instance.sceneRoot->setAnimationManager(&instance.animations);

    auto transformNode = ui::node({});
    instance.transformNode = instance.sceneRoot->addChild(std::move(transformNode));
    if (instance.widget != nullptr) {
      instance.transformNode->addChild(instance.widget->releaseRoot());
    }

    instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
    if (m_wayland != nullptr) {
      instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_wayland->setCursorShape(serial, shape);
      });
    }

    if (instance.surface != nullptr) {
      instance.surface->setSceneRoot(instance.sceneRoot.get());
    }
  }
}

void DesktopWidgetsHost::updateWallpaperMask(DesktopWidgetInstance& instance) {
  if (instance.surface == nullptr || m_wayland == nullptr || m_config == nullptr || m_textureCache == nullptr) {
    return;
  }

  const auto maskIt = m_wallpaperMasks.find(instance.effectiveOutputName);
  const WaylandOutput* output = desktop_widgets::findOutputByKey(*m_wayland, instance.effectiveOutputName);
  if (maskIt == m_wallpaperMasks.end()
      || output == nullptr
      || m_config->getWallpaperPath(instance.effectiveOutputName) != maskIt->second.descriptor.wallpaperPath) {
    instance.surface->setWallpaperMask(std::nullopt);
    return;
  }

  const TextureHandle texture = m_textureCache->peekAlphaMask(maskIt->second.descriptor.path);
  if (!texture.valid() || texture.width <= 0 || texture.height <= 0) {
    instance.surface->setWallpaperMask(std::nullopt);
    return;
  }

  const auto fillMode = m_config->config().wallpaper.fillMode;
  const WallpaperSpanParams span = fillMode == WallpaperFillMode::Span
      ? computeWallpaperSpanParams(m_wayland->outputs(), output->name)
      : WallpaperSpanParams{};
  instance.surface->setWallpaperMask(
      WallpaperMaskDrawParams{
          .texture = texture.id,
          .surfaceWidth = static_cast<float>(instance.surface->width()),
          .surfaceHeight = static_cast<float>(instance.surface->height()),
          .surfaceOffsetX = static_cast<float>(instance.surface->marginLeft()),
          .surfaceOffsetY = static_cast<float>(instance.surface->marginTop()),
          .outputWidth = desktop_widgets::outputLogicalWidth(*output),
          .outputHeight = desktop_widgets::outputLogicalHeight(*output),
          .imageWidth = static_cast<float>(texture.width),
          .imageHeight = static_cast<float>(texture.height),
          .fillMode = static_cast<float>(fillMode),
          .span = span,
      }
  );
}

void DesktopWidgetsHost::prepareFrame(DesktopWidgetInstance& instance, bool needsUpdate, bool needsLayout) {
  if (instance.widget == nullptr || instance.surface == nullptr || m_renderContext == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());
  Renderer& renderer = instance.surface->renderTarget().renderer();

  buildScene(instance);

  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0F;
  instance.widget->setContentScale(desktop_widgets::widgetContentScale(baseUiScale));
  instance.widget->setBox(instance.state.boxWidth, instance.state.boxHeight);

  if (needsUpdate) {
    instance.widget->update(renderer);
  }
  if (needsLayout) {
    instance.widget->layout(renderer);
    instance.intrinsicWidth = std::max(1.0F, instance.widget->intrinsicWidth());
    instance.intrinsicHeight = std::max(1.0F, instance.widget->intrinsicHeight());
  }

  if (m_wayland != nullptr) {
    desktop_widgets::clampStateToOutput(*m_wayland, instance.state, instance.intrinsicWidth, instance.intrinsicHeight);
  }

  float outputW = 1920.0F;
  float outputH = 1080.0F;
  if (m_wayland != nullptr) {
    if (const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, instance.state);
        output != nullptr) {
      outputW = desktop_widgets::outputLogicalWidth(*output);
      outputH = desktop_widgets::outputLogicalHeight(*output);
    }
  }

  const WidgetTransformClippedGeometry geometry = computeClippedWidgetSurfaceGeometry(
      instance.state.cx, instance.state.cy, instance.intrinsicWidth, instance.intrinsicHeight, 1.0F,
      instance.state.rotationRad, outputW, outputH
  );

  if (instance.surface->width() != geometry.surfaceWidth || instance.surface->height() != geometry.surfaceHeight) {
    instance.surface->requestSize(geometry.surfaceWidth, geometry.surfaceHeight);
  }
  instance.surface->setMargins(geometry.marginTop, 0, 0, geometry.marginLeft);
  updateWallpaperMask(instance);

  if (instance.sceneRoot != nullptr) {
    instance.sceneRoot->setFrameSize(
        static_cast<float>(instance.surface->width()), static_cast<float>(instance.surface->height())
    );
  }
  if (instance.transformNode != nullptr) {
    instance.transformNode->setFrameSize(instance.intrinsicWidth, instance.intrinsicHeight);
    instance.transformNode->setPosition(
        geometry.contentOffsetX - instance.intrinsicWidth * 0.5F,
        geometry.contentOffsetY - instance.intrinsicHeight * 0.5F
    );
    instance.transformNode->setRotation(instance.state.rotationRad);
    float flipScaleX = 1.0F;
    float flipScaleY = 1.0F;
    desktop_widgets::widgetNodeScale(instance.state, flipScaleX, flipScaleY);
    instance.transformNode->setScale(flipScaleX, flipScaleY);
  }

  if (instance.widget->needsFrameTick()) {
    instance.surface->requestFrameTick();
  }

  if (instance.widget->hasVisibleBackground()) {
    const float radius = instance.widget->backgroundRadius();
    auto blurStrips = Surface::tessellateRotatedRoundedRect(
        geometry.contentOffsetX, geometry.contentOffsetY, instance.intrinsicWidth, instance.intrinsicHeight, radius,
        instance.state.rotationRad
    );
    instance.surface->setBlurRegion(blurStrips);
  } else {
    instance.surface->clearBlurRegion();
  }
}

bool DesktopWidgetsHost::onPointerEvent(const PointerEvent& event) {
  if (!m_visible || m_instances.empty())
    return false;

  wl_surface* eventSurface = event.surface;
  if (eventSurface == nullptr && m_wayland != nullptr)
    eventSurface = m_wayland->lastPointerSurface();

  DesktopWidgetInstance* target = nullptr;
  for (auto& instance : m_instances) {
    if (instance->surface != nullptr && instance->surface->wlSurface() == eventSurface) {
      target = instance.get();
      break;
    }
  }
  if (target == nullptr)
    return false;

  switch (event.type) {
  case PointerEvent::Type::Enter:
    target->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Leave:
    target->inputDispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    target->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Button:
    target->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.pressed
    );
    break;
  case PointerEvent::Type::Axis:
    target->inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }

  if (target->sceneRoot != nullptr && (target->sceneRoot->layoutDirty() || target->sceneRoot->paintDirty())) {
    if (target->sceneRoot->layoutDirty()) {
      target->surface->requestLayout();
    } else {
      target->surface->requestRedraw();
    }
  }

  return true;
}
