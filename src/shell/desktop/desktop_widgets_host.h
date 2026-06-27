#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "wayland/layer_surface.h"

#include <memory>
#include <string>
#include <vector>

class ConfigService;
class RenderContext;
class WaylandConnection;
struct PointerEvent;
struct WaylandOutput;
struct wl_output;

class DesktopWidgetsHost {
public:
  DesktopWidgetsHost() = default;

  void initialize(const DesktopWidgetServices& services);
  void show(const DesktopWidgetsSnapshot& snapshot);
  void hide();
  void rebuild(const DesktopWidgetsSnapshot& snapshot);
  void onOutputChange();
  void onSecondTick();
  void requestUpdate();
  void requestLayout();
  void requestRedraw();
  bool onPointerEvent(const PointerEvent& event);

private:
  struct DesktopWidgetInstance {
    DesktopWidgetState state;
    std::string effectiveOutputName;
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    std::unique_ptr<Node> sceneRoot;
    Node* transformNode = nullptr;
    std::unique_ptr<DesktopWidget> widget;
    float intrinsicWidth = 0.0f;
    float intrinsicHeight = 0.0f;
  };

  void syncInstances();
  void createInstance(const DesktopWidgetState& state, const WaylandOutput& output);
  void buildScene(DesktopWidgetInstance& instance);
  void prepareFrame(DesktopWidgetInstance& instance, bool needsUpdate, bool needsLayout);
  [[nodiscard]] DesktopWidgetInstance* findInstance(const std::string& id);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::unique_ptr<DesktopWidgetFactory> m_factory;
  DesktopWidgetsSnapshot m_snapshot;
  bool m_visible = false;
  std::vector<std::unique_ptr<DesktopWidgetInstance>> m_instances;
};
