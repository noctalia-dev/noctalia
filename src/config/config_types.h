#pragma once

#include "config/config_limits.h"
#include "core/input/key_chord.h"
#include "system/sysmon_threshold_profile.h"
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

// A capsule group: an ordered set of member widgets sharing one capsule + style. `id` is opaque and
// auto-generated. A group appears in a bar lane as a single token (see makeCapsuleGroupToken); its
// members live inside the group, not loose in the lane.
struct BarCapsuleGroupStyle {
  std::string id;
  std::vector<std::string> members; // ordered member widget references
  bool enabled = true;
  ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant);
  // True when `border` is explicitly present (empty value = no outline); mirrors bar/widget border semantics.
  bool borderSpecified = false;
  std::optional<ColorSpec> border;
  std::optional<ColorSpec> foreground;
  float padding = Style::barCapsulePadding;
  std::optional<float> radius;
  float opacity = 1.0f;

  bool operator==(const BarCapsuleGroupStyle&) const = default;
};

// A lane entry referencing a capsule group is the literal "group:" prefix + the group id. The colon
// cannot appear in a widget instance id, so group tokens never collide with widget references.
inline constexpr std::string_view kCapsuleGroupTokenPrefix = "group:";
[[nodiscard]] bool isCapsuleGroupToken(std::string_view laneEntry);
[[nodiscard]] std::string capsuleGroupTokenId(std::string_view laneEntry);
[[nodiscard]] std::string makeCapsuleGroupToken(std::string_view groupId);

struct BarDeadZoneOverride {
  std::optional<std::string> command;
  std::optional<std::string> rightCommand;
  std::optional<std::string> middleCommand;
  std::optional<std::string> scrollUpCommand;
  std::optional<std::string> scrollDownCommand;

  bool operator==(const BarDeadZoneOverride&) const = default;
};

struct BarMonitorOverride {
  std::string match;
  std::optional<std::string> position;
  std::optional<bool> enabled;
  std::optional<bool> autoHide;
  std::optional<bool> showOnWorkspaceSwitch;
  std::optional<bool> reserveSpace;
  std::optional<std::string> layer; // top | overlay
  std::optional<std::int32_t> thickness;
  std::optional<float> backgroundOpacity;
  std::optional<ColorSpec> border;
  std::optional<float> borderWidth;
  std::optional<std::int32_t> radius;
  std::optional<std::int32_t> radiusTopLeft;
  std::optional<std::int32_t> radiusTopRight;
  std::optional<std::int32_t> radiusBottomLeft;
  std::optional<std::int32_t> radiusBottomRight;
  std::optional<std::int32_t> marginEnds;         // inset from each end of the bar along its main axis
  std::optional<std::int32_t> marginEdge;         // distance from the nearest screen edge (floats the bar when > 0)
  std::optional<std::int32_t> marginOppositeEdge; // extra reserved space on the inward side of the bar
  std::optional<std::int32_t> padding;            // main-axis padding from bar edges to start/end sections
  std::optional<std::int32_t> widgetSpacing;      // gap between widgets within a section
  std::optional<bool> shadow;                     // use the global shell shadow on this bar
  std::optional<bool> contactShadow;              // dark gradient between attached panel and bar
  std::optional<std::int32_t> panelOverlap;       // logical px the attached panel overlaps the bar edge (seam tuning)
  std::optional<float> capsuleThickness;          // capsule cross-size as a fraction of bar thickness
  std::optional<std::string> fontFamily;          // unset = inherit shell.font_family
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
  std::optional<ColorSpec> widgetIconColor;
  std::optional<std::vector<BarCapsuleGroupStyle>> widgetCapsuleGroups;
  std::optional<double> widgetCapsulePadding;
  std::optional<double> widgetCapsuleRadius;
  std::optional<double> widgetCapsuleOpacity;
  std::optional<bool> hoverHighlight;
  BarDeadZoneOverride deadZone;

  bool operator==(const BarMonitorOverride&) const = default;
};

struct BarDeadZoneConfig {
  std::string command;
  std::string rightCommand;
  std::string middleCommand;
  std::string scrollUpCommand;
  std::string scrollDownCommand;

  bool operator==(const BarDeadZoneConfig&) const = default;
};

struct BarConfig {
  std::string name = "default";
  std::string position = "top";
  bool enabled = true;
  bool autoHide = false;             // slide out when the pointer leaves; reveal on edge approach
  bool showOnWorkspaceSwitch = true; // with auto_hide: briefly reveal when the active workspace changes
  bool reserveSpace = true;          // reserve compositor exclusive zone; applies with or without auto_hide
  std::string layer = "top";         // top | overlay — attached panels use the same layer
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
  std::int32_t marginEnds = 180;       // inset from each end of the bar along its main axis
  std::int32_t marginEdge = 10;        // distance from the nearest screen edge (floats the bar when > 0)
  std::int32_t marginOppositeEdge = 0; // extra reserved space on the inward side of the bar
  std::int32_t padding = 14;           // main-axis padding from bar edges to start/end sections
  std::int32_t widgetSpacing = 6;      // gap between widgets within a section
  bool shadow = true;                  // use the global shell shadow
  bool contactShadow = false;          // dark gradient between attached panel and bar
  // Logical px the attached panel overlaps the bar edge so their seam is hidden. The ideal value depends on the
  // compositor and the output's fractional scale (physical-pixel rounding), so it is exposed for per-bar/per-monitor
  // tuning. Negative values pull the panel away from the bar.
  std::int32_t panelOverlap = 1;
  float capsuleThickness = 0.76f; // capsule cross-size as a fraction of bar thickness
  float scale = 1.0f;             // content scale multiplier for glyphs and text
  int fontWeight = 500;           // primary label weight for bar widgets
  // Typeface for this bar's widgets; unset inherits shell.font_family. Per-widget `font_family` overrides.
  std::optional<std::string> fontFamily;
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
  // Default primary label color for all widgets on this bar (same as per-widget `color`); per-widget `color`
  // overrides.
  std::optional<ColorSpec> widgetColor;
  // Default icon color for all widgets on this bar (same as per-widget `color`); per-widget `color`
  // overrides.
  std::optional<ColorSpec> widgetIconColor;
  std::vector<BarCapsuleGroupStyle> widgetCapsuleGroups;
  // Inner padding between capsule edge and widget content (logical px), multiplied by widget content scale on the bar.
  float widgetCapsulePadding = Style::barCapsulePadding;
  // Capsule corner radius in logical pixels before content-scale; unset means automatic pill radius.
  std::optional<double> widgetCapsuleRadius;
  // Capsule background opacity multiplier (0.0–1.0).
  float widgetCapsuleOpacity = 1.0f;
  // True when `capsule_border` appears under `[bar.*]` (empty value = no outline for widgets that inherit border).
  bool widgetCapsuleBorderSpecified = false;
  std::optional<ColorSpec> widgetCapsuleBorder;
  // Soft tint of a widget's foreground color over the widget under the pointer (per member in capsule groups).
  bool hoverHighlight = true;
  BarDeadZoneConfig deadZone;
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
  // "lock" | "logout" | "suspend" | "lock_and_suspend" | "reboot" | "shutdown" | "command"
  std::string action;
  bool enabled = true;
  // When set, runs via `process::runAsync` (shell string) instead of the built-in handler.
  std::optional<std::string> command = std::nullopt;
  std::optional<std::string> label = std::nullopt;
  std::optional<std::string> glyph = std::nullopt;
  SessionActionButtonVariant variant = SessionActionButtonVariant::Default;
  std::optional<KeyChord> shortcut = std::nullopt;
  /// When > 0, the action arms a countdown (seconds) before running; activate again to confirm immediately.
  double countdownSeconds = 0.0;

  bool operator==(const SessionPanelActionConfig&) const = default;
};

struct ShellSessionConfig {
  std::vector<SessionPanelActionConfig> actions;
  // Optional overrides for built-in session power commands. Empty = auto-detect at runtime.
  struct ShellSessionPowerConfig {
    // Shell strings run with `/bin/sh -lc` (shell=True).
    // When unset, Noctalia tries a prioritized backend list (systemd/logind/privileged helpers).
    std::optional<std::string> suspend;
    std::optional<std::string> reboot;
    std::optional<std::string> shutdown;

    bool operator==(const ShellSessionPowerConfig&) const = default;
  } power;

  bool operator==(const ShellSessionConfig&) const = default;
};

struct ShellGreeterSyncConfig {
  // Shell prefix that replaces the default pkexec/run0 escalator before the apply helper
  // path and staging directory. Empty = pkexec or run0. Example: "ghostty -e pkexec"
  std::string privilegeCommand;
  bool autoSync = false;

  bool operator==(const ShellGreeterSyncConfig&) const = default;
};

struct IdleBehaviorConfig {
  std::string name;
  bool enabled = true;
  double timeoutSeconds = 0.0;
  /// lock | screen_off | suspend | lock_and_suspend | command (custom shell strings)
  std::string action;
  std::string command;
  std::string resumeCommand;
  /// When `action` is `suspend`, lock the session before running suspend so lock surfaces are ready (recommended).
  bool lockBeforeSuspend = true;

  bool operator==(const IdleBehaviorConfig&) const = default;
};

struct NotificationFilterConfig {
  std::string name;
  bool enabled = true;
  /// Case-insensitive token matched against app name (exact/substring), desktop entry, or category.
  std::string match;
  bool showToast = true;
  bool saveHistory = true;
  bool playSound = true;
  bool allowPermanent = true;
  /// Empty = allow low, normal, and critical. Otherwise only listed urgencies pass this filter.
  std::vector<std::string> allowedUrgencies;

  bool operator==(const NotificationFilterConfig&) const = default;
};

struct IdleConfig {
  std::vector<IdleBehaviorConfig> behaviors;
  /// When > 0, after the compositor reports idle the shell fades a fullscreen overlay (surface color)
  /// from transparent to opaque over this many seconds, then runs `command`. Compositor activity during
  /// the fade cancels. When 0, the idle command runs immediately with no overlay.
  float preActionFadeSeconds = 2.0f;

  bool operator==(const IdleConfig&) const = default;
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
  LockAndSuspend,
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

void normalizeIdleBehaviorAction(IdleBehaviorConfig& behavior);
[[nodiscard]] ResolvedIdleBehavior resolveIdleBehaviorActions(const IdleBehaviorConfig& behavior);

enum class KeybindAction : std::uint8_t {
  Validate = 0,
  Cancel = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
  TabNext = 6,
  TabPrevious = 7,
};

[[nodiscard]] std::vector<KeyChord> defaultKeybindSet(KeybindAction action);

using WidgetSettingValue = std::variant<bool, std::int64_t, double, std::string, std::vector<std::string>>;
using ConfigOverrideValue = std::variant<
    bool, std::int64_t, double, std::string, std::vector<std::string>, std::vector<ShortcutConfig>,
    std::vector<SessionPanelActionConfig>, std::vector<IdleBehaviorConfig>, std::vector<NotificationFilterConfig>,
    std::vector<KeyChord>, std::vector<BarCapsuleGroupStyle>>;

// Optional rounded “capsule” behind a bar widget (see `[widget.*] capsule_*` in CONFIG.md).
// Corner shape, border width, and edge softness are fixed in the shell code; padding/radius are configurable.
struct WidgetBarCapsuleSpec {
  bool enabled = false;
  ColorSpec fill = colorSpecFromRole(ColorRole::SurfaceVariant);
  // Opaque group ID (auto-generated). Adjacent widgets in the same section with the same non-empty ID share one
  // shell and inherit the group's `BarCapsuleGroupStyle`.
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
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> tables;

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
  [[nodiscard]] std::unordered_map<std::string, std::string>
  getStringMap(const std::string& key, const std::unordered_map<std::string, std::string>& fallback = {}) const;
  [[nodiscard]] bool hasSetting(const std::string& key) const;

  bool operator==(const WidgetConfig&) const = default;
};

// Merges `[bar.*]` capsule defaults with `[widget.*]` overrides (see CONFIG.md). Size/style fields such as
// `radius` are populated even when `enabled` is false so widgets can reuse capsule styling internally.
[[nodiscard]] WidgetBarCapsuleSpec resolveWidgetBarCapsuleSpec(const BarConfig& bar, const WidgetConfig* widget);

// Returns the group for `id` on this bar, or nullptr if `id` is empty or unregistered.
[[nodiscard]] const BarCapsuleGroupStyle* findBarCapsuleGroupStyle(const BarConfig& bar, const std::string& id);

// Builds the capsule spec a group's member widgets render with (style taken from the group).
[[nodiscard]] WidgetBarCapsuleSpec capsuleSpecFromGroup(const BarConfig& bar, const BarCapsuleGroupStyle& group);
[[nodiscard]] float
resolveWidgetContentScale(float barScale, const WidgetConfig* widget, std::string_view context = "widget.scale");

// Color spec for user color strings: either a palette color role token or a hex color.
[[nodiscard]] ColorSpec colorSpecFromConfigString(const std::string& raw, std::string_view context = {});

// Serializes a color spec back to its config string form (palette role token or hex).
[[nodiscard]] std::string colorSpecToConfigString(const ColorSpec& spec);

// Shared output selector matching used by monitor-scoped config and IPC selectors.
// Matches connector name exactly, or a word-boundary token within output description.
[[nodiscard]] bool outputMatchesSelector(const std::string& match, const WaylandOutput& output);

enum class WallpaperFillMode : std::uint8_t {
  Center = 0,
  Crop = 1,
  Fit = 2,
  Stretch = 3,
  Repeat = 4,
  Span = 5,
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

  bool operator==(const WallpaperMonitorOverride&) const = default;
};

struct WallpaperAutomationConfig {
  enum class Order : std::uint8_t {
    Random = 0,
    Alphabetical = 1,
  };

  bool enabled = false;
  std::int32_t intervalSeconds = 1800;
  Order order = Order::Random;
  bool recursive = true;

  bool operator==(const WallpaperAutomationConfig&) const = default;
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
  bool transitionOnStartup = false;
  std::string directory;      // empty = ~/Pictures/Wallpapers
  std::string directoryLight; // empty = directory
  std::string directoryDark;  // empty = directory
  bool perMonitorDirectories = false;
  WallpaperAutomationConfig automation;
  std::vector<WallpaperMonitorOverride> monitorOverrides;

  bool operator==(const WallpaperConfig&) const = default;
};

struct BackdropConfig {
  bool enabled = false;
  float blurIntensity = 0.5f;
  float tintIntensity = 0.3f;

  bool operator==(const BackdropConfig&) const = default;
};

struct LockscreenConfig {
  bool enabled = true;
  bool fingerprint = true;
  bool allowEmptyPassword = false;
  bool blurredDesktop = false;
  float blurIntensity = 0.5f;
  float tintIntensity = 0.3f;
  std::string wallpaper;
  std::vector<std::string> monitors;

  bool operator==(const LockscreenConfig&) const = default;
};

[[nodiscard]] inline bool isLockScreenEnabled(const LockscreenConfig& lockscreen) noexcept {
  return lockscreen.enabled;
}

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

enum class DockEdge : std::uint8_t {
  Top = 0,
  Bottom = 1,
  Left = 2,
  Right = 3,
};

constexpr EnumOption<DockEdge> kDockEdges[] = {
    {DockEdge::Top, "top", "settings.options.edge.top"},
    {DockEdge::Bottom, "bottom", "settings.options.edge.bottom"},
    {DockEdge::Left, "left", "settings.options.edge.left"},
    {DockEdge::Right, "right", "settings.options.edge.right"},
};

enum class DockLauncherPosition : std::uint8_t {
  None = 0,
  Start = 1,
  End = 2,
};

constexpr EnumOption<DockLauncherPosition> kDockLauncherPositions[] = {
    {DockLauncherPosition::None, "none", "settings.options.dock-launcher-position.none"},
    {DockLauncherPosition::Start, "start", "settings.options.dock-launcher-position.start"},
    {DockLauncherPosition::End, "end", "settings.options.dock-launcher-position.end"},
};

struct DockConfig {
  bool enabled = false; // opt-in; dock is hidden by default
  DockEdge position = DockEdge::Bottom;
  bool activeMonitorOnly = false;    // render only on preferred active output
  std::int32_t iconSize = 48;        // icon size in pixels (before ui_scale)
  std::int32_t mainAxisPadding = 16; // inner padding along the icon row (main axis)
  std::int32_t crossAxisPadding = 8; // inner padding perpendicular to the icon row
  std::int32_t itemSpacing = 6;      // gap between items
  float backgroundOpacity = 0.88f;
  std::int32_t radius = 16;            // dock background corner radius
  std::int32_t radiusTopLeft = 16;     // dock background top-left corner radius
  std::int32_t radiusTopRight = 16;    // dock background top-right corner radius
  std::int32_t radiusBottomLeft = 16;  // dock background bottom-left corner radius
  std::int32_t radiusBottomRight = 16; // dock background bottom-right corner radius
  bool concaveEdgeCorners = false;     // carve concave corners on the side that touches the screen edge
  std::int32_t marginEnds = 0;         // inset from each end of the dock along its main axis
  std::int32_t marginEdge = 8;         // distance from the nearest screen edge (floats the dock when > 0)
  bool shadow = true;                  // use the global shell shadow
  bool showRunning = true;             // also show running apps not in pinned list
  bool autoHide = false;               // fade out when not hovered (overlay mode)
  bool reserveSpace = true;            // reserve compositor exclusive zone; applies with or without auto_hide
  float activeScale = 1.0f;            // focused app icon scale
  float inactiveScale = 0.85f;         // non-focused app icon scale
  bool magnification = true;           // magnify icons near the pointer (macOS-style)
  float magnificationScale = 1.45f;    // max icon scale multiplier at the pointer center
  float activeOpacity = 1.0f;          // focused app icon opacity
  float inactiveOpacity = 0.85f;       // non-focused app icon opacity
  bool showDots = false;               // show optional running window dots below app icons
  bool showInstanceCount = true;       // show a badge with count when app has >1 window
  DockLauncherPosition launcherPosition = DockLauncherPosition::None;
  std::string launcherIcon = "grid-dots";   // Tabler glyph name
  std::string launcherCustomImage = "";     // image path; overrides launcherIcon glyph when set
  bool launcherCustomImageColorize = false; // tint the custom image with the icon color role
  std::vector<std::string> pinned;          // desktop entry IDs to always show
  std::vector<std::string> monitors;        // connector names to show on; empty = all outputs
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
  // Box size of the widget's grid tile, in logical px. 0 means "unsized": the tile
  // auto-fits the content's natural size. Resizing in the editor sets explicit values.
  float boxWidth = 0.0f;
  float boxHeight = 0.0f;
  float rotationRad = 0.0f;
  bool flipX = false;
  bool flipY = false;
  bool enabled = true;
  std::unordered_map<std::string, WidgetSettingValue> settings;

  bool operator==(const DesktopWidgetState&) const = default;
};

struct DesktopWidgetsConfig {
  bool enabled = true;
  std::int32_t schemaVersion = 2;
  DesktopWidgetsGridState grid;
  std::vector<DesktopWidgetState> widgets;

  bool operator==(const DesktopWidgetsConfig&) const = default;
};

struct LockscreenWidgetsConfig {
  bool enabled = false;
  std::int32_t schemaVersion = 2;
  DesktopWidgetsGridState grid;
  std::vector<DesktopWidgetState> widgets;

  bool operator==(const LockscreenWidgetsConfig&) const = default;
};

struct OsdKindsConfig {
  bool volume = true;
  bool volumeOutput = true;
  bool volumeInput = true;
  bool brightness = true;
  bool wifi = true;
  bool bluetooth = true;
  bool powerProfile = true;
  bool caffeine = true;
  bool nightlight = true;
  bool dnd = true;
  bool lockKeys = true;
  bool keyboardLayout = true;
  bool media = true;
  bool privacy = true;
  bool operator==(const OsdKindsConfig&) const = default;
};

struct OsdConfig {
  std::string position = "top_center";
  std::string positionVertical = "top_center";
  std::string orientation = "horizontal";
  float scale = 1.0f;
  float backgroundOpacity = 0.97f;
  int offsetX = 20;
  int offsetY = 8;
  std::vector<std::string> monitors;
  OsdKindsConfig kinds;

  bool operator==(const OsdConfig&) const = default;
};

struct NotificationConfig {
  bool enableDaemon = true;
  bool showAppName = true;
  bool showActions = true;
  std::string position = "top_right";
  std::string layer = "top"; // top | overlay
  float scale = 1.0f;
  float backgroundOpacity = 0.97f; // toast card background alpha (0.0–1.0)
  int offsetX = 20;                // absolute horizontal margin from the screen edge
  int offsetY = 8;                 // absolute vertical margin from the screen edge
  std::vector<std::string> monitors;
  bool collapseOnDismiss = true;
  std::vector<NotificationFilterConfig> filters;

  bool operator==(const NotificationConfig&) const = default;
};

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

enum class ShadowDirection : std::uint8_t {
  Center = 0,
  Down = 1,
  Up = 2,
  Left = 3,
  Right = 4,
  DownLeft = 5,
  DownRight = 6,
  UpLeft = 7,
  UpRight = 8,
};

constexpr EnumOption<ShadowDirection> kShadowDirections[] = {
    {ShadowDirection::Center, "center", "settings.options.shell.shadow-direction.center"},
    {ShadowDirection::Down, "down", "settings.options.shell.shadow-direction.down"},
    {ShadowDirection::Up, "up", "settings.options.shell.shadow-direction.up"},
    {ShadowDirection::Left, "left", "settings.options.shell.shadow-direction.left"},
    {ShadowDirection::Right, "right", "settings.options.shell.shadow-direction.right"},
    {ShadowDirection::DownLeft, "down_left", "settings.options.shell.shadow-direction.down-left"},
    {ShadowDirection::DownRight, "down_right", "settings.options.shell.shadow-direction.down-right"},
    {ShadowDirection::UpLeft, "up_left", "settings.options.shell.shadow-direction.up-left"},
    {ShadowDirection::UpRight, "up_right", "settings.options.shell.shadow-direction.up-right"},
};

struct ShadowDirectionOffset {
  std::int32_t x;
  std::int32_t y;
};

constexpr ShadowDirectionOffset shadowDirectionOffset(ShadowDirection dir) noexcept {
  switch (dir) {
  case ShadowDirection::Center:
    return {0, 0};
  case ShadowDirection::Down:
    return {0, 2};
  case ShadowDirection::Up:
    return {0, -2};
  case ShadowDirection::Left:
    return {-2, 0};
  case ShadowDirection::Right:
    return {2, 0};
  case ShadowDirection::DownLeft:
    return {-2, 2};
  case ShadowDirection::DownRight:
    return {2, 2};
  case ShadowDirection::UpLeft:
    return {-2, -2};
  case ShadowDirection::UpRight:
    return {2, -2};
  }
  return {0, 2};
}

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
};

constexpr EnumOption<PanelPlacement> kPanelPlacements[] = {
    {PanelPlacement::Attached, "attached", "settings.options.shell.panel-placement.attached"},
    {PanelPlacement::Floating, "floating", "settings.options.shell.panel-placement.floating"},
};

// Screen-anchor tokens for a floating panel's `<panel>_position`. "auto" keeps the
// panel bar-relative (the historical floating behavior); "center" reserves the
// screen center; the rest anchor to a screen edge/corner. Same vocabulary as the
// OSD/notification `position`.
constexpr std::string_view kPanelPositions[] = {"auto",          "center",      "top_left",     "top_center",
                                                "top_right",     "center_left", "center_right", "bottom_left",
                                                "bottom_center", "bottom_right"};

constexpr EnumOption<WallpaperFillMode> kWallpaperFillModes[] = {
    {WallpaperFillMode::Center, "center", "settings.options.wallpaper.fill.center"},
    {WallpaperFillMode::Crop, "crop", "settings.options.wallpaper.fill.crop"},
    {WallpaperFillMode::Fit, "fit", "settings.options.wallpaper.fill.fit"},
    {WallpaperFillMode::Stretch, "stretch", "settings.options.wallpaper.fill.stretch"},
    {WallpaperFillMode::Repeat, "repeat", "settings.options.wallpaper.fill.repeat"},
    {WallpaperFillMode::Span, "span", "settings.options.wallpaper.fill.span"},
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

// One config-driven dmenu-style launcher entry. The provider runs `command`, splits
// its stdout into newline-separated candidates, and on activation either runs `exec`
// (with {selection} substituted) or copies the selection to the clipboard.
struct DmenuEntryConfig {
  // Canonical flat identifier; the [shell.launcher.dmenu.entry.<id>] table key. Used as
  // the provider id suffix (dmenu.<id>) and the usage-tracking key.
  std::string id;
  // Shell string run via /bin/sh -lc; stdout lines become candidates. A tab in a line
  // splits it into title \t description (the raw line is still the selection value).
  std::string command;
  // When set, the activated line is substituted into {selection} and run detached.
  // When unset, the selection is copied to the clipboard.
  std::optional<std::string> exec;
  // Launcher prefix routing (e.g. "/ssh"). Empty leaves the entry reachable only via
  // global = true, otherwise it is unreachable (surfaced as a config warning).
  std::optional<std::string> prefix;
  std::optional<std::string> label; // Provider overview title; defaults to the id.
  std::optional<std::string> glyph; // Tabler glyph name; defaults to "terminal".
  bool global = false;              // Include results in non-prefixed search.

  bool operator==(const DmenuEntryConfig&) const = default;
};

struct ShellConfig {
  struct AnimationConfig {
    bool enabled = true;
    float speed = 1.0f;

    bool operator==(const AnimationConfig&) const = default;
  };

  struct ShadowConfig {
    ShadowDirection direction = ShadowDirection::Down;
    float alpha = 0.55f;

    bool operator==(const ShadowConfig&) const = default;
  };

  struct PanelConfig {
    PanelTransparencyMode transparencyMode = PanelTransparencyMode::Solid;
    bool borders = true; // panel shell outline and in-panel section cards
    bool shadow = true;  // cast the global [shell.shadow] from panel surfaces
    PanelPlacement launcherPlacement = PanelPlacement::Floating;
    PanelPlacement clipboardPlacement = PanelPlacement::Floating;
    PanelPlacement controlCenterPlacement = PanelPlacement::Attached;
    PanelPlacement wallpaperPlacement = PanelPlacement::Attached;
    PanelPlacement sessionPlacement = PanelPlacement::Attached;
    PanelPlacement polkitPlacement = PanelPlacement::Floating;
    // Floating screen position per panel (one of kPanelPositions). "auto" = bar-relative.
    // Launcher/clipboard default to "center" (the historical centered placement).
    std::string launcherPosition = "center";
    std::string clipboardPosition = "center";
    std::string controlCenterPosition = "auto";
    std::string wallpaperPosition = "auto";
    std::string sessionPosition = "auto";
    std::string polkitPosition = "center";
    std::int32_t floatingOffset = 8; // logical px gap between a floating/detached panel and the bar edge
    bool openNearClickControlCenter = false;
    bool openNearClickLauncher = false;
    bool openNearClickClipboard = false;
    bool openNearClickWallpaper = false;
    bool openNearClickSession = false;

    bool operator==(const PanelConfig&) const = default;
  };

  // Launcher behavior/appearance. Panel placement for the launcher surface stays
  // under [shell.panel] (launcher_placement/position/open_near_click_launcher),
  // parallel to every other surface.
  struct LauncherConfig {
    bool categories = true;
    bool showIcons = true;
    bool compact = false;
    bool appGrid = false;
    bool sessionSearch = false;
    bool sortByUsage = true;

    struct DmenuConfig {
      std::vector<DmenuEntryConfig> entries;

      bool operator==(const DmenuConfig&) const = default;
    } dmenu;

    bool operator==(const LauncherConfig&) const = default;
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

  struct ScreenshotConfig {
    bool saveToFile = true;
    bool copyToClipboard = true;
    bool freezeScreen = true;
    bool confirmRegion = false;
    bool pipeToCommand = false;
    std::string pipeCommand;
    std::string directory;       // empty = ~/Pictures
    std::string filenamePattern; // empty = screenshot_%Y%m%d_%H%M%S

    bool operator==(const ScreenshotConfig&) const = default;
  };

  struct PrivacyConfig {
    std::string micFilterRegex;
    std::string camFilterRegex;
    std::string screenFilterRegex;

    bool operator==(const PrivacyConfig&) const = default;
  };

  float uiScale = 1.0f;
  float cornerRadiusScale = 1.0f;
  std::string fontFamily = "sans-serif";
  std::string lang; // empty = auto-detect from $LC_ALL/$LC_MESSAGES/$LANG
  std::string timeFormat = "{:%H:%M}";
  std::string dateFormat = "%A, %x";
  bool offlineMode = false;
  /// Resolve and show the connection's external (WAN) IP in the Control Center network tab.
  bool externalIpEnabled = false;
  bool telemetryEnabled = false;
  bool setupWizardEnabled = true;
  bool niriOverviewTypeToLaunchEnabled = false;
  bool polkitAgent = false;
  PasswordMaskStyle passwordMaskStyle = PasswordMaskStyle::CircleFilled;
  AnimationConfig animation;
  std::string avatarPath;
  bool settingsShowAdvanced = true;
  bool middleClickOpensWidgetSettings = true;
  bool showLocation = true;
  bool appIconColorize = false;
  std::optional<ColorSpec> appIconColor;
  bool launchAppsAsSystemdServices = false;
  std::string launchAppsCustomCommand;
  /// When false, disables Wayland clipboard integration (history panel, data-control binding, Input paste/copy hooks).
  bool clipboardEnabled = true;
  /// Maximum unpinned clipboard history entries retained (pinned entries are exempt).
  int clipboardHistoryMaxEntries = static_cast<int>(noctalia::config::kClipboardHistoryDefaultEntries);
  /// When true, clearing clipboard history or deleting unpinned entries from the panel asks for confirmation first.
  bool clipboardConfirmClearHistory = true;
  /// Disables per-app tracking and Control Center usage UI.
  bool screenTimeEnabled = false;
  bool sharedGlContext = true;
  bool disableMipmaps = false;
  ClipboardAutoPasteMode clipboardAutoPaste = ClipboardAutoPasteMode::Auto;
  std::string clipboardImageActionCommand;
  ShadowConfig shadow;
  PanelConfig panel;
  LauncherConfig launcher;
  ScreenCornersConfig screenCorners;
  MprisConfig mpris;
  ScreenshotConfig screenshot;
  PrivacyConfig privacy;
  ShellSessionConfig session;
  ShellGreeterSyncConfig greeterSync;

  bool operator==(const ShellConfig&) const = default;
};

struct WeatherConfig {
  bool enabled = true;
  bool effects = true;
  std::int32_t refreshMinutes = 30;
  std::string unit = "metric";

  bool operator==(const WeatherConfig&) const = default;
};

struct CalendarConfig {
  // A single connected account. Credentials (OAuth tokens / CalDAV app-password) are NOT stored
  // here; they live in state.toml keyed by id. id must be [a-z0-9_] (used as a state key).
  struct Account {
    std::string id;
    std::string type; // "google" | "caldav"
    std::string displayName;
    std::string color;                  // optional "#rrggbb" override
    std::string provider;               // "icloud" | "custom" (caldav only)
    std::string serverUrl;              // CalDAV discovery root (custom only; provider presets own theirs)
    std::string username;               // CalDAV login (caldav only)
    std::vector<std::string> calendars; // discovered collection ids; empty = all

    bool operator==(const Account&) const = default;
  };

  bool enabled = false;
  std::int32_t refreshMinutes = 15;
  std::vector<Account> accounts;

  bool operator==(const CalendarConfig&) const = default;
};

struct SystemConfig {
  struct MonitorConfig {
    // A poll value of 0 disables that metric entirely (no sampling, no wakeups);
    // any non-zero value is clamped to [kMinPollSeconds, kMaxPollSeconds].
    static constexpr float kDisabledPollSeconds = 0.0f;
    static constexpr float kMinPollSeconds = 1.0f;
    static constexpr float kMaxPollSeconds = 120.0f;

    bool enabled = true;
    std::string cpuTempSensorPath;
    float cpuPollSeconds = 2.0f;
    // Disabled by default so laptops with a discrete GPU are not woken just to sample it.
    float gpuPollSeconds = kDisabledPollSeconds;
    float memoryPollSeconds = 2.0f;
    float networkPollSeconds = 3.0f;
    float diskPollSeconds = 10.0f;
    double cpuUsageActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuUsage).activityDefault;
    double cpuUsageCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuUsage).criticalDefault;
    double cpuTempActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuTemp).activityDefault;
    double cpuTempCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::CpuTemp).criticalDefault;
    double gpuTempActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuTemp).activityDefault;
    double gpuTempCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuTemp).criticalDefault;
    double gpuUsageActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuUsage).activityDefault;
    double gpuUsageCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuUsage).criticalDefault;
    double gpuVramActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuVram).activityDefault;
    double gpuVramCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::GpuVram).criticalDefault;
    double ramPctActivityThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::RamPct).activityDefault;
    double ramPctCriticalThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::RamPct).criticalDefault;
    double swapPctActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::SwapPct).activityDefault;
    double swapPctCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::SwapPct).criticalDefault;
    double diskPctActivityThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskPct).activityDefault;
    double diskPctCriticalThreshold =
        noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::DiskPct).criticalDefault;
    double netRxActivityThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetRx).activityDefault;
    double netRxCriticalThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetRx).criticalDefault;
    double netTxActivityThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetTx).activityDefault;
    double netTxCriticalThreshold = noctalia::sysmon::thresholdProfile(noctalia::sysmon::Stat::NetTx).criticalDefault;

    bool operator==(const MonitorConfig&) const = default;
  };

  MonitorConfig monitor;

  bool operator==(const SystemConfig&) const = default;
};

struct AudioConfig {
  bool enableOverdrive = false;
  bool enableSounds = false;
  float soundVolume = 0.5f;
  std::string volumeChangeSound;
  std::string notificationSound;

  bool operator==(const AudioConfig&) const = default;
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
  bool syncBrightnessOfAllMonitors = false;
  std::vector<std::string> ddcutilIgnoreMmids;
  std::vector<BrightnessMonitorOverride> monitorOverrides;
  float minimumBrightness = 0.0f;

  bool operator==(const BrightnessConfig&) const = default;
};

struct BatteryDeviceWarningThreshold {
  std::string selector;
  // 0 disables the low-battery warning notification and widget warning state for this device.
  std::int32_t warningThreshold = 10;

  bool operator==(const BatteryDeviceWarningThreshold&) const = default;
};

struct BatteryConfig {
  // 0 disables the low-battery warning notification and widget warning state by default.
  std::int32_t warningThreshold = 10;
  std::vector<BatteryDeviceWarningThreshold> deviceThresholds;

  bool operator==(const BatteryConfig&) const = default;
};

struct KeybindsConfig {
  std::vector<KeyChord> validate;
  std::vector<KeyChord> cancel;
  std::vector<KeyChord> left;
  std::vector<KeyChord> right;
  std::vector<KeyChord> up;
  std::vector<KeyChord> down;
  std::vector<KeyChord> tabNext;
  std::vector<KeyChord> tabPrevious;

  bool operator==(const KeybindsConfig&) const = default;
};

struct NightLightConfig {
  // Day temperature must be higher than night temperature by at least this much.
  static constexpr std::int32_t kTemperatureMin = 1000;
  static constexpr std::int32_t kTemperatureMax = 10000;
  static constexpr std::int32_t kTemperatureGap = 100;

  bool enabled = false;
  bool force = false;
  std::int32_t dayTemperature = 6500;
  std::int32_t nightTemperature = 4000;

  bool operator==(const NightLightConfig&) const = default;
};

struct LocationConfig {
  // Single source of truth for "where am I". Resolution priority:
  //   auto_locate (IP) -> address (geocoded) -> manual latitude/longitude -> manual sunrise/sunset.
  // Consumed by the weather service, night light, and theme auto mode.
  bool autoLocate = false; // resolve coordinates from IP geolocation
  std::string address;     // geocoded when auto_locate is off and this is non-empty
  std::string sunset;      // HH:MM night start, used only when no coordinates resolve
  std::string sunrise;     // HH:MM day start, used only when no coordinates resolve
  std::optional<double> latitude;
  std::optional<double> longitude;

  bool operator==(const LocationConfig&) const = default;
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
  BatteryCharging,
  BatteryDischarging,
  BatteryPlugged,
  BatteryPercentageChanged,
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
    {HookKind::BatteryCharging, "battery_charging", ""},
    {HookKind::BatteryDischarging, "battery_discharging", ""},
    {HookKind::BatteryPlugged, "battery_plugged", ""},
    {HookKind::BatteryPercentageChanged, "battery_percentage_changed", ""},
    {HookKind::PowerProfileChanged, "power_profile_changed", ""},
};

static_assert(sizeof(kHookKinds) / sizeof(kHookKinds[0]) == static_cast<std::size_t>(HookKind::Count));

struct HooksConfig {
  std::array<std::vector<std::string>, static_cast<std::size_t>(HookKind::Count)> commands{};

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

struct WallpaperFavorite {
  std::string path;
  ThemeMode themeMode = ThemeMode::Auto;
  std::optional<PaletteSource> paletteSource;
  std::string builtinPalette;
  std::string communityPalette;
  std::string customPalette;
  std::string wallpaperScheme;

  bool operator==(const WallpaperFavorite&) const = default;
};

enum class ControlCenterSidebarMode : std::uint8_t {
  Full = 0,
  Compact = 1,
  None = 2,
};

constexpr EnumOption<ControlCenterSidebarMode> kControlCenterSidebarModes[] = {
    {ControlCenterSidebarMode::Full, "full", "settings.options.control-center.sidebar.full"},
    {ControlCenterSidebarMode::Compact, "compact", "settings.options.control-center.sidebar.compact"},
    {ControlCenterSidebarMode::None, "none", "settings.options.control-center.sidebar.none"},
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

  bool operator==(const ThemeConfig&) const = default;
};

struct ControlCenterConfig {
  static constexpr std::int32_t kDefaultWidth = 700;

  struct CalendarTabConfig {
    bool showEventsCard = true;
    bool operator==(const CalendarTabConfig&) const = default;
  };

  std::vector<ShortcutConfig> shortcuts;
  std::vector<std::string> hiddenTabs; // tab keys (see kTabs) the user has hidden; empty = all available shown
  ControlCenterSidebarMode sidebarMode = ControlCenterSidebarMode::Compact;
  ControlCenterSidebarMode sidebarSectionMode = ControlCenterSidebarMode::Compact;
  std::int32_t width = kDefaultWidth; // full-sidebar logical width; compact/none modes scale down from this
  CalendarTabConfig calendarTab;
  bool operator==(const ControlCenterConfig&) const = default;
};

// A plugin source: where plugin code comes from. `Git` is a repo URL the host
// caches, updates, and exports plugin runtime files from; `Path` is an
// immutable local directory (e.g. a Nix store path) the host treats read-only
// (update/auto-update/remove are no-ops).
enum class PluginSourceKind : std::uint8_t {
  Git = 0,
  Path = 1,
};

constexpr EnumOption<PluginSourceKind> kPluginSourceKinds[] = {
    {PluginSourceKind::Git, "git", "settings.options.plugins.source.git"},
    {PluginSourceKind::Path, "path", "settings.options.plugins.source.path"},
};

struct PluginSourceConfig {
  PluginSourceKind kind = PluginSourceKind::Git;
  std::string name;        // stable handle (also the clone subdir for git sources)
  std::string location;    // git URL or local path
  bool autoUpdate = false; // git-only, opt-in
  bool enabled = true;     // disabled sources are not scanned; their clone is kept
  bool operator==(const PluginSourceConfig&) const = default;
};

// Distribution config: where plugins come from and which are turned on. User
// intent → config (declarative-friendly); clones live under the state dir.
struct PluginsConfig {
  std::vector<PluginSourceConfig> sources;
  std::vector<std::string> enabled; // active plugin ids ("author/plugin"); opt-in for every source
  // Plugin-level setting overrides, keyed by plugin id then setting key. Seeded
  // into every entry runtime of the plugin (widget/shortcut/service). Open-ended
  // (validated against the manifest schema), so compared via configEqual rather
  // than the defaulted operator== (which lacks int/double coercion).
  std::unordered_map<std::string, std::unordered_map<std::string, WidgetSettingValue>> pluginSettings;
  bool operator==(const PluginsConfig&) const = default;
};

// Default sources seeded when [plugins] declares no [[plugins.source]]: the
// official + community plugin repos (auto-update off).
[[nodiscard]] std::vector<PluginSourceConfig> defaultPluginSources();
[[nodiscard]] bool isDefaultPluginSourceName(std::string_view name);
// Source names are stable user-facing handles and git source storage directory names.
// Keep them flat so they can never escape the plugin source cache.
[[nodiscard]] bool isValidPluginSourceName(std::string_view name);
struct HotCornersConfig {
  bool enabled = false;

  struct Corner {
    std::string action = "none";
    std::string command;
    bool operator==(const Corner&) const = default;
  };

  Corner topLeft;
  Corner topRight;
  Corner bottomLeft;
  Corner bottomRight;

  bool operator==(const HotCornersConfig&) const = default;
};

struct Config {
  std::vector<BarConfig> bars;
  std::unordered_map<std::string, WidgetConfig> widgets;
  WallpaperConfig wallpaper;
  BackdropConfig backdrop;
  LockscreenConfig lockscreen;
  LockscreenWidgetsConfig lockscreenWidgets;
  DockConfig dock;
  DesktopWidgetsConfig desktopWidgets;
  HotCornersConfig hotCorners;
  ShellConfig shell;
  OsdConfig osd;
  NotificationConfig notification;
  WeatherConfig weather;
  CalendarConfig calendar;
  SystemConfig system;
  AudioConfig audio;
  BrightnessConfig brightness;
  BatteryConfig battery;
  KeybindsConfig keybinds;
  NightLightConfig nightlight;
  LocationConfig location;
  IdleConfig idle;
  HooksConfig hooks;
  ThemeConfig theme;
  ControlCenterConfig controlCenter;
  PluginsConfig plugins;
};
// Which top-level config sections changed across a reload. Default-constructed
// to all-true (conservative: "assume everything changed") so any path that does
// not compute a precise diff still fans the reload out to every subscriber.
struct ConfigChangeSet {
  bool bars = true;
  bool widgets = true;
  bool desktopWidgets = true;
  bool lockscreenWidgets = true;
  bool wallpaper = true;
  bool backdrop = true;
  bool lockscreen = true;
  bool dock = true;
  bool shell = true;
  bool osd = true;
  bool notification = true;
  bool weather = true;
  bool calendar = true;
  bool system = true;
  bool audio = true;
  bool brightness = true;
  bool battery = true;
  bool keybinds = true;
  bool nightlight = true;
  bool location = true;
  bool idle = true;
  bool hooks = true;
  bool theme = true;
  bool controlCenter = true;
  bool plugins = true;
  bool hotCorners = true;

  [[nodiscard]] bool any() const noexcept {
    return bars
        || widgets
        || desktopWidgets
        || lockscreenWidgets
        || wallpaper
        || backdrop
        || lockscreen
        || dock
        || shell
        || osd
        || notification
        || weather
        || calendar
        || system
        || audio
        || brightness
        || battery
        || keybinds
        || nightlight
        || location
        || idle
        || hooks
        || theme
        || controlCenter
        || plugins
        || hotCorners;
  }
};

// Per-section diff using the same comparison semantics as configEqual()
// (bars/widgets/desktop widgets get their specialized comparators). Implemented
// in config_overrides.cpp alongside those comparators.
[[nodiscard]] ConfigChangeSet computeConfigChangeSet(const Config& prev, const Config& next);
