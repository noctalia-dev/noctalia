#pragma once

#include "core/timer_manager.h"
#include "shell/control_center/shortcut_services.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class Button;
class Box;
class CompositorPlatform;
class HttpClient;
class ConfigService;
class DependencyService;
class Glyph;
class GridView;
class Image;
class Label;
class Shortcut;
class Wallpaper;

struct ShortcutPad {
  std::unique_ptr<Shortcut> shortcut;
  Button* button = nullptr;
  Glyph* glyph = nullptr;
  Label* label = nullptr;
};

class HomeTab : public Tab {
public:
  HomeTab(
      MprisService* mpris, HttpClient* httpClient, WeatherService* weather, PipeWireService* audio,
      PowerProfilesService* powerProfiles, ConfigService* config, INetworkService* network, BluetoothService* bluetooth,
      GammaService* nightLight, noctalia::theme::ThemeService* theme, NotificationManager* notifications,
      IdleInhibitor* idleInhibitor, DependencyService* dependencies, CompositorPlatform* platform,
      Wallpaper* wallpaper = nullptr
  );
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
  void layoutCardButton(Renderer& renderer, Flex* card, Button* button);
  void syncWallpaperBackground(Renderer& renderer);
  void sync(Renderer& renderer);
  void syncScaledFonts();
  void syncShortcuts();
  bool resizeMediaArtToCard();
  void onPanelCardOpacityChanged(float opacity) override;

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  WeatherService* m_weather = nullptr;
  ConfigService* m_config = nullptr;
  Wallpaper* m_wallpaper = nullptr;
  ShortcutServices m_services;
  bool m_active = false;

  Flex* m_rootLayout = nullptr;
  Flex* m_bottomRow = nullptr;
  Flex* m_dateTimeCard = nullptr;
  Flex* m_mediaCard = nullptr;
  Flex* m_mediaText = nullptr;
  Flex* m_userCard = nullptr;
  Flex* m_userMain = nullptr;
  Image* m_userAvatar = nullptr;

  Label* m_timeLabel = nullptr;
  Label* m_dateLabel = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Label* m_weatherLine = nullptr;
  Label* m_userFacts = nullptr;
  Button* m_settingsButton = nullptr;
  Button* m_sessionButton = nullptr;
  Button* m_wallpaperButton = nullptr;
  Button* m_mediaButton = nullptr;
  Button* m_weatherButton = nullptr;
  std::string m_loadedAvatarPath;

  Image* m_wallpaperBg = nullptr;
  Box* m_wallpaperGradient = nullptr;

  Label* m_mediaTrack = nullptr;
  Label* m_mediaArtist = nullptr;
  Label* m_mediaStatus = nullptr;
  Label* m_mediaProgress = nullptr;
  Flex* m_mediaArtSlot = nullptr;
  Glyph* m_mediaArtFallback = nullptr;
  Image* m_mediaArt = nullptr;
  std::string m_loadedMediaArtUrl;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::string m_mediaPositionBusName;
  std::string m_mediaPositionTrackId;
  std::string m_mediaPositionTrackSignature;
  std::string m_mediaLastPlaybackStatus;
  std::int64_t m_mediaPositionUs = 0;
  std::chrono::steady_clock::time_point m_mediaPositionSampleAt{};
  std::chrono::steady_clock::time_point m_nextRealtimeUpdateAt{};
  std::chrono::steady_clock::time_point m_lastRealtimeMprisPollAt{};
  Timer m_progressTimer;

  GridView* m_shortcutsGrid = nullptr;
  std::vector<ShortcutPad> m_shortcutPads;
};
