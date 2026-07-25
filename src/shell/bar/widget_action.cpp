#include "shell/bar/widget_action.h"

#include "config/config_types.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <format>
#include <utility>

namespace noctalia::bar {

  namespace {

    constexpr Logger kLog("bar.actions");
    constexpr std::string_view kActionsTableKey = "actions";

  } // namespace

  std::string WidgetAction::commandLine() const {
    if (args.empty()) {
      return verb;
    }
    return std::format("{} {}", verb, args);
  }

  std::expected<WidgetAction, std::string> parseWidgetAction(std::string_view value) {
    const std::string_view trimmed = StringUtils::trimRightView(StringUtils::trimLeftView(value));
    if (trimmed.empty()) {
      return std::unexpected(std::string("action is empty (use \"none\" to unbind)"));
    }

    const auto space = trimmed.find(' ');
    const std::string_view verb = space == std::string_view::npos ? trimmed : trimmed.substr(0, space);
    const std::string_view tail =
        space == std::string_view::npos ? std::string_view{} : StringUtils::trimLeftView(trimmed.substr(space + 1));

    if (verb == kNoneVerb) {
      if (!tail.empty()) {
        return std::unexpected(std::format("\"{}\" takes no arguments", kNoneVerb));
      }
      return WidgetAction{.kind = WidgetAction::Kind::None};
    }

    if (verb == kExecVerb) {
      if (tail.empty()) {
        return std::unexpected(std::format("\"{}\" needs a command line", kExecVerb));
      }
      return WidgetAction{.kind = WidgetAction::Kind::Exec, .args = std::string(tail)};
    }

    return WidgetAction{
        .kind = WidgetAction::Kind::Ipc,
        .verb = std::string(verb),
        .args = std::string(tail),
    };
  }

  const WidgetActionBindings::ActionTable* findActionTable(const WidgetConfig* config) {
    if (config == nullptr) {
      return nullptr;
    }
    const auto it = config->tables.find(std::string(kActionsTableKey));
    return it != config->tables.end() ? &it->second : nullptr;
  }

  void WidgetActionBindings::resolve(const Inputs& inputs) {
    m_actions = {};

    // Layers 1 and 2 are declared in C++ and are a programming error if malformed, so a bad entry
    // is loud but does not disable the widget.
    const auto applyDefaults = [this](std::span<const GestureBinding> defaults, std::string_view origin) {
      for (const auto& binding : defaults) {
        auto parsed = parseWidgetAction(binding.action);
        if (!parsed.has_value()) {
          kLog.error(
              "{} declares an invalid default for '{}': {}", origin, gestureConfigKey(binding.gesture), parsed.error()
          );
          continue;
        }
        m_actions[static_cast<std::size_t>(binding.gesture)] = std::move(*parsed);
      }
    };
    applyDefaults(inputs.builtinDefaults, "built-in widget defaults");
    const std::string typeOrigin = std::format("'{}' widget defaults", inputs.widgetType);
    applyDefaults(inputs.widgetDefaults, typeOrigin);

    // Layers 3 and 4 come from user config: every rejection names its exact config path.
    const auto applyConfig = [&](const ActionTable* table, std::string_view context, bool barWide) {
      if (table == nullptr) {
        return;
      }
      for (const auto& [key, value] : *table) {
        const auto gesture = parseGestureKey(key);
        if (!gesture.has_value()) {
          kLog.error("{}.{}.{}: unknown gesture", context, kActionsTableKey, key);
          continue;
        }
        if (inputs.reserved.contains(*gesture)) {
          // A bar-wide binding is expected to skip the widgets that own the gesture per item, so
          // only an explicit per-widget binding is a mistake worth shouting about.
          if (barWide) {
            kLog.debug(
                "{}.{}.{}: not applied to '{}', which handles it on its individual items", context, kActionsTableKey,
                key, inputs.widgetName
            );
          } else {
            kLog.error(
                "{}.{}.{}: '{}' handles this on its individual items and it cannot be rebound", context,
                kActionsTableKey, key, inputs.widgetName
            );
          }
          continue;
        }
        auto parsed = parseWidgetAction(value);
        if (!parsed.has_value()) {
          kLog.error("{}.{}.{}: {}", context, kActionsTableKey, key, parsed.error());
          continue;
        }
        m_actions[static_cast<std::size_t>(*gesture)] = std::move(*parsed);
      }
    };
    applyConfig(inputs.barActions, inputs.barContext, true);
    applyConfig(inputs.widgetActions, inputs.widgetContext, false);

    // Reserved gestures never dispatch, even if a default declared one.
    for (const auto gesture : allGestures()) {
      if (inputs.reserved.contains(gesture)) {
        m_actions[static_cast<std::size_t>(gesture)].reset();
      }
    }
  }

  const WidgetAction* WidgetActionBindings::find(Gesture gesture) const noexcept {
    const auto& action = m_actions[static_cast<std::size_t>(gesture)];
    if (!action.has_value() || !action->isBound()) {
      return nullptr;
    }
    return &*action;
  }

  bool WidgetActionBindings::empty() const noexcept { return boundGestures().empty(); }

  GestureMask WidgetActionBindings::boundGestures() const noexcept {
    GestureMask mask;
    for (const auto gesture : allGestures()) {
      if (find(gesture) != nullptr) {
        mask.set(gesture);
      }
    }
    return mask;
  }

} // namespace noctalia::bar
