#pragma once

#include <functional>
#include <optional>
#include <sdbus-c++/Types.h>
#include <string>
#include <string_view>
#include <vector>

namespace sdbus {
  class IConnection;
} // namespace sdbus

namespace logind {

  // Which lookup answered. Only XdgSessionId and ProcessId identify THIS
  // process' session; UserDisplay is a best-effort guess (see below), so
  // callers log the source rather than treating them as equivalent.
  enum class SessionSource { XdgSessionId, ProcessId, UserDisplay };

  [[nodiscard]] std::string_view describe(SessionSource source);

  struct ResolvedSession {
    sdbus::ObjectPath path;
    SessionSource source = SessionSource::XdgSessionId;
  };

  struct UserSession {
    std::string id;
    sdbus::ObjectPath path;
  };

  // org.freedesktop.login1.User: the Display session plus every session this
  // user owns.
  struct UserSessions {
    std::string displayId;
    sdbus::ObjectPath displayPath;
    std::vector<UserSession> all;
  };

  // True when the user has more than one session, i.e. the Display session is
  // a guess: logind picks it per user, not per caller, so a shell started by
  // the systemd user manager can be handed a session it is not running in.
  [[nodiscard]] bool displaySessionIsAmbiguous(const UserSessions& sessions);

  // The three lookups, injectable so the fallback order and the Display choice
  // can be tested without a bus. Each returns nullopt when it does not answer.
  struct SessionLookups {
    std::function<std::optional<std::string>()> xdgSessionId;
    std::function<std::optional<sdbus::ObjectPath>(const std::string& id)> sessionById;
    std::function<std::optional<sdbus::ObjectPath>()> sessionByProcessId;
    std::function<std::optional<UserSessions>()> userSessions;
  };

  // XDG_SESSION_ID, then GetSessionByPID, then this user's Display session.
  // The last one exists because neither of the first two works when the shell
  // is started by the systemd user manager: user@.service lives outside the
  // login session's cgroup, so GetSessionByPID answers NoSessionForPID, and
  // XDG_SESSION_ID is not in that manager's environment either.
  [[nodiscard]] std::optional<ResolvedSession> resolveSession(const SessionLookups& lookups);

  // Same resolution, driven against logind on `connection`.
  [[nodiscard]] std::optional<ResolvedSession> resolveSession(sdbus::IConnection& connection);

} // namespace logind
