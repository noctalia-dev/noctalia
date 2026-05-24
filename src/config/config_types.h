#pragma once

#include "core/key_chord.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

struct WaylandOutput;

struct BarMonitorOverride {
  std::string match;
  std::optional<bool> enabled;
  std::optional<bool> autoHide;
  std::optional<bool> reserveSpace;
  std::optional<std::int32_t> thickness;
  std::optional<float> backgroundOpacity;
  std::optional<ColorSpec> border;
  std::optional<float> borderWidth;
  std::optional<std::int32_t> radius;
  std::optional<std::int32_t> radiusTopLeft;
  std::optional<std::int32_t> radiusTopRight;
  std::optional<std::int32_t> radiusBottomLeft;
  std::optional<std::int32_t> radiusBottomRight;
  std::optional<std::int32_t> marginEnds;    // inset from each end of the bar along its main axis
  std::optional<std::int32_t> marginEdge;    // distance from the nearest screen edge (floats the bar when > 0)
  std::optional<std::int32_t> padding;       // main-axis padding from bar edges to start/end sections
  std::optional<std::int32_t> widgetSpacing; // gap between widgets within a section
  std::optional<bool> shadow;                // use the global shell shadow on this bar
  std::optional<bool> contactShadow;         // dark gradient between attached panel and bar
  std::optional<float> scale;
  std::optional<std::vector<std::string>> startWidgets;
  std::optional<std::vector<std::string>> centerWidgets;
  std::optional<std::vector<std::string>> endWidgets;
  std::optional<bool> widgetCapsuleDefault;
  std::optional<ColorSpec> widgetCapsuleFill;
  bool widgetCapsuleBorderSpecified = false;
  std::optional<ColorSpec> widgetCapsuleBorder;
  std::optional<ColorSpec> widgetCapsuleForeground;
  std::optional<ColorSpec> widgetColor;
  std::optional<std::vector<std::string>> widgetCapsuleGroups;
  std::optional<double> widgetCapsulePadding;
  std::optional<double> widgetCapsuleRadius;
  std::optional<double> widgetCapsuleOpacity;

  bool operator==(const BarMonitorOverride&) const = default;
};

struct BarConfig {
  std::string name = "default";
  std::string position = "top";
  bool enabled = true;
  bool autoHide = false;    // slide out when the pointer leaves; reveal on edge approach
  bool reserveSpace = true; // reserve compositor exclusive zone when auto_hide is false
  std::int32_t thickness = Style::barThicknessDefault;
  float backgroundOpacity = 1.0f;
  // Inside outline for the bar background; attached panels inherit the resolved values.
  ColorSpec border = colorSpecFromRole(ColorRole::Outline);
  float borderWidth = 0.0f;
  std::int32_t radius = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusTopLeft = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusTopRight = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusBottomLeft = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t radiusBottomRight = static_cast<std::int32_t>(Style::radiusXl);
  std::int32_t marginEnds = 180;  // inset from each end of the bar along its main axis
  std::int32_t marginEdge = 10;   // distance from the nearest screen edge (floats the bar when > 0)
  std::int32_t padding = 14;      // main-axis padding from bar edges to start/end sections
  std::int32_t widgetSpacing = 6; // gap between widgets within a section
  bool shadow = true;             // use the global shell shadow
  bool contactShadow = false;     // dark gradient between attached panel and bar
  float scale = 1.0f;             // content scale multiplier for glyphs and text
  int fontWeight = 500;           // primary label weight for bar widgets
  std::vector<std::string> startWidgets = {"launcher", "wallpaper", "workspaces"};
  std::vector<std::string> centerWidgets = {"clock"};
  std::vector<std::string> endWidgets = {"media",   "tray",           "notifications", "clipboard",
                                         "network", "bluetooth",      "volume",        "brightness",
                                         "battery", "control-center", "session"};
  // When true, widgets on this bar use a capsule unless `[widget.*] capsule = false`.
  bool widgetCapsuleDefault = false;
  ColorSpec widgetCapsuleFill = colorSpecFromRole(ColorRole::SurfaceVariant);
  // When set, bar widgets with capsules use this for icon + primary label color unless overridden per widget.
  std::optional<ColorSpec> widgetCapsuleForeground;
  // Default icon + primary label color for all widgets on this bar (same as per-widget `color`); per-widget `color`
  // overrides.
  std::optional<ColorSpec> widgetColor;
  std::vector<std::string> widgetCapsuleGroups;
  // Inner padding between capsule edge and widget content (logical px), multiplied by widget content scale on the bar.
  float widgetCapsulePadding = Style::barCapsulePadding;
  // Capsule corner radius in logical pixels before content-scale; unset means automatic pill radius.
  std::optional<double> widgetCapsuleRadius;
  // Capsule background opacity multiplier (0.0–1.0).
  float widgetCapsuleOpacity = 1.0f;
  // True when `capsule_border` appears under `[bar.*]` (empty value = no outline for widgets that inherit border).
  bool widgetCapsuleBorderSpecified = false;
  std::optional<ColorSpec> widgetCapsuleBorder;
  std::vector<BarMonitorOverride> monitorOverrides;

  bool operator==(const BarConfig&) const = default;
};

struct ShortcutConfig {
  std::string type;
  bool operator==(const ShortcutConfig&) const = default;
};

enum class SessionActionButtonVariant : std::uint8_t {
  Default,
  Primary,
  Secondary,
  Destructive,
  Outline,
  Ghost,
};

struct SessionPanelActionConfig {
  // "lock" | "logout" | "suspend" | "reboot" | "shutdown" | "command"
  std::string action;
  bool enabled = true;
  // When set, runs via `process::runAsync` (shell string) instead of the built-in handler.
  std::optional<std::string> command;
  std::optional<std::string> label;
  std::optional<std::string> glyph;
  SessionActionButtonVariant variant = SessionActionButtonVariant::Default;
  std::optional<KeyChord> shortcut;

  bool operator==(const SessionPanelActionConfig&) const = default;
};

struct ShellSessionConfig {
  std::vector<SessionPanelActionConfig> actions;

  bool operator==(const ShellSessionConfig&) const = default;
};

struct IdleBehaviorConfig {
  std::string name;
  bool enabled = true;
  std::int32_t timeoutSeconds = 0;
  /// lock | screen_off | suspend | command (custom shell strings)
  std::string action;
  std::string command;
  std::string resumeCommand;
  /// When `action` is `suspend`, lock the session before running suspend so lock surfaces are ready (recommended).
  bool lockBeforeSuspend = true;

  bool operator==(const IdleBehaviorConfig&) const = default;
};

struct IdleConfig {
  std::vector<IdleBehaviorConfig> behaviors;
  /// When > 0, after the compositor reports idle the shell fades a fullscreen overlay (surface color)
  /// from transparent to opaque over this many seconds, then runs `command`. Compositor activity during
  /// the fade cancels. When 0, the idle command runs immediately with no overlay.
  float preActionFadeSeconds = 2.0f;
};

[[nodiscard]] std::vector<ShortcutConfig> defaultControlCenterShortcuts();
[[nodiscard]] std::vector<SessionPanelActionConfig> defaultSessionPanelActions();
[[nodiscard]] std::vector<IdleBehaviorConfig> defaultIdleBehaviors();

enum class IdleActionKind : std::uint8_t {
  None = 0,
  Command,
  Lock,
  ScreenOff,
  ScreenOn,
  Suspend,
};

struct IdleActionRequest {
  IdleActionKind kind = IdleActionKind::None;
  std::string command;
  bool lockBeforeSuspend = true;

  bool operator==(const IdleActionRequest&) const = default;
};

struct ResolvedIdleBehavior {
  IdleActionRequest idleAction;
  IdleActionRequest resumeAction;

  bool operator==(const ResolvedIdleBehavior&) const = default;
};

void inferIdleBehaviorActionFromLegacyFields(IdleBehaviorConfig& behavior);
[[nodiscard]] ResolvedIdleBehavior resolveIdleBehaviorActions(const IdleBehaviorConfig& behavior);

enum class KeybindAction : std::uint8_t {
  Validate = 0,
  Cancel = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
};

[[nodiscard]] std::vector<KeyChord> defaultKeybindSet(KeybindAction action);

using WidgetSettingValue = std::variant<bool, std::int64_t, double, std::string, std::vector<std::string>>;
using ConfigOverrideValue = std::variant<
    bool, std::int64_t, double, std::string, std::vector<std::string>, std::vector<ShortcutConfig>,
    std::vector<SessionPanelActionConfig>, std::vector<IdleBehaviorConfig>, std::vector<KeyChord>>;

// Optional rounded “capsule” behind a bar widget (see `[widget.*] capsule_*` in CONFIG.md).
// Corner shape, border width, and edge softness are fixed in the shell code; padding/radius are configurable.
struct WidgetBarCapsuleSpec {
  bool enabled = false;
  ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant);
  // Adjacent widgets in the same section with the same non-empty group and identical capsule styling share one shell.
  std::string group;
  // Set only when `capsule_border` is present and non-empty in config; otherwise no outline.
  std::optional<ColorSpec> border;
  // Icon + primary label color when the capsule is visible; unset = widget defaults.
  std::optional<ColorSpec> foreground;
  // Inner padding in logical pixels before content-scale (see `capsule_padding` / bar default).
  float padding = Style::barCapsulePadding;
  // Corner radius in logical pixels before content-scale; unset means automatic pill radius.
  std::optional<float> radius;
  // Capsule background opacity multiplier (0.0–1.0).
  float opacity = 1.0f;

  bool operator==(const WidgetBarCapsuleSpec&) const = default;
};

struct WidgetConfig {
  std::string type; // widget type (e.g. "clock", "spacer"); defaults to the entry name
  std::unordered_map<std::string, WidgetSettingValue> settings;

  [[nodiscard]] std::string getString(const std::string& key, const std::string& fallback = {}) const;
  [[nodiscard]] std::vector<std::string>
  getStringList(const std::string& key, const std::vector<std::string>& fallback = {}) const;
  [[nodiscard]] std::int64_t getInt(const std::string& key, std::int64_t fallback = 0) const;
  [[nodiscard]] double getDouble(const std::string& key, double fallback = 0.0) const;
  [[nodiscard]] bool getBool(const std::string& key, bool fallback = false) const;
  [[nodiscard]] ColorSpec
  getColorSpec(const std::string& key, const ColorSpec& fallback, std::string_view context = {}) const;
  [[nodiscard]] std::optional<ColorSpec>
  getOptionalColorSpec(const std::string& key, std::string_view context = {}) const;
  [[nodiscard]] bool hasSetting(const std::string& key) const;

  bool operator==(const WidgetConfig&) const = default;
};

// Merges `[bar.*]` capsule defaults with `[widget.*]` overrides (see CONFIG.md). Size/style fields such as
// `radius` are populated even when `enabled` is false so widgets can reuse capsule styling internally.
[[nodiscard]] WidgetBarCapsuleSpec resolveWidgetBarCapsuleSpec(const BarConfig& bar, const WidgetConfig* widget);

// Color spec for user color strings: either a palette color role token or a hex color.
[[nodiscard]] ColorSpec colorSpecFromConfigString(const std::string& raw, std::string_view context = {});

// Shared output selector matching used by monitor-scoped config and IPC selectors.
// Matches connector name exactly, or a word-boundary token within output description.
[[nodiscard]] bool outputMatchesSelector(const std::string& match, const WaylandOutput& output);

enum class WallpaperFillMode : std::uint8_t {
  Center = 0,
  Crop = 1,
  Fit = 2,
  Stretch = 3,
  Repeat = 4,
};

enum class WallpaperTransition : std::uint8_t {
  Fade = 0,
  Wipe = 1,
  Disc = 2,
  Stripes = 3,
  Zoom = 4,
  Honeycomb = 5,
};

struct WallpaperMonitorOverride {
  std::string match;
  std::optional<bool> enabled;
  std::optional<ColorSpec> fillColor;
  std::optional<std::string> directory;
  std::optional<std::string> directoryLight;
  std::optional<std::string> directoryDark;
};

struct WallpaperAutomationConfig {
  enum class Order : std::uint8_t {
    Random = 0,
    Alphabetical = 1,
  };

  bool enabled = false;
  std::int32_t intervalMinutes = 0; // 0 = disabled
  Order order = Order::Random;
  bool recursive = true;
};

struct WallpaperConfig {
  bool enabled = true;
  WallpaperFillMode fillMode = WallpaperFillMode::Crop;
  std::optional<ColorSpec> fillColor;
  std::vector<WallpaperTransition> transitions = {WallpaperTransition::Fade, WallpaperTransition::Wipe,
                                                  WallpaperTransition::Disc, WallpaperTransition::Stripes,
                                                  WallpaperTransition::Zoom, WallpaperTransition::Honeycomb};
  float transitionDurationMs = 1500.0f;
  float edgeSmoothness = 0.3f;
  std::string directory;
  std::string directoryLight;
  std::string directoryDark;
  bool perMonitorDirectories = false;
  WallpaperAutomationConfig automation;
  std::vector<WallpaperMonitorOverride> monitorOverrides;
};

struct BackdropConfig {
  bool enabled = false;
  float blurIntensity = 0.5f;
  float tintIntensity = 0.3f;
};

struct DockConfig {
  bool enabled = false;            // opt-in; dock is hidden by default
  std::string position = "bottom"; // top | bottom | left | right
  bool activeMonitorOnly = false;  // render only on preferred active output
  std::int32_t iconSize = 48;      // icon size in pixels (before ui_scale)
  std::int32_t padding = 8;        // inner padding around the icon row
  std::int32_t itemSpacing = 6;    // gap between items
  float backgroundOpacity = 0.88f;
  std::int32_t radius = 16;               // dock background corner radius
  std::int32_t radiusTopLeft = 16;        // dock background top-left corner radius
  std::int32_t radiusTopRight = 16;       // dock background top-right corner radius
  std::int32_t radiusBottomLeft = 16;     // dock background bottom-left corner radius
  std::int32_t radiusBottomRight = 16;    // dock background bottom-right corner radius
  std::int32_t marginEnds = 0;            // inset from each end of the dock along its main axis
  std::int32_t marginEdge = 8;            // distance from the nearest screen edge (floats the dock when > 0)
  bool shadow = true;                     // use the global shell shadow
  bool showRunning = true;                // also show running apps not in pinned list
  bool autoHide = false;                  // fade out when not hovered (overlay mode)
  bool reserveSpace = false;              // keep compositor exclusive zone even while auto-hidden
  float activeScale = 1.0f;               // focused app icon scale
  float inactiveScale = 0.85f;            // non-focused app icon scale
  float activeOpacity = 1.0f;             // focused app icon opacity
  float inactiveOpacity = 0.85f;          // non-focused app icon opacity
  bool showDots = false;                  // show optional running window dots beside app icons
  bool showInstanceCount = true;          // show a badge with count when app has >1 window
  std::string launcherPosition = "none";  // none | start | end
  std::string launcherIcon = "grid-dots"; // Tabler glyph name
  std::vector<std::string> pinned;        // desktop entry IDs to always show
  std::vector<std::string> monitors;      // connector names to show on; empty = all outputs

  bool operator==(const DockConfig&) const = default;
};

struct DesktopWidgetsGridState {
  bool visible = true;
  std::int32_t cellSize = 16;
  std::int32_t majorInterval = 4;

  bool operator==(const DesktopWidgetsGridState&) const = default;
};

struct DesktopWidgetState {
  std::string id;
  std::string type = "clock";
  std::string outputName;
  float cx = 0.0f;
  float cy = 0.0f;
  float scale = 1.0f;
  float rotationRad = 0.0f;
  bool enabled = true;
  std::unordered_map<std::string, WidgetSettingValue> settings;

  bool operator==(const DesktopWidgetState&) const = default;
};

struct DesktopWidgetsConfig {
  bool enabled = true;
  std::int32_t schemaVersion = 1;
  DesktopWidgetsGridState grid;
  std::vector<DesktopWidgetState> widgets;

  bool operator==(const DesktopWidgetsConfig&) const = default;
};

struct OsdConfig {
  std::string position = "top_right";
  std::string orientation = "horizontal";
  float scale = 1.0f;
  bool lockKeys = true;
  bool keyboardLayout = true;
};

struct NotificationConfig {
  bool enableDaemon = true;
  std::string position = "top_right";
  std::string layer = "top"; // top | overlay
  float scale = 1.0f;
  float backgroundOpacity = 0.97f; // toast card background alpha (0.0–1.0)
  int offsetX = 20;                // absolute horizontal margin from the screen edge
  int offsetY = 8;                 // absolute vertical margin from the screen edge
  std::vector<std::string> monitors;
};

template <typename T> struct EnumOption {
  T value;
  std::string_view key;
  std::string_view labelKey;
};

template <typename T, std::size_t N>
constexpr std::optional<T> enumFromKey(const EnumOption<T> (&options)[N], std::string_view key) {
  for (const auto& opt : options) {
    if (opt.key == key) {
      return opt.value;
    }
  }
  return std::nullopt;
}

template <typename T, std::size_t N> constexpr std::string_view enumToKey(const EnumOption<T> (&options)[N], T value) {
  for (const auto& opt : options) {
    if (opt.value == value) {
      return opt.key;
    }
  }
  return {};
}

constexpr EnumOption<SessionActionButtonVariant> kSessionActionButtonVariants[] = {
    {SessionActionButtonVariant::Default, "default", "settings.session-actions.variant.default"},
    {SessionActionButtonVariant::Primary, "primary", "settings.session-actions.variant.primary"},
    {SessionActionButtonVariant::Secondary, "secondary", "settings.session-actions.variant.secondary"},
    {SessionActionButtonVariant::Destructive, "destructive", "settings.session-actions.variant.destructive"},
    {SessionActionButtonVariant::Outline, "outline", "settings.session-actions.variant.outline"},
    {SessionActionButtonVariant::Ghost, "ghost", "settings.session-actions.variant.ghost"},
};

enum class ClipboardAutoPasteMode : std::uint8_t {
  Off = 0,
  Auto = 1,
  CtrlV = 2,
  CtrlShiftV = 3,
  ShiftInsert = 4,
};

constexpr EnumOption<ClipboardAutoPasteMode> kClipboardAutoPasteModes[] = {
    {ClipboardAutoPasteMode::Off, "off", "common.states.off"},
    {ClipboardAutoPasteMode::Auto, "auto", "common.states.auto"},
    {ClipboardAutoPasteMode::CtrlV, "ctrl_v", "settings.options.clipboard.auto-paste.ctrl-v"},
    {ClipboardAutoPasteMode::CtrlShiftV, "ctrl_shift_v", "settings.options.clipboard.auto-paste.ctrl-shift-v"},
    {ClipboardAutoPasteMode::ShiftInsert, "shift_insert", "settings.options.clipboard.auto-paste.shift-insert"},
};

enum class PasswordMaskStyle : std::uint8_t {
  CircleFilled = 0,
  RandomIcons = 1,
};

constexpr EnumOption<PasswordMaskStyle> kPasswordMaskStyles[] = {
    {PasswordMaskStyle::CircleFilled, "default", "settings.options.shell.password-style.filled-circles"},
    {PasswordMaskStyle::RandomIcons, "random", "settings.options.shell.password-style.random-icons"},
};

enum class PanelTransparencyMode : std::uint8_t {
  Solid = 0,
  Soft = 1,
  Glass = 2,
};

constexpr EnumOption<PanelTransparencyMode> kPanelTransparencyModes[] = {
    {PanelTransparencyMode::Solid, "solid", "settings.options.shell.panel-transparency.solid"},
    {PanelTransparencyMode::Soft, "soft", "settings.options.shell.panel-transparency.soft"},
    {PanelTransparencyMode::Glass, "glass", "settings.options.shell.panel-transparency.glass"},
};

[[nodiscard]] float
panelCardOpacityForTransparencyMode(PanelTransparencyMode mode, float panelBackgroundOpacity) noexcept;
[[nodiscard]] float detachedPanelBackgroundOpacityForTransparencyMode(PanelTransparencyMode mode) noexcept;

enum class PanelPlacement : std::uint8_t {
  Attached = 0,
  Floating = 1,
  Centered = 2,
};

constexpr EnumOption<PanelPlacement> kPanelPlacements[] = {
    {PanelPlacement::Attached, "attached", "settings.options.shell.panel-placement.attached"},
    {PanelPlacement::Floating, "floating", "settings.options.shell.panel-placement.floating"},
    {PanelPlacement::Centered, "centered", "settings.options.shell.panel-placement.centered"},
};

constexpr EnumOption<WallpaperFillMode> kWallpaperFillModes[] = {
    {WallpaperFillMode::Center, "center", "settings.options.wallpaper.fill.center"},
    {WallpaperFillMode::Crop, "crop", "settings.options.wallpaper.fill.crop"},
    {WallpaperFillMode::Fit, "fit", "settings.options.wallpaper.fill.fit"},
    {WallpaperFillMode::Stretch, "stretch", "settings.options.wallpaper.fill.stretch"},
    {WallpaperFillMode::Repeat, "repeat", "settings.options.wallpaper.fill.repeat"},
};

constexpr EnumOption<WallpaperAutomationConfig::Order> kWallpaperAutomationOrders[] = {
    {WallpaperAutomationConfig::Order::Random, "random", "settings.options.wallpaper.order.random"},
    {WallpaperAutomationConfig::Order::Alphabetical, "alphabetical", "settings.options.wallpaper.order.alphabetical"},
};

constexpr EnumOption<WallpaperTransition> kWallpaperTransitions[] = {
    {WallpaperTransition::Disc, "disc", "settings.options.wallpaper.transition.disc"},
    {WallpaperTransition::Fade, "fade", "settings.options.wallpaper.transition.fade"},
    {WallpaperTransition::Honeycomb, "honeycomb", "settings.options.wallpaper.transition.honeycomb"},
    {WallpaperTransition::Stripes, "stripes", "settings.options.wallpaper.transition.stripes"},
    {WallpaperTransition::Wipe, "wipe", "settings.options.wallpaper.transition.wipe"},
    {WallpaperTransition::Zoom, "zoom", "settings.options.wallpaper.transition.zoom"},
};

struct ShellConfig {
  struct AnimationConfig {
    bool enabled = true;
    float speed = 1.0f;
  };

  struct ShadowConfig {
    std::int32_t blur = 12;
    std::int32_t offsetX = 0;
    std::int32_t offsetY = 6;
    float alpha = 0.55f;

    bool operator==(const ShadowConfig&) const = default;
  };

  struct PanelConfig {
    bool backgroundBlur = true; // request compositor blur behind panels via ext-background-effect-v1
    PanelTransparencyMode transparencyMode = PanelTransparencyMode::Solid;
    bool borders = true; // panel shell outline and in-panel section cards
    bool shadow = true;  // cast the global [shell.shadow] from panel surfaces
    PanelPlacement launcherPlacement = PanelPlacement::Centered;
    PanelPlacement clipboardPlacement = PanelPlacement::Centered;
    PanelPlacement controlCenterPlacement = PanelPlacement::Attached;
    PanelPlacement wallpaperPlacement = PanelPlacement::Attached;
    PanelPlacement sessionPlacement = PanelPlacement::Attached;
    bool openNearClickControlCenter = false;
    bool openNearClickLauncher = false;
    bool openNearClickClipboard = false;
    bool openNearClickWallpaper = false;
    bool openNearClickSession = false;
    bool launcherCategories = true;

    bool operator==(const PanelConfig&) const = default;
  };

  struct ScreenCornersConfig {
    bool enabled = false;
    std::int32_t size = 32;

    bool operator==(const ScreenCornersConfig&) const = default;
  };

  struct MprisConfig {
    std::vector<std::string> blacklist;

    bool operator==(const MprisConfig&) const = default;
  };

  float uiScale = 1.0f;
  float cornerRadiusScale = 1.0f;
  std::string fontFamily = "sans-serif";
  std::string lang; // empty = auto-detect from $LC_ALL/$LC_MESSAGES/$LANG
  std::string timeFormat = "{:%H:%M}";
  std::string dateFormat = "%A, %x";
  bool offlineMode = false;
  bool telemetryEnabled = false;
  bool niriOverviewTypeToLaunchEnabled = false;
  bool polkitAgent = false;
  PasswordMaskStyle passwordMaskStyle = PasswordMaskStyle::CircleFilled;
  AnimationConfig animation;
  std::string avatarPath;
  bool settingsShowAdvanced = false;
  bool middleClickOpensWidgetSettings = true;
  bool showLocation = true;
  bool launchAppsAsSystemdServices = false;
  /// When false, disables Wayland clipboard integration (history panel, data-control binding, Input paste/copy hooks).
  bool clipboardEnabled = true;
  /// Maximum unpinned clipboard history entries retained (pinned entries are exempt).
  int clipboardHistoryMaxEntries = 50;
  /// Disables per-app tracking and Control Center usage UI.
  bool screenTimeEnabled = false;
  ClipboardAutoPasteMode clipboardAutoPaste = ClipboardAutoPasteMode::Auto;
  std::string clipboardImageActionCommand;
  ShadowConfig shadow;
  PanelConfig panel;
  ScreenCornersConfig screenCorners;
  MprisConfig mpris;
  ShellSessionConfig session;
};

struct WeatherConfig {
  bool enabled = true;
  bool autoLocate = false;
  bool effects = true;
  std::string address;
  std::int32_t refreshMinutes = 30;
  std::string unit = "metric";
};

struct SystemConfig {
  struct MonitorConfig {
    static constexpr float kMinPollSeconds = 1.0f;
    static constexpr float kMaxPollSeconds = 120.0f;

    bool enabled = true;
    float cpuPollSeconds = 2.0f;
    float gpuPollSeconds = 5.0f;
    float memoryPollSeconds = 2.0f;
    float networkPollSeconds = 3.0f;
    float diskPollSeconds = 10.0f;
  };

  MonitorConfig monitor;
};

struct AudioConfig {
  bool enableOverdrive = false;
  bool enableSounds = false;
  float soundVolume = 0.5f;
  std::string volumeChangeSound;
  std::string notificationSound;
};

enum class BrightnessBackendPreference : std::uint8_t {
  Auto = 0,
  None = 1,
  Backlight = 2,
  Ddcutil = 3,
};

constexpr EnumOption<BrightnessBackendPreference> kBrightnessBackendPreferences[] = {
    {BrightnessBackendPreference::Auto, "auto", ""},
    {BrightnessBackendPreference::None, "none", ""},
    {BrightnessBackendPreference::Backlight, "backlight", ""},
    {BrightnessBackendPreference::Ddcutil, "ddcutil", ""},
};

struct BrightnessMonitorOverride {
  std::string match;
  std::optional<BrightnessBackendPreference> backend;

  bool operator==(const BrightnessMonitorOverride&) const = default;
};

struct BrightnessConfig {
  bool enableDdcutil = false;
  std::vector<std::string> ddcutilIgnoreMmids;
  std::vector<BrightnessMonitorOverride> monitorOverrides;

  bool operator==(const BrightnessConfig&) const = default;
};

struct KeybindsConfig {
  std::vector<KeyChord> validate;
  std::vector<KeyChord> cancel;
  std::vector<KeyChord> left;
  std::vector<KeyChord> right;
  std::vector<KeyChord> up;
  std::vector<KeyChord> down;
};

struct NightLightConfig {
  // Day temperature must be higher than night temperature by at least this much.
  static constexpr std::int32_t kTemperatureMin = 1000;
  static constexpr std::int32_t kTemperatureMax = 10000;
  static constexpr std::int32_t kTemperatureGap = 100;

  bool enabled = false;
  bool force = false;
  bool useWeatherLocation = true; // prefer WeatherService coordinates; fall back to manual schedule/location if absent
  std::string startTime;          // HH:MM sunset (night start), used for manual/fallback schedule
  std::string stopTime;           // HH:MM sunrise (day start)
  std::optional<double> latitude;
  std::optional<double> longitude;
  std::int32_t dayTemperature = 6500;
  std::int32_t nightTemperature = 4000;
};

enum class HookKind : std::uint8_t {
  Started = 0,
  WallpaperChanged,
  ColorsChanged,
  ThemeModeChanged,
  SessionLocked,
  SessionUnlocked,
  LoggingOut,
  Rebooting,
  ShuttingDown,
  WifiEnabled,
  WifiDisabled,
  BluetoothEnabled,
  BluetoothDisabled,
  BatteryStateChanged,
  BatteryUnderThreshold,
  PowerProfileChanged,
  Count
};

constexpr EnumOption<HookKind> kHookKinds[] = {
    {HookKind::Started, "started", ""},
    {HookKind::WallpaperChanged, "wallpaper_changed", ""},
    {HookKind::ColorsChanged, "colors_changed", ""},
    {HookKind::ThemeModeChanged, "theme_mode_changed", ""},
    {HookKind::SessionLocked, "session_locked", ""},
    {HookKind::SessionUnlocked, "session_unlocked", ""},
    {HookKind::LoggingOut, "logging_out", ""},
    {HookKind::Rebooting, "rebooting", ""},
    {HookKind::ShuttingDown, "shutting_down", ""},
    {HookKind::WifiEnabled, "wifi_enabled", ""},
    {HookKind::WifiDisabled, "wifi_disabled", ""},
    {HookKind::BluetoothEnabled, "bluetooth_enabled", ""},
    {HookKind::BluetoothDisabled, "bluetooth_disabled", ""},
    {HookKind::BatteryStateChanged, "battery_state_changed", ""},
    {HookKind::BatteryUnderThreshold, "battery_under_threshold", ""},
    {HookKind::PowerProfileChanged, "power_profile_changed", ""},
};

static_assert(sizeof(kHookKinds) / sizeof(kHookKinds[0]) == static_cast<std::size_t>(HookKind::Count));

struct HooksConfig {
  std::array<std::vector<std::string>, static_cast<std::size_t>(HookKind::Count)> commands{};
  // When > 0, `battery_under_threshold` fires when charge crosses from above to at or below this value.
  // When 0, the under-threshold hook never runs.
  std::int32_t batteryLowPercentThreshold = 0;

  bool operator==(const HooksConfig&) const = default;
};

std::optional<HookKind> hookKindFromKey(std::string_view key);
std::string_view hookKindKey(HookKind kind);

enum class PaletteSource : std::uint8_t {
  Builtin = 0,
  Wallpaper = 1,
  Community = 2,
  Custom = 3,
};

constexpr EnumOption<PaletteSource> kPaletteSources[] = {
    {PaletteSource::Builtin, "builtin", "settings.options.theme.source.built-in"},
    {PaletteSource::Wallpaper, "wallpaper", "settings.options.theme.source.wallpaper"},
    {PaletteSource::Community, "community", "settings.options.theme.source.community"},
    {PaletteSource::Custom, "custom", "settings.options.theme.source.custom"},
};

enum class ThemeMode : std::uint8_t {
  Dark = 0,
  Light = 1,
  Auto = 2,
};

constexpr EnumOption<ThemeMode> kThemeModes[] = {
    {ThemeMode::Dark, "dark", "settings.options.theme.mode.dark"},
    {ThemeMode::Light, "light", "settings.options.theme.mode.light"},
    {ThemeMode::Auto, "auto", "common.states.auto"},
};

struct ThemeConfig {
  struct TemplateColorConfig {
    std::string name;
    std::string color;
    bool blend = true;

    bool operator==(const TemplateColorConfig&) const = default;
  };

  struct TemplateInputPathModesConfig {
    std::string dark;
    std::string light;

    bool operator==(const TemplateInputPathModesConfig&) const = default;
  };

  struct TemplateCompareColorConfig {
    std::string name;
    std::string color;

    bool operator==(const TemplateCompareColorConfig&) const = default;
  };

  struct UserTemplateConfig {
    std::string id;
    bool enabled = true;
    std::string inputPath;
    std::optional<TemplateInputPathModesConfig> inputPathModes;
    std::vector<std::string> outputPaths;
    std::string outputPathDynamic;
    std::string compareTo;
    std::vector<TemplateCompareColorConfig> colorsToCompare;
    std::string preHook;
    std::string postHook;
    int index = 0;

    bool operator==(const UserTemplateConfig&) const = default;
  };

  struct TemplatesConfig {
    bool enableBuiltinTemplates = true;
    std::vector<std::string> builtinIds;
    bool enableCommunityTemplates = true;
    std::vector<std::string> communityIds;
    std::vector<TemplateColorConfig> customColors;
    std::vector<UserTemplateConfig> userTemplates;

    bool operator==(const TemplatesConfig&) const = default;
  };

  PaletteSource source = PaletteSource::Builtin;
  std::string builtinPalette = "Noctalia";
  std::string communityPalette = "Oxocarbon";
  std::string customPalette;
  std::string wallpaperScheme = "m3-content";
  ThemeMode mode = ThemeMode::Dark;
  TemplatesConfig templates;
};

struct ControlCenterConfig {
  std::vector<ShortcutConfig> shortcuts;
  bool compact = true;
  bool operator==(const ControlCenterConfig&) const = default;
};

struct Config {
  std::vector<BarConfig> bars;
  std::unordered_map<std::string, WidgetConfig> widgets;
  WallpaperConfig wallpaper;
  BackdropConfig backdrop;
  DockConfig dock;
  DesktopWidgetsConfig desktopWidgets;
  ShellConfig shell;
  OsdConfig osd;
  NotificationConfig notification;
  WeatherConfig weather;
  SystemConfig system;
  AudioConfig audio;
  BrightnessConfig brightness;
  KeybindsConfig keybinds;
  NightLightConfig nightlight;
  IdleConfig idle;
  HooksConfig hooks;
  ThemeConfig theme;
  ControlCenterConfig controlCenter;
};
