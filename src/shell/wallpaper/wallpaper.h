#pragma once

#include "shell/wallpaper/wallpaper_instance.h"
#include "ui/signal.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ConfigService;
class IpcService;
class RenderContext;
class SharedTextureCache;
class WaylandConnection;
struct WaylandOutput;

class Wallpaper {
public:
  Wallpaper();
  ~Wallpaper();

  bool initialize(
      WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, SharedTextureCache* textureCache
  );
  void onOutputChange();
  void onStateChange();
  void onSecondTick();
  void registerIpc(IpcService& ipc);

  [[nodiscard]] TextureHandle currentTexture() const;

private:
  void reload();
  void syncInstances();
  void resetAutomationState();
  void runAutomation(std::int64_t minuteStamp);
  [[nodiscard]] bool switchToRandomWallpaper(std::optional<std::string_view> connector = std::nullopt);
  void createInstance(const WaylandOutput& output);
  void loadWallpaper(WallpaperInstance& instance, const std::string& path);
  void startTransition(WallpaperInstance& instance);
  void updateRendererState(WallpaperInstance& instance);
  void releaseInstanceTextures(WallpaperInstance& inst);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  bool m_wallpaperEnabled = false;
  std::int64_t m_lastAutomationMinuteStamp = -1;
  std::int64_t m_lastAutomationSwitchMinute = -1;
  Signal<>::ScopedConnection m_paletteConn;
  std::vector<std::unique_ptr<WallpaperInstance>> m_instances;
};
