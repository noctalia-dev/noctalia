#pragma once

#include "shell/switcher/window_switcher_tile.h"
#include "wayland/wayland_seat.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AsyncTextureCache;
class CompositorPlatform;
class ConfigService;
class IpcService;
class RenderContext;
class WaylandConnection;
struct wl_output;

// Fullscreen Alt+Tab style window switcher with a centered 5×5 grid.
class WindowSwitcher {
public:
  WindowSwitcher() = default;
  ~WindowSwitcher();

  void initialize(
      WaylandConnection& wayland, RenderContext* renderContext, CompositorPlatform& platform, ConfigService* config,
      AsyncTextureCache* asyncTextures
  );
  void registerIpc(IpcService& ipc);
  void onOutputChange();
  void onToplevelChange();
  void show(wl_output* output);

  [[nodiscard]] bool isActive() const noexcept { return m_active; }
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);

private:
  struct Instance;

  void hide();
  void refreshWindows();
  void setSelectedIndex(std::size_t index);
  void cycleSelection(int delta);
  void navigateGrid(int colDelta, int rowDelta);
  void activateSelected();
  void closeWindowAt(std::size_t index);
  void requestSceneUpdate();
  [[nodiscard]] bool matchesTrigger(const KeyboardEvent& event) const noexcept;
  [[nodiscard]] bool isModifierRelease(const KeyboardEvent& event) const noexcept;
  void ensureSurface();
  void destroySurface();
  void prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& instance, std::uint32_t width, std::uint32_t height);
  void positionGrid(Instance& instance, float screenW, float screenH);
  void syncGridSelection();

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;

  Instance* m_instance = nullptr;
  std::vector<WindowSwitcherEntry> m_windows;
  std::size_t m_selectedIndex = 0;
  std::size_t m_gridColumns = 5;
  wl_output* m_output = nullptr;
  bool m_active = false;
};
