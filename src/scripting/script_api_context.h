#pragma once

#include "core/toml.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Pointer-only use; keeps the scripting layer off the system layer's header.
class SystemMonitorService;

namespace scripting {

  // A connected output as exposed to plugin scripts via noctalia.outputs().
  struct ScriptOutputInfo {
    std::string name; // DRM connector name, e.g. "DP-1"
    std::string description;
    int width = 0; // logical size
    int height = 0;
    int x = 0; // position in logical output space
    int y = 0;
    int scale = 1;
    bool focused = false;
  };

  class ScriptApiContext {
  public:
    using LoadSoundHook =
        std::function<std::optional<std::string>(std::uint64_t, const std::string&, const std::string&)>;
    using PlaySoundHook = std::function<void(std::uint64_t, const std::string&)>;
    using UnloadPluginSoundsHook = std::function<void(std::uint64_t)>;
    using SetWallpaperMaskHook =
        std::function<void(std::uint64_t, const std::string&, const std::string&, const std::string&)>;
    using ClearWallpaperMasksHook = std::function<void(std::uint64_t)>;

    [[nodiscard]] bool isDarkMode() const noexcept { return m_darkMode.load(std::memory_order_relaxed); }
    void setDarkMode(bool dark) noexcept { m_darkMode.store(dark, std::memory_order_relaxed); }

    void setWallpaperDirectory(std::string directory) {
      std::scoped_lock lock(m_mutex);
      m_wallpaperDirectory = std::move(directory);
    }

    [[nodiscard]] std::string wallpaperDirectory() const {
      std::scoped_lock lock(m_mutex);
      return m_wallpaperDirectory;
    }

    void setWallpaperPaths(std::unordered_map<std::string, std::string> paths) {
      std::scoped_lock lock(m_mutex);
      m_wallpaperPaths = std::move(paths);
    }

    [[nodiscard]] std::optional<std::string> wallpaperPath(const std::string& outputName) const {
      std::scoped_lock lock(m_mutex);
      const auto it = m_wallpaperPaths.find(outputName);
      return it != m_wallpaperPaths.end() ? std::optional<std::string>{it->second} : std::nullopt;
    }

    // Output snapshot — refreshed on the main thread, copied out under lock so that
    // script bindings (which run on a worker thread) read it race-free.
    void setOutputs(std::vector<ScriptOutputInfo> outputs) {
      std::scoped_lock lock(m_mutex);
      m_outputs = std::move(outputs);
    }

    [[nodiscard]] std::vector<ScriptOutputInfo> outputs() const {
      std::scoped_lock lock(m_mutex);
      return m_outputs;
    }

    // Effective shell config, serialized via config_export::serialize — published on the
    // main thread on every config (re)load, read from script worker threads. The pointed-to
    // table is immutable; bindings hold the shared_ptr for the duration of one lookup.
    void setConfigSnapshot(std::shared_ptr<const toml::table> snapshot) {
      std::scoped_lock lock(m_mutex);
      m_configSnapshot = std::move(snapshot);
    }

    [[nodiscard]] std::shared_ptr<const toml::table> configSnapshot() const {
      std::scoped_lock lock(m_mutex);
      return m_configSnapshot;
    }

    // Latest text clipboard content — mirrored from ClipboardService on the
    // main thread so script bindings read it synchronously and race-free.
    void setClipboardText(std::optional<std::string> text) {
      std::scoped_lock lock(m_mutex);
      m_clipboardText = std::move(text);
    }

    [[nodiscard]] std::optional<std::string> clipboardText() const {
      std::scoped_lock lock(m_mutex);
      return m_clipboardText;
    }

    // Shell [shell].time_format / date_format — mirrored for noctalia.timeFormat() / dateFormat().
    void setTimeFormat(std::string format) {
      std::scoped_lock lock(m_mutex);
      m_timeFormat = std::move(format);
    }

    [[nodiscard]] std::string timeFormat() const {
      std::scoped_lock lock(m_mutex);
      return m_timeFormat;
    }

    void setDateFormat(std::string format) {
      std::scoped_lock lock(m_mutex);
      m_dateFormat = std::move(format);
    }

    [[nodiscard]] std::string dateFormat() const {
      std::scoped_lock lock(m_mutex);
      return m_dateFormat;
    }

    // Toggles the host wallpaper surface on an output. Wired to Wallpaper in Application;
    // invoked only on the main thread (from dispatchSideEffects).
    void setWallpaperEnabledHook(std::function<void(const std::string&, bool)> hook) {
      m_wallpaperEnabledHook = std::move(hook);
    }

    void invokeWallpaperEnabled(const std::string& connector, bool enabled) const {
      if (m_wallpaperEnabledHook) {
        m_wallpaperEnabledHook(connector, enabled);
      }
    }

    // Applies and persists a wallpaper image. Empty connector targets all outputs.
    // Wired to Wallpaper in Application; invoked only on the main thread.
    void setWallpaperHook(std::function<void(const std::string&, const std::string&)> hook) {
      m_wallpaperHook = std::move(hook);
    }

    void invokeSetWallpaper(const std::string& connector, const std::string& path) const {
      if (m_wallpaperHook) {
        m_wallpaperHook(connector, path);
      }
    }

    void setWallpaperMaskHook(SetWallpaperMaskHook hook) { m_wallpaperMaskHook = std::move(hook); }

    void invokeSetWallpaperMask(
        std::uint64_t ownerId, const std::string& outputName, const std::string& path, const std::string& wallpaperPath
    ) const {
      if (m_wallpaperMaskHook) {
        m_wallpaperMaskHook(ownerId, outputName, path, wallpaperPath);
      }
    }

    void setClearWallpaperMasksHook(ClearWallpaperMasksHook hook) { m_clearWallpaperMasksHook = std::move(hook); }

    [[nodiscard]] ClearWallpaperMasksHook clearWallpaperMasksHook() const { return m_clearWallpaperMasksHook; }

    // The live system monitor, or nullptr when it is unavailable. Unlike the hooks around it this
    // is read straight from a script worker thread: SystemMonitorService::latest() is mutex-guarded
    // and returns a copy, so no main-thread marshalling is needed. The pointer itself is atomic
    // because it is published, and cleared on teardown, from the main thread.
    void setSystemMonitor(SystemMonitorService* monitor) noexcept {
      m_systemMonitor.store(monitor, std::memory_order_release);
    }

    [[nodiscard]] SystemMonitorService* systemMonitor() const noexcept {
      return m_systemMonitor.load(std::memory_order_acquire);
    }

    // Toggles a host panel by id. Wired to PanelManager in Application; main thread only.
    void setTogglePanelHook(std::function<void(const std::string&)> hook) { m_togglePanelHook = std::move(hook); }

    void invokeTogglePanel(const std::string& panelId) const {
      if (m_togglePanelHook) {
        m_togglePanelHook(panelId);
      }
    }

    // Opens the settings window at a plugin's settings. Wired to PanelManager in Application;
    // main thread only.
    void setOpenPluginSettingsHook(std::function<void(const std::string&)> hook) {
      m_openPluginSettingsHook = std::move(hook);
    }

    void invokeOpenPluginSettings(const std::string& pluginId) const {
      if (m_openPluginSettingsHook) {
        m_openPluginSettingsHook(pluginId);
      }
    }

    void setLoadSoundHook(LoadSoundHook hook) { m_loadSoundHook = std::move(hook); }

    [[nodiscard]] std::optional<std::string>
    invokeLoadSound(std::uint64_t ownerId, const std::string& name, const std::string& path) const {
      if (!m_loadSoundHook) {
        return "sound playback is unavailable";
      }
      return m_loadSoundHook(ownerId, name, path);
    }

    void setPlaySoundHook(PlaySoundHook hook) { m_playSoundHook = std::move(hook); }

    void invokePlaySound(std::uint64_t ownerId, const std::string& name) const {
      if (m_playSoundHook) {
        m_playSoundHook(ownerId, name);
      }
    }

    void setUnloadPluginSoundsHook(UnloadPluginSoundsHook hook) { m_unloadPluginSoundsHook = std::move(hook); }

    [[nodiscard]] UnloadPluginSoundsHook unloadPluginSoundsHook() const { return m_unloadPluginSoundsHook; }

  private:
    std::atomic<SystemMonitorService*> m_systemMonitor{nullptr};
    std::atomic<bool> m_darkMode{true};
    mutable std::mutex m_mutex;
    std::shared_ptr<const toml::table> m_configSnapshot;
    std::string m_wallpaperDirectory;
    std::string m_timeFormat;
    std::string m_dateFormat;
    std::vector<ScriptOutputInfo> m_outputs;
    std::unordered_map<std::string, std::string> m_wallpaperPaths;
    std::optional<std::string> m_clipboardText;
    std::function<void(const std::string&, bool)> m_wallpaperEnabledHook;
    std::function<void(const std::string&, const std::string&)> m_wallpaperHook;
    SetWallpaperMaskHook m_wallpaperMaskHook;
    ClearWallpaperMasksHook m_clearWallpaperMasksHook;
    std::function<void(const std::string&)> m_togglePanelHook;
    std::function<void(const std::string&)> m_openPluginSettingsHook;
    LoadSoundHook m_loadSoundHook;
    PlaySoundHook m_playSoundHook;
    UnloadPluginSoundsHook m_unloadPluginSoundsHook;
  };

} // namespace scripting
