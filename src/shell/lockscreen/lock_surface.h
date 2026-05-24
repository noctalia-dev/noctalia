#pragma once

#include "config/config_service.h"
#include "render/core/color.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct ext_session_lock_surface_v1;
struct ext_session_lock_v1;
struct wl_output;

class Button;
class Box;
class Input;
class Label;
class SharedTextureCache;
class WallpaperNode;
struct KeyboardEvent;
struct PointerEvent;

class LockSurface : public Surface {
public:
  explicit LockSurface(WaylandConnection& connection, ConfigService* config = nullptr);
  ~LockSurface() override;

  using Surface::initialize;
  bool initialize() override { return false; }
  bool initialize(ext_session_lock_v1* lock, wl_output* output, std::int32_t scale);
  void setLockedState(bool locked);
  void setPromptState(std::string user, std::string password, std::string status, bool error);
  void setTextureCache(SharedTextureCache* cache) noexcept { m_textureCache = cache; }
  void setWallpaperPath(std::string wallpaperPath);
  void setWallpaperFillMode(WallpaperFillMode fillMode);
  void setWallpaperFillColor(Color fillColor);
  void setOnLogin(std::function<void()> onLogin);
  void setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged);
  void selectAllPassword();
  void clearPasswordSelection();
  void onSecondTick();
  void onThemeChanged();
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] wl_output* output() const noexcept { return m_output; }

  static void handleConfigure(
      void* data, ext_session_lock_surface_v1* lockSurface, std::uint32_t serial, std::uint32_t width,
      std::uint32_t height
  );

private:
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void applyWallpaperTexture();
  void updateClockText();
  void layoutScene(std::uint32_t width, std::uint32_t height);
  void updateCopy();

  ext_session_lock_surface_v1* m_lockSurface = nullptr;
  wl_output* m_output = nullptr;
  ConfigService* m_config = nullptr;
  Node m_root;
  WallpaperNode* m_wallpaper = nullptr;
  Box* m_backdrop = nullptr;
  Label* m_clockShadow = nullptr;
  Label* m_clock = nullptr;
  Box* m_loginPanel = nullptr;
  Input* m_passwordField = nullptr;
  Button* m_loginButton = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  TextureHandle m_wallpaperTexture{};
  std::string m_wallpaperPath;
  WallpaperFillMode m_wallpaperFillMode = WallpaperFillMode::Crop;
  Color m_wallpaperFillColor = rgba(0.0f, 0.0f, 0.0f, 0.0f);
  bool m_wallpaperDirty = false;
  InputDispatcher m_inputDispatcher;
  std::function<void()> m_onLogin;
  std::function<void(const std::string&)> m_onPasswordChanged;
  bool m_locked = false;
  std::string m_user;
  std::string m_password;
  std::string m_status;
  bool m_error = false;
  bool m_clockShadowEnabled = true;
};
