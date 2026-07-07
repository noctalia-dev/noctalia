#pragma once

#include "config/config_types.h"
#include "ui/signal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class ConfigService;
class IpcService;
class RenderContext;
class SharedTextureCache;
class WaylandConnection;
enum class WallpaperTransitionDirection;
struct TextureHandle;
struct WallpaperInstance;
struct PointerEvent;
struct WaylandOutput;
struct wl_surface;

struct WallpaperChange {
  std::string path;
  std::string connector;
};

class Wallpaper {
public:
  Wallpaper();
  ~Wallpaper();

  bool initialize(
      WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, SharedTextureCache* textureCache
  );
  void onOutputChange();
  // Mark an output as driven by an external wallpaper source (e.g. an mpvpaper plugin):
  // its Background surface is torn down so the external surface shows through. Runtime-only
  // (not persisted) — it clears on restart and is re-asserted by the owner.
  void setOutputExternallyManaged(const std::string& connector, bool managed);
  [[nodiscard]] std::vector<WallpaperChange> onStateChange();
  void onSecondTick();
  void onGpuResourcesInvalidated();
  void registerIpc(IpcService& ipc);
  // Apply and persist a wallpaper image. nullopt connector targets all connected
  // outputs plus the default. Returns false if the path does not exist or the
  // connector is unknown. Shared by the wallpaper-set IPC handler and plugin scripts.
  bool applyWallpaperImage(const std::optional<std::string>& connector, const std::string& path);
  void setAutomationGate(std::function<bool()> gate);
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  bool onPointerEvent(const PointerEvent& event);

  [[nodiscard]] TextureHandle currentTexture() const;
  [[nodiscard]] std::string currentPath() const;

  // Emits whenever the displayed wallpaper (path/texture) changes, including
  // automation rotation and transition completion. Lets consumers pre-warm
  // previews while their UI is closed.
  [[nodiscard]] Signal<>& changed() noexcept { return m_changed; }

private:
  enum class TransitionRedirect {
    Unrelated,
    AlreadyTargeting,
    Redirected,
  };

  enum class PickWallpaper {
    Random,
    Previous,
    Next,
  };

  [[nodiscard]] bool isConnectorKnown(std::string_view connector) const;
  // Persist a resolved image path to a single connector, or to every connected
  // output plus the default when no connector is given.
  void applyResolvedWallpaper(const std::optional<std::string>& connector, const std::string& resolvedPath);
  void reload();
  void syncInstances();
  void applyStartupAutomation(std::int64_t secondStamp);
  void resetAutomationState();
  void runAutomation(std::int64_t secondStamp);
  [[nodiscard]] bool automationAllowed() const noexcept;
  [[nodiscard]] bool switchWallpaperTo(PickWallpaper action, std::optional<std::string_view> connector = std::nullopt);
  void createInstance(const WaylandOutput& output);
  [[nodiscard]] TextureHandle acquireTexture(const std::string& path);
  void releaseTexture(TextureHandle& handle, const std::string& path);
  void loadWallpaper(WallpaperInstance& instance, const std::string& path);
  TransitionRedirect redirectActiveTransition(WallpaperInstance& instance, const std::string& path);
  void startTransition(WallpaperInstance& instance);
  void startTransitionAnimation(WallpaperInstance& instance, float fromTime, WallpaperTransitionDirection direction);
  void finishTransition(WallpaperInstance& instance);
  void promotePendingWallpaper(WallpaperInstance& instance);
  void discardPendingWallpaper(WallpaperInstance& instance);
  void runQueuedWallpaper(WallpaperInstance& instance);
  void updateRendererState(WallpaperInstance& instance);
  void releaseInstanceTextures(WallpaperInstance& inst);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  bool m_wallpaperEnabled = false;
  WallpaperConfig m_lastWallpaperConfig{};
  std::int64_t m_lastAutomationSecondStamp = -1;
  std::int64_t m_lastAutomationSwitchSecond = -1;
  std::function<bool()> m_automationGate;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<> m_changed;
  std::vector<std::unique_ptr<WallpaperInstance>> m_instances;
  std::unordered_set<std::string> m_externallyManagedOutputs;
};
