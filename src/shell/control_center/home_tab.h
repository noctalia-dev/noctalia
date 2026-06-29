#pragma once

#include "core/timer_manager.h"
#include "render/core/thumbnail_service.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/control_center/tab.h"
#include "ui/signal.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class AccountsService;
class Button;
class Box;
class CompositorPlatform;
class HttpClient;
class IpcService;
class ConfigService;
class DependencyService;
class Glyph;
class GridView;
class Image;
class InputArea;
class Label;
class Shortcut;
class Wallpaper;
class ClipboardService;
namespace scripting {
  class ScriptApiContext;
}

struct ShortcutPad {
  std::unique_ptr<Shortcut> shortcut;
  Button* button = nullptr;
  Glyph* glyph = nullptr;
  Label* label = nullptr;
};

class HomeTab : public Tab {
public:
  explicit HomeTab(const ControlCenterServices& services);
  ~HomeTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void layoutWallpaperBackground(Renderer& renderer);
  // Adds a card overlay for pointer and/or keyboard activation.
  struct CardOverlayOptions {
    bool keyboardFocus = true;
    bool pointerHitTest = true;
  };
  InputArea* addCardOverlay(Flex& card, std::function<void()> onActivate);
  InputArea* addCardOverlay(Flex& card, std::function<void()> onActivate, CardOverlayOptions options);
  void layoutCardOverlays();
  void syncWallpaperBackground(Renderer& renderer);
  void ensureWallpaperThumbnail(const std::string& path, int targetPx);
  void startCrispFade();
  void cancelCrispFade();
  void sync(Renderer& renderer);
  void syncScaledFonts();
  void syncShortcuts();
  bool resizeMediaArtToCard();
  void onPanelCardOpacityChanged(float opacity) override;

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  WeatherService* m_weather = nullptr;
  ConfigService* m_config = nullptr;
  AccountsService* m_accounts = nullptr;
  Wallpaper* m_wallpaper = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  ShortcutServices m_services;
  bool m_active = false;

  Flex* m_rootLayout = nullptr;
  Flex* m_bottomRow = nullptr;
  Flex* m_dateTimeCard = nullptr;
  Flex* m_mediaCard = nullptr;
  Flex* m_mediaText = nullptr;
  Flex* m_userCard = nullptr;
  Flex* m_userMain = nullptr;
  InputArea* m_userAvatarArea = nullptr;
  Image* m_userAvatar = nullptr;

  Label* m_timeLabel = nullptr;
  Label* m_dateLabel = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Label* m_weatherLine = nullptr;
  Label* m_userHost = nullptr;
  Label* m_userUptime = nullptr;
  Label* m_userVersion = nullptr;
  Button* m_settingsButton = nullptr;
  Button* m_sessionButton = nullptr;
  InputArea* m_userCardKeyboardArea = nullptr;
  InputArea* m_userCardArea = nullptr;
  InputArea* m_mediaCardArea = nullptr;
  InputArea* m_dateTimeCardArea = nullptr;
  std::string m_loadedAvatarPath;
  int m_loadedAvatarSize = 0;

  // Two stacked layers: m_wallpaperPlaceholder shows the resident full-screen
  // wallpaper texture immediately (slightly soft), m_wallpaperBg holds the crisp
  // card-sized thumbnail and crossfades in over it once decoded.
  Image* m_wallpaperPlaceholder = nullptr;
  Image* m_wallpaperBg = nullptr;
  Box* m_wallpaperGradient = nullptr;
  std::string m_loadedWallpaperPath;
  int m_loadedWallpaperSize = 0;
  std::string m_crispWorkingPath;
  int m_crispWorkingSize = 0;
  bool m_crispShown = false;
  bool m_crispNeedsFade = false;
  std::uint32_t m_wallpaperCrispAnimId = 0;
  ThumbnailService::Subscription m_thumbnailPendingSub;
  Signal<>::ScopedConnection m_wallpaperChangedConn;

  Label* m_mediaTrack = nullptr;
  Label* m_mediaArtist = nullptr;
  Label* m_mediaStatus = nullptr;
  Label* m_mediaProgress = nullptr;
  Flex* m_mediaArtSlot = nullptr;
  Glyph* m_mediaArtFallback = nullptr;
  Image* m_mediaArt = nullptr;
  std::string m_loadedMediaArtUrl;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  std::string m_mediaPositionBusName;
  std::string m_mediaPositionTrackId;
  std::string m_mediaPositionTrackSignature;
  std::string m_mediaLastPlaybackStatus;
  std::int64_t m_mediaPositionUs = 0;
  std::chrono::steady_clock::time_point m_mediaPositionSampleAt;
  std::chrono::steady_clock::time_point m_nextRealtimeUpdateAt;
  std::chrono::steady_clock::time_point m_lastRealtimeMprisPollAt;
  Timer m_progressTimer;

  GridView* m_shortcutsGrid = nullptr;
  std::vector<ShortcutPad> m_shortcutPads;
};
