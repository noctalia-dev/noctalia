#pragma once

#include "config/config_types.h"

#include <optional>
#include <string_view>

// Metadata for session/power action identifiers ("lock", "logout", "suspend",
// "lock_and_suspend", "reboot", "shutdown", "command"). Shared by the session
// panel, the launcher session provider, and the settings editor so the action
// vocabulary, i18n label keys, and default glyphs live in one place.
namespace session_action {

  [[nodiscard]] bool isKnown(std::string_view action);
  [[nodiscard]] const char* labelKey(std::string_view action);
  [[nodiscard]] const char* defaultGlyph(std::string_view action);

  // IPC names use hyphens (lock-and-suspend); config stores underscores.
  [[nodiscard]] std::optional<std::string_view> canonicalActionName(std::string_view ipcOrConfigAction);

  // First enabled `shell.session.actions` row for `action`, or the built-in default row when
  // the configured list omits that action entirely.
  [[nodiscard]] std::optional<SessionPanelActionConfig>
  resolveConfiguredAction(const ShellSessionConfig& session, std::string_view action);

} // namespace session_action
