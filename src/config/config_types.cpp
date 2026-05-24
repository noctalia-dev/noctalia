#include "config/config_types.h"

#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
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

std::vector<SessionPanelActionConfig> defaultSessionPanelActions() {
  return {
      SessionPanelActionConfig{
          "lock", true, std::nullopt, std::nullopt, std::nullopt, SessionActionButtonVariant::Default,
          KeyChord{XKB_KEY_1, 0}
      },
      SessionPanelActionConfig{
          "logout", true, std::nullopt, std::nullopt, std::nullopt, SessionActionButtonVariant::Default,
          KeyChord{XKB_KEY_2, 0}
      },
      SessionPanelActionConfig{
          "suspend", true, std::nullopt, std::nullopt, std::nullopt, SessionActionButtonVariant::Default,
          KeyChord{XKB_KEY_3, 0}
      },
      SessionPanelActionConfig{
          "reboot", true, std::nullopt, std::nullopt, std::nullopt, SessionActionButtonVariant::Default,
          KeyChord{XKB_KEY_4, 0}
      },
      SessionPanelActionConfig{
          "shutdown", true, std::nullopt, std::nullopt, std::nullopt, SessionActionButtonVariant::Destructive,
          KeyChord{XKB_KEY_5, 0}
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
          .name = "suspend",
          .enabled = false,
          .timeoutSeconds = 900,
          .action = "suspend",
          .command = "",
          .resumeCommand = "",
          .lockBeforeSuspend = true,
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
    return std::clamp(backgroundOpacity + 0.10f, 0.78f, 0.88f);
  case PanelTransparencyMode::Glass:
    return std::clamp(backgroundOpacity + 0.02f, 0.58f, 0.70f);
  }
  return 1.0f;
}

float detachedPanelBackgroundOpacityForTransparencyMode(PanelTransparencyMode mode) noexcept {
  switch (mode) {
  case PanelTransparencyMode::Solid:
    return 1.0f;
  case PanelTransparencyMode::Soft:
    return 0.90f;
  case PanelTransparencyMode::Glass:
    return 0.72f;
  }
  return 1.0f;
}

void inferIdleBehaviorActionFromLegacyFields(IdleBehaviorConfig& behavior) {
  if (!behavior.action.empty()) {
    return;
  }
  if (behavior.command == "noctalia:screen-lock") {
    behavior.action = "lock";
    return;
  }
  if (behavior.command == "noctalia:dpms-off") {
    behavior.action = "screen_off";
    return;
  }
  if (behavior.command == "noctalia:suspend") {
    behavior.action = "suspend";
    return;
  }
  behavior.action = "command";
}

ResolvedIdleBehavior resolveIdleBehaviorActions(const IdleBehaviorConfig& behavior) {
  IdleBehaviorConfig tmp = behavior;
  inferIdleBehaviorActionFromLegacyFields(tmp);
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
        .idleAction =
            IdleActionRequest{
                .kind = IdleActionKind::Suspend, .command = {}, .lockBeforeSuspend = tmp.lockBeforeSuspend
            },
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

bool WidgetConfig::hasSetting(const std::string& key) const { return settings.find(key) != settings.end(); }

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

  if (widget != nullptr && widget->hasSetting("capsule_group")) {
    spec.group = StringUtils::trim(widget->getString("capsule_group", ""));
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

ColorSpec colorSpecFromConfigString(const std::string& raw, std::string_view context) {
  return parseColorSpecString(raw, context);
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
          (pos + match.size() == output.description.size() ||
           std::isspace(static_cast<unsigned char>(output.description[pos + match.size()])) != 0);
      if (startOk && endOk) {
        return true;
      }
      ++pos;
    }
  }
  return false;
}
