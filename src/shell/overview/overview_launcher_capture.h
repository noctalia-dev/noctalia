#pragma once

#include "wayland/wayland_seat.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class CompositorPlatform;
class LayerSurface;
class PanelManager;
class RenderContext;
class WaylandConnection;
struct wl_output;

// Niri does not expose passive overview text events. While overview is open,
// keep tiny keyboard-exclusive layer surfaces so typing can open the launcher
// without allocating full-output buffers.
class OverviewLauncherCapture {
public:
  using OpenLauncherCallback =
      std::function<void(std::string_view initialQuery, wl_output* output, std::string_view sourceBarName)>;

  bool initialize(
      WaylandConnection& wayland, RenderContext* renderContext, CompositorPlatform& platform, PanelManager& panelManager
  );
  void setOpenLauncherCallback(OpenLauncherCallback callback);
  void setEnabled(bool enabled);

  void sync();
  void onOutputChange();
  [[nodiscard]] bool handleKeyboardEvent(const KeyboardEvent& event);

private:
  struct Instance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
  };

  [[nodiscard]] bool shouldBeActive() const;
  [[nodiscard]] bool surfacesMatchOutputs() const;
  [[nodiscard]] bool handleNiriOverviewKey(const KeyboardEvent& event) const;
  [[nodiscard]] bool sendNiriAction(const char* actionName) const;
  void ensureSurfaces();
  void destroySurfaces();
  [[nodiscard]] Instance* instanceForKeyboardSurface(wl_surface* surface) noexcept;

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  CompositorPlatform* m_platform = nullptr;
  PanelManager* m_panelManager = nullptr;
  OpenLauncherCallback m_openLauncher;
  std::vector<std::unique_ptr<Instance>> m_instances;
  bool m_enabled = false;
};
