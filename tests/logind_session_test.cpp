// Shared logind session resolution: the fallback order (XDG_SESSION_ID, then
// GetSessionByPID, then the user's Display session) and the best-effort nature
// of that last step, which can name a session this process is not running in.

#include "dbus/logind/logind_session.h"

#include <optional>
#include <print>
#include <string>
#include <vector>

namespace {

  int g_failures = 0;

  void expectTrue(const char* what, bool cond) {
    if (!cond) {
      std::println(stderr, "logind_session_test: FAIL: {}", what);
      ++g_failures;
    }
  }

  void expectPath(const char* what, const std::optional<logind::ResolvedSession>& actual, const std::string& expected) {
    if (!actual.has_value()) {
      std::println(stderr, "logind_session_test: FAIL: {} = unresolved, expected \"{}\"", what, expected);
      ++g_failures;
      return;
    }
    if (std::string(actual->path.c_str()) != expected) {
      std::println(
          stderr, "logind_session_test: FAIL: {} = \"{}\", expected \"{}\"", what, std::string(actual->path.c_str()),
          expected
      );
      ++g_failures;
    }
  }

  void
  expectSource(const char* what, const std::optional<logind::ResolvedSession>& actual, logind::SessionSource expected) {
    if (!actual.has_value() || actual->source != expected) {
      std::println(
          stderr, "logind_session_test: FAIL: {} resolved via {}, expected {}", what,
          actual.has_value() ? logind::describe(actual->source) : "nothing", logind::describe(expected)
      );
      ++g_failures;
    }
  }

  struct Calls {
    int byId = 0;
    int byPid = 0;
    int userSessions = 0;
  };

} // namespace

int main() {
  const auto never = [](const std::string&) -> std::optional<sdbus::ObjectPath> { return std::nullopt; };

  // XDG_SESSION_ID wins when logind knows the id: it names THIS session.
  {
    Calls calls;
    const logind::SessionLookups lookups{
        .xdgSessionId = [] { return std::optional<std::string>{"3"}; },
        .sessionById = [&](const std::string& id) -> std::optional<sdbus::ObjectPath> {
          ++calls.byId;
          return id == "3" ? std::optional{sdbus::ObjectPath{"/org/freedesktop/login1/session/_33"}} : std::nullopt;
        },
        .sessionByProcessId = [&]() -> std::optional<sdbus::ObjectPath> {
          ++calls.byPid;
          return sdbus::ObjectPath{"/org/freedesktop/login1/session/pid"};
        },
        .userSessions = [&]() -> std::optional<logind::UserSessions> {
          ++calls.userSessions;
          return std::nullopt;
        },
    };

    const auto session = logind::resolveSession(lookups);
    expectPath("XDG_SESSION_ID session", session, "/org/freedesktop/login1/session/_33");
    expectSource("XDG_SESSION_ID session", session, logind::SessionSource::XdgSessionId);
    expectTrue("no fallback is consulted once the id resolves", calls.byPid == 0 && calls.userSessions == 0);
  }

  // A stale XDG_SESSION_ID (logind no longer knows it) falls through to the pid.
  {
    Calls calls;
    const logind::SessionLookups lookups{
        .xdgSessionId = [] { return std::optional<std::string>{"9"}; },
        .sessionById = [&](const std::string&) -> std::optional<sdbus::ObjectPath> {
          ++calls.byId;
          return std::nullopt;
        },
        .sessionByProcessId = [] { return std::optional{sdbus::ObjectPath{"/org/freedesktop/login1/session/_31"}}; },
        .userSessions = [&]() -> std::optional<logind::UserSessions> {
          ++calls.userSessions;
          return std::nullopt;
        },
    };

    const auto session = logind::resolveSession(lookups);
    expectPath("stale id falls back to pid", session, "/org/freedesktop/login1/session/_31");
    expectSource("stale id falls back to pid", session, logind::SessionSource::ProcessId);
    expectTrue("the stale id was tried first", calls.byId == 1);
    expectTrue("the Display session is not needed", calls.userSessions == 0);
  }

  // The reported bug: started by the systemd user manager, so there is no
  // XDG_SESSION_ID and GetSessionByPID answers NoSessionForPID. The user's
  // Display session is the only thing left.
  {
    const logind::SessionLookups lookups{
        .xdgSessionId = []() -> std::optional<std::string> { return std::nullopt; },
        .sessionById = never,
        .sessionByProcessId = []() -> std::optional<sdbus::ObjectPath> { return std::nullopt; },
        .userSessions =
            [] {
              return std::optional{logind::UserSessions{
                  .displayId = "3",
                  .displayPath = sdbus::ObjectPath{"/org/freedesktop/login1/session/_33"},
                  .all = {{.id = "3", .path = sdbus::ObjectPath{"/org/freedesktop/login1/session/_33"}}},
              }};
            },
    };

    const auto session = logind::resolveSession(lookups);
    expectPath("user manager falls back to Display", session, "/org/freedesktop/login1/session/_33");
    expectSource("user manager falls back to Display", session, logind::SessionSource::UserDisplay);
  }

  // Explicit best-effort: with several concurrent sessions the Display session
  // is still used, but it is flagged as a guess (and logged as one) because
  // logind picks it per user, not per caller.
  {
    const logind::UserSessions sessions{
        .displayId = "3",
        .displayPath = sdbus::ObjectPath{"/org/freedesktop/login1/session/_33"},
        .all = {
            {.id = "3", .path = sdbus::ObjectPath{"/org/freedesktop/login1/session/_33"}},
            {.id = "5", .path = sdbus::ObjectPath{"/org/freedesktop/login1/session/_35"}},
        },
    };
    expectTrue(
        "one session is unambiguous",
        !logind::displaySessionIsAmbiguous(
            logind::UserSessions{
                .displayId = sessions.displayId,
                .displayPath = sessions.displayPath,
                .all = {sessions.all.front()},
            }
        )
    );
    expectTrue("concurrent sessions are ambiguous", logind::displaySessionIsAmbiguous(sessions));

    const logind::SessionLookups lookups{
        .xdgSessionId = []() -> std::optional<std::string> { return std::nullopt; },
        .sessionById = never,
        .sessionByProcessId = []() -> std::optional<sdbus::ObjectPath> { return std::nullopt; },
        .userSessions = [&] { return std::optional{sessions}; },
    };

    const auto session = logind::resolveSession(lookups);
    expectPath("ambiguous Display session is still used", session, "/org/freedesktop/login1/session/_33");
    expectSource("ambiguous Display session is still used", session, logind::SessionSource::UserDisplay);
  }

  // A user with no graphical session at all: nothing to guess with.
  {
    const logind::SessionLookups lookups{
        .xdgSessionId = []() -> std::optional<std::string> { return std::nullopt; },
        .sessionById = never,
        .sessionByProcessId = []() -> std::optional<sdbus::ObjectPath> { return std::nullopt; },
        .userSessions =
            [] {
              return std::optional{logind::UserSessions{
                  .displayId = {},
                  .displayPath = sdbus::ObjectPath{},
                  .all = {{.id = "7", .path = sdbus::ObjectPath{"/org/freedesktop/login1/session/_37"}}},
              }};
            },
    };

    expectTrue("no Display session resolves to nothing", !logind::resolveSession(lookups).has_value());
  }

  // Every lookup dark (no bus, no user object) resolves to nothing rather than
  // to a path the caller would then talk to.
  {
    const logind::SessionLookups lookups{
        .xdgSessionId = []() -> std::optional<std::string> { return std::nullopt; },
        .sessionById = never,
        .sessionByProcessId = []() -> std::optional<sdbus::ObjectPath> { return std::nullopt; },
        .userSessions = []() -> std::optional<logind::UserSessions> { return std::nullopt; },
    };

    expectTrue("all lookups failing resolves to nothing", !logind::resolveSession(lookups).has_value());
  }

  // Missing lookups must not be called blindly.
  {
    const logind::SessionLookups lookups{};
    expectTrue("empty lookups resolve to nothing", !logind::resolveSession(lookups).has_value());
  }

  if (g_failures > 0) {
    std::println(stderr, "logind_session_test: {} failure(s)", g_failures);
    return 1;
  }
  return 0;
}
