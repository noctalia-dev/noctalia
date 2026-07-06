// Round-trip + golden tests for the declarative config schema.
//
// The schema is now the single source for both serialize (config_export::serialize →
// writeTable) and parse (parseConfigTable → readInto), so there is no legacy code
// to compare against. What still earns its keep:
//   - read inverse — readInto(writeTable(x)) == x for every section: the schema's
//                    read and write are mutual inverses (catches a field whose read
//                    key != write key, or a lossy codec).
//   - bar golden   — config_export::serialize(probe)["bar"] stays byte-identical to a captured
//                    reference (locks the resolve-and-flatten monitor-override emit).
//   - clamp goldens — pin parse-time range behavior.

#include "config/config_export.h"
#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/input/key_chord.h"
#include "core/toml.h"
#include "scripting/plugin_id.h"

#include <cstdio>
#include <sstream>
#include <string>

using namespace noctalia::config::schema;

namespace {

  int g_failures = 0;

  void fail(const std::string& message) {
    std::fprintf(stderr, "config_schema_roundtrip: FAIL: %s\n", message.c_str());
    ++g_failures;
  }

  // Mirror of ConfigService::formatToml so serialized output matches exactly.
  std::string formatToml(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{
        table, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    return out.str();
  }

  // readInto(writeTable(x)) must reconstruct x. `serialized` is config_export::serialize(probe),
  // whose section emit IS writeTable(section), so this exercises the real schema
  // round-trip via the actual serializer.
  template <typename T>
  void checkReadInverse(
      const std::string& section, const toml::table& serialized, const T& expected, const Schema<T>& schema
  ) {
    const auto* sectionTbl = serialized[section].as_table();
    if (sectionTbl == nullptr) {
      fail(section + ": config_export::serialize emitted no [" + section + "] table");
      return;
    }
    T roundtrip{};
    Diagnostics diag;
    readInto(*sectionTbl, roundtrip, schema, section, diag);
    if (!(roundtrip == expected)) {
      fail(section + ": read inverse did not reconstruct the original value");
    }
  }

  void checkPluginSourceNameValidation() {
    const std::string valid[] = {"official", "my-repo", "team.plugins", "repo_2", "A1"};
    for (const auto& name : valid) {
      if (!isValidPluginSourceName(name)) {
        fail("plugins: rejected valid source name " + name);
      }
    }

    const std::string invalid[] = {"", ".", "..", "../repo", "repo/name", "repo name", "-repo", "_repo"};
    for (const auto& name : invalid) {
      if (isValidPluginSourceName(name)) {
        fail("plugins: accepted invalid source name " + name);
      }
    }

    const toml::table root = toml::parse(R"(
enabled = ["me/hello", "../bad", "missing-slash", "me/foo/bar"]

[[source]]
name = "good-repo"
kind = "git"
location = "https://example.invalid/good"

[[source]]
name = "../bad"
kind = "git"
location = "https://example.invalid/bad"
)");

    PluginsConfig plugins;
    Diagnostics diag;
    readInto(root, plugins, pluginsSchema(), "plugins", diag);
    if (plugins.sources.size() != 1 || plugins.sources[0].name != "good-repo") {
      fail("plugins: schema did not keep only valid source names");
    }
    if (plugins.enabled.size() != 1 || plugins.enabled[0] != "me/hello") {
      fail("plugins: schema did not keep only valid enabled plugin ids");
    }
    bool sawWarning = false;
    bool sawEnabledWarning = false;
    for (const auto& entry : diag.entries) {
      if (entry.severity == Diagnostics::Severity::Warning && entry.path == "plugins.source.name") {
        sawWarning = true;
      }
      if (entry.severity == Diagnostics::Severity::Warning && entry.path == "plugins.enabled") {
        sawEnabledWarning = true;
      }
    }
    if (!sawWarning) {
      fail("plugins: schema did not warn for invalid source name");
    }
    if (!sawEnabledWarning) {
      fail("plugins: schema did not warn for invalid enabled plugin id");
    }
  }

  void checkPluginIdValidation() {
    const std::string valid[] = {"noctalia/screen_recorder", "me/hello", "Team/repo_2", "a/b.c-d"};
    for (const auto& id : valid) {
      if (!scripting::isValidPluginId(id)) {
        fail("plugins: rejected valid plugin id " + id);
      }
      if (!scripting::pluginSubdirFromId(id).has_value()) {
        fail("plugins: did not derive subdir for valid plugin id " + id);
      }
    }

    const std::string invalid[] = {
        "", "hello", "me/", "/hello", "me/foo/bar", "me/../hello", "me/foo bar", "../foo", "me/.hidden"
    };
    for (const auto& id : invalid) {
      if (scripting::isValidPluginId(id)) {
        fail("plugins: accepted invalid plugin id " + id);
      }
      if (scripting::pluginSubdirFromId(id).has_value()) {
        fail("plugins: derived subdir for invalid plugin id " + id);
      }
    }
  }

  // A fully-specified bar with a fully-specified monitor override. Every override
  // optional is set so the resolve-and-flatten write round-trips back into the
  // same override on read (a partial override would come back fully resolved).
  BarConfig makeProbeBar() {
    BarConfig bar;
    bar.name = "default";
    bar.position = "bottom";
    bar.enabled = false;
    bar.autoHide = true;
    bar.showOnWorkspaceSwitch = true;
    bar.reserveSpace = false;
    bar.layer = "overlay";
    bar.thickness = 44;
    bar.backgroundOpacity = 0.85f;
    bar.border = colorSpecFromConfigString("#123456");
    bar.borderWidth = 2.0f;
    bar.radius = 18;
    bar.radiusTopLeft = 4;
    bar.radiusTopRight = 6;
    bar.radiusBottomLeft = 8;
    bar.radiusBottomRight = 10;
    bar.marginEnds = 100;
    bar.marginEdge = 5;
    bar.marginOppositeEdge = 12;
    bar.deadZone.command = "notify-send bar-left";
    bar.deadZone.rightCommand = "notify-send bar-right";
    bar.deadZone.middleCommand = "notify-send bar-middle";
    bar.deadZone.scrollUpCommand = "notify-send bar-scroll-up";
    bar.deadZone.scrollDownCommand = "notify-send bar-scroll-down";
    bar.padding = 12;
    bar.widgetSpacing = 8;
    bar.shadow = false;
    bar.contactShadow = true;
    bar.panelOverlap = 2;
    bar.capsuleThickness = 0.5f;
    bar.scale = 2.0f;
    bar.fontWeight = 600;
    bar.fontFamily = "Inter";
    bar.startWidgets = {"launcher"};
    bar.centerWidgets = {"clock", "weather"};
    bar.endWidgets = {"battery"};
    bar.widgetCapsuleDefault = true;
    bar.widgetCapsuleFill = colorSpecFromConfigString("#abcdef");
    bar.widgetCapsuleForeground = colorSpecFromConfigString("#fedcba");
    bar.widgetColor = colorSpecFromConfigString("#0a0b0c");
    bar.widgetIconColor = colorSpecFromConfigString("#0c0b0a");
    bar.widgetCapsulePadding = 16.0f;
    bar.widgetCapsuleRadius = 12.0;
    bar.widgetCapsuleOpacity = 0.9f;
    bar.widgetCapsuleBorderSpecified = true;
    bar.widgetCapsuleBorder = colorSpecFromConfigString("#111213");
    bar.hoverHighlight = false;
    BarCapsuleGroupStyle group;
    group.id = "grp1";
    group.members = {"clock", "weather"};
    group.fill = colorSpecFromConfigString("#222324");
    group.borderSpecified = true;
    group.border = colorSpecFromConfigString("#333435");
    group.foreground = colorSpecFromConfigString("#444546");
    group.padding = 20.0f;
    group.radius = 14.0f;
    group.opacity = 0.8f;
    bar.widgetCapsuleGroups = {group};

    BarMonitorOverride ovr;
    ovr.match = "DP-1";
    ovr.position = "top";
    ovr.enabled = true;
    ovr.autoHide = false;
    ovr.showOnWorkspaceSwitch = true;
    ovr.reserveSpace = true;
    ovr.layer = "top";
    ovr.thickness = 50;
    ovr.backgroundOpacity = 0.7f;
    ovr.border = colorSpecFromConfigString("#a1a2a3");
    ovr.borderWidth = 3.0f;
    ovr.radius = 22;
    ovr.radiusTopLeft = 1;
    ovr.radiusTopRight = 2;
    ovr.radiusBottomLeft = 3;
    ovr.radiusBottomRight = 4;
    ovr.marginEnds = 70;
    ovr.marginEdge = 9;
    ovr.marginOppositeEdge = 4;
    ovr.deadZone.command = "notify-send bar-left";
    ovr.deadZone.rightCommand = "notify-send bar-right";
    ovr.deadZone.middleCommand = "notify-send monitor-middle";
    ovr.deadZone.scrollUpCommand = "notify-send monitor-scroll-up";
    ovr.deadZone.scrollDownCommand = "notify-send bar-scroll-down";
    ovr.padding = 11;
    ovr.widgetSpacing = 7;
    ovr.shadow = true;
    ovr.contactShadow = false;
    ovr.panelOverlap = -1;
    ovr.capsuleThickness = 0.25f;
    ovr.scale = 1.5f;
    ovr.fontFamily = "Fira Sans";
    ovr.startWidgets = std::vector<std::string>{"tray"};
    ovr.centerWidgets = std::vector<std::string>{"media"};
    ovr.endWidgets = std::vector<std::string>{"volume"};
    ovr.widgetCapsuleDefault = false;
    ovr.widgetCapsuleFill = colorSpecFromConfigString("#b1b2b3");
    ovr.widgetCapsuleBorderSpecified = true;
    ovr.widgetCapsuleBorder = colorSpecFromConfigString("#c1c2c3");
    ovr.widgetCapsuleForeground = colorSpecFromConfigString("#d1d2d3");
    ovr.widgetColor = colorSpecFromConfigString("#e1e2e3");
    ovr.widgetIconColor = colorSpecFromConfigString("#e3e2e1");
    ovr.hoverHighlight = true;
    BarCapsuleGroupStyle ogroup;
    ogroup.id = "ogrp";
    ogroup.members = {"volume"};
    ogroup.fill = colorSpecFromConfigString("#f1f2f3");
    ogroup.borderSpecified = true;
    ogroup.border = colorSpecFromConfigString("#0f0e0d");
    ogroup.foreground = colorSpecFromConfigString("#0c0b0a");
    ogroup.padding = 18.0f;
    ogroup.radius = 9.0f;
    ogroup.opacity = 0.6f;
    ovr.widgetCapsuleGroups = std::vector<BarCapsuleGroupStyle>{ogroup};
    ovr.widgetCapsulePadding = 24.0;
    ovr.widgetCapsuleRadius = 30.0;
    ovr.widgetCapsuleOpacity = 0.5;
    bar.monitorOverrides = {ovr};
    return bar;
  }

  // Build a config whose migrated sections hold non-default values, so parity
  // checks exercise real serialization rather than all-defaults.
  Config makeProbe() {
    Config c;
    c.audio = AudioConfig{true, true, 0.73f, "change.ogg", "notify.ogg"};
    c.weather = WeatherConfig{false, false, 17, "imperial"};
    c.osd.position = "bottom_left";
    c.osd.positionVertical = "top_right";
    c.osd.orientation = "vertical";
    c.osd.scale = 1.4f;
    c.osd.backgroundOpacity = 0.42f;
    c.osd.offsetX = 33;
    c.osd.offsetY = 11;
    c.osd.monitors = {"DP-1", "HDMI-A-1"};
    c.osd.kinds.lockKeys = false;
    c.osd.kinds.keyboardLayout = false;
    c.backdrop = BackdropConfig{true, 0.8f, 0.2f};
    c.lockscreen = LockscreenConfig{
        .blurredDesktop = true, .blurIntensity = 0.6f, .tintIntensity = 0.25f, .monitors = {"DP-1"}
    };
    c.system.monitor.enabled = false;
    c.system.monitor.cpuTempSensorPath = "/sys/class/hwmon/hwmon3/temp1_input";
    c.system.monitor.cpuPollSeconds = 5.0f;
    c.system.monitor.gpuPollSeconds = 4.0f;
    c.system.monitor.memoryPollSeconds = 6.0f;
    c.system.monitor.networkPollSeconds = 7.0f;
    c.system.monitor.diskPollSeconds = 12.0f;
    c.nightlight = NightLightConfig{true, true, 6000, 3500}; // gap satisfied
    c.location.autoLocate = true;
    c.location.address = "Berlin";
    c.location.sunset = "20:30";
    c.location.sunrise = "06:15";
    c.location.latitude = 52.52;
    c.location.longitude = 13.405;
    c.notification = NotificationConfig{
        false,
        false,
        false,
        "bottom_left",
        "overlay",
        1.3f,
        0.5f,
        12,
        6,
        {"DP-2"},
        false,
        {NotificationFilterConfig{
            .name = "discord",
            .enabled = true,
            .match = "discord",
            .showToast = false,
            .saveHistory = false,
            .playSound = false,
            .allowPermanent = false,
            .allowedUrgencies = {"normal", "critical"},
        }},
    };
    c.dock.enabled = true;
    c.dock.position = DockEdge::Left;
    c.dock.iconSize = 40;
    c.dock.radius = 20;
    c.dock.radiusTopLeft = 10;
    c.dock.radiusTopRight = 12;
    c.dock.radiusBottomLeft = 14;
    c.dock.radiusBottomRight = 16;
    c.dock.launcherPosition = DockLauncherPosition::Start;
    c.dock.pinned = {"firefox.desktop"};
    c.dock.monitors = {"DP-1"};
    c.brightness.enableDdcutil = true;
    c.brightness.ddcutilIgnoreMmids = {"ABC123"};
    c.brightness.monitorOverrides = {
        {"DP-1", BrightnessBackendPreference::Ddcutil},
        {"eDP-1", std::nullopt},
    };
    c.battery.warningThreshold = 15;
    c.battery.deviceThresholds = {{"BAT0", 10}, {"hidpp:1", 25}};
    c.controlCenter.sidebarMode = ControlCenterSidebarMode::Full;
    c.controlCenter.sidebarSectionMode = ControlCenterSidebarMode::None;
    c.controlCenter.calendarTab.showEventsCard = false;
    c.controlCenter.shortcuts = {{"wifi"}, {"bluetooth"}};
    c.calendar.enabled = true;
    c.calendar.refreshMinutes = 30;
    c.calendar.accounts = {
        {"acc1", "google", "Work", "#ff0000", "", "", "", {}},
        {"acc2", "caldav", "Home", "", "custom", "https://dav.example.com/remote.php/dav/", "user", {"personal"}},
    };
    // Explicit chords so write→read round-trips (empty would emit defaults instead).
    c.keybinds.validate = {*parseKeyChordSpec("Return")};
    c.keybinds.cancel = {*parseKeyChordSpec("Escape")};
    c.keybinds.left = {*parseKeyChordSpec("Left")};
    c.keybinds.right = {*parseKeyChordSpec("Right")};
    c.keybinds.up = {*parseKeyChordSpec("Up")};
    c.keybinds.down = {*parseKeyChordSpec("Down")};
    c.keybinds.tabNext = defaultKeybindSet(KeybindAction::TabNext);
    c.keybinds.tabPrevious = defaultKeybindSet(KeybindAction::TabPrevious);
    c.hooks.commands[0] = {"notify-send hi"};
    c.hooks.commands[2] = {"cmd-a", "cmd-b"};
    c.idle.preActionFadeSeconds = 3.0f;
    // Explicit normalized actions so normalizeIdleBehaviorAction is a no-op on read.
    c.idle.behaviors = {
        {"dim", true, 60, "lock", "", "", true},
        {"off", false, 300, "screen_off", "", "", true},
    };
    c.wallpaper.enabled = false;
    c.wallpaper.fillColor = colorSpecFromConfigString("#ff8800");
    c.wallpaper.transitions = {WallpaperTransition::Wipe, WallpaperTransition::Zoom};
    c.wallpaper.transitionDurationMs = 2000.0f;
    c.wallpaper.edgeSmoothness = 0.5f;
    c.wallpaper.directory = "/srv/wallpapers"; // absolute: expandUserPath leaves it unchanged
    c.wallpaper.automation.enabled = true;
    c.wallpaper.automation.intervalSeconds = 30;
    c.wallpaper.automation.order = WallpaperAutomationConfig::Order::Alphabetical;
    c.wallpaper.monitorOverrides = {
        {"DP-1", true, colorSpecFromConfigString("#00ff00"), std::string("/srv/wp1"), std::nullopt, std::nullopt},
    };
    c.shell.uiScale = 1.25f;
    c.shell.fontFamily = "Inter";
    c.shell.lang = "en_US";
    c.shell.timeFormat = "{:%H:%M:%S}";
    c.shell.passwordMaskStyle = PasswordMaskStyle::RandomIcons;
    c.shell.clipboardHistoryMaxEntries = 80;
    c.shell.clipboardAutoPaste = ClipboardAutoPasteMode::CtrlV;
    c.shell.avatarPath = "/home/u/face.png";
    c.shell.animation.speed = 1.5f;
    c.shell.shadow.direction = ShadowDirection::UpLeft;
    c.shell.panel.transparencyMode = PanelTransparencyMode::Glass;
    c.shell.panel.launcherPlacement = PanelPlacement::Floating;
    c.shell.launcher.compact = true;
    c.shell.launcher.sessionSearch = true;
    c.shell.launcher.sortByUsage = false;
    c.shell.screenCorners.enabled = true;
    c.shell.screenCorners.size = 24;
    c.shell.mpris.blacklist = {"firefox"};
    c.shell.screenshot.directory = "/shots";
    c.shell.screenshot.pipeToCommand = true;
    c.shell.session.actions = {
        SessionPanelActionConfig{
            "lock",
            true,
            std::nullopt,
            std::string("Lock"),
            std::string("lock"),
            SessionActionButtonVariant::Primary,
            parseKeyChordSpec("Ctrl+l"),
        },
        SessionPanelActionConfig{
            "shutdown", false, std::nullopt, std::nullopt, std::nullopt, SessionActionButtonVariant::Destructive,
            std::nullopt
        },
    };
    c.shell.session.power.suspend = "zzz";
    c.shell.session.power.reboot = "sudo -n reboot";
    c.shell.session.power.shutdown = "sudo -n poweroff";
    c.theme.source = PaletteSource::Wallpaper;
    c.theme.builtinPalette = "Tokyo";
    c.theme.mode = ThemeMode::Light;
    c.theme.templates.enableBuiltinTemplates = false;
    c.theme.templates.builtinIds = {"a", "b"};
    c.theme.templates.customColors = {{"accent", "#112233", true}, {"bg", "#000000", false}};
    c.theme.templates.userTemplates = {
        ThemeConfig::UserTemplateConfig{
            "tmpl1",
            true,
            "/in.png",
            ThemeConfig::TemplateInputPathModesConfig{"/d.png", "/l.png"},
            {"/out1", "/out2"},
            "/dyn",
            "compareX",
            {{"c1", "#aabbcc"}},
            "pre",
            "post",
            3,
        },
    };
    c.bars = {makeProbeBar()};
    return c;
  }

  void checkClamps() {
    // sound_volume above the max clamps to 1.0.
    {
      auto t = toml::parse("sound_volume = 2.5");
      AudioConfig a{};
      Diagnostics d;
      readInto(t, a, audioSchema(), "audio", d);
      if (a.soundVolume != 1.0f) {
        fail("audio.sound_volume clamp: expected 1.0");
      }
    }
    // osd.offset_x has a min-only floor at 0.
    {
      auto t = toml::parse("offset_x = -5");
      OsdConfig o{};
      Diagnostics d;
      readInto(t, o, osdSchema(), "osd", d);
      if (o.offsetX != 0) {
        fail("osd.offset_x floor: expected 0");
      }
    }
    // Unknown enum-like string is left untouched on a plain string field (no enum here),
    // so just verify osd.scale below the min clamps up.
    {
      auto t = toml::parse("scale = 0.1");
      OsdConfig o{};
      Diagnostics d;
      readInto(t, o, osdSchema(), "osd", d);
      if (o.scale != 0.5f) {
        fail("osd.scale clamp: expected 0.5");
      }
    }
    // Clipboard history count accepts large text-heavy histories but still has
    // an explicit config ceiling.
    {
      auto t = toml::parse("clipboard_history_max_entries = 25000");
      ShellConfig s{};
      Diagnostics d;
      readInto(t, s, shellSchema(), "shell", d);
      if (s.clipboardHistoryMaxEntries != 10000) {
        fail("shell.clipboard_history_max_entries clamp: expected 10000");
      }
    }
  }

} // namespace

int main() {
  // Captured from the pre-refactor config_export::serialize for the fully-specified probe
  // bar. Pins byte-identical bar serialization across the schema migration: the
  // resolve-and-flatten monitor write and the conditional/optional fields must
  // emit exactly these bytes.
  const char* const kBarGolden =
      R"(order = [ "default" ]

[default]
auto_hide = true
background_opacity = 0.85000002384185791
border = "#123456"
border_width = 2.0
capsule = true
capsule_border = "#111213"
capsule_fill = "#ABCDEF"
capsule_foreground = "#FEDCBA"
capsule_opacity = 0.89999997615814209
capsule_padding = 16.0
capsule_radius = 12.0
capsule_thickness = 0.5
center = [ "clock", "weather" ]
color = "#0A0B0C"
contact_shadow = true
enabled = false
end = [ "battery" ]
font_family = "Inter"
font_weight = 600
hover_highlight = false
icon_color = "#0C0B0A"
layer = "overlay"
margin_edge = 5
margin_ends = 100
margin_opposite_edge = 12
padding = 12
panel_overlap = 2
position = "bottom"
radius = 18
radius_bottom_left = 8
radius_bottom_right = 10
radius_top_left = 4
radius_top_right = 6
reserve_space = false
scale = 2.0
shadow = false
show_on_workspace_switch = true
start = [ "launcher" ]
thickness = 44
widget_spacing = 8

    [default.dead_zone]
    command = "notify-send bar-left"
    middle_command = "notify-send bar-middle"
    right_command = "notify-send bar-right"
    scroll_down_command = "notify-send bar-scroll-down"
    scroll_up_command = "notify-send bar-scroll-up"

    [default.monitor.DP-1]
    auto_hide = false
    background_opacity = 0.69999998807907104
    border = "#A1A2A3"
    border_width = 3.0
    capsule = false
    capsule_border = "#C1C2C3"
    capsule_fill = "#B1B2B3"
    capsule_foreground = "#D1D2D3"
    capsule_opacity = 0.5
    capsule_padding = 24.0
    capsule_radius = 30.0
    capsule_thickness = 0.25
    center = [ "media" ]
    color = "#E1E2E3"
    contact_shadow = false
    enabled = true
    end = [ "volume" ]
    font_family = "Fira Sans"
    font_weight = 600
    hover_highlight = true
    icon_color = "#E3E2E1"
    layer = "top"
    margin_edge = 9
    margin_ends = 70
    margin_opposite_edge = 4
    match = "DP-1"
    padding = 11
    panel_overlap = -1
    position = "top"
    radius = 22
    radius_bottom_left = 3
    radius_bottom_right = 4
    radius_top_left = 1
    radius_top_right = 2
    reserve_space = true
    scale = 1.5
    shadow = true
    show_on_workspace_switch = true
    start = [ "tray" ]
    thickness = 50
    widget_spacing = 7

        [default.monitor.DP-1.dead_zone]
        command = "notify-send bar-left"
        middle_command = "notify-send monitor-middle"
        right_command = "notify-send bar-right"
        scroll_down_command = "notify-send bar-scroll-down"
        scroll_up_command = "notify-send monitor-scroll-up"

        [[default.monitor.DP-1.capsule_group]]
        border = "#0F0E0D"
        enabled = true
        fill = "#F1F2F3"
        foreground = "#0C0B0A"
        id = "ogrp"
        members = [ "volume" ]
        opacity = 0.60000002384185791
        padding = 18.0
        radius = 9.0

    [[default.capsule_group]]
    border = "#333435"
    enabled = true
    fill = "#222324"
    foreground = "#444546"
    id = "grp1"
    members = [ "clock", "weather" ]
    opacity = 0.80000001192092896
    padding = 20.0
    radius = 14.0)";

  const Config probe = makeProbe();
  const toml::table serialized = config_export::serialize(probe);

  // Bar: write parity against the captured golden, plus read-inverse via the
  // schemas (reconstructing the bar exactly as config_service does).
  {
    const std::string fresh = formatToml(*serialized["bar"].as_table());
    if (fresh != kBarGolden) {
      fail(
          "bar: serialization drifted from golden\n--- golden ---\n"
          + std::string(kBarGolden)
          + "\n--- fresh ---\n"
          + fresh
      );
    }
  }
  {
    const auto* barTbl = serialized["bar"]["default"].as_table();
    BarConfig rt;
    rt.name = "default";
    Diagnostics diag;
    if (auto v = (*barTbl)["position"].value<std::string>()) {
      rt.position = *v;
    }
    readInto(*barTbl, rt, barFieldsSchema(), "bar.default", diag);
    if (const auto* monMap = (*barTbl)["monitor"].as_table()) {
      for (const auto& [monName, monNode] : *monMap) {
        if (const auto* monTbl = monNode.as_table()) {
          BarMonitorOverride ovr;
          ovr.match = std::string(monName.str());
          readInto(*monTbl, ovr, barMonitorOverrideSchema(), "bar.default.monitor", diag);
          rt.monitorOverrides.push_back(std::move(ovr));
        }
      }
    }
    if (!(rt == probe.bars[0])) {
      fail("bar: read inverse did not reconstruct the original bar (incl. monitor override)");
    }
  }

  checkReadInverse("audio", serialized, probe.audio, audioSchema());
  checkReadInverse("weather", serialized, probe.weather, weatherSchema());
  checkReadInverse("osd", serialized, probe.osd, osdSchema());
  checkReadInverse("backdrop", serialized, probe.backdrop, backdropSchema());
  checkReadInverse("lockscreen", serialized, probe.lockscreen, lockscreenSchema());
  checkReadInverse("system", serialized, probe.system, systemSchema());
  checkReadInverse("nightlight", serialized, probe.nightlight, nightlightSchema());
  checkReadInverse("location", serialized, probe.location, locationSchema());
  checkReadInverse("notification", serialized, probe.notification, notificationSchema());
  checkReadInverse("dock", serialized, probe.dock, dockSchema());
  checkReadInverse("brightness", serialized, probe.brightness, brightnessSchema());
  checkReadInverse("battery", serialized, probe.battery, batterySchema());
  checkReadInverse("control_center", serialized, probe.controlCenter, controlCenterSchema());
  checkReadInverse("calendar", serialized, probe.calendar, calendarSchema());
  checkReadInverse("keybinds", serialized, probe.keybinds, keybindsSchema());
  checkReadInverse("hooks", serialized, probe.hooks, hooksSchema());
  checkReadInverse("idle", serialized, probe.idle, idleSchema());
  checkReadInverse("wallpaper", serialized, probe.wallpaper, wallpaperSchema());
  checkReadInverse("theme", serialized, probe.theme, themeSchema());
  checkReadInverse("shell", serialized, probe.shell, shellSchema());

  checkPluginIdValidation();
  checkPluginSourceNameValidation();
  checkClamps();

  if (g_failures == 0) {
    std::puts("config_schema_roundtrip: all checks passed");
    return 0;
  }
  std::fprintf(stderr, "config_schema_roundtrip: %d failure(s)\n", g_failures);
  return 1;
}
