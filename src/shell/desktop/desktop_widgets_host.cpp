#include "shell/desktop/desktop_widgets_host.h"

#include "config/config_service.h"
#include "core/log.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/widget_transform.h"
#include "time/time_format.h"
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

void DesktopWidgetsHost::initialize(const DesktopWidgetServices& services) {
  m_wayland = &services.wayland;
  m_config = services.config;
  m_renderContext = services.renderContext;
  m_factory = std::make_unique<DesktopWidgetFactory>(services.runtime);
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

  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0f;
  auto widget = m_factory->create(state.type, state.settings, desktop_widgets::widgetContentScale(baseUiScale));
  if (widget == nullptr) {
    return;
  }

  widget->create();
  widget->setBox(state.boxWidth, state.boxHeight);
  widget->update(*m_renderContext);
  widget->layout(*m_renderContext);

  const float intrinsicWidth = std::max(1.0f, widget->intrinsicWidth());
  const float intrinsicHeight = std::max(1.0f, widget->intrinsicHeight());

  DesktopWidgetState clampedState = state;
  if (m_wayland != nullptr) {
    desktop_widgets::clampStateToOutput(*m_wayland, clampedState, intrinsicWidth, intrinsicHeight);
  }

  const float outW = desktop_widgets::outputLogicalWidth(output);
  const float outH = desktop_widgets::outputLogicalHeight(output);
  const WidgetTransformClippedGeometry geometry = computeClippedWidgetSurfaceGeometry(
      clampedState.cx, clampedState.cy, intrinsicWidth, intrinsicHeight, 1.0f, clampedState.rotationRad, outW, outH
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
    rawInstance->widget->onFrameTick(deltaMs, *m_renderContext);
  });

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("desktop widgets host: failed to initialize widget {} on {}", state.id, instance->effectiveOutputName);
    return;
  }

  m_instances.push_back(std::move(instance));
}

void DesktopWidgetsHost::buildScene(DesktopWidgetInstance& instance) {
  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = std::make_unique<Node>();
    instance.sceneRoot->setAnimationManager(&instance.animations);

    auto transformNode = std::make_unique<Node>();
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

void DesktopWidgetsHost::prepareFrame(DesktopWidgetInstance& instance, bool needsUpdate, bool needsLayout) {
  if (instance.widget == nullptr || instance.surface == nullptr || m_renderContext == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());

  buildScene(instance);

  const float baseUiScale = m_config != nullptr ? m_config->config().accessibility.uiScale : 1.0f;
  instance.widget->setContentScale(desktop_widgets::widgetContentScale(baseUiScale));
  instance.widget->setBox(instance.state.boxWidth, instance.state.boxHeight);

  if (needsUpdate) {
    instance.widget->update(*m_renderContext);
  }
  if (needsLayout) {
    instance.widget->layout(*m_renderContext);
    instance.intrinsicWidth = std::max(1.0f, instance.widget->intrinsicWidth());
    instance.intrinsicHeight = std::max(1.0f, instance.widget->intrinsicHeight());
  }

  if (m_wayland != nullptr) {
    desktop_widgets::clampStateToOutput(*m_wayland, instance.state, instance.intrinsicWidth, instance.intrinsicHeight);
  }

  float outputW = 1920.0f;
  float outputH = 1080.0f;
  if (m_wayland != nullptr) {
    if (const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, instance.state);
        output != nullptr) {
      outputW = desktop_widgets::outputLogicalWidth(*output);
      outputH = desktop_widgets::outputLogicalHeight(*output);
    }
  }

  const WidgetTransformClippedGeometry geometry = computeClippedWidgetSurfaceGeometry(
      instance.state.cx, instance.state.cy, instance.intrinsicWidth, instance.intrinsicHeight, 1.0f,
      instance.state.rotationRad, outputW, outputH
  );

  if (instance.surface->width() != geometry.surfaceWidth || instance.surface->height() != geometry.surfaceHeight) {
    instance.surface->requestSize(geometry.surfaceWidth, geometry.surfaceHeight);
  }
  instance.surface->setMargins(geometry.marginTop, 0, 0, geometry.marginLeft);

  if (instance.sceneRoot != nullptr) {
    instance.sceneRoot->setFrameSize(
        static_cast<float>(instance.surface->width()), static_cast<float>(instance.surface->height())
    );
  }
  if (instance.transformNode != nullptr) {
    instance.transformNode->setFrameSize(instance.intrinsicWidth, instance.intrinsicHeight);
    instance.transformNode->setPosition(
        geometry.contentOffsetX - instance.intrinsicWidth * 0.5f,
        geometry.contentOffsetY - instance.intrinsicHeight * 0.5f
    );
    instance.transformNode->setRotation(instance.state.rotationRad);
    float flipScaleX = 1.0f;
    float flipScaleY = 1.0f;
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
