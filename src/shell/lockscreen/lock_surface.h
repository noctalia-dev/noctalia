#pragma once

#include "capture/screencopy_capture.h"
#include "config/config_service.h"
#include "render/animation/animation_manager.h"
#include "render/core/blur_cache.h"
#include "render/core/color.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "wayland/surface.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct ext_session_lock_surface_v1;
struct ext_session_lock_v1;
struct wl_output;

class Button;
class Box;
class Flex;
class Glyph;
class HttpClient;
class Image;
class Input;
class Label;
class MprisService;
class Renderer;
class SessionActionRunner;
class SharedTextureCache;
class WallpaperNode;
class WeatherService;
struct KeyboardEvent;
struct PointerEvent;
struct SessionPanelActionConfig;

class LockscreenWidgetsHost;

class LockSurface : public Surface {
public:
  explicit LockSurface(WaylandConnection& connection, ConfigService* config = nullptr);
  ~LockSurface() override;

  using Surface::initialize;
  bool initialize() override { return false; }
  bool initialize(ext_session_lock_v1* lock, wl_output* output, std::int32_t scale);
  void setLockedState(bool locked);
  void setPromptState(std::string user, std::string password, std::string status, bool error, bool authenticating);
  void setKeyboardIndicators(bool capsLock, bool hasMultipleLayouts, bool layoutSwitchable, std::string layoutLabel);
  void setTextureCache(SharedTextureCache* cache) noexcept { m_textureCache = cache; }
  void setWallpaperPath(std::string wallpaperPath);
  void setWallpaperFillMode(WallpaperFillMode fillMode);
  void setWallpaperFillColor(Color fillColor);
  void setDesktopCapture(std::optional<ScreencopyImage> capture);
  void setBackgroundStyle(float blurIntensity, float tintIntensity);
  void setBlackout(bool blackout);
  [[nodiscard]] bool isBlackout() const noexcept { return m_blackout; }
  void setOnLogin(std::function<void()> onLogin);
  void setOnCycleLayout(std::function<void()> onCycleLayout);
  void setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged);
  void setLoginBoxServices(
      SessionActionRunner* sessionActions, MprisService* mpris, const WeatherService* weather, HttpClient* httpClient
  );
  void selectAllPassword();
  void clearPasswordSelection();
  void onThemeChanged();
  void onGpuResourcesInvalidated();
  void prepareForGraphicsReset() noexcept;
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] wl_output* output() const noexcept { return m_output; }
  [[nodiscard]] bool hasDesktopCapture() const noexcept;
  [[nodiscard]] Node* widgetLayer() noexcept { return m_widgetLayer; }
  void setOutputKey(std::string outputKey) { m_outputKey = std::move(outputKey); }
  void setWidgetsHost(LockscreenWidgetsHost* host) noexcept { m_widgetsHost = host; }

  [[nodiscard]] bool firstFrameRendered() const noexcept { return m_firstFrameRendered; }
  void setRenderCallback(std::function<void()> callback) { m_renderCallback = std::move(callback); }

  static void handleConfigure(
      void* data, ext_session_lock_surface_v1* lockSurface, std::uint32_t serial, std::uint32_t width,
      std::uint32_t height
  );

protected:
  void render() override;

private:
  struct ForecastColumn {
    Flex* column = nullptr;
    Label* day = nullptr;
    Glyph* glyph = nullptr;
    Label* temps = nullptr;
  };

  void prepareFrame(bool needsUpdate, bool needsLayout);
  void applyWallpaperTexture();
  void applyBlurredDesktopTexture();
  void releaseWallpaperTextureRef(const std::string& path);
  void releaseCaptureTextures();
  void layoutScene(std::uint32_t width, std::uint32_t height);
  void updateCopy();
  void syncRegularExtras(Renderer& renderer);
  void rebuildSessionButtons();
  void ensureLayoutChipInPasswordRow();
  [[nodiscard]] std::vector<SessionPanelActionConfig> resolveSessionActions() const;
  [[nodiscard]] lockscreen_login_box::LoginBoxStyle resolveLoginStyle() const;
  [[nodiscard]] bool isLoginBoxEnabled() const;
  [[nodiscard]] std::string resolveStatusText(const lockscreen_login_box::LoginBoxStyle& style, bool& isError) const;
  [[nodiscard]] bool passwordFieldContainsPoint(float sceneX, float sceneY) const;
  void focusPasswordField();

  ext_session_lock_surface_v1* m_lockSurface = nullptr;
  wl_output* m_output = nullptr;
  ConfigService* m_config = nullptr;
  // Declared before m_root so it outlives the scene: ~Node cancels its animations through this manager.
  AnimationManager m_animations;
  Node m_root;
  Node* m_backgroundLayer = nullptr;
  Node* m_widgetLayer = nullptr;
  WallpaperNode* m_wallpaper = nullptr;
  Box* m_tintOverlay = nullptr;
  Box* m_backdrop = nullptr;
  Flex* m_loginPanel = nullptr;
  Flex* m_infoRow = nullptr;
  Flex* m_mediaBlock = nullptr;
  Image* m_mediaArt = nullptr;
  Glyph* m_mediaFallbackGlyph = nullptr;
  Flex* m_mediaTextColumn = nullptr;
  Label* m_mediaTitle = nullptr;
  Label* m_mediaArtist = nullptr;
  Flex* m_weatherBlock = nullptr;
  Flex* m_weatherCurrent = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Flex* m_weatherTextColumn = nullptr;
  Label* m_weatherTemp = nullptr;
  Label* m_weatherMeta = nullptr;
  Flex* m_forecastRow = nullptr;
  std::array<ForecastColumn, 5> m_forecastColumns{};
  Flex* m_authPanel = nullptr;
  Label* m_authLabel = nullptr;
  Flex* m_loginContentRow = nullptr;
  Input* m_passwordField = nullptr;
  Button* m_loginButton = nullptr;
  Button* m_layoutChip = nullptr;
  Flex* m_sessionRow = nullptr;
  std::vector<Button*> m_sessionButtons;
  SharedTextureCache* m_textureCache = nullptr;
  TextureHandle m_wallpaperTexture{};
  TextureHandle m_blurredWallpaperTexture{};
  TextureHandle m_captureSourceTexture{};
  TextureHandle m_blurredDesktopTexture{};
  BlurCache m_blurCache;
  BlurCache m_wallpaperBlurCache;
  std::optional<ScreencopyImage> m_desktopCapture;
  float m_blurIntensity = 0.5f;
  float m_tintIntensity = 0.3f;
  float m_regularContentScale = 1.0f;
  bool m_blackout = false;
  bool m_captureDirty = true;
  std::string m_wallpaperPath;
  std::string m_textureWallpaperPath;
  WallpaperFillMode m_wallpaperFillMode = WallpaperFillMode::Crop;
  Color m_wallpaperFillColor = rgba(0.0f, 0.0f, 0.0f, 0.0f);
  bool m_wallpaperDirty = false;
  InputDispatcher m_inputDispatcher;
  std::function<void()> m_onLogin;
  std::function<void()> m_onCycleLayout;
  std::function<void(const std::string&)> m_onPasswordChanged;
  bool m_locked = false;
  std::string m_user;
  std::string m_password;
  std::string m_status;
  bool m_error = false;
  bool m_authenticating = false;
  bool m_capsLock = false;
  bool m_hasMultipleLayouts = false;
  bool m_layoutSwitchable = false;
  std::string m_layoutLabel;
  std::string m_outputKey;
  LockscreenWidgetsHost* m_widgetsHost = nullptr;
  bool m_firstFrameRendered = false;
  std::function<void()> m_renderCallback;

  SessionActionRunner* m_sessionActions = nullptr;
  MprisService* m_mpris = nullptr;
  const WeatherService* m_weather = nullptr;
  HttpClient* m_httpClient = nullptr;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  std::string m_lastArtUrl;
  std::string m_lastMediaTitle;
  std::string m_lastMediaArtist;
  std::string m_lastWeatherFingerprint;
  std::vector<std::string> m_lastSessionActionKeys;
};
