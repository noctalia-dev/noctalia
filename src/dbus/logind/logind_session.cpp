#include "dbus/logind/logind_session.h"

#include "core/log.h"

#include <cstdlib>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <unistd.h>
#include <utility>

namespace {
  constexpr Logger kLog("logind");

  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kLogindUserInterface = "org.freedesktop.login1.User";
} // namespace

namespace logind {

  std::string_view describe(SessionSource source) {
    switch (source) {
    case SessionSource::XdgSessionId:
      return "XDG_SESSION_ID";
    case SessionSource::ProcessId:
      return "pid";
    case SessionSource::UserDisplay:
      return "user Display session";
    }
    return "unknown";
  }

  bool displaySessionIsAmbiguous(const UserSessions& sessions) { return sessions.all.size() > 1; }

  std::optional<ResolvedSession> resolveSession(const SessionLookups& lookups) {
    if (lookups.xdgSessionId && lookups.sessionById) {
      if (const auto sessionId = lookups.xdgSessionId(); sessionId.has_value() && !sessionId->empty()) {
        if (auto path = lookups.sessionById(*sessionId); path.has_value() && !path->empty()) {
          return ResolvedSession{.path = std::move(*path), .source = SessionSource::XdgSessionId};
        }
      }
    }

    if (lookups.sessionByProcessId) {
      if (auto path = lookups.sessionByProcessId(); path.has_value() && !path->empty()) {
        return ResolvedSession{.path = std::move(*path), .source = SessionSource::ProcessId};
      }
    }

    if (!lookups.userSessions) {
      return std::nullopt;
    }
    const auto sessions = lookups.userSessions();
    if (!sessions.has_value()) {
      return std::nullopt;
    }
    if (sessions->displayPath.empty()) {
      kLog.warn("logind reports no display session for this user");
      return std::nullopt;
    }

    // Deliberate best-effort: with several concurrent sessions this can pick a
    // session other than the one this process runs in. Say so out loud rather
    // than have a lock request quietly land on the wrong seat.
    if (displaySessionIsAmbiguous(*sessions)) {
      kLog.warn(
          "logind: {} concurrent sessions for this user, falling back to the Display session {} ({}) — it may not be "
          "the session this process runs in",
          sessions->all.size(), sessions->displayId, static_cast<std::string>(sessions->displayPath)
      );
    } else {
      kLog.debug("resolved logind session via user Display: {}", static_cast<std::string>(sessions->displayPath));
    }
    return ResolvedSession{.path = sessions->displayPath, .source = SessionSource::UserDisplay};
  }

  std::optional<ResolvedSession> resolveSession(sdbus::IConnection& connection) {
    try {
      auto managerProxy = sdbus::createProxy(connection, kLogindBusName, kLogindObjectPath);

      const SessionLookups lookups{
          .xdgSessionId = []() -> std::optional<std::string> {
            const char* sessionId = std::getenv("XDG_SESSION_ID");
            if (sessionId == nullptr || sessionId[0] == '\0') {
              return std::nullopt;
            }
            return std::string(sessionId);
          },
          .sessionById = [&](const std::string& id) -> std::optional<sdbus::ObjectPath> {
            try {
              sdbus::ObjectPath sessionPath;
              managerProxy->callMethod("GetSession")
                  .onInterface(kLogindManagerInterface)
                  .withArguments(id)
                  .storeResultsTo(sessionPath);
              return sessionPath;
            } catch (const sdbus::Error& e) {
              kLog.debug("failed to resolve logind session via XDG_SESSION_ID={}: {}", id, e.what());
              return std::nullopt;
            }
          },
          .sessionByProcessId = [&]() -> std::optional<sdbus::ObjectPath> {
            try {
              sdbus::ObjectPath sessionPath;
              managerProxy->callMethod("GetSessionByPID")
                  .onInterface(kLogindManagerInterface)
                  .withArguments(static_cast<std::uint32_t>(::getpid()))
                  .storeResultsTo(sessionPath);
              return sessionPath;
            } catch (const sdbus::Error& e) {
              kLog.debug("failed to resolve logind session by pid: {}", e.what());
              return std::nullopt;
            }
          },
          .userSessions = [&]() -> std::optional<UserSessions> {
            try {
              sdbus::ObjectPath userPath;
              managerProxy->callMethod("GetUser")
                  .onInterface(kLogindManagerInterface)
                  .withArguments(static_cast<std::uint32_t>(::getuid()))
                  .storeResultsTo(userPath);
              auto userProxy = sdbus::createProxy(connection, kLogindBusName, userPath);

              UserSessions sessions;
              // Display is (so): the session id plus its object path.
              const sdbus::Variant display = userProxy->getProperty("Display").onInterface(kLogindUserInterface);
              const auto displaySession = display.get<sdbus::Struct<std::string, sdbus::ObjectPath>>();
              sessions.displayId = std::get<0>(displaySession);
              sessions.displayPath = std::get<1>(displaySession);

              // Sessions is a(so) — only needed to tell an unambiguous pick
              // from a guess, so a failure here downgrades the log, not the
              // lookup.
              try {
                const sdbus::Variant all = userProxy->getProperty("Sessions").onInterface(kLogindUserInterface);
                for (const auto& entry : all.get<std::vector<sdbus::Struct<std::string, sdbus::ObjectPath>>>()) {
                  sessions.all.push_back(UserSession{.id = std::get<0>(entry), .path = std::get<1>(entry)});
                }
              } catch (const sdbus::Error& e) {
                kLog.debug("failed to list this user's logind sessions: {}", e.what());
              }
              return sessions;
            } catch (const sdbus::Error& e) {
              kLog.debug("failed to resolve logind session via the user's Display: {}", e.what());
              return std::nullopt;
            }
          },
      };

      return resolveSession(lookups);
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to resolve logind session: {}", e.what());
      return std::nullopt;
    }
  }

} // namespace logind
