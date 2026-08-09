#pragma once

#include "cli/schema.h"

#include <span>
#include <string_view>

namespace noctalia::ipc {

  using cli_schema::CliCommand;
  using cli_schema::CliPositional;

  // ── Choices ────────────────────────────────────────────────────────

  inline constexpr std::string_view kThemeModes[] = {"dark", "light", "auto"}; //
  inline constexpr std::string_view kBuiltinPalettes[] = {"Ayu",       "Catppuccin", "Dracula",  "Eldritch",
                                                          "Gruvbox",   "Kanagawa",   "Noctalia", "Nord",
                                                          "Rosé Pine", "Tokyo-Night"};
  inline constexpr std::string_view kWallpaperSchemes[] = {"m3-tonal-spot", "m3-content",    "m3-fruit-salad",
                                                           "m3-rainbow",    "m3-monochrome", "vibrant",
                                                           "faithful",      "soft",          "dysfunctional",
                                                           "muted"};
  inline constexpr std::string_view kEffectsProfileKinds[] = {"output", "input"};
  inline constexpr std::string_view kBarLayers[] = {"overlay", "top"};
  inline constexpr std::string_view kBooleanStates[] = {"on", "off", "true", "false", "1", "0"};
  inline constexpr std::string_view kPowerProfiles[] = {"power-saver", "balanced", "performance"};
  inline constexpr std::string_view kLogLevels[] = {"trace", "debug", "info", "warn", "error", "off"};
  inline constexpr std::string_view kAutoHideStates[] = {"on", "off", "smart", "true", "false", "1", "0"};
  inline constexpr std::string_view kMediaActions[] = {"play", "pause", "play-pause", "stop", "next", "previous"};
  inline constexpr std::string_view kSessionActions[] = {"lock",      "logout", "suspend",
                                                         "hibernate", "reboot", "shutdown"};

  // ── Positionals ────────────────────────────────────────────────────────

  // Shared
  // msg <cmd-up/down> [step]
  inline constexpr CliPositional kStepOptionalPositionals[] = {
      {.name = "step", .description = "optional step value (e.g. 5 or 5%)", .required = false}
  };

  // msg cmd-osd [value]
  inline constexpr CliPositional kOptionalValuePositionals[] = {
      {.name = "value", .description = "optional value/percentage", .required = false}
  };

  // msg bar-auto-hide-set <on|off|smart|true|false|1|0> [bar-name] [monitor-selector]
  inline constexpr CliPositional kBarAutoHideSetPositionals[] = {
      {.name = "state",
       .description = "auto-hide state",
       .choices = kAutoHideStates,
       .required = true,
       .missingError = "error: usage: bar-auto-hide-set <on|off|smart|true|false|1|0> [bar-name] [monitor-selector]"},
      {.name = "bar-name", .description = "target bar name", .required = false},
      {.name = "monitor-selector", .description = "target monitor selector", .required = false}
  };

  // msg bar-hide [bar-name] [monitor-selector]
  inline constexpr CliPositional kBarHidePositionals[] = {
      {.name = "bar-name", .description = "target bar name", .required = false},
      {.name = "monitor-selector", .description = "target monitor selector", .required = false}
  };

  // msg bar-layer-set <layer> [id]
  inline constexpr CliPositional kBarLayerSetPositionals[] = {
      {.name = "layer", .description = "bar layer", .choices = kBarLayers, .required = true},
      {.name = "id", .description = "bar instance id", .required = false}
  };

  // msg bar-reserve-toggle [bar-name] [monitor-selector]
  inline constexpr CliPositional kBarReserveTogglePositionals[] = {
      {.name = "bar-name", .description = "target bar name", .required = false},
      {.name = "monitor-selector", .description = "target monitor selector", .required = false}
  };

  // msg bar-show [bar-name] [monitor-selector]
  inline constexpr CliPositional kBarShowPositionals[] = {
      {.name = "bar-name", .description = "target bar name", .required = false},
      {.name = "monitor-selector", .description = "target monitor selector", .required = false}
  };

  // msg bar-toggle [bar-name] [monitor-selector]
  inline constexpr CliPositional kBarTogglePositionals[] = {
      {.name = "bar-name", .description = "target bar name", .required = false},
      {.name = "monitor-selector", .description = "target monitor selector", .required = false}
  };

  // msg brightness-set [monitor-selector] <value>
  inline constexpr CliPositional kBrightnessSetPositionals[] = {
      {.name = "target", .description = "optional monitor selector (e.g. current, *, all)", .required = false},
      {.name = "value", .description = "brightness value/percentage", .required = false}
  };

  // msg clipboard-copy <text>
  inline constexpr CliPositional kClipboardCopyPositionals[] = {
      {.name = "text", .description = "text to copy", .required = true}
  };

  // color-scheme-set builtin <palette>
  inline constexpr CliPositional kColorSchemeBuiltinPositionals[] = {
      {.name = "name", .description = "built-in palette name", .choices = kBuiltinPalettes, .required = true}
  };

  // color-scheme-set wallpaper <scheme>
  inline constexpr CliPositional kColorSchemeWallpaperPositionals[] = {
      {.name = "scheme", .description = "generator scheme", .choices = kWallpaperSchemes, .required = true}
  };

  // color-scheme-set community <id>
  inline constexpr CliPositional kColorSchemeCommunityPositionals[] = {
      {.name = "id", .description = "community palette id", .required = true}
  };

  // color-scheme-set custom <palette>
  inline constexpr CliPositional kColorSchemeCustomPositionals[] = {
      {.name = "filename", .description = "palette file name without .json", .required = true}
  };

  inline constexpr CliCommand kColorSchemeSubcommands[] = {
      {.name = "builtin",
       .summary = "built-in palette name, such as Noctalia",
       .positionals = kColorSchemeBuiltinPositionals},
      {.name = "wallpaper",
       .summary = "generator scheme for wallpaper colors",
       .positionals = kColorSchemeWallpaperPositionals},
      {.name = "community", .summary = "community palette id", .positionals = kColorSchemeCommunityPositionals},
      {.name = "custom",
       .summary = "palette file name without .json under ~/.config/noctalia/palettes/",
       .positionals = kColorSchemeCustomPositionals}
  };

  // msg effects-profile-set <output|input> <profile>
  inline constexpr CliPositional kEffectsProfileSetPositionals[] = {
      {.name = "kind", .description = "profile kind", .choices = kEffectsProfileKinds, .required = true},
      {.name = "profile", .description = "effects profile name", .required = true}
  };

  // msg keyboard-backlight-set <value>
  inline constexpr CliPositional kKeyboardBacklightSetPositionals[] = {
      {.name = "value", .description = "backlight value", .required = true}
  };

  // msg log-level-set <level>
  inline constexpr CliPositional kLogLevelSetPositionals[] = {
      {.name = "level", .description = "log level", .choices = kLogLevels, .required = true}
  };

  // msg media <action>
  inline constexpr CliPositional kMediaPositionals[] = {
      {.name = "action", .description = "media action", .choices = kMediaActions, .required = true}
  };

  // msg mic-volume-set <value>
  inline constexpr CliPositional kMicVolumeSetPositionals[] = {
      {.name = "value", .description = "volume percentage", .required = true}
  };

  // msg notification-dnd-set <state>
  inline constexpr CliPositional kNotificationDndSetPositionals[] = {
      {.name = "state",
       .description = "dnd state",
       .choices = kBooleanStates,
       .required = true,
       .missingError = "error: notification-dnd-set requires <on|off|true|false|1|0>",
       .invalidChoiceError = "error: notification-dnd-set requires <on|off|true|false|1|0>"}
  };

  // msg notification-show <summary> <body>
  inline constexpr CliPositional kNotificationPositionals[] = {
      {.name = "summary",
       .description = "notification summary",
       .required = true,
       .missingError = "missing notification summary"},
      {.name = "body",
       .description = "notification body",
       .required = true,
       .missingError = "missing notification body"}
  };

  // msg panel-close [id]
  inline constexpr CliPositional kPanelClosePositionals[] = {
      {.name = "id", .description = "panel id", .required = false}
  };

  // msg panel-open <id> [context]
  // msg panel-toggle <id> [context]
  inline constexpr CliPositional kPanelOpenTogglePositionals[] = {
      {.name = "id", .description = "panel id", .required = true},
      {.name = "context", .description = "optional context string", .required = false}
  };

  // msg plugin <author/plugin:entry> <target[:bar-name]> <event> [payload]
  inline constexpr CliPositional kPluginPositionals[] = {
      {.name = "entry", .description = "plugin entry (author/plugin:entry)", .required = true},
      {.name = "target", .description = "target instance (target[:bar-name])", .required = true},
      {.name = "event", .description = "event name", .required = true},
      {.name = "payload", .description = "optional JSON payload", .required = false}
  };

  // msg power-cycle [direction]
  inline constexpr CliPositional kPowerCyclePositionals[] = {
      {.name = "direction", .description = "optional direction", .required = false}
  };

  // msg plugins <action> [source action]
  inline constexpr CliCommand kMsgPluginsSourceSubcommands[] = {
      {.name = "list", .summary = "List plugin sources"},
      {.name = "add", .summary = "Add a plugin source"},
      {.name = "remove", .summary = "Remove a plugin source"},
  };

  inline constexpr CliCommand kMsgPluginsSubcommands[] = {
      {.name = "list", .summary = "List installed plugins"},
      {.name = "enable", .summary = "Enable a plugin"},
      {.name = "disable", .summary = "Disable a plugin"},
      {.name = "update", .summary = "Update plugins"},
      {.name = "source", .summary = "Manage plugin sources", .subcommands = kMsgPluginsSourceSubcommands},
  };

  // msg power-set <profile>
  inline constexpr CliPositional kPowerSetPositionals[] = {
      {.name = "profile", .description = "power profile name", .choices = kPowerProfiles, .required = true}
  };

  // msg screenshot-fullscreen [monitor-selector]
  // msg window-switcher [monitor-selector]
  inline constexpr CliPositional kMonitorSelectorOptionalPositionals[] = {
      {.name = "monitor-selector", .description = "target monitor selector", .required = false}
  };

  // msg session <action>
  inline constexpr CliPositional kSessionPositionals[] = {
      {.name = "action", .description = "session action", .choices = kSessionActions, .required = true}
  };

  // msg settings-open [context]
  // msg settings-toggle [context]
  inline constexpr CliPositional kSettingsContextPositionals[] = {
      {.name = "context", .description = "optional settings context", .required = false}
  };

  // msg settings-open-plugin <id>
  inline constexpr CliPositional kSettingsOpenPluginPositionals[] = {
      {.name = "id", .description = "plugin id", .required = true}
  };

  // msg settings-open-widget <id>
  inline constexpr CliPositional kSettingsOpenWidgetPositionals[] = {
      {.name = "id", .description = "widget id", .required = true}
  };

  // msg theme-mode-set <dark|light|auto>
  inline constexpr CliPositional kThemeModePositionals[] = {
      {.name = "mode",
       .description = "theme mode",
       .choices = kThemeModes,
       .required = true,
       .invalidChoiceError = "expected dark, light, or auto"}
  };

  // msg volume-set <value>
  inline constexpr CliPositional kVolumeSetPositionals[] = {
      {.name = "value", .description = "volume percentage", .required = true}
  };

  // msg wallpaper-<get|next|previous|random> [connector]
  inline constexpr CliPositional kWallpaperConnectorPositionals[] = {
      {.name = "connector", .description = "target monitor connector", .required = false}
  };

  // msg wallpaper-set <path>
  inline constexpr CliPositional kWallpaperSetPositionals[] = {
      {.name = "path", .description = "wallpaper file path", .required = true}
  };

  // msg workspace-alert-add <alert>
  inline constexpr CliPositional kWorkspaceAlertAddPositionals[] = {
      {.name = "alert", .description = "alert name", .required = true}
  };

  // msg workspace-alert-add-window <window>
  inline constexpr CliPositional kWorkspaceAlertAddWindowPositionals[] = {
      {.name = "window", .description = "window id/title", .required = true}
  };

  // msg workspace-alert-clear <alert>
  inline constexpr CliPositional kWorkspaceAlertClearPositionals[] = {
      {.name = "alert", .description = "alert name", .required = true}
  };

  // msg workspace-switch <workspace>
  inline constexpr CliPositional kWorkspacePositionals[] = {
      {.name = "workspace",
       .description = "workspace index or name",
       .required = true,
       .missingError = "missing workspace target"}
  };

  // ── IPC subcommands ────────────────────────────────────────────────────────
  inline constexpr CliCommand kIpcSubcommands[] = {
      {.name = "--help", .summary = "Show help message"},
      {.name = "bar-auto-hide-set",
       .summary = "Set auto-hide state for a bar",
       .positionals = kBarAutoHideSetPositionals},
      {.name = "bar-hide",
       .summary = "Hide one or all bars and release their layout gaps",
       .positionals = kBarHidePositionals,
       .extraArgError = "error: usage: bar-hide [bar-name] [monitor-selector]"},
      {.name = "bar-layer-set", .summary = "Set one or all bar layers", .positionals = kBarLayerSetPositionals},
      {.name = "bar-reserve-toggle",
       .summary = "Toggle reserve space for one or all bars",
       .positionals = kBarReserveTogglePositionals,
       .extraArgError = "error: usage: bar-reserve-toggle [bar-name] [monitor-selector]"},
      {.name = "bar-show",
       .summary = "Show one or all bars",
       .positionals = kBarShowPositionals,
       .extraArgError = "error: usage: bar-show [bar-name] [monitor-selector]"},
      {.name = "bar-toggle",
       .summary = "Toggle visibility for one or all bars",
       .positionals = kBarTogglePositionals,
       .extraArgError = "error: usage: bar-toggle [bar-name] [monitor-selector]"},
      {.name = "bluetooth-disable", .summary = "Disable Bluetooth"},
      {.name = "bluetooth-enable", .summary = "Enable Bluetooth"},
      {.name = "bluetooth-status", .summary = "Print Bluetooth state"},
      {.name = "bluetooth-toggle", .summary = "Toggle Bluetooth"},
      {.name = "brightness-down",
       .summary = "Decrease brightness",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: brightness-down [step]"},
      {.name = "brightness-list-backlight-devices", .summary = "List available sysfs backlight device names"},
      {.name = "brightness-osd", .summary = "Show brightness OSD"},
      {.name = "brightness-set",
       .summary = "Set brightness",
       .positionals = kBrightnessSetPositionals,
       .extraArgError = "error: usage: brightness-set [monitor-selector] <value>"},
      {.name = "brightness-up",
       .summary = "Increase brightness",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: brightness-up [step]"},
      {.name = "caffeine-disable", .summary = "Disable caffeine (idle inhibitor)"},
      {.name = "caffeine-enable", .summary = "Enable caffeine"},
      {.name = "caffeine-toggle", .summary = "Toggle caffeine"},
      {.name = "clipboard-clear", .summary = "Clear clipboard history"},
      {.name = "clipboard-copy", .summary = "Copy text to clipboard", .positionals = kClipboardCopyPositionals},
      {.name = "clipboard-text", .summary = "Print clipboard text"},
      {.name = "color-scheme-get", .summary = "Print active color scheme"},
      {.name = "color-scheme-set",
       .summary = "Set palette source and selection",
       .subcommands = kColorSchemeSubcommands},
      {.name = "config-reload", .summary = "Reload the config file"},
      {.name = "desktop-widgets-edit", .summary = "Open the desktop widgets editor"},
      {.name = "desktop-widgets-exit", .summary = "Close the desktop widgets editor"},
      {.name = "desktop-widgets-hide", .summary = "Hide desktop widgets now"},
      {.name = "desktop-widgets-show", .summary = "Show desktop widgets now"},
      {.name = "desktop-widgets-toggle", .summary = "Toggle desktop widgets visibility"},
      {.name = "desktop-widgets-toggle-edit", .summary = "Toggle desktop widgets edit mode"},
      {.name = "dock-hide", .summary = "Hide the dock"},
      {.name = "dock-reload", .summary = "Reload dock configuration"},
      {.name = "dock-show", .summary = "Show the dock"},
      {.name = "dock-toggle", .summary = "Toggle dock visibility"},
      {.name = "dpms-off", .summary = "Turn monitors off"},
      {.name = "dpms-on", .summary = "Turn monitors on"},
      {.name = "effects-profile-set",
       .summary = "Set the EasyEffects output or input profile",
       .positionals = kEffectsProfileSetPositionals,
       .extraArgError = "error: usage: effects-profile-set <output|input> <profile>"},
      {.name = "greeter-sync", .summary = "Sync wallpaper, colors, and monitor layout to Noctalia Greeter"},
      {.name = "keyboard-backlight-down",
       .summary = "Decrease all keyboard backlights",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: keyboard-backlight-down [step]"},
      {.name = "keyboard-backlight-osd", .summary = "Show keyboard backlight OSD"},
      {.name = "keyboard-backlight-set",
       .summary = "Set all keyboard backlights",
       .positionals = kKeyboardBacklightSetPositionals},
      {.name = "keyboard-backlight-toggle", .summary = "Toggle all keyboard backlights on/off"},
      {.name = "keyboard-backlight-up",
       .summary = "Increase all keyboard backlights",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: keyboard-backlight-up [step]"},
      {.name = "keyboard-layout-cycle", .summary = "Switch to the next keyboard layout"},
      {.name = "lockscreen-widgets-edit", .summary = "Open the lockscreen widgets editor"},
      {.name = "lockscreen-widgets-exit", .summary = "Close the lockscreen widgets editor"},
      {.name = "lockscreen-widgets-toggle-edit", .summary = "Toggle lockscreen widgets edit mode"},
      {.name = "log-level-set", .summary = "Set the console log level", .positionals = kLogLevelSetPositionals},
      {.name = "log-level-status", .summary = "Print the current console log level"},
      {.name = "media", .summary = "Control active media playback", .positionals = kMediaPositionals},
      {.name = "mic-mute", .summary = "Toggle microphone mute"},
      {.name = "mic-volume-down",
       .summary = "Decrease microphone volume",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: mic-volume-down [step]"},
      {.name = "mic-volume-osd",
       .summary = "Show the microphone volume OSD",
       .positionals = kOptionalValuePositionals,
       .extraArgError = "error: mic-volume-osd accepts at most one optional [value]"},
      {.name = "mic-volume-set", .summary = "Set microphone volume", .positionals = kMicVolumeSetPositionals},
      {.name = "mic-volume-up",
       .summary = "Increase microphone volume",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: mic-volume-up [step]"},
      {.name = "network-toggle", .summary = "Disconnect or reconnect network"},
      {.name = "nightlight-disable", .summary = "Disable night light schedule"},
      {.name = "nightlight-enable", .summary = "Enable night light schedule"},
      {.name = "nightlight-force-toggle", .summary = "Toggle forced night light mode"},
      {.name = "nightlight-toggle", .summary = "Toggle night light schedule"},
      {.name = "notification-clear-active", .summary = "Dismiss all currently active notifications"},
      {.name = "notification-clear-history", .summary = "Clear notification history"},
      {.name = "notification-dnd-set",
       .summary = "Set notification Do Not Disturb state",
       .positionals = kNotificationDndSetPositionals},
      {.name = "notification-dnd-status", .summary = "Print notification Do Not Disturb state"},
      {.name = "notification-dnd-toggle", .summary = "Toggle notification Do Not Disturb state"},
      {.name = "notification-invoke-latest", .summary = "Invoke default action of most recent notification"},
      {.name = "notification-show",
       .summary = "Show an internal Noctalia notification",
       .positionals = kNotificationPositionals},
      {.name = "osd-disable", .summary = "Disable all OSD popups"},
      {.name = "osd-enable", .summary = "Enable all OSD popups"},
      {.name = "osd-toggle", .summary = "Toggle all OSD popups"},
      {.name = "panel-close",
       .summary = "Close the active panel, or close the named panel",
       .positionals = kPanelClosePositionals},
      {.name = "panel-open",
       .summary = "Open a panel by id",
       .positionals = kPanelOpenTogglePositionals,
       .extraArgError = "error: usage: panel-open <id> [context]"},
      {.name = "panel-toggle",
       .summary = "Toggle a panel by id",
       .positionals = kPanelOpenTogglePositionals,
       .extraArgError = "error: usage: panel-toggle <id> [context]"},
      {.name = "plugin",
       .summary = "Dispatch an event to a plugin entry",
       .positionals = kPluginPositionals,
       .extraArgError = "error: usage: plugin <author/plugin:entry> <target[:bar-name]> <event> [payload]"},
      {.name = "plugins", .summary = "Manage plugins and sources", .subcommands = kMsgPluginsSubcommands},
      {.name = "power-cycle",
       .summary = "Step through UPower ordered profile list",
       .positionals = kPowerCyclePositionals,
       .extraArgError = "error: usage: power-cycle [direction]"},
      {.name = "power-set", .summary = "Set the UPower power profile", .positionals = kPowerSetPositionals},
      {.name = "screenshot-fullscreen",
       .summary = "Capture monitor or all outputs",
       .positionals = kMonitorSelectorOptionalPositionals,
       .extraArgError = "error: usage: screenshot-fullscreen [monitor-selector]"},
      {.name = "screenshot-region", .summary = "Start an interactive region screenshot"},
      {.name = "session", .summary = "Run a built-in session action", .positionals = kSessionPositionals},
      {.name = "settings-close", .summary = "Close the settings window"},
      {.name = "settings-open",
       .summary = "Open the settings window",
       .positionals = kSettingsContextPositionals,
       .extraArgError = "error: usage: settings-open [context]"},
      {.name = "settings-open-plugin",
       .summary = "Open the settings window at a plugin",
       .positionals = kSettingsOpenPluginPositionals},
      {.name = "settings-open-widget",
       .summary = "Open the settings window at a bar widget",
       .positionals = kSettingsOpenWidgetPositionals},
      {.name = "settings-toggle",
       .summary = "Toggle the settings window",
       .positionals = kSettingsContextPositionals,
       .extraArgError = "error: usage: settings-toggle [context]"},
      {.name = "status", .summary = "Print current state as JSON"},
      {.name = "taskbar-cycle", .summary = "Step to the adjacent task or workspace group"},
      {.name = "templates-apply", .summary = "Apply configured theme templates"},
      {.name = "theme-mode-get", .summary = "Print current resolved theme mode"},
      {.name = "theme-mode-set", .summary = "Set theme mode", .positionals = kThemeModePositionals},
      {.name = "theme-mode-toggle", .summary = "Toggle theme mode between dark and light"},
      {.name = "volume-down",
       .summary = "Decrease speaker volume",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: volume-down [step]"},
      {.name = "volume-mute", .summary = "Toggle speaker mute"},
      {.name = "volume-osd",
       .summary = "Show the volume OSD",
       .positionals = kOptionalValuePositionals,
       .extraArgError = "error: volume-osd accepts at most one optional [value]"},
      {.name = "volume-set", .summary = "Set speaker volume", .positionals = kVolumeSetPositionals},
      {.name = "volume-up",
       .summary = "Increase speaker volume",
       .positionals = kStepOptionalPositionals,
       .extraArgError = "error: usage: volume-up [step]"},
      {.name = "wallpaper-get",
       .summary = "Print default wallpaper path",
       .positionals = kWallpaperConnectorPositionals,
       .extraArgError = "error: usage: wallpaper-get [connector]"},
      {.name = "wallpaper-next",
       .summary = "Switch to the next wallpaper",
       .positionals = kWallpaperConnectorPositionals,
       .extraArgError = "error: usage: wallpaper-next [connector]"},
      {.name = "wallpaper-previous",
       .summary = "Switch to the previous wallpaper",
       .positionals = kWallpaperConnectorPositionals,
       .extraArgError = "error: usage: wallpaper-previous [connector]"},
      {.name = "wallpaper-random",
       .summary = "Switch to a random wallpaper",
       .positionals = kWallpaperConnectorPositionals,
       .extraArgError = "error: usage: wallpaper-random [connector]"},
      {.name = "wifi-disable", .summary = "Disable Wi-Fi"},
      {.name = "wifi-enable", .summary = "Enable Wi-Fi"},
      {.name = "wifi-status", .summary = "Print Wi-Fi state"},
      {.name = "wifi-toggle", .summary = "Toggle Wi-Fi"},
      {.name = "window-switcher",
       .summary = "Open or close the window switcher overlay",
       .positionals = kMonitorSelectorOptionalPositionals,
       .extraArgError = "error: usage: window-switcher [monitor-selector]"},
      {.name = "workspace-alert-add", .summary = "Add a workspace alert", .positionals = kWorkspaceAlertAddPositionals},
      {.name = "workspace-alert-add-window",
       .summary = "Add a workspace alert for a window",
       .positionals = kWorkspaceAlertAddWindowPositionals},
      {.name = "workspace-alert-clear",
       .summary = "Clear a workspace alert",
       .positionals = kWorkspaceAlertClearPositionals},
      {.name = "workspace-alert-clear-all", .summary = "Clear all workspace alerts"},
      {.name = "workspace-alert-status", .summary = "Print workspace alerts"},
      {.name = "workspace-switch", .summary = "Switch to the adjacent workspace", .positionals = kWorkspacePositionals}
  };

  inline constexpr std::span<const cli_schema::CliCommand> getIpcSubcommands() { return kIpcSubcommands; }

  inline constexpr const char* kNotificationHelpText = "Usage: noctalia msg notification-show <summary> <body>...\n"
                                                       "\n"
                                                       "Shows a desktop notification.\n";

  inline constexpr CliCommand kNotificationCmd = {
      .name = "notification-show",
      .summary = "Show an internal Noctalia notification",
      .helpText = kNotificationHelpText,
      .positionals = kNotificationPositionals
  };

  inline constexpr CliCommand kMsgCmd = {
      .name = "msg", .summary = "Send IPC commands to the running Noctalia instance", .subcommands = kIpcSubcommands
  };

} // namespace noctalia::ipc
