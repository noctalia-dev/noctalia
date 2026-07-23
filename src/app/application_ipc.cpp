#include "app/main_loop.h"
#include "application.h"
#include "application_internal.h"
#include "compositors/compositor_detect.h"
#include "config/config_types.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/logind/logind_service.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_manager_service.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/wpa_supplicant_service.h"
#include "dbus/notification/kde_notification_client.h"
#include "dbus/notification/notification_dbus_host.h"
#include "dbus/notification/notification_service.h"
#include "dbus/polkit/polkit_agent.h"
#include "dbus/polkit/polkit_poll_source.h"
#include "dbus/polkit/polkit_session_support.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus.h"
#include "dbus/system_bus_poll_source.h"
#include "dbus/tray/tray_service.h"
#include "dbus/upower/upower_service.h"
#include "debug/debug_service.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "ipc/ipc_arg_parse.h"
#include "launcher/app_provider.h"
#include "launcher/dmenu_provider.h"
#include "launcher/emoji_provider.h"
#include "launcher/math_provider.h"
#include "launcher/plugin_launcher_provider.h"
#include "launcher/session_provider.h"
#include "launcher/wallpaper_provider.h"
#include "launcher/window_provider.h"
#include "notification/notifications.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/sound_player.h"
#include "render/animation/motion_service.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/clipboard/clipboard_panel.h"
#include "shell/clipboard/clipboard_paste.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/panel/plugin_panel.h"
#include "shell/polkit/polkit_panel.h"
#include "shell/session/session_ipc.h"
#include "shell/session/session_panel.h"
#include "shell/setup_wizard/setup_wizard_panel.h"
#include "shell/test/test_panel.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/tray/tray_drawer_panel.h"
#include "shell/wallpaper/panel/wallpaper_panel.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/brightness_poll_source.h"
#include "system/brightness_service.h"
#include "system/distro_info.h"
#include "system/easyeffects_service.h"
#include "system/keyboard_backlight_service.h"
#include "system/system_monitor_service.h"
#include "ui/app_icon_colorization.h"
#include "ui/controls/input.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <malloc.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
  constexpr Logger kLog("app");
} // namespace

void Application::initIpc() {
  if (m_ipcService.start()) {
    kLog.info("IPC socket at {}", m_ipcService.socketPath());
  } else {
    kLog.warn("IPC disabled: could not bind socket");
  }

  m_dmenuIpc.setLauncherPanel(m_launcherPanel);
  m_dmenuIpc.setPanelManager(&m_panelManager);
  m_dmenuIpc.start();

  m_ipcService.registerHandler(
      "status",
      [this](const std::string&) -> std::string {
        const bool panelOpen = m_panelManager.isOpen();
        std::string json = "{\n";
        json += "  \"barVisible\": ";
        json += m_bar.isVisible() ? "true" : "false";
        json += ",\n  \"panelOpen\": ";
        json += panelOpen ? "true" : "false";
        json += ",\n  \"activePanelId\": ";
        json += panelOpen ? ("\"" + m_panelManager.activePanelId() + "\"") : "null";
        json += ",\n  \"locked\": ";
        json += m_lockScreen.isActive() ? "true" : "false";
        json += "\n}\n";
        return json;
      },
      "status", "Print current state as JSON"
  );

  m_ipcService.registerHandler(
      "log-level-set",
      [](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: log-level-set requires <debug|info|warn|error>\n";
        }

        const auto level = parseLogLevel(parts[0]);
        if (!level.has_value()) {
          return "error: invalid log level (use debug, info, warn, or error)\n";
        }

        setLogLevel(*level);
        kLog.info("log level set to {}", logLevelName(*level));
        return "ok\n";
      },
      "log-level-set <debug|info|warn|error>", "Set the console log level"
  );

  m_ipcService.registerHandler(
      "log-level-status",
      [](const std::string&) -> std::string { return std::string(logLevelName(currentLogLevel())) + "\n"; },
      "log-level-status", "Print the current console log level"
  );

  auto applyNotificationDnd = [this](bool enabled) {
    m_notificationManager.setDoNotDisturb(enabled);
    m_bar.refresh();
    if (m_panelManager.isOpenPanel("control-center")) {
      m_panelManager.refresh();
    }
  };

  m_ipcService.registerHandler(
      "notification-dnd-set",
      [this, applyNotificationDnd](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: notification-dnd-set requires <on|off|true|false|1|0>\n";
        }
        const std::string& value = parts[0];
        std::optional<bool> nextState;
        if (value == "on" || value == "true" || value == "1") {
          nextState = true;
        } else if (value == "off" || value == "false" || value == "0") {
          nextState = false;
        } else {
          return "error: invalid value (use on/off, true/false, 1/0)\n";
        }

        const bool currentState = m_notificationManager.doNotDisturb();
        if (currentState == *nextState) {
          return "ok\n";
        }
        applyNotificationDnd(*nextState);
        m_osdOverlay.show(dndOsdContent(*nextState));
        return "ok\n";
      },
      "notification-dnd-set <on|off|true|false|1|0>", "Set notification Do Not Disturb state"
  );

  m_ipcService.registerHandler(
      "notification-dnd-toggle",
      [this, applyNotificationDnd](const std::string&) -> std::string {
        const bool nextState = !m_notificationManager.doNotDisturb();
        applyNotificationDnd(nextState);
        m_osdOverlay.show(dndOsdContent(nextState));
        return "ok\n";
      },
      "notification-dnd-toggle", "Toggle notification Do Not Disturb state"
  );

  m_ipcService.registerHandler(
      "notification-dnd-status",
      [this](const std::string&) -> std::string { return m_notificationManager.doNotDisturb() ? "on\n" : "off\n"; },
      "notification-dnd-status", "Print notification Do Not Disturb state"
  );

  m_ipcService.registerHandler(
      "notification-clear-active",
      [this](const std::string&) -> std::string {
        std::vector<uint32_t> activeIds;
        activeIds.reserve(m_notificationManager.all().size());
        for (const auto& notification : m_notificationManager.all()) {
          activeIds.push_back(notification.id);
        }
        for (const uint32_t id : activeIds) {
          (void)m_notificationManager.close(id, CloseReason::Dismissed);
        }
        if (m_panelManager.isOpenPanel("control-center")) {
          m_panelManager.refresh();
        }
        return "ok\n";
      },
      "notification-clear-active", "Dismiss all currently active notifications"
  );

  m_ipcService.registerHandler(
      "notification-invoke-latest",
      [this](const std::string&) -> std::string {
        // Mirror the toast left-click behaviour for the most recent active notification:
        // invoke its "default" action so the source application raises/focuses its window.
        // all() stores notifications oldest-first (push_back), so iterate in reverse for newest.
        const auto& notifications = m_notificationManager.all();
        for (const auto& notification : std::views::reverse(notifications)) {
          const auto& actions = notification.actions; // pairs: [key, label, ...]; "default" must be first.
          if (actions.size() >= 2 && actions[0] == "default") {
            if (!m_notificationManager.invokeAction(notification.id, "default", true)) {
              return "error: invokeAction failed\n";
            }
            if (m_panelManager.isOpenPanel("control-center")) {
              m_panelManager.refresh();
            }
            return "ok\n";
          }
        }
        return "ok\n"; // No active notification carries a default action; nothing to do.
      },
      "notification-invoke-latest", "Invoke the default action of the most recent active notification"
  );

  m_ipcService.registerHandler(
      "notification-clear-history",
      [this](const std::string&) -> std::string {
        m_notificationManager.clearHistory();
        if (m_panelManager.isOpenPanel("control-center")) {
          m_panelManager.refresh();
        }
        return "ok\n";
      },
      "notification-clear-history", "Clear notification history"
  );

  m_ipcService.registerHandler(
      "notification-show",
      [this](const std::string& args) -> std::string {
        const std::string input = StringUtils::trim(args);
        if (input.empty()) {
          return "error: notification-show requires <summary> or <json-payload>\n";
        }

        std::string appName = "Noctalia";
        std::string summary;
        std::string body;
        Urgency urgency = Urgency::Normal;
        int32_t timeoutMs = kDefaultNotificationTimeout;
        std::optional<std::string> icon;
        std::optional<std::string> category;
        std::optional<std::string> desktopEntry;

        auto parseUrgency = [](const std::string& value, Urgency& outUrgency) -> bool {
          if (value == "low") {
            outUrgency = Urgency::Low;
            return true;
          }
          if (value == "normal") {
            outUrgency = Urgency::Normal;
            return true;
          }
          if (value == "critical") {
            outUrgency = Urgency::Critical;
            return true;
          }
          return false;
        };

        if (!input.empty() && input.front() == '{') {
          const nlohmann::json payload = nlohmann::json::parse(input, nullptr, false);
          if (payload.is_discarded() || !payload.is_object()) {
            return "error: notification-show JSON payload must be an object\n";
          }

          if (const auto it = payload.find("app_name"); it != payload.end()) {
            if (!it->is_string()) {
              return "error: notification-show field 'app_name' must be a string\n";
            }
            appName = it->get<std::string>();
          }

          if (const auto it = payload.find("summary"); it != payload.end()) {
            if (!it->is_string()) {
              return "error: notification-show field 'summary' must be a string\n";
            }
            summary = it->get<std::string>();
          }

          if (const auto it = payload.find("body"); it != payload.end()) {
            if (!it->is_string()) {
              return "error: notification-show field 'body' must be a string\n";
            }
            body = it->get<std::string>();
          }

          if (const auto it = payload.find("urgency"); it != payload.end()) {
            if (!it->is_string()) {
              return "error: notification-show field 'urgency' must be low, normal, or critical\n";
            }
            const std::string urgencyValue = StringUtils::toLower(it->get<std::string>());
            if (!parseUrgency(urgencyValue, urgency)) {
              return "error: notification-show field 'urgency' must be low, normal, or critical\n";
            }
          }

          if (const auto it = payload.find("timeout_ms"); it != payload.end()) {
            if (!it->is_number_integer()) {
              return "error: notification-show field 'timeout_ms' must be an integer\n";
            }
            const auto timeoutValue = it->get<std::int64_t>();
            if (timeoutValue < 0 || timeoutValue > std::numeric_limits<std::int32_t>::max()) {
              return "error: notification-show field 'timeout_ms' must be between 0 and 2147483647\n";
            }
            timeoutMs = static_cast<std::int32_t>(timeoutValue);
          }

          if (const auto it = payload.find("icon"); it != payload.end()) {
            if (!it->is_string()) {
              return "error: notification-show field 'icon' must be a string\n";
            }
            icon = it->get<std::string>();
          }

          if (const auto it = payload.find("category"); it != payload.end()) {
            if (!it->is_string()) {
              return "error: notification-show field 'category' must be a string\n";
            }
            category = it->get<std::string>();
          }

          if (const auto it = payload.find("desktop_entry"); it != payload.end()) {
            if (!it->is_string()) {
              return "error: notification-show field 'desktop_entry' must be a string\n";
            }
            desktopEntry = it->get<std::string>();
          }
        } else {
          constexpr std::string_view kBodyDelimiter = " -- ";
          const std::size_t delimiterPos = input.find(kBodyDelimiter);
          if (delimiterPos == std::string::npos) {
            summary = input;
          } else {
            summary = StringUtils::trim(input.substr(0, delimiterPos));
            body = StringUtils::trim(input.substr(delimiterPos + kBodyDelimiter.size()));
          }
        }

        summary = StringUtils::trim(summary);
        if (summary.empty()) {
          return "error: notification-show requires a non-empty summary\n";
        }

        if (icon.has_value()) {
          const std::string& iconValue = *icon;
          const bool hasExplicitPrefix = iconValue.starts_with("noctalia-glyph:");
          const bool looksLikePath =
              iconValue.starts_with('/') || iconValue.starts_with("~/") || iconValue.contains('/');
          const bool looksLikeFileUri = iconValue.starts_with("file:");
          const bool looksLikeRemoteUrl = iconValue.starts_with("http://") || iconValue.starts_with("https://");
          if (!hasExplicitPrefix && !looksLikePath && !looksLikeFileUri && !looksLikeRemoteUrl) {
            icon = "noctalia-glyph:" + iconValue;
          }
        }

        (void)m_notificationManager.addInternal(
            std::move(appName), std::move(summary), std::move(body), urgency, timeoutMs, std::move(icon), std::nullopt,
            std::move(category), std::move(desktopEntry)
        );
        return "ok\n";
      },
      "notification-show <summary [-- body]|json>", "Show an internal Noctalia notification"
  );

  m_ipcService.registerHandler(
      "clipboard-clear",
      [this](const std::string&) -> std::string {
        m_panelManager.clearClipboardHistory();
        return "ok\n";
      },
      "clipboard-clear", "Clear clipboard history"
  );

  m_ipcService.registerHandler(
      "dpms-on",
      [this](const std::string&) -> std::string {
        if (!m_compositorPlatform.setOutputPower(true)) {
          return "error: failed to execute dpms-on command\n";
        }
        return "ok\n";
      },
      "dpms-on", "Turn monitors on"
  );

  m_ipcService.registerHandler(
      "dpms-off",
      [this](const std::string&) -> std::string {
        if (!m_compositorPlatform.setOutputPower(false)) {
          return "error: failed to execute dpms-off command\n";
        }
        return "ok\n";
      },
      "dpms-off", "Turn monitors off"
  );

  auto workspaceAlertStatus = [this]() {
    const auto tokens = m_workspaceAlertService.tokens();
    if (tokens.empty()) {
      return std::string{"(no workspace alerts)\n"};
    }
    return StringUtils::join(tokens, "\n") + "\n";
  };

  m_ipcService.registerHandler(
      "workspace-alert-add",
      [this](const std::string& args) -> std::string {
        const std::string workspace = StringUtils::trim(args);
        if (workspace.empty()) {
          return "error: workspace-alert-add requires <workspace>\n";
        }
        if (!m_compositorPlatform.isKnownWorkspaceAlertKey(workspace)) {
          return "error: unknown workspace '" + workspace + "'\n";
        }
        // Store unconditionally. The overlay only marks inactive rows, so
        // alerting the active workspace simply shows once the user leaves it and
        // auto-clears on return; on per-output compositors this also lets an
        // inactive duplicate alert even when the same workspace is active elsewhere.
        (void)m_workspaceAlertService.add(workspace);
        m_bar.refresh();
        return "ok\n";
      },
      "workspace-alert-add <workspace>", "Add a workspace alert (by number, name, or id)"
  );
  m_ipcService.registerHandler(
      "workspace-alert-add-window",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: workspace-alert-add-window requires <window-id>\n";
        }
        const auto workspace = m_compositorPlatform.workspaceAlertKeyForWindow(parts[0]);
        if (!workspace.has_value()) {
          return "error: could not resolve workspace for window id '" + parts[0] + "'\n";
        }
        (void)m_workspaceAlertService.add(*workspace);
        m_bar.refresh();
        return "ok\n";
      },
      "workspace-alert-add-window <window-id>", "Add a workspace alert for a window"
  );
  m_ipcService.registerHandler(
      "workspace-alert-clear",
      [this](const std::string& args) -> std::string {
        const std::string workspace = StringUtils::trim(args);
        if (workspace.empty()) {
          return "error: workspace-alert-clear requires <workspace>\n";
        }
        (void)m_workspaceAlertService.clear(workspace);
        m_bar.refresh();
        return "ok\n";
      },
      "workspace-alert-clear <workspace>", "Clear a workspace alert"
  );
  m_ipcService.registerHandler(
      "workspace-alert-clear-all",
      [this](const std::string& args) -> std::string {
        if (!noctalia::ipc::splitWords(args).empty()) {
          return "error: workspace-alert-clear-all takes no arguments\n";
        }
        m_workspaceAlertService.clearAll();
        m_bar.refresh();
        return "ok\n";
      },
      "workspace-alert-clear-all", "Clear all workspace alerts"
  );
  m_ipcService.registerHandler(
      "workspace-alert-status",
      [workspaceAlertStatus](const std::string& args) -> std::string {
        if (!noctalia::ipc::splitWords(args).empty()) {
          return "error: workspace-alert-status takes no arguments\n";
        }
        return workspaceAlertStatus();
      },
      "workspace-alert-status", "Print workspace alerts"
  );

  registerSessionIpc(m_ipcService, m_sessionActionRunner, m_lockScreen, m_configService);

  if (m_powerProfilesService != nullptr) {
    m_powerProfilesService->registerIpc(m_ipcService, [this](std::string_view profile) {
      m_osdOverlay.show(powerProfileOsdContent(profile));
    });
  }
  if (m_networkService != nullptr) {
    m_networkService->registerIpc(m_ipcService, [this](bool enabled) { m_osdOverlay.show(wifiOsdContent(enabled)); });
  }
  if (m_bluetoothService != nullptr) {
    m_bluetoothService->registerIpc(m_ipcService, [this](bool enabled) {
      m_osdOverlay.show(bluetoothOsdContent(enabled));
    });
  }

  if (m_brightnessService != nullptr) {
    m_brightnessService->registerIpc(m_ipcService, [this]() {
      m_brightnessOsd.suppressFor(std::chrono::milliseconds(250));
    });
  }
  if (m_keyboardBacklightService != nullptr) {
    m_keyboardBacklightService->registerIpc(m_ipcService);
  }
  m_ipcService.registerHandler(
      "keyboard-backlight-osd",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: keyboard-backlight-osd requires <value>\n";
        }
        const auto value = noctalia::ipc::parseNormalizedOrPercent(parts[0]);
        if (!value.has_value()) {
          return "error: invalid keyboard backlight value (use percent like 65 or 65%, or normalized like 0.65)\n";
        }
        m_keyboardBacklightOsd.showValue(*value);
        return "ok\n";
      },
      "keyboard-backlight-osd <value>", "Show keyboard backlight OSD without changing brightness"
  );
  m_ipcService.registerHandler(
      "brightness-osd",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: brightness-osd requires <value>\n";
        }
        const auto value = noctalia::ipc::parseNormalizedOrPercent(parts[0]);
        if (!value.has_value()) {
          return "error: invalid brightness value (use percent like 65 or 65%, or normalized like 0.65)\n";
        }
        m_brightnessOsd.showValue(*value);
        return "ok\n";
      },
      "brightness-osd <value>", "Show brightness OSD without changing brightness"
  );
  m_ipcService.registerHandler(
      "volume-osd",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() > 1) {
          return "error: volume-osd accepts at most one optional [value]\n";
        }
        if (m_pipewireService == nullptr) {
          return "error: audio unavailable\n";
        }
        const auto* sink = m_pipewireService->defaultSink();
        if (sink == nullptr) {
          return "error: no default output\n";
        }
        float volume = sink->volume;
        if (parts.size() == 1) {
          const auto value = noctalia::ipc::parseNormalizedOrPercent(
              parts[0], maxAudioVolume(m_configService.config().audio) * 100.0f
          );
          if (!value.has_value()) {
            return "error: invalid volume value (use percent like 65 or 65%, or normalized like 0.65)\n";
          }
          volume = *value;
        }
        m_audioOsd.showOutputValue(volume, sink->muted);
        return "ok\n";
      },
      "volume-osd [value]", "Show the volume OSD without changing volume (defaults to the current volume)"
  );
  m_ipcService.registerHandler(
      "mic-volume-osd",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() > 1) {
          return "error: mic-volume-osd accepts at most one optional [value]\n";
        }
        if (m_pipewireService == nullptr) {
          return "error: audio unavailable\n";
        }
        const auto* source = m_pipewireService->defaultSource();
        if (source == nullptr) {
          return "error: no default input\n";
        }
        float volume = source->volume;
        if (parts.size() == 1) {
          const auto value = noctalia::ipc::parseNormalizedOrPercent(
              parts[0], maxAudioVolume(m_configService.config().audio) * 100.0f
          );
          if (!value.has_value()) {
            return "error: invalid mic volume value (use percent like 65 or 65%, or normalized like 0.65)\n";
          }
          volume = *value;
        }
        m_audioOsd.showInputValue(volume, source->muted);
        return "ok\n";
      },
      "mic-volume-osd [value]",
      "Show the microphone volume OSD without changing volume (defaults to the current volume)"
  );
  m_configService.registerIpc(m_ipcService);
  scripting::PluginIpcRouter::instance().setPlatform(&m_compositorPlatform);
  m_ipcService.registerHandler(
      "plugin",
      [](const std::string& args) -> std::string { return scripting::PluginIpcRouter::instance().dispatch(args); },
      "plugin <author/plugin:entry> <target[:bar-name]> <event> [payload]", "Dispatch an event to a plugin entry"
  );
  m_ipcService.registerHandler(
      "plugins",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.empty()) {
          return "error: plugins <list|enable|disable> [author/plugin]\n";
        }
        const std::string& cmd = parts[0];
        if (cmd == "list") {
          std::string out;
          // Local-only: IPC handlers run on the main loop, so the listing must never
          // clone or lazy-fetch; it reflects the last-fetched local catalog.
          for (const auto& s : m_pluginManager.list(scripting::CatalogAccess::LocalOnly)) {
            const std::string dependencies =
                s.dependencies.empty() ? std::string{} : " requires " + StringUtils::join(s.dependencies, ", ");
            out += std::format(
                "{} [{}] {}{}{}{}{}\n", s.id, s.source, s.version.empty() ? "-" : s.version,
                s.enabled ? " enabled" : "", s.compatible ? "" : " incompatible", s.deprecated ? " deprecated" : "",
                dependencies
            );
          }
          return out.empty() ? "(no plugins)\n" : out;
        }
        if (cmd == "enable") {
          if (parts.size() != 2) {
            return "error: plugins enable <author/plugin>\n";
          }
          const auto res = m_pluginManager.enable(parts[1]);
          if (!res.ok) {
            return "error: " + res.error + "\n";
          }
          return m_pluginManager.isEnabling(parts[1]) ? "ok (exporting in background)\n" : "ok\n";
        }
        if (cmd == "disable") {
          if (parts.size() != 2) {
            return "error: plugins disable <author/plugin>\n";
          }
          m_pluginManager.disable(parts[1]);
          return "ok\n";
        }
        if (cmd == "update") {
          if (parts.size() != 2) {
            return "error: plugins update <source-name>\n";
          }
          m_pluginManager.update(parts[1]);
          return "ok (updating in background)\n";
        }
        if (cmd == "source") {
          if (parts.size() < 2) {
            return "error: plugins source <list|add|remove> ...\n";
          }
          const std::string& sub = parts[1];
          if (sub == "list") {
            std::string out;
            for (const auto& s : m_configService.config().plugins.sources) {
              out += std::format("{} {} {}\n", s.name, enumToKey(kPluginSourceKinds, s.kind), s.location);
            }
            return out.empty() ? "(no sources)\n" : out;
          }
          if (sub == "add") {
            if (parts.size() < 5) {
              return "error: plugins source add <name> <git|path> <location>\n";
            }
            const auto kind = enumFromKey(kPluginSourceKinds, parts[3]);
            if (!kind.has_value()) {
              return "error: source kind must be 'git' or 'path'\n";
            }
            if (!isValidPluginSourceName(parts[2])) {
              return "error: source name must use letters, digits, '.', '_' or '-', starting with a letter or digit\n";
            }
            PluginSourceConfig source{
                .kind = *kind,
                .name = parts[2],
                .location = parts[4],
            };
            m_pluginManager.addSource(source);
            return "ok\n";
          }
          if (sub == "remove") {
            if (parts.size() != 3) {
              return "error: plugins source remove <name>\n";
            }
            if (isDefaultPluginSourceName(parts[2])) {
              return "error: built-in plugin sources cannot be removed from IPC\n";
            }
            m_pluginManager.removeSource(parts[2]);
            return "ok\n";
          }
          return "error: unknown plugins source subcommand '" + sub + "'\n";
        }
        return "error: unknown plugins subcommand '" + cmd + "'\n";
      },
      "plugins <list|enable|disable|update|source> ...",
      "Manage plugins and sources (list/enable/disable/update, source list/add/remove)"
  );
  m_bar.registerIpc(m_ipcService);
  m_desktopWidgetsController.registerIpc(m_ipcService);
  m_lockscreenWidgetsController.registerIpc(m_ipcService);
  m_panelManager.registerIpc(m_ipcService);
  m_idleInhibitor.registerIpc(m_ipcService);
  m_gammaService.registerIpc(m_ipcService);
  m_themeService.registerIpc(m_ipcService);
  m_templateApplyService.registerIpc(m_ipcService);
  m_dock.registerIpc(m_ipcService);
  m_wallpaper.registerIpc(m_ipcService);
  greeter::registerIpc(
      m_ipcService, m_configService, [this]() { return m_themeService.resolvedMode(); }, &m_compositorPlatform,
      [this]() { return m_logindService != nullptr; }
  );
  if (m_mprisService) {
    m_mprisService->registerIpc(m_ipcService);
  }
  if (m_pipewireService) {
    m_pipewireService->registerIpc(m_ipcService, m_configService);
  }
  if (m_easyEffectsService) {
    m_easyEffectsService->registerIpc(
        m_ipcService, m_configService, [this](AudioEffectsProfileKind kind, std::string_view profile) {
          m_osdOverlay.show(effectsProfileOsdContent(kind, profile));
        }
    );
  }
  m_screenshotService.registerIpc(m_ipcService, m_configService);
  m_windowSwitcher.registerIpc(m_ipcService);
}

bool Application::runShellCommand(const std::string& command) {
  if (!process::runAsync(command)) {
    kLog.warn("command failed to launch: {}", command);
    return false;
  }
  return true;
}

bool Application::runShellCommandBlocking(const std::string& command) {
  const auto result = process::runSync(command);
  if (!result) {
    kLog.warn("command failed: {} exit_code={} stderr={}", command, result.exitCode, result.err);
    return false;
  }
  return true;
}

bool Application::runIdleAction(const IdleActionRequest& action) {
  switch (action.kind) {
  case IdleActionKind::None:
    return true;
  case IdleActionKind::Command:
    return runShellCommand(action.command);
  case IdleActionKind::Lock:
    if (!m_configService.isLockScreenEnabled()) {
      return true;
    }
    return m_sessionActionRunner.lock();
  case IdleActionKind::ScreenOff:
    return m_compositorPlatform.setOutputPower(false);
  case IdleActionKind::ScreenOn:
    return m_compositorPlatform.setOutputPower(true);
  case IdleActionKind::Suspend:
    return m_sessionActionRunner.requestSuspendDetached();
  case IdleActionKind::LockAndSuspend:
    if (!m_configService.isLockScreenEnabled()) {
      return m_sessionActionRunner.requestSuspendDetached();
    }
    return m_sessionActionRunner.lockThenSuspendDetached();
  }
  return false;
}
