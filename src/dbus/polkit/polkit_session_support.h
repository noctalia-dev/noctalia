#pragma once

#include <string_view>

namespace polkit_session {

  bool hasXdgSessionId() noexcept;

  // True when polkit can resolve a graphical session (logind/elogind) or XDG_SESSION_ID is set.
  bool likelySupportsInSessionPolkitAgent(bool logindOnSystemBus) noexcept;

  bool isNoSessionForPidError(std::string_view error) noexcept;

} // namespace polkit_session
