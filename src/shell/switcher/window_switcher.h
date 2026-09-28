#pragma once

#include "shell/switcher/window_switcher_tile.h"
#include "system/icon_resolver.h"
#include "wayland/wayland_seat.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AsyncTextureCache;
class CompositorPlatform;
class ConfigService;
class IpcService;
class RenderContext;
class ToplevelThumbnailCapture;
class WaylandConnection;
struct wl_output;

// Fullscreen keyboard-driven switcher with selectable preview presentations.
class WindowSwitcher {
public:
  WindowSwitcher();
  ~WindowSwitcher();

  void initialize(
      WaylandConnection& wayland, RenderContext* renderContext, CompositorPlatform& platform, ConfigService* config,
      AsyncTextureCache* asyncTextures
  );
  void registerIpc(IpcService& ipc);
  void onConfigReload();
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
  void navigateList(int delta);
  void activateSelected();
  void closeWindowAt(std::size_t index);
  void requestSceneUpdate();
  void startThumbnailCaptures();
  void captureNextThumbnail();
  void cancelThumbnailCaptures();
  [[nodiscard]] bool matchesTrigger(const KeyboardEvent& event) const noexcept;
  [[nodiscard]] bool isModifierRelease(const KeyboardEvent& event) const noexcept;
  void ensureSurface();
  void destroySurface();
  void prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& instance, std::uint32_t width, std::uint32_t height);
  void positionPanel(Instance& instance, float screenW, float screenH);
  void syncSelection(bool animate);
  void prioritizeSelectedThumbnail();
  [[nodiscard]] bool mruEnabled() const;
  void recordFocusedWindow();
  void promoteMruKey(const std::string& key);
  void showFromShortcut(wl_output* output, std::uint32_t modifiers);
  void showWithDirection(wl_output* output, int direction);
  void captureShortcutModifiers(std::uint32_t modifiers);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;

  Instance* m_instance = nullptr;
  IconResolver m_iconResolver;
  std::vector<WindowSwitcherEntry> m_windows;
  struct ThumbnailRequest {
    std::string windowKey;
    std::uintptr_t captureHandle = 0;
  };
  std::unique_ptr<ToplevelThumbnailCapture> m_thumbnailCapture;
  std::deque<ThumbnailRequest> m_thumbnailQueue;
  std::deque<std::string> m_mruKeys;
  std::size_t m_selectedIndex = 0;
  wl_output* m_output = nullptr;
  std::uint32_t m_shortcutModifiers = 0;
  bool m_active = false;
  bool m_shortcutSession = false;
};
