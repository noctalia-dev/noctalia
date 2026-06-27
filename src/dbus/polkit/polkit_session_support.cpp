#include "dbus/polkit/polkit_session_support.h"

#include <cstdlib>

namespace polkit_session {

  bool hasXdgSessionId() noexcept {
    const char* sessionId = std::getenv("XDG_SESSION_ID");
    return sessionId != nullptr && sessionId[0] != '\0';
  }

  bool likelySupportsInSessionPolkitAgent(const bool logindOnSystemBus) noexcept {
    return hasXdgSessionId() || logindOnSystemBus;
  }

  bool isNoSessionForPidError(const std::string_view error) noexcept { return error.contains("No session for pid"); }

} // namespace polkit_session
