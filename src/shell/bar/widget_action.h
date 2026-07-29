#pragma once

#include "shell/bar/widget_gesture.h"

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct WidgetConfig;

namespace noctalia::bar {

  // Reserved first tokens in the action grammar. They can never be IPC command names, or a
  // binding would resolve to two different things; tests/ipc_service_test.cpp asserts this.
  inline constexpr std::string_view kExecVerb = "exec";
  inline constexpr std::string_view kNoneVerb = "none";

  // A resolved gesture action.
  //
  //   <verb> [args...]     an IpcService command line
  //   exec <command line>  a shell command
  //   none                 explicitly unbound, overriding any inherited default
  //
  // Resolution is a single lookup in a single namespace. An unrecognised verb is an error; it
  // never falls through to a shell command.
  struct WidgetAction {
    enum class Kind : std::uint8_t { Ipc, Exec, None };

    Kind kind = Kind::None;
    std::string verb; // Ipc only
    std::string args; // Ipc: the tail; Exec: the whole command line

    [[nodiscard]] bool isBound() const noexcept { return kind != Kind::None; }
    // The line handed to IpcService::execute(). Ipc actions only.
    [[nodiscard]] std::string commandLine() const;

    bool operator==(const WidgetAction&) const = default;
  };

  // Grammar-level parse. Whether an Ipc verb is actually registered is settled at dispatch time
  // against the live registry: bars are built before any IPC handler exists, and `noctalia config
  // validate` has no running service either. Returns a human-readable reason on failure.
  [[nodiscard]] std::expected<WidgetAction, std::string> parseWidgetAction(std::string_view value);

  struct GestureBinding {
    Gesture gesture;
    std::string_view action;
  };

  // Bindings for one widget, merged from four last-wins layers:
  //   1. built-in defaults shared by every widget
  //   2. the widget type's declared defaults
  //   3. [bar.<name>.actions]
  //   4. [widget.<name>.actions]
  class WidgetActionBindings {
  public:
    using ActionTable = std::unordered_map<std::string, std::string>;

    struct Inputs {
      std::span<const GestureBinding> builtinDefaults;
      std::span<const GestureBinding> widgetDefaults;
      const ActionTable* barActions = nullptr;
      const ActionTable* widgetActions = nullptr;
      GestureMask reserved;
      // Config path prefixes for diagnostics, e.g. "widget.media" and "bar.default".
      std::string_view widgetContext;
      std::string_view barContext;
      // Widget instance name, used to say which widget rejected a bar-wide binding.
      std::string_view widgetName;
      // Widget type, so a malformed declared default names its source.
      std::string_view widgetType;
    };

    void resolve(const Inputs& inputs);

    // nullptr when the gesture is unbound.
    [[nodiscard]] const WidgetAction* find(Gesture gesture) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] GestureMask boundGestures() const noexcept;

  private:
    std::array<std::optional<WidgetAction>, kGestureCount> m_actions;
  };

  // Reads an `actions` sub-table off a widget config entry, or nullptr when absent.
  [[nodiscard]] const WidgetActionBindings::ActionTable* findActionTable(const WidgetConfig* config);

} // namespace noctalia::bar
