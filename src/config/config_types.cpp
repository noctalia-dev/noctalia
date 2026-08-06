#include "config/config_types.h"

#include "core/input/key_modifiers.h"
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
       .location = "https://github.com/noctalia-dev/official-plugins"},
      {.kind = PluginSourceKind::Git,
       .name = "community",
       .location = "https://github.com/noctalia-dev/community-plugins"},
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
    return {
        {.sym = XKB_KEY_Return, .modifiers = 0},
        {.sym = XKB_KEY_KP_Enter, .modifiers = 0},
        {.sym = XKB_KEY_space, .modifiers = 0},
    };
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
  case KeybindAction::TabNext:
    return {{.sym = XKB_KEY_Tab, .modifiers = 0}};
  case KeybindAction::TabPrevious:
    return {{.sym = XKB_KEY_ISO_Left_Tab, .modifiers = KeyMod::Shift}};
  case KeybindAction::Delete:
    return {{.sym = XKB_KEY_Delete, .modifiers = 0}};
  case KeybindAction::Copy:
    return {{.sym = XKB_KEY_c, .modifiers = KeyMod::Ctrl}};
  case KeybindAction::Save:
    return {{.sym = XKB_KEY_s, .modifiers = KeyMod::Ctrl}};
  }
  return {};
}

float panelCardOpacityForTransparencyMode(PanelTransparencyMode mode, float panelBackgroundOpacity) noexcept {
  const float backgroundOpacity = std::clamp(panelBackgroundOpacity, 0.0F, 1.0F);
  switch (mode) {
  case PanelTransparencyMode::Solid:
    return 1.0F;
  case PanelTransparencyMode::Soft:
    return std::clamp(backgroundOpacity + 0.08F, 0.82F, 0.92F);
  case PanelTransparencyMode::Glass:
    return std::clamp(backgroundOpacity + 0.10F, 0.62F, 0.75F);
  }
  return 1.0F;
}

float detachedPanelBackgroundOpacityForTransparencyMode(PanelTransparencyMode mode) noexcept {
  switch (mode) {
  case PanelTransparencyMode::Solid:
    return 1.0F;
  case PanelTransparencyMode::Soft:
    return 0.80F;
  case PanelTransparencyMode::Glass:
    return 0.55F;
  }
  return 1.0F;
}

void normalizeIdleBehaviorAction(IdleBehaviorConfig& behavior) {
  if (behavior.action == "suspend" && behavior.lockBeforeSuspend) {
    behavior.action = "lock_and_suspend";
  }
}

ResolvedIdleBehavior resolveIdleBehaviorActions(const IdleBehaviorConfig& behavior) {
  IdleBehaviorConfig tmp = behavior;
  normalizeIdleBehaviorAction(tmp);
  const std::string& act = tmp.action;

  if (act == "lock") {
    return {
        .idleAction = idleAction(IdleActionKind::Lock),
        .resumeAction = {},
        .resumeCommand = tmp.resumeCommand,
    };
  }
  if (act == "screen_off") {
    return {
        .idleAction = idleAction(IdleActionKind::ScreenOff),
        .resumeAction = idleAction(IdleActionKind::ScreenOn),
        .resumeCommand = tmp.resumeCommand,
    };
  }
  if (act == "suspend") {
    return {
        .idleAction = idleAction(IdleActionKind::Suspend),
        .resumeAction = {},
        .resumeCommand = tmp.resumeCommand,
    };
  }
  if (act == "lock_and_suspend") {
    return {
        .idleAction = idleAction(IdleActionKind::LockAndSuspend),
        .resumeAction = {},
        .resumeCommand = tmp.resumeCommand,
    };
  }
  return {
      .idleAction = commandIdleAction(behavior.command),
      .resumeAction = {},
      .resumeCommand = behavior.resumeCommand,
  };
}

const WidgetSettingValue* WidgetConfig::findSetting(const std::string& key) const {
  const auto it = settings.find(key);
  return it != settings.end() ? &it->second : nullptr;
}

std::string WidgetConfig::getString(const std::string& key, const std::string& fallback) const {
  const auto* value = findSetting(key);
  const auto decoded = value != nullptr ? noctalia::config::widgetSettingValueAs<std::string>(*value) : std::nullopt;
  return decoded.value_or(fallback);
}

std::vector<std::string>
WidgetConfig::getStringList(const std::string& key, const std::vector<std::string>& fallback) const {
  const auto* value = findSetting(key);
  const auto decoded =
      value != nullptr ? noctalia::config::widgetSettingValueAs<std::vector<std::string>>(*value) : std::nullopt;
  return decoded.value_or(fallback);
}

std::int64_t WidgetConfig::getInt(const std::string& key, std::int64_t fallback) const {
  const auto* value = findSetting(key);
  const auto decoded = value != nullptr ? noctalia::config::widgetSettingValueAs<std::int64_t>(*value) : std::nullopt;
  return decoded.value_or(fallback);
}

double WidgetConfig::getDouble(const std::string& key, double fallback) const {
  const auto* value = findSetting(key);
  const auto decoded = value != nullptr ? noctalia::config::widgetSettingValueAs<double>(*value) : std::nullopt;
  return decoded.value_or(fallback);
}

bool WidgetConfig::getBool(const std::string& key, bool fallback) const {
  const auto* value = findSetting(key);
  const auto decoded = value != nullptr ? noctalia::config::widgetSettingValueAs<bool>(*value) : std::nullopt;
  return decoded.value_or(fallback);
}

ColorSpec
WidgetConfig::getColorSpec(const std::string& key, const ColorSpec& fallback, std::string_view context) const {
  const auto* value = findSetting(key);
  const auto decoded = value != nullptr
      ? noctalia::config::widgetSettingValueAs<ColorSpec>(*value, context.empty() ? std::string_view(key) : context)
      : std::nullopt;
  return decoded.value_or(fallback);
}

std::optional<ColorSpec> WidgetConfig::getOptionalColorSpec(const std::string& key, std::string_view context) const {
  const auto* value = findSetting(key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (const auto* v = std::get_if<std::string>(value)) {
    if (StringUtils::trim(*v).empty()) {
      return std::nullopt;
    }
  }
  return noctalia::config::widgetSettingValueAs<ColorSpec>(*value, context.empty() ? std::string_view(key) : context);
}

std::unordered_map<std::string, std::string>
WidgetConfig::getStringMap(const std::string& key, const std::unordered_map<std::string, std::string>& fallback) const {
  const auto it = tables.find(key);
  if (it == tables.end()) {
    return fallback;
  }
  return it->second;
}

bool WidgetConfig::hasSetting(const std::string& key) const { return findSetting(key) != nullptr; }

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
        static_cast<float>(widget->getDouble("capsule_padding", static_cast<double>(spec.padding))), 0.0F, 48.0F
    );
  }
  if (bar.widgetCapsuleRadius.has_value()) {
    spec.radius = std::clamp(static_cast<float>(*bar.widgetCapsuleRadius), 0.0F, 80.0F);
  }
  if (widget != nullptr) {
    const auto radius = widget->settings.find("capsule_radius");
    if (radius != widget->settings.end()
        && (std::holds_alternative<double>(radius->second) || std::holds_alternative<std::int64_t>(radius->second))) {
      spec.radius = std::clamp(
          static_cast<float>(widget->getDouble("capsule_radius", static_cast<double>(spec.radius.value_or(0.0F)))),
          0.0F, 80.0F
      );
    }
  }
  spec.opacity = bar.widgetCapsuleOpacity;
  if (widget != nullptr && widget->hasSetting("capsule_opacity")) {
    spec.opacity = std::clamp(
        static_cast<float>(widget->getDouble("capsule_opacity", static_cast<double>(spec.opacity))), 0.0F, 1.0F
    );
  }
  spec.hoverHighlight = bar.hoverHighlight;

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

namespace {
  void collectCapsuleGroupRefs(const std::vector<std::string>& lane, std::set<std::string>& out) {
    for (const auto& entry : lane) {
      if (isCapsuleGroupToken(entry)) {
        out.insert(capsuleGroupTokenId(entry));
      }
    }
  }

  const std::vector<std::string>&
  effectiveLane(const std::optional<std::vector<std::string>>& monitorLane, const std::vector<std::string>& barLane) {
    return monitorLane.has_value() ? *monitorLane : barLane;
  }
} // namespace

std::set<std::string> capsuleGroupRefsForBarScope(const BarConfig& bar) {
  std::set<std::string> refs;
  collectCapsuleGroupRefs(bar.startWidgets, refs);
  collectCapsuleGroupRefs(bar.centerWidgets, refs);
  collectCapsuleGroupRefs(bar.endWidgets, refs);
  for (const auto& ovr : bar.monitorOverrides) {
    if (ovr.widgetCapsuleGroups.has_value()) {
      continue;
    }
    collectCapsuleGroupRefs(effectiveLane(ovr.startWidgets, bar.startWidgets), refs);
    collectCapsuleGroupRefs(effectiveLane(ovr.centerWidgets, bar.centerWidgets), refs);
    collectCapsuleGroupRefs(effectiveLane(ovr.endWidgets, bar.endWidgets), refs);
  }
  return refs;
}

std::set<std::string> capsuleGroupRefsForMonitorScope(const BarConfig& bar, const BarMonitorOverride& monitorOverride) {
  std::set<std::string> refs;
  collectCapsuleGroupRefs(effectiveLane(monitorOverride.startWidgets, bar.startWidgets), refs);
  collectCapsuleGroupRefs(effectiveLane(monitorOverride.centerWidgets, bar.centerWidgets), refs);
  collectCapsuleGroupRefs(effectiveLane(monitorOverride.endWidgets, bar.endWidgets), refs);
  return refs;
}

std::vector<BarCapsuleGroupStyle> reconcileCapsuleGroups(
    const std::vector<BarCapsuleGroupStyle>& current, const std::vector<BarCapsuleGroupStyle>& base,
    const std::set<std::string>& referenced
) {
  std::vector<BarCapsuleGroupStyle> out;
  out.reserve(current.size() + base.size());
  std::vector<bool> consumed(current.size(), false);
  for (const auto& baseGroup : base) {
    bool matched = false;
    for (std::size_t i = 0; i < current.size(); ++i) {
      if (!consumed[i] && current[i].id == baseGroup.id) {
        out.push_back(current[i]);
        consumed[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched && referenced.contains(baseGroup.id)) {
      out.push_back(baseGroup);
    }
  }
  for (std::size_t i = 0; i < current.size(); ++i) {
    if (!consumed[i] && referenced.contains(current[i].id)) {
      out.push_back(current[i]);
    }
  }
  return out;
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
    spec.radius = std::clamp(static_cast<float>(*bar.widgetCapsuleRadius), 0.0F, 80.0F);
  } else {
    spec.radius = std::nullopt;
  }
  spec.opacity = group.opacity;
  spec.accordion = group.accordion;
  spec.accordionDirection = group.accordionDirection;
  spec.widgetSpacing =
      group.widgetSpacing.has_value() ? std::optional<float>{static_cast<float>(*group.widgetSpacing)} : std::nullopt;
  spec.hoverHighlight = bar.hoverHighlight;
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

  return barScale * std::clamp(static_cast<float>(widgetScale), 0.2F, 2.5F);
}

CommonWidgetOptions resolveCommonWidgetOptions(
    const BarConfig& bar, const WidgetConfig* widget, std::string_view widgetType, float barScale
) {
  CommonWidgetOptions options;
  options.interactive = widgetType != "spacer";
  options.contentScale = resolveWidgetContentScale(barScale, widget);
  options.capsule = resolveWidgetBarCapsuleSpec(bar, widget);
  if (widget == nullptr) {
    return options;
  }

  options.enabled = widget->getBool("enabled", true);
  options.anchor = widget->getBool("anchor", false);
  options.interactive = widget->getBool("interactive", options.interactive);
  options.color = widget->getOptionalColorSpec("color", "widget.color");
  options.iconColor = widget->getOptionalColorSpec("icon_color", "widget.icon_color");
  if (const auto* fontWeight = widget->findSetting("font_weight");
      fontWeight != nullptr && std::holds_alternative<std::int64_t>(*fontWeight)) {
    options.labelFontWeight = std::get<std::int64_t>(*fontWeight);
  }
  options.labelFontFamily = widget->getString("font_family");
  options.scrollRepeat = widget->getString("scroll_repeat", "auto");
  options.enableScroll = widget->getBool("enable_scroll", true);
  return options;
}

ColorSpec colorSpecFromConfigString(const std::string& raw, std::string_view context) {
  return parseColorSpecString(raw, context);
}

namespace {
  int colorByteForExport(float value) { return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F)); }

  std::string colorToConfigString(const Color& color) {
    if (color.a >= 0.999F) {
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

  // Each identity field is searched independently so selector semantics don't depend on
  // whether the compositor advertised output management, nor on any join order.
  for (const std::string* field : {&output.description, &output.make, &output.model, &output.serialNumber}) {
    if (StringUtils::containsWholeToken(*field, match)) {
      return true;
    }
  }
  return false;
}
