#pragma once

#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <vector>

class ConfigService;
class RenderContext;
class WaylandConnection;

class ScreenCorners {
public:
  ScreenCorners() = default;
  ~ScreenCorners() = default;

  ScreenCorners(const ScreenCorners&) = delete;
  ScreenCorners& operator=(const ScreenCorners&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  void onOutputChange();
  void onConfigReload();
  void requestRedraw();

private:
  struct Corner {
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    std::uint32_t builtWidth = 0;
    std::uint32_t builtHeight = 0;
  };

  struct OutputInstance {
    wl_output* output = nullptr;
    Corner corners[4];
  };

  void ensureSurfaces();
  void destroySurfaces();
  void buildCornerScene(Corner& corner, std::uint32_t width, std::uint32_t height, int cornerIndex);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  bool m_lastEnabled = false;
  std::int32_t m_lastSize = 0;
  std::vector<std::unique_ptr<OutputInstance>> m_instances;
};
