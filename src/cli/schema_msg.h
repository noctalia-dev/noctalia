#pragma once

#include "cli/schema.h"
#include "theme/scheme.h"

#include <array>
#include <string_view>

namespace noctalia::cli {

  inline constexpr std::array<std::string_view, 10> kBuiltinPaletteNames{
      "Ayu", "Catppuccin", "Dracula", "Eldritch", "Gruvbox", "Kanagawa", "Noctalia", "Nord", "Rosé Pine", "Tokyo-Night",
  };

  inline constexpr std::array kMsgColorSchemeSetBuiltinPositionals{
      Positional{"name", {}, kBuiltinPaletteNames, true, false, false},
  };
  inline constexpr std::array kMsgColorSchemeSetWallpaperPositionals{
      Positional{"name", {}, theme::kSchemeNames, true, false, false},
  };
  inline constexpr std::array kMsgColorSchemeSetCommunityPositionals{
      Positional{"name", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgColorSchemeSetCustomPositionals{
      Positional{"name", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgColorSchemeSetSubcommands{
      Command{"builtin", "Use a built-in palette", {}, {}, {}, kMsgColorSchemeSetBuiltinPositionals, {}, false},
      Command{
          "wallpaper", "Use a wallpaper generator scheme", {}, {}, {}, kMsgColorSchemeSetWallpaperPositionals, {}, false
      },
      Command{"community", "Use a community palette", {}, {}, {}, kMsgColorSchemeSetCommunityPositionals, {}, false},
      Command{"custom", "Use a custom palette", {}, {}, {}, kMsgColorSchemeSetCustomPositionals, {}, false},
  };

  inline constexpr std::array kMsgPluginsPluginPositionals{
      Positional{"author/plugin", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgPluginsUpdatePositionals{
      Positional{"source-name", {}, {}, true, false, false},
  };
  inline constexpr std::array<std::string_view, 2> kMsgPluginSourceKindChoices{"git", "path"};
  inline constexpr std::array kMsgPluginsSourceAddPositionals{
      Positional{"name", {}, {}, true, false, false},
      Positional{"kind", {}, kMsgPluginSourceKindChoices, true, false, false},
      Positional{"location", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgPluginsSourceRemovePositionals{
      Positional{"name", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgPluginsSourceSubcommands{
      Command{"list", "List plugin sources", {}, {}, {}, {}, {}, false},
      Command{"add", "Add a plugin source", {}, {}, {}, kMsgPluginsSourceAddPositionals, {}, false},
      Command{"remove", "Remove a plugin source", {}, {}, {}, kMsgPluginsSourceRemovePositionals, {}, false},
  };
  inline constexpr std::array kMsgPluginsSubcommands{
      Command{"list", "List installed plugins", {}, {}, {}, {}, {}, false},
      Command{"enable", "Enable a plugin", {}, {}, {}, kMsgPluginsPluginPositionals, {}, false},
      Command{"disable", "Disable a plugin", {}, {}, {}, kMsgPluginsPluginPositionals, {}, false},
      Command{"update", "Update a plugin source", {}, {}, {}, kMsgPluginsUpdatePositionals, {}, false},
      Command{"source", "Manage plugin sources", {}, {}, {}, {}, kMsgPluginsSourceSubcommands, false},
  };

  inline constexpr std::array<std::string_view, 7> kMsgBarAutoHideSetStateChoices{"on",    "off", "smart", "true",
                                                                                  "false", "1",   "0"};
  inline constexpr std::array<std::string_view, 2> kMsgBarLayerSetLayerChoices{"top", "overlay"};
  inline constexpr std::array<std::string_view, 2> kMsgEffectsProfileSetKindChoices{"output", "input"};
  inline constexpr std::array<std::string_view, 4> kMsgLogLevelSetLevelChoices{"debug", "info", "warn", "error"};
  inline constexpr std::array<std::string_view, 8> kMsgMediaActionChoices{"next",        "previous",       "toggle",
                                                                          "play",        "pause",          "stop",
                                                                          "next-player", "previous-player"};
  inline constexpr std::array<std::string_view, 6> kMsgNotificationDndSetStateChoices{"on",    "off", "true",
                                                                                      "false", "1",   "0"};
  inline constexpr std::array<std::string_view, 2> kMsgPowerCycleDirectionChoices{"next", "prev"};
  inline constexpr std::array<std::string_view, 3> kMsgScreenshotFullscreenModeChoices{"pick", "monitor", "all"};
  inline constexpr std::array<std::string_view, 6> kMsgSessionActionChoices{"lock",   "suspend", "lock-and-suspend",
                                                                            "logout", "reboot",  "shutdown"};
  inline constexpr std::array<std::string_view, 2> kMsgTaskbarCycleDirectionChoices{"next", "prev"};
  inline constexpr std::array<std::string_view, 3> kMsgThemeModeSetModeChoices{"dark", "light", "auto"};
  inline constexpr std::array<std::string_view, 2> kMsgWorkspaceSwitchDirectionChoices{"next", "prev"};

  inline constexpr std::array kMsgBarAutoHideSetPositionals{
      Positional{"state", {}, kMsgBarAutoHideSetStateChoices, true, false, false},
      Positional{"bar-name", {}, {}, false, false, false},
      Positional{"monitor-selector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgBarHidePositionals{
      Positional{"bar-name", {}, {}, false, false, false},
      Positional{"monitor-selector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgBarLayerSetPositionals{
      Positional{"layer", {}, kMsgBarLayerSetLayerChoices, true, false, false},
      Positional{"bar-name", {}, {}, false, false, false},
      Positional{"monitor-selector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgBarReserveTogglePositionals{
      Positional{"bar-name", {}, {}, false, false, false},
      Positional{"monitor-selector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgBarShowPositionals{
      Positional{"bar-name", {}, {}, false, false, false},
      Positional{"monitor-selector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgBarTogglePositionals{
      Positional{"bar-name", {}, {}, false, false, false},
      Positional{"monitor-selector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgBrightnessDownPositionals{
      Positional{"target", {}, {}, false, false, false},
      Positional{"step", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgBrightnessOsdPositionals{
      Positional{"value", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgBrightnessSetPositionals{
      Positional{"target", {}, {}, false, false, false},
      Positional{"value", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgBrightnessUpPositionals{
      Positional{"target", {}, {}, false, false, false},
      Positional{"step", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgClipboardCopyPositionals{
      Positional{"text", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgEffectsProfileSetPositionals{
      Positional{"kind", {}, kMsgEffectsProfileSetKindChoices, true, false, false},
      Positional{"profile", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgKeyboardBacklightOsdPositionals{
      Positional{"value", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgKeyboardBacklightSetPositionals{
      Positional{"value", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgLogLevelSetPositionals{
      Positional{"level", {}, kMsgLogLevelSetLevelChoices, true, false, false},
  };
  inline constexpr std::array kMsgMediaPositionals{
      Positional{"action", {}, kMsgMediaActionChoices, true, false, false},
  };
  inline constexpr std::array kMsgMicVolumeDownPositionals{
      Positional{"step", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgMicVolumeOsdPositionals{
      Positional{"value", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgMicVolumeSetPositionals{
      Positional{"value", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgMicVolumeUpPositionals{
      Positional{"step", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgNotificationDndSetPositionals{
      Positional{"state", {}, kMsgNotificationDndSetStateChoices, true, false, false},
  };
  inline constexpr std::array kMsgNotificationShowPositionals{
      Positional{"summary", {}, {}, true, false, false},
      Positional{"body", {}, {}, true, false, true},
  };
  inline constexpr std::array kMsgPanelClosePositionals{
      Positional{"id", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgPanelOpenPositionals{
      Positional{"id", {}, {}, true, false, false},
      Positional{"context", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgPanelTogglePositionals{
      Positional{"id", {}, {}, true, false, false},
      Positional{"context", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgPluginPositionals{
      Positional{"author/plugin:entry", {}, {}, true, false, false},
      Positional{"target[:bar-name]", {}, {}, true, false, false},
      Positional{"event", {}, {}, true, false, false},
      Positional{"payload", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgPowerCyclePositionals{
      Positional{"direction", {}, kMsgPowerCycleDirectionChoices, false, false, false},
  };
  inline constexpr std::array kMsgPowerSetPositionals{
      Positional{"profile", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgScreenshotFullscreenPositionals{
      Positional{"mode", {}, kMsgScreenshotFullscreenModeChoices, false, false, false},
  };
  inline constexpr std::array kMsgSessionPositionals{
      Positional{"action", {}, kMsgSessionActionChoices, true, false, false},
  };
  inline constexpr std::array kMsgSettingsOpenPositionals{
      Positional{"context", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgSettingsOpenPluginPositionals{
      Positional{"plugin-id", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgSettingsOpenWidgetPositionals{
      Positional{"bar-name", {}, {}, false, false, false},
      Positional{"widget-name", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgSettingsTogglePositionals{
      Positional{"context", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgTaskbarCyclePositionals{
      Positional{"direction", {}, kMsgTaskbarCycleDirectionChoices, true, false, false},
  };
  inline constexpr std::array kMsgThemeModeSetPositionals{
      Positional{"mode", {}, kMsgThemeModeSetModeChoices, true, false, false},
  };
  inline constexpr std::array kMsgVolumeDownPositionals{
      Positional{"step", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgVolumeOsdPositionals{
      Positional{"value", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgVolumeSetPositionals{
      Positional{"value", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgVolumeUpPositionals{
      Positional{"step", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgWallpaperGetPositionals{
      Positional{"connector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgWallpaperNextPositionals{
      Positional{"connector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgWallpaperPreviousPositionals{
      Positional{"connector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgWallpaperRandomPositionals{
      Positional{"connector", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgWallpaperSetPositionals{
      Positional{"connector", {}, {}, false, false, false},
      Positional{"path", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgWindowSwitcherPositionals{
      Positional{"action", {}, {}, false, false, false},
  };
  inline constexpr std::array kMsgWorkspaceAlertAddPositionals{
      Positional{"workspace", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgWorkspaceAlertAddWindowPositionals{
      Positional{"window-id", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgWorkspaceAlertClearPositionals{
      Positional{"workspace", {}, {}, true, false, false},
  };
  inline constexpr std::array kMsgWorkspaceSwitchPositionals{
      Positional{"direction", {}, kMsgWorkspaceSwitchDirectionChoices, true, false, false},
  };

  namespace msg {
    inline constexpr Command barAutoHideSet{
        "bar-auto-hide-set", "Set auto-hide state for a bar", {}, {}, {}, kMsgBarAutoHideSetPositionals, {}, false
    };
    inline constexpr Command barHide{"bar-hide", "Hide one or all bars and release their layout gaps",
                                     {},         {},
                                     {},         kMsgBarHidePositionals,
                                     {},         false};
    inline constexpr Command barLayerSet{
        "bar-layer-set", "Set one or all bar layers", {}, {}, {}, kMsgBarLayerSetPositionals, {}, false
    };
    inline constexpr Command barReserveToggle{
        "bar-reserve-toggle",
        "Toggle reserve space for one or all bars",
        {},
        {},
        {},
        kMsgBarReserveTogglePositionals,
        {},
        false
    };
    inline constexpr Command barShow{"bar-show", "Show one or all bars", {}, {}, {}, kMsgBarShowPositionals, {}, false};
    inline constexpr Command barToggle{
        "bar-toggle", "Toggle visibility for one or all bars", {}, {}, {}, kMsgBarTogglePositionals, {}, false
    };
    inline constexpr Command bluetoothDisable{"bluetooth-disable", "Disable Bluetooth", {}, {}, {}, {}, {}, false};
    inline constexpr Command bluetoothEnable{"bluetooth-enable", "Enable Bluetooth", {}, {}, {}, {}, {}, false};
    inline constexpr Command bluetoothStatus{"bluetooth-status", "Print Bluetooth state", {}, {}, {}, {}, {}, false};
    inline constexpr Command bluetoothToggle{"bluetooth-toggle", "Toggle Bluetooth", {}, {}, {}, {}, {}, false};
    inline constexpr Command brightnessDown{
        "brightness-down",
        "Decrease brightness (defaults to current monitor)",
        {},
        {},
        {},
        kMsgBrightnessDownPositionals,
        {},
        false
    };
    inline constexpr Command brightnessListBacklightDevices{
        "brightness-list-backlight-devices", "List available sysfs backlight device names", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command brightnessOsd{
        "brightness-osd",
        "Show brightness OSD without changing brightness",
        {},
        {},
        {},
        kMsgBrightnessOsdPositionals,
        {},
        false
    };
    inline constexpr Command brightnessSet{"brightness-set",
                                           "Set brightness (defaults to current monitor)",
                                           {},
                                           {},
                                           {},
                                           kMsgBrightnessSetPositionals,
                                           {},
                                           false};
    inline constexpr Command brightnessUp{
        "brightness-up",
        "Increase brightness (defaults to current monitor)",
        {},
        {},
        {},
        kMsgBrightnessUpPositionals,
        {},
        false
    };
    inline constexpr Command caffeineDisable{
        "caffeine-disable", "Disable caffeine (idle inhibitor)", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command caffeineEnable{
        "caffeine-enable", "Enable caffeine (idle inhibitor)", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command caffeineToggle{
        "caffeine-toggle", "Toggle caffeine (idle inhibitor)", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command clipboardClear{"clipboard-clear", "Clear clipboard history", {}, {}, {}, {}, {}, false};
    inline constexpr Command clipboardCopy{
        "clipboard-copy", "Copy text to the clipboard", {}, {}, {}, kMsgClipboardCopyPositionals, {}, false
    };
    inline constexpr Command clipboardText{
        "clipboard-text",
        "Print the most recent clipboard text (empty when the selection holds no text)",
        {},
        {},
        {},
        {},
        {},
        false
    };
    inline constexpr Command colorSchemeGet{
        "color-scheme-get",
        "Print active color scheme: <source> <name> (source is builtin, wallpaper, community, or custom)",
        {},
        {},
        {},
        {},
        {},
        false
    };
    inline constexpr Command colorSchemeSet{
        "color-scheme-set",
        "Set palette source and selection in settings.toml (builtin name, wallpaper generator scheme, community id, "
        "or custom scheme folder name)",
        {},
        {},
        {},
        {},
        kMsgColorSchemeSetSubcommands,
        false
    };
    inline constexpr Command configReload{"config-reload", "Reload the config file", {}, {}, {}, {}, {}, false};
    inline constexpr Command desktopWidgetsEdit{
        "desktop-widgets-edit", "Open the desktop widgets editor", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command desktopWidgetsExit{
        "desktop-widgets-exit", "Close the desktop widgets editor", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command desktopWidgetsHide{
        "desktop-widgets-hide",
        "Hide desktop widgets now (runtime only; does not change the saved setting)",
        {},
        {},
        {},
        {},
        {},
        false
    };
    inline constexpr Command desktopWidgetsShow{
        "desktop-widgets-show",
        "Show desktop widgets now (runtime only; does not change the saved setting)",
        {},
        {},
        {},
        {},
        {},
        false
    };
    inline constexpr Command desktopWidgetsToggle{
        "desktop-widgets-toggle",
        "Toggle desktop widgets visibility (runtime only; does not change the saved setting)",
        {},
        {},
        {},
        {},
        {},
        false
    };
    inline constexpr Command desktopWidgetsToggleEdit{
        "desktop-widgets-toggle-edit", "Toggle desktop widgets edit mode", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command dockHide{"dock-hide", "Hide the dock (persists override)", {}, {}, {}, {}, {}, false};
    inline constexpr Command dockReload{"dock-reload", "Reload dock configuration", {}, {}, {}, {}, {}, false};
    inline constexpr Command dockShow{"dock-show", "Show the dock (persists override)", {}, {}, {}, {}, {}, false};
    inline constexpr Command dockToggle{"dock-toggle", "Toggle dock visibility (persists override)", {}, {}, {}, {}, {},
                                        false};
    inline constexpr Command dpmsOff{"dpms-off", "Turn monitors off", {}, {}, {}, {}, {}, false};
    inline constexpr Command dpmsOn{"dpms-on", "Turn monitors on", {}, {}, {}, {}, {}, false};
    inline constexpr Command effectsProfileSet{
        "effects-profile-set",
        "Set the EasyEffects output or input profile",
        {},
        {},
        {},
        kMsgEffectsProfileSetPositionals,
        {},
        false
    };
    inline constexpr Command greeterSync{
        "greeter-sync", "Sync wallpaper, colors, and monitor layout to Noctalia Greeter", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command keyboardBacklightDown{
        "keyboard-backlight-down", "Decrease all keyboard backlights by one level", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command keyboardBacklightOsd{
        "keyboard-backlight-osd",
        "Show keyboard backlight OSD without changing brightness",
        {},
        {},
        {},
        kMsgKeyboardBacklightOsdPositionals,
        {},
        false
    };
    inline constexpr Command keyboardBacklightSet{
        "keyboard-backlight-set",
        "Set all keyboard backlights (0-100 percentage)",
        {},
        {},
        {},
        kMsgKeyboardBacklightSetPositionals,
        {},
        false
    };
    inline constexpr Command keyboardBacklightToggle{
        "keyboard-backlight-toggle", "Toggle all keyboard backlights on/off", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command keyboardBacklightUp{
        "keyboard-backlight-up", "Increase all keyboard backlights by one level", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command keyboardLayoutCycle{
        "keyboard-layout-cycle", "Switch to the next keyboard layout", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command lockscreenWidgetsEdit{
        "lockscreen-widgets-edit", "Open the lockscreen widgets editor", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command lockscreenWidgetsExit{
        "lockscreen-widgets-exit", "Close the lockscreen widgets editor", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command lockscreenWidgetsToggleEdit{
        "lockscreen-widgets-toggle-edit", "Toggle lockscreen widgets edit mode", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command logLevelSet{
        "log-level-set", "Set the console log level", {}, {}, {}, kMsgLogLevelSetPositionals, {}, false
    };
    inline constexpr Command logLevelStatus{
        "log-level-status", "Print the current console log level", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command media{"media", "Control active media playback", {}, {}, {}, kMsgMediaPositionals, {},
                                   false};
    inline constexpr Command micMute{"mic-mute", "Toggle microphone mute", {}, {}, {}, {}, {}, false};
    inline constexpr Command micVolumeDown{
        "mic-volume-down", "Decrease microphone volume", {}, {}, {}, kMsgMicVolumeDownPositionals, {}, false
    };
    inline constexpr Command micVolumeOsd{
        "mic-volume-osd",
        "Show the microphone volume OSD without changing volume (defaults to the current volume)",
        {},
        {},
        {},
        kMsgMicVolumeOsdPositionals,
        {},
        false
    };
    inline constexpr Command micVolumeSet{
        "mic-volume-set", "Set microphone volume", {}, {}, {}, kMsgMicVolumeSetPositionals, {}, false
    };
    inline constexpr Command micVolumeUp{
        "mic-volume-up", "Increase microphone volume", {}, {}, {}, kMsgMicVolumeUpPositionals, {}, false
    };
    inline constexpr Command networkToggle{"network-toggle",
                                           "Disconnect the active network, or reconnect when nothing is connected",
                                           {},
                                           {},
                                           {},
                                           {},
                                           {},
                                           false};
    inline constexpr Command nightlightDisable{
        "nightlight-disable", "Disable night light schedule", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command nightlightEnable{
        "nightlight-enable", "Enable night light schedule", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command nightlightForceToggle{
        "nightlight-force-toggle", "Toggle forced night light mode", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command nightlightToggle{
        "nightlight-toggle", "Toggle night light schedule", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command notificationClearActive{
        "notification-clear-active", "Dismiss all currently active notifications", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command notificationClearHistory{
        "notification-clear-history", "Clear notification history", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command notificationDndSet{
        "notification-dnd-set",
        "Set notification Do Not Disturb state",
        {},
        {},
        {},
        kMsgNotificationDndSetPositionals,
        {},
        false
    };
    inline constexpr Command notificationDndStatus{
        "notification-dnd-status", "Print notification Do Not Disturb state", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command notificationDndToggle{
        "notification-dnd-toggle", "Toggle notification Do Not Disturb state", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command notificationInvokeLatest{
        "notification-invoke-latest",
        "Invoke the default action of the most recent active notification",
        {},
        {},
        {},
        {},
        {},
        false
    };
    inline constexpr Command notificationShow{"notification-show",
                                              "Show an internal Noctalia notification",
                                              {},
                                              {},
                                              {},
                                              kMsgNotificationShowPositionals,
                                              {},
                                              false};
    inline constexpr Command osdDisable{"osd-disable", "Disable OSD popups", {}, {}, {}, {}, {}, false};
    inline constexpr Command osdEnable{"osd-enable", "Enable OSD popups", {}, {}, {}, {}, {}, false};
    inline constexpr Command osdToggle{"osd-toggle", "Toggle OSD popups", {}, {}, {}, {}, {}, false};
    inline constexpr Command panelClose{
        "panel-close",
        "Close the active panel, or close the named panel if it is active",
        {},
        {},
        {},
        kMsgPanelClosePositionals,
        {},
        false
    };
    inline constexpr Command panelOpen{
        "panel-open", "Open a panel by id, optionally with context (e.g. launcher /emo, control-center audio)",
        {},           {},
        {},           kMsgPanelOpenPositionals,
        {},           false
    };
    inline constexpr Command panelToggle{
        "panel-toggle",
        "Toggle a panel by id, optionally with context (e.g. launcher /emo, control-center audio)",
        {},
        {},
        {},
        kMsgPanelTogglePositionals,
        {},
        false
    };
    inline constexpr Command plugin{
        "plugin", "Dispatch an event to a plugin entry", {}, {}, {}, kMsgPluginPositionals, {}, false
    };
    inline constexpr Command plugins{
        "plugins",
        "Manage plugins and sources (list/enable/disable/update, source list/add/remove)",
        {},
        {},
        {},
        {},
        kMsgPluginsSubcommands,
        false
    };
    inline constexpr Command powerCycle{
        "power-cycle",
        "Step through UPower's ordered profile list, forward by default (wraps)",
        {},
        {},
        {},
        kMsgPowerCyclePositionals,
        {},
        false
    };
    inline constexpr Command powerSet{
        "power-set", "Set the UPower power profile (e.g. performance, balanced, power-saver)",
        {},          {},
        {},          kMsgPowerSetPositionals,
        {},          false
    };
    inline constexpr Command screenshotFullscreen{
        "screenshot-fullscreen",
        "Capture the focused monitor by default, pick interactively with pick, or all outputs with all",
        {},
        {},
        {},
        kMsgScreenshotFullscreenPositionals,
        {},
        false
    };
    inline constexpr Command screenshotRegion{
        "screenshot-region", "Start an interactive region screenshot", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command session{"session", "Run a built-in session action", {}, {},
                                     {},        kMsgSessionPositionals,          {}, false};
    inline constexpr Command settingsClose{"settings-close", "Close the settings window", {}, {}, {}, {}, {}, false};
    inline constexpr Command settingsOpen{
        "settings-open",
        "Open the settings window, or focus it if already open, optionally at a specific section",
        {},
        {},
        {},
        kMsgSettingsOpenPositionals,
        {},
        false
    };
    inline constexpr Command settingsOpenPlugin{
        "settings-open-plugin",
        "Open the settings window at a plugin's settings (e.g. noctalia/notes)",
        {},
        {},
        {},
        kMsgSettingsOpenPluginPositionals,
        {},
        false
    };
    inline constexpr Command settingsOpenWidget{
        "settings-open-widget",
        "Open the settings window at a bar widget; from a widget gesture, targets that widget",
        {},
        {},
        {},
        kMsgSettingsOpenWidgetPositionals,
        {},
        false
    };
    inline constexpr Command settingsToggle{
        "settings-toggle",
        "Toggle the settings window, optionally at a specific section",
        {},
        {},
        {},
        kMsgSettingsTogglePositionals,
        {},
        false
    };
    inline constexpr Command status{"status", "Print current state as JSON", {}, {}, {}, {}, {}, false};
    inline constexpr Command taskbarCycle{
        "taskbar-cycle",
        "Step to the adjacent task or workspace group in the invoking taskbar",
        {},
        {},
        {},
        kMsgTaskbarCyclePositionals,
        {},
        false
    };
    inline constexpr Command templatesApply{
        "templates-apply", "Apply configured theme templates for the current palette", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command themeModeGet{
        "theme-mode-get", "Print the current resolved theme mode", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command themeModeSet{"theme-mode-set",
                                          "Set theme mode and persist to settings.toml",
                                          {},
                                          {},
                                          {},
                                          kMsgThemeModeSetPositionals,
                                          {},
                                          false};
    inline constexpr Command themeModeToggle{
        "theme-mode-toggle", "Toggle theme mode between dark and light", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command volumeDown{
        "volume-down", "Decrease speaker volume", {}, {}, {}, kMsgVolumeDownPositionals, {}, false
    };
    inline constexpr Command volumeMute{"volume-mute", "Toggle speaker mute", {}, {}, {}, {}, {}, false};
    inline constexpr Command volumeOsd{
        "volume-osd", "Show the volume OSD without changing volume (defaults to the current volume)",
        {},           {},
        {},           kMsgVolumeOsdPositionals,
        {},           false
    };
    inline constexpr Command volumeSet{"volume-set", "Set speaker volume",     {}, {},
                                       {},           kMsgVolumeSetPositionals, {}, false};
    inline constexpr Command volumeUp{"volume-up", "Increase speaker volume", {}, {},
                                      {},          kMsgVolumeUpPositionals,   {}, false};
    inline constexpr Command wallpaperGet{
        "wallpaper-get",
        "Print default wallpaper path, or effective path for an output",
        {},
        {},
        {},
        kMsgWallpaperGetPositionals,
        {},
        false
    };
    inline constexpr Command wallpaperNext{"wallpaper-next",
                                           "Switch to the next wallpaper immediately",
                                           {},
                                           {},
                                           {},
                                           kMsgWallpaperNextPositionals,
                                           {},
                                           false};
    inline constexpr Command wallpaperPrevious{
        "wallpaper-previous",
        "Switch to the previous wallpaper immediately",
        {},
        {},
        {},
        kMsgWallpaperPreviousPositionals,
        {},
        false
    };
    inline constexpr Command wallpaperRandom{"wallpaper-random",
                                             "Switch to a random wallpaper immediately",
                                             {},
                                             {},
                                             {},
                                             kMsgWallpaperRandomPositionals,
                                             {},
                                             false};
    inline constexpr Command wallpaperSet{
        "wallpaper-set",
        "Set wallpaper for all or a specific output (persisted)",
        {},
        {},
        {},
        kMsgWallpaperSetPositionals,
        {},
        false
    };
    inline constexpr Command wifiDisable{"wifi-disable", "Disable Wi-Fi", {}, {}, {}, {}, {}, false};
    inline constexpr Command wifiEnable{"wifi-enable", "Enable Wi-Fi", {}, {}, {}, {}, {}, false};
    inline constexpr Command wifiStatus{"wifi-status", "Print Wi-Fi state", {}, {}, {}, {}, {}, false};
    inline constexpr Command wifiToggle{"wifi-toggle", "Toggle Wi-Fi", {}, {}, {}, {}, {}, false};
    inline constexpr Command windowSwitcher{"window-switcher",
                                            "Open or close the window switcher overlay",
                                            {},
                                            {},
                                            {},
                                            kMsgWindowSwitcherPositionals,
                                            {},
                                            false};
    inline constexpr Command workspaceAlertAdd{
        "workspace-alert-add",
        "Add a workspace alert (by number, name, or id)",
        {},
        {},
        {},
        kMsgWorkspaceAlertAddPositionals,
        {},
        false
    };
    inline constexpr Command workspaceAlertAddWindow{
        "workspace-alert-add-window",
        "Add a workspace alert for a window",
        {},
        {},
        {},
        kMsgWorkspaceAlertAddWindowPositionals,
        {},
        false
    };
    inline constexpr Command workspaceAlertClear{
        "workspace-alert-clear", "Clear a workspace alert", {}, {}, {}, kMsgWorkspaceAlertClearPositionals, {}, false
    };
    inline constexpr Command workspaceAlertClearAll{
        "workspace-alert-clear-all", "Clear all workspace alerts", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command workspaceAlertStatus{
        "workspace-alert-status", "Print workspace alerts", {}, {}, {}, {}, {}, false
    };
    inline constexpr Command workspaceSwitch{
        "workspace-switch",
        "Switch to the adjacent workspace on the target monitor (stops at both ends)",
        {},
        {},
        {},
        kMsgWorkspaceSwitchPositionals,
        {},
        false
    };
  } // namespace msg

  inline constexpr std::array kMsgSubcommands{
      msg::barAutoHideSet,
      msg::barHide,
      msg::barLayerSet,
      msg::barReserveToggle,
      msg::barShow,
      msg::barToggle,
      msg::bluetoothDisable,
      msg::bluetoothEnable,
      msg::bluetoothStatus,
      msg::bluetoothToggle,
      msg::brightnessDown,
      msg::brightnessListBacklightDevices,
      msg::brightnessOsd,
      msg::brightnessSet,
      msg::brightnessUp,
      msg::caffeineDisable,
      msg::caffeineEnable,
      msg::caffeineToggle,
      msg::clipboardClear,
      msg::clipboardCopy,
      msg::clipboardText,
      msg::colorSchemeGet,
      msg::colorSchemeSet,
      msg::configReload,
      msg::desktopWidgetsEdit,
      msg::desktopWidgetsExit,
      msg::desktopWidgetsHide,
      msg::desktopWidgetsShow,
      msg::desktopWidgetsToggle,
      msg::desktopWidgetsToggleEdit,
      msg::dockHide,
      msg::dockReload,
      msg::dockShow,
      msg::dockToggle,
      msg::dpmsOff,
      msg::dpmsOn,
      msg::effectsProfileSet,
      msg::greeterSync,
      msg::keyboardBacklightDown,
      msg::keyboardBacklightOsd,
      msg::keyboardBacklightSet,
      msg::keyboardBacklightToggle,
      msg::keyboardBacklightUp,
      msg::keyboardLayoutCycle,
      msg::lockscreenWidgetsEdit,
      msg::lockscreenWidgetsExit,
      msg::lockscreenWidgetsToggleEdit,
      msg::logLevelSet,
      msg::logLevelStatus,
      msg::media,
      msg::micMute,
      msg::micVolumeDown,
      msg::micVolumeOsd,
      msg::micVolumeSet,
      msg::micVolumeUp,
      msg::networkToggle,
      msg::nightlightDisable,
      msg::nightlightEnable,
      msg::nightlightForceToggle,
      msg::nightlightToggle,
      msg::notificationClearActive,
      msg::notificationClearHistory,
      msg::notificationDndSet,
      msg::notificationDndStatus,
      msg::notificationDndToggle,
      msg::notificationInvokeLatest,
      msg::notificationShow,
      msg::osdDisable,
      msg::osdEnable,
      msg::osdToggle,
      msg::panelClose,
      msg::panelOpen,
      msg::panelToggle,
      msg::plugin,
      msg::plugins,
      msg::powerCycle,
      msg::powerSet,
      msg::screenshotFullscreen,
      msg::screenshotRegion,
      msg::session,
      msg::settingsClose,
      msg::settingsOpen,
      msg::settingsOpenPlugin,
      msg::settingsOpenWidget,
      msg::settingsToggle,
      msg::status,
      msg::taskbarCycle,
      msg::templatesApply,
      msg::themeModeGet,
      msg::themeModeSet,
      msg::themeModeToggle,
      msg::volumeDown,
      msg::volumeMute,
      msg::volumeOsd,
      msg::volumeSet,
      msg::volumeUp,
      msg::wallpaperGet,
      msg::wallpaperNext,
      msg::wallpaperPrevious,
      msg::wallpaperRandom,
      msg::wallpaperSet,
      msg::wifiDisable,
      msg::wifiEnable,
      msg::wifiStatus,
      msg::wifiToggle,
      msg::windowSwitcher,
      msg::workspaceAlertAdd,
      msg::workspaceAlertAddWindow,
      msg::workspaceAlertClear,
      msg::workspaceAlertClearAll,
      msg::workspaceAlertStatus,
      msg::workspaceSwitch,
  };

  inline constexpr Command kMsgCmd{
      "msg", "Send a command to the running instance", {}, {}, {}, {}, kMsgSubcommands, false,
  };

  [[nodiscard]] constexpr const Command* findMsgCommand(std::string_view name) {
    for (const Command& command : kMsgCmd.subcommands) {
      if (command.name == name)
        return &command;
    }
    return nullptr;
  }

} // namespace noctalia::cli
