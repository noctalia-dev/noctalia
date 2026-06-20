#include "config/config_types.h"

#include "render/core/color.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  IdleActionRequest commandIdleAction(std::string command) {
    if (command.empty()) {
      return {};
    }
    return IdleActionRequest{.kind = IdleActionKind::Command, .command = std::move(command)};
  }

  IdleActionRequest idleAction(IdleActionKind kind) { return IdleActionRequest{.kind = kind, .command = {}}; }

  std::string colorSpecError(const std::string& raw, std::string_view context) {
    std::string message;
    if (!context.empty()) {
      message += context;
      message += ": ";
    }
    message += "invalid color value \"";
    message += raw;
    message += "\" (expected a color role token or hex color)";
    return message;
  }

  ColorSpec parseColorSpecString(const std::string& raw, std::string_view context) {
    const std::string trimmed = StringUtils::trim(raw);
    Color fixed;
    if (tryParseHexColor(trimmed, fixed)) {
      return fixedColorSpec(fixed);
    }
    if (auto role = colorRoleFromToken(trimmed)) {
      return colorSpecFromRole(*role);
    }
    throw std::runtime_error(colorSpecError(raw, context));
  }

} // namespace

std::vector<ShortcutConfig> defaultControlCenterShortcuts() {
  return {
      {"wifi"}, {"bluetooth"}, {"caffeine"}, {"nightlight"}, {"notification"}, {"power_profile"},
  };
}

std::vector<PluginSourceConfig> defaultPluginSources() {
  return {
      {.kind = PluginSourceKind::Git,
       .name = "official",
       .location = "https://github.com/noctalia-dev/official-plugins",
       .autoUpdate = false},
      {.kind = PluginSourceKind::Git,
       .name = "community",
       .location = "https://github.com/noctalia-dev/community-plugins",
       .autoUpdate = false},
  };
}

bool isDefaultPluginSourceName(std::string_view name) {
  const auto sources = defaultPluginSources();
  return std::ranges::contains(sources, name, &PluginSourceConfig::name);
}

bool isValidPluginSourceName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  const auto first = static_cast<unsigned char>(name.front());
  if (std::isalnum(first) == 0) {
    return false;
  }
  for (const char ch : name) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) != 0 || ch == '_' || ch == '-' || ch == '.') {
      continue;
    }
    return false;
  }
  return name != "." && name != "..";
}

std::vector<SessionPanelActionConfig> defaultSessionPanelActions() {
  return {
      SessionPanelActionConfig{
          .action = "lock",
          .shortcut = KeyChord{.sym = XKB_KEY_1},
      },
      SessionPanelActionConfig{
          .action = "logout",
          .shortcut = KeyChord{.sym = XKB_KEY_2},
      },
      SessionPanelActionConfig{
          .action = "lock_and_suspend",
          .shortcut = KeyChord{.sym = XKB_KEY_3},
      },
      SessionPanelActionConfig{
          .action = "reboot",
          .shortcut = KeyChord{.sym = XKB_KEY_4},
      },
      SessionPanelActionConfig{
          .action = "shutdown",
          .variant = SessionActionButtonVariant::Destructive,
          .shortcut = KeyChord{.sym = XKB_KEY_5},
      },
  };
}

std::vector<IdleBehaviorConfig> defaultIdleBehaviors() {
  return {
      IdleBehaviorConfig{
          .name = "lock",
          .enabled = false,
          .timeoutSeconds = 600,
          .action = "lock",
          .command = "",
          .resumeCommand = "",
      },
      IdleBehaviorConfig{
          .name = "screen-off",
          .enabled = false,
          .timeoutSeconds = 660,
          .action = "screen_off",
          .command = "",
          .resumeCommand = "",
      },
      IdleBehaviorConfig{
          .name = "lock-and-suspend",
          .enabled = false,
          .timeoutSeconds = 900,
          .action = "lock_and_suspend",
          .command = "",
          .resumeCommand = "",
      },
  };
}

std::vector<KeyChord> defaultKeybindSet(KeybindAction action) {
  switch (action) {
  case KeybindAction::Validate:
    return {{.sym = XKB_KEY_Return, .modifiers = 0}, {.sym = XKB_KEY_KP_Enter, .modifiers = 0}};
  case KeybindAction::Cancel:
    return {{.sym = XKB_KEY_Escape, .modifiers = 0}};
  case KeybindAction::Left:
    return {{.sym = XKB_KEY_Left, .modifiers = 0}};
  case KeybindAction::Right:
    return {{.sym = XKB_KEY_Right, .modifiers = 0}};
  case KeybindAction::Up:
    return {{.sym = XKB_KEY_Up, .modifiers = 0}};
  case KeybindAction::Down:
    return {{.sym = XKB_KEY_Down, .modifiers = 0}};
  }
  return {};
}

float panelCardOpacityForTransparencyMode(PanelTransparencyMode mode, float panelBackgroundOpacity) noexcept {
  const float backgroundOpacity = std::clamp(panelBackgroundOpacity, 0.0f, 1.0f);
  switch (mode) {
  case PanelTransparencyMode::Solid:
    return 1.0f;
  case PanelTransparencyMode::Soft:
    return std::clamp(backgroundOpacity + 0.08f, 0.82f, 0.92f);
  case PanelTransparencyMode::Glass:
    return std::clamp(backgroundOpacity + 0.10f, 0.62f, 0.75f);
  }
  return 1.0f;
}

float detachedPanelBackgroundOpacityForTransparencyMode(PanelTransparencyMode mode) noexcept {
  switch (mode) {
  case PanelTransparencyMode::Solid:
    return 1.0f;
  case PanelTransparencyMode::Soft:
    return 0.80f;
  case PanelTransparencyMode::Glass:
    return 0.55f;
  }
  return 1.0f;
}

void inferIdleBehaviorActionFromLegacyFields(IdleBehaviorConfig& behavior) {
  if (!behavior.action.empty()) {
    return;
  }
  if (behavior.command == "noctalia:session lock") {
    behavior.action = "lock";
    return;
  }
  if (behavior.command == "noctalia:dpms-off") {
    behavior.action = "screen_off";
    return;
  }
  if (behavior.command == "noctalia:session suspend") {
    behavior.action = "suspend";
    return;
  }
  if (behavior.command == "noctalia:session lock-and-suspend") {
    behavior.action = "lock_and_suspend";
    return;
  }
  behavior.action = "command";
}

void normalizeIdleBehaviorAction(IdleBehaviorConfig& behavior) {
  inferIdleBehaviorActionFromLegacyFields(behavior);
  if (behavior.action == "suspend" && behavior.lockBeforeSuspend) {
    behavior.action = "lock_and_suspend";
  }
}

ResolvedIdleBehavior resolveIdleBehaviorActions(const IdleBehaviorConfig& behavior) {
  IdleBehaviorConfig tmp = behavior;
  normalizeIdleBehaviorAction(tmp);
  const std::string& act = tmp.action;
  const auto resume = [&tmp](IdleActionRequest fallback) {
    return tmp.resumeCommand.empty() ? std::move(fallback) : commandIdleAction(tmp.resumeCommand);
  };

  if (act == "lock") {
    return {
        .idleAction = idleAction(IdleActionKind::Lock),
        .resumeAction = resume({}),
    };
  }
  if (act == "screen_off") {
    return {
        .idleAction = idleAction(IdleActionKind::ScreenOff),
        .resumeAction = resume(idleAction(IdleActionKind::ScreenOn)),
    };
  }
  if (act == "suspend") {
    return {
        .idleAction = idleAction(IdleActionKind::Suspend),
        .resumeAction = resume({}),
    };
  }
  if (act == "lock_and_suspend") {
    return {
        .idleAction = idleAction(IdleActionKind::LockAndSuspend),
        .resumeAction = resume({}),
    };
  }
  return {
      .idleAction = commandIdleAction(behavior.command),
      .resumeAction = commandIdleAction(behavior.resumeCommand),
  };
}

std::string WidgetConfig::getString(const std::string& key, const std::string& fallback) const {
  auto it = settings.find(key);
  if (it == settings.end()) {
    return fallback;
  }
  if (const auto* v = std::get_if<std::string>(&it->second)) {
    return *v;
  }
  return fallback;
}

std::vector<std::string>
WidgetConfig::getStringList(const std::string& key, const std::vector<std::string>& fallback) const {
  auto it = settings.find(key);
  if (it == settings.end()) {
    return fallback;
  }
  if (const auto* v = std::get_if<std::vector<std::string>>(&it->second)) {
    return *v;
  }
  if (const auto* v = std::get_if<std::string>(&it->second)) {
    return {*v};
  }
  return fallback;
}

std::int64_t WidgetConfig::getInt(const std::string& key, std::int64_t fallback) const {
  auto it = settings.find(key);
  if (it == settings.end()) {
    return fallback;
  }
  if (const auto* v = std::get_if<std::int64_t>(&it->second)) {
    return *v;
  }
  if (const auto* v = std::get_if<double>(&it->second)) {
    return static_cast<std::int64_t>(std::llround(*v));
  }
  return fallback;
}

double WidgetConfig::getDouble(const std::string& key, double fallback) const {
  auto it = settings.find(key);
  if (it == settings.end()) {
    return fallback;
  }
  if (const auto* v = std::get_if<double>(&it->second)) {
    return *v;
  }
  // Allow int -> double promotion.
  if (const auto* v = std::get_if<std::int64_t>(&it->second)) {
    return static_cast<double>(*v);
  }
  return fallback;
}

bool WidgetConfig::getBool(const std::string& key, bool fallback) const {
  auto it = settings.find(key);
  if (it == settings.end()) {
    return fallback;
  }
  if (const auto* v = std::get_if<bool>(&it->second)) {
    return *v;
  }
  return fallback;
}

ColorSpec
WidgetConfig::getColorSpec(const std::string& key, const ColorSpec& fallback, std::string_view context) const {
  auto it = settings.find(key);
  if (it == settings.end()) {
    return fallback;
  }
  if (const auto* v = std::get_if<std::string>(&it->second)) {
    return colorSpecFromConfigString(*v, context.empty() ? std::string_view(key) : context);
  }
  return fallback;
}

std::optional<ColorSpec> WidgetConfig::getOptionalColorSpec(const std::string& key, std::string_view context) const {
  auto it = settings.find(key);
  if (it == settings.end()) {
    return std::nullopt;
  }
  if (const auto* v = std::get_if<std::string>(&it->second)) {
    if (StringUtils::trim(*v).empty()) {
      return std::nullopt;
    }
    return colorSpecFromConfigString(*v, context.empty() ? std::string_view(key) : context);
  }
  return std::nullopt;
}

std::unordered_map<std::string, std::string>
WidgetConfig::getStringMap(const std::string& key, const std::unordered_map<std::string, std::string>& fallback) const {
  const auto it = tables.find(key);
  if (it == tables.end()) {
    return fallback;
  }
  return it->second;
}

bool WidgetConfig::hasSetting(const std::string& key) const { return settings.contains(key); }

WidgetBarCapsuleSpec resolveWidgetBarCapsuleSpec(const BarConfig& bar, const WidgetConfig* widget) {
  WidgetBarCapsuleSpec spec{};
  const bool widgetHasCapsuleKey = widget != nullptr && widget->hasSetting("capsule");
  const bool widgetHasFillKey = widget != nullptr && widget->hasSetting("capsule_fill");
  const bool widgetHasBorderKey = widget != nullptr && widget->hasSetting("capsule_border");

  if (widgetHasCapsuleKey) {
    spec.enabled = widget->getBool("capsule", false);
  } else {
    spec.enabled = bar.widgetCapsuleDefault;
  }

  spec.padding = bar.widgetCapsulePadding;
  if (widget != nullptr && widget->hasSetting("capsule_padding")) {
    spec.padding = std::clamp(
        static_cast<float>(widget->getDouble("capsule_padding", static_cast<double>(spec.padding))), 0.0f, 48.0f
    );
  }
  if (bar.widgetCapsuleRadius.has_value()) {
    spec.radius = std::clamp(static_cast<float>(*bar.widgetCapsuleRadius), 0.0f, 80.0f);
  }
  if (widget != nullptr && widget->hasSetting("capsule_radius")) {
    spec.radius = std::clamp(
        static_cast<float>(widget->getDouble("capsule_radius", static_cast<double>(spec.radius.value_or(0.0f)))), 0.0f,
        80.0f
    );
  }
  spec.opacity = bar.widgetCapsuleOpacity;
  if (widget != nullptr && widget->hasSetting("capsule_opacity")) {
    spec.opacity = std::clamp(
        static_cast<float>(widget->getDouble("capsule_opacity", static_cast<double>(spec.opacity))), 0.0f, 1.0f
    );
  }

  if (!spec.enabled) {
    return spec;
  }

  if (widgetHasFillKey) {
    spec.fill = widget->getColorSpec("capsule_fill", bar.widgetCapsuleFill, "widget.capsule_fill");
  } else {
    spec.fill = bar.widgetCapsuleFill;
  }

  if (widgetHasBorderKey) {
    spec.border = widget->getOptionalColorSpec("capsule_border", "widget.capsule_border");
  } else if (bar.widgetCapsuleBorderSpecified) {
    spec.border = bar.widgetCapsuleBorder;
  } else {
    spec.border = std::nullopt;
  }

  if (widget != nullptr && widget->hasSetting("capsule_foreground")) {
    spec.foreground = widget->getOptionalColorSpec("capsule_foreground", "widget.capsule_foreground");
  } else if (bar.widgetCapsuleForeground.has_value()) {
    spec.foreground = bar.widgetCapsuleForeground;
  } else {
    spec.foreground = std::nullopt;
  }
  return spec;
}

const BarCapsuleGroupStyle* findBarCapsuleGroupStyle(const BarConfig& bar, const std::string& id) {
  if (id.empty()) {
    return nullptr;
  }
  for (const auto& group : bar.widgetCapsuleGroups) {
    if (group.id == id) {
      return &group;
    }
  }
  return nullptr;
}

WidgetBarCapsuleSpec capsuleSpecFromGroup(const BarConfig& bar, const BarCapsuleGroupStyle& group) {
  WidgetBarCapsuleSpec spec;
  spec.enabled = true;
  spec.group = group.id;
  spec.fill = group.fill;
  spec.border = group.borderSpecified ? group.border : std::nullopt;
  spec.foreground = group.foreground;
  spec.padding = group.padding;
  // "Auto" radius (no explicit group radius) inherits the bar's capsule radius; unset at both levels = pill.
  if (group.radius.has_value()) {
    spec.radius = group.radius;
  } else if (bar.widgetCapsuleRadius.has_value()) {
    spec.radius = std::clamp(static_cast<float>(*bar.widgetCapsuleRadius), 0.0f, 80.0f);
  } else {
    spec.radius = std::nullopt;
  }
  spec.opacity = group.opacity;
  return spec;
}

bool isCapsuleGroupToken(std::string_view laneEntry) { return laneEntry.starts_with(kCapsuleGroupTokenPrefix); }

std::string capsuleGroupTokenId(std::string_view laneEntry) {
  if (!isCapsuleGroupToken(laneEntry)) {
    return {};
  }
  return std::string(laneEntry.substr(kCapsuleGroupTokenPrefix.size()));
}

std::string makeCapsuleGroupToken(std::string_view groupId) {
  return std::string(kCapsuleGroupTokenPrefix) + std::string(groupId);
}

float resolveWidgetContentScale(float barScale, const WidgetConfig* widget, std::string_view context) {
  if (widget == nullptr) {
    return barScale;
  }

  const auto it = widget->settings.find("scale");
  if (it == widget->settings.end()) {
    return barScale;
  }

  double widgetScale = 1.0;
  if (const auto* rawDouble = std::get_if<double>(&it->second)) {
    if (!std::isfinite(*rawDouble)) {
      throw std::runtime_error(std::string(context) + ": expected finite number");
    }
    widgetScale = *rawDouble;
  } else if (const auto* rawInt = std::get_if<std::int64_t>(&it->second)) {
    widgetScale = static_cast<double>(*rawInt);
  } else {
    throw std::runtime_error(std::string(context) + ": expected finite number");
  }

  return barScale * std::clamp(static_cast<float>(widgetScale), 0.2f, 2.5f);
}

ColorSpec colorSpecFromConfigString(const std::string& raw, std::string_view context) {
  return parseColorSpecString(raw, context);
}

namespace {
  int colorByteForExport(float value) { return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); }

  std::string colorToConfigString(const Color& color) {
    if (color.a >= 0.999f) {
      return formatRgbHex(color);
    }
    char buffer[16];
    std::snprintf(
        buffer, sizeof(buffer), "#%02X%02X%02X%02X", colorByteForExport(color.r), colorByteForExport(color.g),
        colorByteForExport(color.b), colorByteForExport(color.a)
    );
    return std::string(buffer);
  }
} // namespace

std::string colorSpecToConfigString(const ColorSpec& spec) {
  if (spec.role.has_value()) {
    return std::string(colorRoleToken(*spec.role));
  }
  Color color = spec.fixed;
  color.a *= spec.alpha;
  return colorToConfigString(color);
}

std::optional<HookKind> hookKindFromKey(std::string_view key) { return enumFromKey(kHookKinds, key); }

std::string_view hookKindKey(HookKind kind) {
  const std::string_view key = enumToKey(kHookKinds, kind);
  return key.empty() ? "unknown" : key;
}

bool outputMatchesSelector(const std::string& match, const WaylandOutput& output) {
  // Exact connector name match.
  if (!output.connectorName.empty() && match == output.connectorName) {
    return true;
  }

  // Word-boundary substring match on description. A bare substring search would
  // let "DP-1" match "eDP-1" inside descriptions like "BOE 0x0BCA eDP-1".
  if (!output.description.empty()) {
    std::size_t pos = 0;
    while ((pos = output.description.find(match, pos)) != std::string::npos) {
      const bool startOk = (pos == 0 || std::isspace(static_cast<unsigned char>(output.description[pos - 1])) != 0);
      const bool endOk =
          (pos + match.size() == output.description.size()
           || std::isspace(static_cast<unsigned char>(output.description[pos + match.size()])) != 0);
      if (startOk && endOk) {
        return true;
      }
      ++pos;
    }
  }
  return false;
}
