#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "theme/palette.h"
#include "theme/scheme.h"
#include "ui/palette.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

class ConfigService;
class HttpClient;
class IpcService;

namespace noctalia::theme {

  class ThemeService {
  public:
    using ChangeCallback = std::function<void()>;
    // Carries the resolved [theme].mode: the app-facing mode templates and the GTK color
    // scheme run in. Shell-facing consumers read resolvedShellMode()/isLightMode().
    using ResolvedCallback = std::function<void(const GeneratedPalette&, std::string_view)>;

    ThemeService(ConfigService& config, HttpClient& httpClient);
    ~ThemeService();

    ThemeService(const ThemeService&) = delete;
    ThemeService& operator=(const ThemeService&) = delete;

    // Snaps the palette to the resolved theme (no fade). Used at startup.
    void apply();

    // Resolves the target theme and cross-fades to it.
    void onConfigReload();
    void onWallpaperChange();
    void onAutoSchemeChanged();
    void setAutoCoordinates(std::optional<double> latitude, std::optional<double> longitude);
    void toggleLightDark();
    void cycleMode();
    [[nodiscard]] ThemeMode configuredMode() const noexcept;
    // Noctalia's own resolved mode ([theme].shell_mode, or [theme].mode when it follows).
    [[nodiscard]] bool isLightMode() const noexcept;
    [[nodiscard]] std::string_view resolvedShellMode() const noexcept;
    // The resolved [theme].mode, which drives apps.
    [[nodiscard]] std::string_view resolvedMode() const noexcept;

    void setChangeCallback(ChangeCallback callback);
    void setResolvedCallback(ResolvedCallback callback);

    void registerIpc(IpcService& ipc);

    // Writes the current wallpaper-generated palette to ~/.config/noctalia/palettes/
    // and switches palette source to custom.
    [[nodiscard]] bool saveWallpaperPaletteAsCustom(std::string* paletteNameOut, std::string* errorOut = nullptr);

  private:
    // Identifies one wallpaper palette generation. mtime makes an edited wallpaper at the
    // same path re-decode; 0 means the stat failed and the result must not be cached.
    struct WallpaperPaletteKey {
      std::string path;
      std::string schemeName;
      Scheme scheme = Scheme::Content;
      std::int64_t mtimeNs = 0;

      bool operator==(const WallpaperPaletteKey&) const = default;
    };

    struct WallpaperLookup {
      std::optional<GeneratedPalette> generated;
      // The palette is being generated off the main thread; keep the current one until it lands.
      bool pending = false;
    };

    void resolveAndSet(bool animate);
    [[nodiscard]] std::optional<WallpaperPaletteKey>
    wallpaperPaletteKey(const ThemeConfig& cfg, const std::string& wallpaperPath) const;
    [[nodiscard]] bool wallpaperCacheMatches(const WallpaperPaletteKey& key) const;
    void storeWallpaperCache(const WallpaperPaletteKey& key, const GeneratedPalette& generated);
    // Decodes on the calling thread; for startup and the explicit save action.
    std::optional<GeneratedPalette> resolveWallpaperGenerated(const ThemeConfig& cfg, const std::string& wallpaperPath);
    // Serves the memoized palette, or queues a background decode and reports it pending.
    WallpaperLookup lookupWallpaperGenerated(const ThemeConfig& cfg, const std::string& wallpaperPath);
    void requestWallpaperPalette(const WallpaperPaletteKey& key);
    void wallpaperWorkerLoop();
    void onWallpaperPaletteDecoded(const WallpaperPaletteKey& key, std::optional<GeneratedPalette> generated);
    void queueResolvedCallback(const GeneratedPalette& generated, std::string_view mode);
    void flushResolvedCallback(bool defer);
    void startTransition(const Palette& target);
    void finishTransition(bool deferResolvedCallback);
    void tickTransition();
    void startCommunityDownload(const std::string& name);
    void rescheduleAutoTimer();
    [[nodiscard]] bool hasAutoMode() const noexcept;

    ConfigService& m_config;
    HttpClient& m_httpClient;
    std::string m_inflightCommunityName;

    // Memoized wallpaper palette (see resolveWallpaperGenerated); any key mismatch re-decodes.
    std::optional<GeneratedPalette> m_wallpaperCacheGenerated;
    WallpaperPaletteKey m_wallpaperCacheKey;

    // Main thread: the decode whose result is still wanted, and the last key that failed so a
    // broken file falls back to the builtin palette instead of re-queueing forever.
    std::optional<WallpaperPaletteKey> m_wallpaperRequested;
    std::optional<WallpaperPaletteKey> m_wallpaperFailed;
    // Single decode worker; only the newest queued request survives.
    std::mutex m_wallpaperWorkerMutex;
    std::condition_variable m_wallpaperWorkerCv;
    std::optional<WallpaperPaletteKey> m_wallpaperQueued;
    bool m_wallpaperWorkerShutdown = false;
    std::thread m_wallpaperWorker;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    ChangeCallback m_changeCallback;
    ResolvedCallback m_resolvedCallback;
    // External template/hooks callbacks are delayed until the shell palette is
    // applied, and deferred callbacks must be able to drop stale resolves.
    std::optional<GeneratedPalette> m_pendingResolvedPalette;
    std::string m_pendingResolvedMode;
    std::uint64_t m_resolvedCallbackGeneration = 0;

    AnimationManager m_animations;
    Timer m_transitionTimer;
    Palette m_fromPalette{};
    Palette m_targetPalette{};
    AnimationManager::Id m_transitionAnimId = 0;
    bool m_transitionResolvedCallbackFlushed = false;
    bool m_isShellLightMode = false;
    bool m_isLightMode = false;
    std::optional<double> m_autoLatitude;
    std::optional<double> m_autoLongitude;
    Timer m_autoTimer;
  };

} // namespace noctalia::theme
