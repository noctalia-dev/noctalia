#pragma once

#include "core/timer_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_seat.h"

#include <memory>
#include <string>
#include <vector>

class Application;
class ConfigService;
class RenderContext;
class WaylandConnection;
struct wl_output;

class HotCorners {
public:
  HotCorners(Application* app);
  ~HotCorners();

  HotCorners(const HotCorners&) = delete;
  HotCorners& operator=(const HotCorners&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  void onOutputChange();
  void onConfigReload();
  bool onPointerEvent(const PointerEvent& event);

private:
  struct Corner {
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher;
    Timer triggerTimer;
  };

  struct OutputInstance {
    wl_output* output = nullptr;
    Corner topLeft;
    Corner topRight;
    Corner bottomLeft;
    Corner bottomRight;
  };

  void ensureSurfaces();
  void destroySurfaces();
  void buildCorner(Corner& corner, int position, wl_output* output);
  void armCorner(Corner& corner, int position, wl_output* output);
  void disarmCorner(Corner& corner);
  void triggerAction(const std::string& action, const std::string& command, wl_output* output);

  Application* m_app = nullptr;
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  bool m_lastEnabled = false;

  std::vector<std::unique_ptr<OutputInstance>> m_instances;
};
