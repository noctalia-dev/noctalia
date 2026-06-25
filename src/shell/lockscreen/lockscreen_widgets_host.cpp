#include "shell/lockscreen/lockscreen_widgets_host.h"

#include "config/config_service.h"
#include "core/log.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lock_surface.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "time/time_format.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <string>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("lockscreen");

  DesktopWidgetState* findStateById(LockscreenWidgetsSnapshot& snapshot, const std::string& id) {
    for (auto& widget : snapshot.widgets) {
      if (widget.id == id) {
        return &widget;
      }
    }
    return nullptr;
  }

} // namespace

void LockscreenWidgetsHost::initialize(const DesktopWidgetServices& services) {
  m_wayland = &services.wayland;
  m_config = services.config;
  m_renderContext = services.renderContext;
  m_factory = std::make_unique<DesktopWidgetFactory>(services.runtime);
}

void LockscreenWidgetsHost::show(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen) {
  m_snapshot = snapshot;
  m_visible = true;
  syncSurfaces(lockScreen);
}

void LockscreenWidgetsHost::hide() {
  for (auto& instance : m_instances) {
    if (instance->widget != nullptr) {
      instance->widget->setFrameTickRequestCallback(nullptr);
      instance->widget->setUpdateCallback(nullptr);
      instance->widget->setLayoutCallback(nullptr);
      instance->widget->setRedrawCallback(nullptr);
    }
    detachFromSurface(*instance);
  }
  m_visible = false;
  m_instances.clear();
}

void LockscreenWidgetsHost::rebuild(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen) {
  m_snapshot = snapshot;
  if (!m_visible) {
    return;
  }
  syncSurfaces(lockScreen);
}

void LockscreenWidgetsHost::onOutputChange(LockScreen& lockScreen) {
  if (!m_visible) {
    return;
  }
  syncSurfaces(lockScreen);
}

void LockscreenWidgetsHost::onSecondTick() {
  if (!m_visible) {
    return;
  }

  const bool minuteBoundary = formatLocalTime("{:%S}") == "00";
  for (auto& instance : m_instances) {
    if (instance->surface == nullptr || instance->widget == nullptr) {
      continue;
    }
    if (instance->widget->wantsSecondTicks() || minuteBoundary) {
      instance->surface->requestUpdate();
    }
  }
}

LockscreenWidgetsHost::WidgetInstance* LockscreenWidgetsHost::findInstance(const std::string& id) {
  for (auto& instance : m_instances) {
    if (instance->state.id == id) {
      return instance.get();
    }
  }
  return nullptr;
}

LockSurface* LockscreenWidgetsHost::findSurfaceForOutput(LockScreen& lockScreen, const std::string& outputKey) const {
  LockSurface* found = nullptr;
  lockScreen.forEachSurface([&](LockSurface& surface) {
    if (found != nullptr || m_wayland == nullptr) {
      return;
    }
    const WaylandOutput* output = m_wayland->findOutputByWl(surface.output());
    if (output == nullptr) {
      return;
    }
    if (desktop_widgets::outputKey(*output) == outputKey) {
      found = &surface;
    }
  });
  return found;
}

void LockscreenWidgetsHost::syncSurfaces(LockScreen& lockScreen) {
  if (!m_visible || m_wayland == nullptr || m_renderContext == nullptr || m_factory == nullptr) {
    return;
  }

  std::erase_if(m_instances, [this](std::unique_ptr<WidgetInstance>& instance) {
    const DesktopWidgetState* state = findStateById(m_snapshot, instance->state.id);
    if (state == nullptr || !state->enabled) {
      detachFromSurface(*instance);
      return true;
    }
    return instance->surface == nullptr;
  });

  for (const auto& state : m_snapshot.widgets) {
    if (!state.enabled || lockscreen_login_box::isLoginBoxWidget(state)) {
      continue;
    }

    const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, state);
    if (output == nullptr) {
      std::erase_if(m_instances, [this, &state](std::unique_ptr<WidgetInstance>& instance) {
        if (instance->state.id == state.id) {
          detachFromSurface(*instance);
          return true;
        }
        return false;
      });
      continue;
    }

    const std::string outputKey = desktop_widgets::outputKey(*output);
    LockSurface* surface = findSurfaceForOutput(lockScreen, outputKey);
    if (surface == nullptr) {
      std::erase_if(m_instances, [this, &state](std::unique_ptr<WidgetInstance>& instance) {
        if (instance->state.id == state.id) {
          detachFromSurface(*instance);
          return true;
        }
        return false;
      });
      continue;
    }

    WidgetInstance* existing = findInstance(state.id);
    if (existing == nullptr) {
      createInstance(state, *surface, *output);
      continue;
    }

    const bool widgetDefinitionChanged = existing->state.type != state.type
        || existing->state.settings != state.settings
        || existing->surface != surface;

    if (widgetDefinitionChanged) {
      detachFromSurface(*existing);
      std::erase_if(m_instances, [this, &state](std::unique_ptr<WidgetInstance>& instance) {
        detachFromSurface(*instance);
        return instance->state.id == state.id;
      });
      createInstance(state, *surface, *output);
      continue;
    }

    if (!(existing->state == state)) {
      existing->state = state;
      if (existing->surface != nullptr) {
        existing->surface->requestLayout();
      }
    }
  }

  lockScreen.forEachSurface([&](LockSurface& surface) { surface.setWidgetsHost(this); });
}

void LockscreenWidgetsHost::createInstance(
    const DesktopWidgetState& state, LockSurface& surface, const WaylandOutput& output
) {
  if (m_factory == nullptr || m_renderContext == nullptr) {
    return;
  }

  const float baseUiScale = m_config != nullptr ? m_config->config().shell.uiScale : 1.0f;
  auto widget = m_factory->create(state.type, state.settings, desktop_widgets::widgetContentScale(baseUiScale));
  if (widget == nullptr) {
    return;
  }

  widget->create();
  widget->setBox(state.boxWidth, state.boxHeight);

  if (surface.renderTarget().isReady()) {
    m_renderContext->makeCurrent(surface.renderTarget());
    widget->update(*m_renderContext);
    widget->layout(*m_renderContext);
  }

  const float intrinsicWidth = std::max(1.0f, widget->intrinsicWidth());
  const float intrinsicHeight = std::max(1.0f, widget->intrinsicHeight());

  DesktopWidgetState clampedState = state;
  desktop_widgets::clampStateToOutput(*m_wayland, clampedState, intrinsicWidth, intrinsicHeight);

  auto instance = std::make_unique<WidgetInstance>();
  instance->state = clampedState;
  instance->surface = &surface;
  instance->widget = std::move(widget);
  instance->intrinsicWidth = intrinsicWidth;
  instance->intrinsicHeight = intrinsicHeight;

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
  instance->widget->setFrameTickRequestCallback([this, rawInstance]() {
    if (rawInstance->surface != nullptr) {
      syncSurfaceFrameTick(rawInstance->surface);
      rawInstance->surface->requestFrameTick();
    }
  });

  attachToSurface(*instance);
  m_instances.push_back(std::move(instance));
  (void)output;
}

void LockscreenWidgetsHost::attachToSurface(WidgetInstance& instance) {
  if (instance.surface == nullptr || instance.widget == nullptr) {
    return;
  }

  Node* layer = instance.surface->widgetLayer();
  if (layer == nullptr) {
    return;
  }

  auto transformNode = std::make_unique<Node>();
  transformNode->setAnimationManager(&instance.animations);
  instance.transformNode = layer->addChild(std::move(transformNode));
  instance.transformNode->addChild(instance.widget->releaseRoot());

  syncSurfaceFrameTick(instance.surface);
  instance.surface->requestLayout();
}

void LockscreenWidgetsHost::syncSurfaceFrameTick(LockSurface* surfacePtr) {
  if (surfacePtr == nullptr) {
    return;
  }

  const bool hasWidgets = std::ranges::any_of(m_instances, [&](const auto& instance) {
    return instance->surface == surfacePtr && instance->widget != nullptr;
  });
  if (!hasWidgets) {
    surfacePtr->setFrameTickCallback(nullptr);
    return;
  }

  auto* host = this;
  surfacePtr->setFrameTickCallback([host, surfacePtr](float deltaMs) {
    if (host->m_renderContext == nullptr) {
      return;
    }
    host->m_renderContext->makeCurrent(surfacePtr->renderTarget());

    bool needsRedraw = false;
    for (auto& instance : host->m_instances) {
      if (instance->surface != surfacePtr) {
        continue;
      }
      instance->animations.tick(deltaMs);
      needsRedraw = needsRedraw || instance->animations.hasActive();
    }

    bool needsContinuousRedraw = needsRedraw;
    for (auto& instance : host->m_instances) {
      if (instance->surface != surfacePtr || instance->widget == nullptr || !instance->widget->needsFrameTick()) {
        continue;
      }
      instance->widget->onFrameTick(deltaMs, *host->m_renderContext);
      needsContinuousRedraw = true;
    }

    if (needsContinuousRedraw) {
      surfacePtr->requestRedraw();
    }
  });
}

void LockscreenWidgetsHost::detachFromSurface(WidgetInstance& instance) {
  LockSurface* surface = instance.surface;

  if (instance.transformNode != nullptr && surface != nullptr) {
    if (Node* layer = surface->widgetLayer(); layer != nullptr) {
      (void)layer->removeChild(instance.transformNode);
    }
    instance.transformNode = nullptr;
  }
  instance.surface = nullptr;

  if (surface != nullptr) {
    syncSurfaceFrameTick(surface);
  }
}

void LockscreenWidgetsHost::prepareFrame(LockSurface& surface, bool needsUpdate, bool needsLayout) {
  if (!m_visible || m_renderContext == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(surface.renderTarget());

  const float baseUiScale = m_config != nullptr ? m_config->config().shell.uiScale : 1.0f;
  const auto surfaceW = static_cast<float>(surface.width());
  const auto surfaceH = static_cast<float>(surface.height());

  Node* layer = surface.widgetLayer();
  if (layer != nullptr) {
    layer->setPosition(0.0f, 0.0f);
    layer->setSize(surfaceW, surfaceH);
  }

  for (auto& instance : m_instances) {
    if (instance->surface != &surface || instance->widget == nullptr) {
      continue;
    }

    instance->widget->setContentScale(desktop_widgets::widgetContentScale(baseUiScale));
    instance->widget->setBox(instance->state.boxWidth, instance->state.boxHeight);

    if (needsUpdate) {
      instance->widget->update(*m_renderContext);
    }
    if (needsLayout) {
      instance->widget->layout(*m_renderContext);
      instance->intrinsicWidth = std::max(1.0f, instance->widget->intrinsicWidth());
      instance->intrinsicHeight = std::max(1.0f, instance->widget->intrinsicHeight());
    }

    if (m_wayland != nullptr) {
      DesktopWidgetState currentState = instance->state;
      if (const DesktopWidgetState* origState = findStateById(m_snapshot, instance->state.id); origState != nullptr) {
        currentState = *origState;
      }
      if (const WaylandOutput* output = desktop_widgets::resolveStateOutput(*m_wayland, currentState);
          output != nullptr) {
        float curW = surfaceW;
        float curH = surfaceH;
        bool isRotated90or270 =
            (output->transform == WL_OUTPUT_TRANSFORM_90
             || output->transform == WL_OUTPUT_TRANSFORM_270
             || output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
             || output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270);
        float refW = isRotated90or270 ? curH : curW;
        float refH = isRotated90or270 ? curW : curH;
        if (refW > 0.0f && refH > 0.0f) {
          currentState.cx = currentState.cx * (curW / refW);
          currentState.cy = currentState.cy * (curH / refH);
        }
      }
      desktop_widgets::clampStateToOutput(
          *m_wayland, currentState, instance->intrinsicWidth, instance->intrinsicHeight
      );
      instance->state = currentState;
    }

    if (instance->transformNode == nullptr) {
      continue;
    }

    instance->transformNode->setFrameSize(instance->intrinsicWidth, instance->intrinsicHeight);
    instance->transformNode->setPosition(
        instance->state.cx - instance->intrinsicWidth * 0.5f, instance->state.cy - instance->intrinsicHeight * 0.5f
    );
    instance->transformNode->setRotation(instance->state.rotationRad);
    float flipScaleX = 1.0f;
    float flipScaleY = 1.0f;
    desktop_widgets::widgetNodeScale(instance->state, flipScaleX, flipScaleY);
    instance->transformNode->setScale(flipScaleX, flipScaleY);
  }
}
