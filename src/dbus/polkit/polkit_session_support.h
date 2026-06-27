#pragma once

#include <string_view>

namespace polkit_session {

  [[nodiscard]] bool hasXdgSessionId() noexcept;

  // True when polkit can resolve a graphical session (logind/elogind) or XDG_SESSION_ID is set.
  [[nodiscard]] bool likelySupportsInSessionPolkitAgent(bool logindOnSystemBus) noexcept;

  [[nodiscard]] bool isNoSessionForPidError(std::string_view error) noexcept;

} // namespace polkit_session
