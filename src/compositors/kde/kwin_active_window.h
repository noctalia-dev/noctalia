#pragma once

#include "compositors/workspace_backend.h"
#include "wayland/wayland_toplevels.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class SessionBus;

namespace sdbus {
  class IObject;
}

namespace compositors::kde {

  // Tracks windows on KDE Plasma via a KWin script that forwards focus and window-list
  // events over D-Bus. KWin does not expose wlr/ext foreign toplevel protocols to us.
  class KwinActiveWindow {
  public:
    using ChangeCallback = std::function<void()>;

    explicit KwinActiveWindow(SessionBus& bus);
    ~KwinActiveWindow();

    KwinActiveWindow(const KwinActiveWindow&) = delete;
    KwinActiveWindow& operator=(const KwinActiveWindow&) = delete;

    void start();
    void stop();
    void setChangeCallback(ChangeCallback callback);

    [[nodiscard]] std::optional<ActiveToplevel> current() const;
    [[nodiscard]] std::vector<std::string> runningAppIds() const;
    [[nodiscard]] std::vector<ToplevelInfo>
    windowsForApp(const std::string& idLower, const std::string& wmClassLower) const;
    [[nodiscard]] std::vector<WorkspaceWindow> trackedWorkspaceWindows() const;
    [[nodiscard]] std::optional<std::string> focusedOutputName() const;
    void activateWindow(const std::string& title, const std::string& appId, const std::string& uuid = {});
    void closeWindow(const std::string& title, const std::string& appId, const std::string& uuid = {});
    [[nodiscard]] bool isAvailable() const noexcept { return m_scriptInstalled; }

  private:
    struct TrackedWindow {
      std::string uuid;
      std::string appId;
      std::string title;
      std::string outputName;
      std::vector<std::string> desktopIds;
    };

    [[nodiscard]] bool ensureScriptFile(std::string& scriptPath) const;
    [[nodiscard]] bool installScript(const std::string& scriptPath);
    void uninstallScript();
    [[nodiscard]] bool runTransientScript(const std::string& source, std::string_view label);
    void onNotifyActiveWindow(const std::string& caption, const std::string& resourceClass, const std::string& uuid);
    void onNotifyWindowList(const std::string& payload);
    void notifyChanged() const;

    SessionBus& m_bus;
    std::unique_ptr<sdbus::IObject> m_object;
    ChangeCallback m_changeCallback;
    std::optional<ActiveToplevel> m_current;
    std::vector<TrackedWindow> m_trackedWindows;
    bool m_scriptInstalled = false;
    bool m_started = false;
    std::uint32_t m_transientScriptSerial = 0;
  };

} // namespace compositors::kde
