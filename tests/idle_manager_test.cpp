#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "ext-idle-notify-v1-test-server-protocol.h"
#include "idle/idle_manager.h"
#include "tests/test_check.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client-core.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace {

  // Sends idle protocol events explicitly so state transitions and timer registrations
  // can be checked without waiting for timeouts or touching the user's compositor.
  class IdleServer {
  public:
    struct Notification {
      std::uint32_t timeoutMs;
      wl_resource* resource;
    };

    IdleServer() {
      if (const char* socket = std::getenv("WAYLAND_SOCKET")) {
        m_previousSocket = socket;
      }
      int sockets[2];
      TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
      m_display = wl_display_create();
      TEST_CHECK(m_display != nullptr);
      TEST_CHECK(wl_client_create(m_display, sockets[0]) != nullptr);
      const std::string socket = std::to_string(sockets[1]);
      TEST_CHECK(setenv("WAYLAND_SOCKET", socket.c_str(), 1) == 0);
      TEST_CHECK(wl_global_create(m_display, &wl_seat_interface, 7, this, &bindSeat) != nullptr);
      TEST_CHECK(wl_global_create(m_display, &ext_idle_notifier_v1_interface, 1, this, &bindNotifier) != nullptr);
      m_thread = std::jthread([this](std::stop_token stop) {
        auto* loop = wl_display_get_event_loop(m_display);
        while (!stop.stop_requested()) {
          TEST_CHECK(wl_event_loop_dispatch(loop, 10) >= 0 || errno == EINTR);
          {
            const std::scoped_lock lock(m_mutex);
            for (const auto& [index, idled] : m_events) {
              auto* resource = m_notifications.at(index).resource;
              TEST_CHECK(resource != nullptr);
              if (idled) {
                ext_idle_notification_v1_send_idled(resource);
              } else {
                ext_idle_notification_v1_send_resumed(resource);
              }
            }
            m_events.clear();
          }
          wl_display_flush_clients(m_display);
        }
      });
    }

    ~IdleServer() {
      m_thread.request_stop();
      m_thread.join();
      wl_display_destroy_clients(m_display);
      wl_display_destroy(m_display);
      if (m_previousSocket) {
        TEST_CHECK(setenv("WAYLAND_SOCKET", m_previousSocket->c_str(), 1) == 0);
      } else {
        TEST_CHECK(unsetenv("WAYLAND_SOCKET") == 0);
      }
    }

    IdleServer(const IdleServer&) = delete;
    IdleServer& operator=(const IdleServer&) = delete;

    [[nodiscard]] std::vector<Notification> notifications() {
      const std::scoped_lock lock(m_mutex);
      return m_notifications;
    }

    [[nodiscard]] std::size_t notification(std::uint32_t timeoutMs) {
      const auto registered = notifications();
      const auto it = std::ranges::find_if(registered, [timeoutMs](const Notification& item) {
        return item.timeoutMs == timeoutMs && item.resource != nullptr;
      });
      TEST_CHECK(it != registered.end());
      return static_cast<std::size_t>(it - registered.begin());
    }

    void send(std::size_t index, bool idled) {
      const std::scoped_lock lock(m_mutex);
      m_events.emplace_back(index, idled);
    }

  private:
    static void destroyResource(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static void bindSeat(wl_client* client, void*, std::uint32_t version, std::uint32_t id) {
      auto* resource = wl_resource_create(client, &wl_seat_interface, static_cast<int>(version), id);
      TEST_CHECK(resource != nullptr);
      static const struct wl_seat_interface implementation = {.release = &destroyResource};
      wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
      wl_seat_send_capabilities(resource, 0);
      wl_seat_send_name(resource, "idle-test");
    }

    static void notificationDestroyed(wl_resource* resource) {
      auto* self = static_cast<IdleServer*>(wl_resource_get_user_data(resource));
      const std::scoped_lock lock(self->m_mutex);
      const auto it = std::ranges::find(self->m_notifications, resource, &Notification::resource);
      TEST_CHECK(it != self->m_notifications.end());
      it->resource = nullptr;
    }

    static void createNotification(
        wl_client* client, wl_resource* notifier, std::uint32_t id, std::uint32_t timeout, wl_resource*
    ) {
      auto* self = static_cast<IdleServer*>(wl_resource_get_user_data(notifier));
      auto* resource = wl_resource_create(client, &ext_idle_notification_v1_interface, 1, id);
      TEST_CHECK(resource != nullptr);
      static const struct ext_idle_notification_v1_interface implementation = {.destroy = &destroyResource};
      wl_resource_set_implementation(resource, &implementation, self, &notificationDestroyed);
      const std::scoped_lock lock(self->m_mutex);
      self->m_notifications.push_back({timeout, resource});
    }

    static void bindNotifier(wl_client* client, void* data, std::uint32_t version, std::uint32_t id) {
      auto* resource = wl_resource_create(client, &ext_idle_notifier_v1_interface, static_cast<int>(version), id);
      TEST_CHECK(resource != nullptr);
      static const struct ext_idle_notifier_v1_interface implementation = {
          .destroy = &destroyResource,
          .get_idle_notification = &createNotification,
      };
      wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

    std::optional<std::string> m_previousSocket;
    wl_display* m_display = nullptr;
    std::jthread m_thread;
    std::mutex m_mutex;
    std::vector<Notification> m_notifications;
    std::vector<std::pair<std::size_t, bool>> m_events;
  };

  struct Action {
    std::string behavior;
    IdleActionKind kind;
    std::string command;
    bool operator==(const Action&) const = default;
  };

  struct Fixture {
    IdleServer server;
    WaylandConnection wayland;
    IdleManager idle;
    std::vector<Action> actions;
    std::function<void()> fadeComplete;
    int fadesEnded = 0;

    explicit Fixture(std::vector<IdleBehaviorConfig> behaviors, float fadeSeconds = 0.0F) {
      TEST_CHECK(wayland.connect());
      TEST_CHECK(idle.initialize(
          wayland,
          [this](const std::string&, std::chrono::milliseconds, bool, std::function<void()> complete) {
            fadeComplete = std::move(complete);
          },
          [this](bool, bool) { ++fadesEnded; }
      ));
      idle.setActionRunner([this](const IdleBehaviorConfig& behavior, const IdleActionRequest& action) {
        actions.push_back({behavior.name, action.kind, action.command});
        if (action.kind == IdleActionKind::Lock) {
          idle.setSessionLocked(true);
        }
        return true;
      });
      idle.reload(IdleConfig{.behaviors = std::move(behaviors), .preActionFadeSeconds = fadeSeconds});
      sync();
    }

    void sync() {
      // A second barrier dispatches events queued after the first sync reply.
      TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
      TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
      for (auto& callback : DeferredCall::takePending()) {
        callback();
      }
      TEST_CHECK(wl_display_roundtrip(wayland.display()) >= 0);
    }

    void send(std::size_t index, bool idled = true) {
      server.send(index, idled);
      sync();
    }

    void finishFade() {
      TEST_CHECK(fadeComplete);
      std::exchange(fadeComplete, {})();
      sync();
    }
  };

  void checkLockPreservesIdleActionsAndTimers() {
    Fixture fixture({
        {.name = "screen-off", .timeoutSeconds = 30, .action = "screen_off", .resumeCommand = "screen-resumed"},
        {.name = "lock", .timeoutSeconds = 45, .action = "lock"},
        {.name = "suspend", .timeoutSeconds = 90, .action = "suspend", .lockBeforeSuspend = false},
        {.name = "custom", .timeoutSeconds = 20, .action = "command", .command = "dim", .resumeCommand = "undim"},
    });
    const auto screen = fixture.server.notification(30000);
    const auto custom = fixture.server.notification(20000);
    const auto lock = fixture.server.notification(45000);
    const auto suspend = fixture.server.notification(90000);
    const auto registered = fixture.server.notifications();
    fixture.send(screen);
    fixture.send(custom);
    fixture.send(lock);
    TEST_CHECK(
        fixture.actions
        == std::vector<Action>({
            {"screen-off", IdleActionKind::ScreenOff, ""},
            {"custom", IdleActionKind::Command, "dim"},
            {"lock", IdleActionKind::Lock, ""},
        })
    );
    TEST_CHECK(fixture.server.notifications().size() == registered.size());
    TEST_CHECK(fixture.server.notification(30000) == screen);
    TEST_CHECK(fixture.server.notification(45000) == lock);
    TEST_CHECK(fixture.server.notification(90000) == suspend);
    fixture.send(suspend);
    TEST_CHECK(fixture.actions.back().kind == IdleActionKind::Suspend);
    fixture.send(screen, false);
    fixture.send(custom, false);
    TEST_CHECK(
        fixture.actions
        == std::vector<Action>({
            {"screen-off", IdleActionKind::ScreenOff, ""},
            {"custom", IdleActionKind::Command, "dim"},
            {"lock", IdleActionKind::Lock, ""},
            {"suspend", IdleActionKind::Suspend, ""},
            {"screen-off", IdleActionKind::ScreenOn, ""},
            {"screen-off", IdleActionKind::Command, "screen-resumed"},
            {"custom", IdleActionKind::Command, "undim"},
        })
    );
    fixture.idle.setSessionLocked(false);
    fixture.sync();
    TEST_CHECK(fixture.actions.size() == 7);
  }

  void checkLockedTimeouts() {
    Fixture fixture({
        {.name = "screen-off", .timeoutSeconds = 60, .action = "screen_off", .lockedTimeoutSeconds = 5},
        {.name = "unchanged",
         .timeoutSeconds = 30,
         .action = "command",
         .command = "unchanged",
         .lockedTimeoutSeconds = 30},
        {.name = "locked-only",
         .timeoutSeconds = 0,
         .action = "command",
         .command = "locked",
         .lockedTimeoutSeconds = 7},
    });
    const auto screen = fixture.server.notification(60000);
    const auto unchanged = fixture.server.notification(30000);
    fixture.idle.setSessionLocked(true);
    fixture.sync();
    TEST_CHECK(fixture.server.notifications()[screen].resource == nullptr);
    const auto lockedScreen = fixture.server.notification(5000);
    const auto lockedOnly = fixture.server.notification(7000);
    TEST_CHECK(fixture.server.notification(30000) == unchanged);
    const auto count = fixture.server.notifications().size();
    fixture.idle.setSessionLocked(true);
    fixture.sync();
    TEST_CHECK(fixture.server.notifications().size() == count);
    fixture.send(lockedScreen);
    fixture.idle.setSessionLocked(false);
    fixture.sync();
    TEST_CHECK(
        fixture.actions
        == std::vector<Action>({
            {"screen-off", IdleActionKind::ScreenOff, ""},
            {"screen-off", IdleActionKind::ScreenOn, ""},
        })
    );
    TEST_CHECK(fixture.server.notifications()[lockedOnly].resource == nullptr);
    TEST_CHECK(fixture.server.notification(60000) != screen);
  }

  void checkAlreadyIdledUsesLockedTimeoutAfterResume() {
    Fixture fixture({
        {.name = "screen-off", .timeoutSeconds = 60, .action = "screen_off", .lockedTimeoutSeconds = 5},
    });
    const auto screen = fixture.server.notification(60000);
    fixture.send(screen);
    fixture.idle.setSessionLocked(true);
    fixture.sync();
    TEST_CHECK(fixture.actions.size() == 1);
    TEST_CHECK(fixture.server.notification(60000) == screen);
    fixture.send(screen, false);
    const auto lockedScreen = fixture.server.notification(5000);
    TEST_CHECK(fixture.server.notifications()[screen].resource == nullptr);
    fixture.send(lockedScreen);
    fixture.send(lockedScreen, false);
    TEST_CHECK(fixture.server.notification(5000) == lockedScreen);
    TEST_CHECK(
        fixture.actions
        == std::vector<Action>({
            {"screen-off", IdleActionKind::ScreenOff, ""},
            {"screen-off", IdleActionKind::ScreenOn, ""},
            {"screen-off", IdleActionKind::ScreenOff, ""},
            {"screen-off", IdleActionKind::ScreenOn, ""},
        })
    );
  }

  void checkLockDuringFade() {
    Fixture fixture(
        {
            {.name = "lock", .timeoutSeconds = 45, .action = "lock"},
            {.name = "screen-off", .timeoutSeconds = 60, .action = "screen_off", .lockedTimeoutSeconds = 5},
        },
        2.0F
    );
    fixture.send(fixture.server.notification(45000));
    const auto screen = fixture.server.notification(60000);
    fixture.send(screen);
    fixture.finishFade();
    TEST_CHECK(fixture.fadesEnded == 1);
    TEST_CHECK(
        fixture.actions
        == std::vector<Action>({
            {"lock", IdleActionKind::Lock, ""},
            {"screen-off", IdleActionKind::ScreenOff, ""},
        })
    );
    TEST_CHECK(fixture.server.notification(60000) == screen);
    fixture.send(screen, false);
    TEST_CHECK(fixture.actions.back().kind == IdleActionKind::ScreenOn);
    TEST_CHECK(fixture.server.notification(5000) != screen);
  }

  void checkLockDuringCancelledFade() {
    Fixture fixture(
        {
            {.name = "screen-off", .timeoutSeconds = 60, .action = "screen_off", .lockedTimeoutSeconds = 5},
        },
        2.0F
    );
    const auto screen = fixture.server.notification(60000);
    fixture.send(screen);
    fixture.idle.setSessionLocked(true);
    fixture.sync();
    TEST_CHECK(fixture.fadesEnded == 0);
    TEST_CHECK(fixture.server.notification(60000) == screen);
    fixture.send(screen, false);
    TEST_CHECK(fixture.fadesEnded == 1);
    fixture.finishFade();
    TEST_CHECK(fixture.actions.empty());
    TEST_CHECK(fixture.server.notification(5000) != screen);
  }

  void outlastInhibitReleaseDebounce(Fixture& fixture) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    TimerManager::instance().tick();
    fixture.sync();
  }

  void checkScreenSaverInhibitCancelsFadeAndRestartsAfterRelease() {
    Fixture fixture({{.name = "screen-off", .timeoutSeconds = 60, .action = "screen_off"}}, 2.0F);
    const auto screen = fixture.server.notification(60000);
    fixture.send(screen);
    fixture.idle.setScreenSaverInhibitLocks(1);
    TEST_CHECK(fixture.fadesEnded == 1);

    // A renewal inside the debounce keeps the countdown instead of restarting it.
    fixture.idle.setScreenSaverInhibitLocks(0);
    fixture.idle.setScreenSaverInhibitLocks(1);
    outlastInhibitReleaseDebounce(fixture);
    TEST_CHECK(fixture.server.notification(60000) == screen);

    // The cancelled fade left an already-idled notification; the release must re-arm it.
    fixture.idle.setScreenSaverInhibitLocks(0);
    TEST_CHECK(fixture.server.notification(60000) == screen);
    outlastInhibitReleaseDebounce(fixture);
    TEST_CHECK(fixture.server.notifications()[screen].resource == nullptr);
    const auto restarted = fixture.server.notification(60000);
    fixture.send(restarted);
    fixture.finishFade();
    TEST_CHECK(fixture.actions == std::vector<Action>({{"screen-off", IdleActionKind::ScreenOff, ""}}));
  }

} // namespace

int main() {
  checkLockPreservesIdleActionsAndTimers();
  checkLockedTimeouts();
  checkAlreadyIdledUsesLockedTimeoutAfterResume();
  checkLockDuringFade();
  checkLockDuringCancelledFade();
  checkScreenSaverInhibitCancelsFadeAndRestartsAfterRelease();
}
