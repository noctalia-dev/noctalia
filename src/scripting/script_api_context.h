#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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

    // Toggles a host panel by id. Wired to PanelManager in Application; main thread only.
    void setTogglePanelHook(std::function<void(const std::string&)> hook) { m_togglePanelHook = std::move(hook); }

    void invokeTogglePanel(const std::string& panelId) const {
      if (m_togglePanelHook) {
        m_togglePanelHook(panelId);
      }
    }

  private:
    std::atomic<bool> m_darkMode{true};
    mutable std::mutex m_mutex;
    std::string m_wallpaperDirectory;
    std::vector<ScriptOutputInfo> m_outputs;
    std::function<void(const std::string&, bool)> m_wallpaperEnabledHook;
    std::function<void(const std::string&, const std::string&)> m_wallpaperHook;
    std::function<void(const std::string&)> m_togglePanelHook;
  };

} // namespace scripting
