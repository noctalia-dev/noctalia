#pragma once

#include "config/config_types.h"
#include "render/animation/animation_manager.h"
#include "shell/desktop/desktop_widget_factory.h"

#include <memory>
#include <string>
#include <vector>

class ConfigService;
class LockScreen;
class LockSurface;
class RenderContext;
class WaylandConnection;

using LockscreenWidgetsSnapshot = LockscreenWidgetsConfig;

class LockscreenWidgetsHost {
public:
  LockscreenWidgetsHost() = default;

  void initialize(const DesktopWidgetServices& services);
  void show(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen);
  void hide();
  void rebuild(const LockscreenWidgetsSnapshot& snapshot, LockScreen& lockScreen);
  void onOutputChange(LockScreen& lockScreen);
  void onSecondTick();
  void prepareFrame(LockSurface& surface, bool needsUpdate, bool needsLayout);

private:
  struct WidgetInstance {
    DesktopWidgetState state;
    LockSurface* surface = nullptr;
    AnimationManager animations;
    Node* transformNode = nullptr;
    std::unique_ptr<DesktopWidget> widget;
    float intrinsicWidth = 0.0f;
    float intrinsicHeight = 0.0f;
  };

  void syncSurfaces(LockScreen& lockScreen);
  void createInstance(const DesktopWidgetState& state, LockSurface& surface, const WaylandOutput& output);
  void attachToSurface(WidgetInstance& instance);
  void detachFromSurface(WidgetInstance& instance);
  void syncSurfaceFrameTick(LockSurface* surface);
  [[nodiscard]] WidgetInstance* findInstance(const std::string& id);
  [[nodiscard]] LockSurface* findSurfaceForOutput(LockScreen& lockScreen, const std::string& outputKey) const;

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::unique_ptr<DesktopWidgetFactory> m_factory;
  LockscreenWidgetsSnapshot m_snapshot;
  bool m_visible = false;
  std::vector<std::unique_ptr<WidgetInstance>> m_instances;
};
