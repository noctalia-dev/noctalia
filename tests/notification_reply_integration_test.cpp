#include "dbus/notification/notification_service.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
  constexpr auto kNotificationsInterface = "org.freedesktop.Notifications";
  const sdbus::ServiceName kNotificationsBusName{"org.freedesktop.Notifications"};
  const sdbus::ObjectPath kNotificationsPath{"/org/freedesktop/Notifications"};
  constexpr auto kIntrospectableInterface = "org.freedesktop.DBus.Introspectable";

  template <typename Predicate>
  bool drainUntil(SessionBus& bus, Predicate&& ready, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      if (std::invoke(ready)) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    bus.processPendingEvents();
    return std::invoke(ready);
  }
} // namespace

int main() {
  SessionBus bus;
  NotificationManager manager;
  NotificationService service(bus, manager);
  TEST_CHECK(service.isHealthy());

  auto client1 = sdbus::createSessionBusConnection();
  auto proxy1 = sdbus::createProxy(*client1, kNotificationsBusName, kNotificationsPath);

  auto client2 = sdbus::createSessionBusConnection();
  auto proxy2 = sdbus::createProxy(*client2, kNotificationsBusName, kNotificationsPath);

  // 1. Verify D-Bus introspection XML exposes NotificationReplied with (uint32 id, string text)
  std::atomic<bool> introDone{false};
  std::string introXml;
  proxy1->callMethodAsync("Introspect")
      .onInterface(kIntrospectableInterface)
      .uponReplyInvoke([&](std::optional<sdbus::Error> error, std::string xml) {
        TEST_CHECK(!error.has_value());
        introXml = std::move(xml);
        introDone = true;
      });

  client1->enterEventLoopAsync();
  client2->enterEventLoopAsync();

  TEST_CHECK(drainUntil(bus, [&] { return introDone.load(); }));
  TEST_CHECK(introXml.contains(R"(<signal name="NotificationReplied">)"));
  TEST_CHECK(introXml.contains(R"(<arg type="u" name="id"/>)"));
  TEST_CHECK(introXml.contains(R"(<arg type="s" name="text"/>)"));

  // Register signal listeners on both clients
  std::atomic<bool> client1Replied{false};
  std::atomic<uint32_t> client1ReplyId{0};
  std::string client1ReplyText;
  std::atomic<bool> client1TokenReceived{false};
  std::string client1Token;

  std::atomic<bool> client2Replied{false};
  std::atomic<uint32_t> client2ReplyId{0};
  std::string client2ReplyText;
  std::atomic<bool> client2TokenReceived{false};
  std::string client2Token;

  proxy1->uponSignal("NotificationReplied")
      .onInterface(kNotificationsInterface)
      .call([&](uint32_t id, const std::string& text) {
        client1ReplyId = id;
        client1ReplyText = text;
        client1Replied = true;
      });

  proxy1->uponSignal("ActivationToken")
      .onInterface(kNotificationsInterface)
      .call([&](uint32_t, const std::string& token) {
        client1Token = token;
        client1TokenReceived = true;
      });

  proxy2->uponSignal("NotificationReplied")
      .onInterface(kNotificationsInterface)
      .call([&](uint32_t id, const std::string& text) {
        client2ReplyId = id;
        client2ReplyText = text;
        client2Replied = true;
      });

  proxy2->uponSignal("ActivationToken")
      .onInterface(kNotificationsInterface)
      .call([&](uint32_t, const std::string& token) {
        client2Token = token;
        client2TokenReceived = true;
      });

  // 2. Client 1 sends a notification with an inline-reply action
  std::atomic<bool> notifyDone{false};
  uint32_t notifId{0};

  proxy1->callMethodAsync("Notify")
      .onInterface(kNotificationsInterface)
      .withArguments(
          std::string{"originating-client"}, uint32_t{0}, std::string{""}, std::string{"Test Notification"},
          std::string{"Please reply"}, std::vector<std::string>{"inline-reply", "Reply"},
          std::map<std::string, sdbus::Variant>{}, int32_t{0}
      )
      .uponReplyInvoke([&](std::optional<sdbus::Error> error, uint32_t id) {
        TEST_CHECK(!error.has_value());
        notifId = id;
        notifyDone = true;
      });

  TEST_CHECK(drainUntil(bus, [&] { return notifyDone.load(); }));
  TEST_CHECK(notifId != 0);

  // Submit inline reply with activation token from NotificationManager (keep active for replacement test)
  const std::string replyText = "Confidential reply text";
  const std::string token = "token-secret-42";
  TEST_CHECK(manager.invokeInlineReply(notifId, replyText, token, false));

  // Drain bus until Client 1 receives NotificationReplied and ActivationToken
  TEST_CHECK(drainUntil(bus, [&] { return client1Replied.load() && client1TokenReceived.load(); }));

  TEST_CHECK(client1ReplyId == notifId);
  TEST_CHECK(client1ReplyText == replyText);
  TEST_CHECK(client1Token == token);

  // Allow time for any misrouted or broadcast signals to arrive at Client 2
  for (int i = 0; i < 50; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(2ms);
  }

  // Client 2 MUST NOT have received either signal (unicast security verification)
  TEST_CHECK(!client2Replied);
  TEST_CHECK(!client2TokenReceived);

  // 3. Client 1 updates/replaces the notification, then receives a second reply
  std::atomic<bool> replaceDone{false};
  uint32_t updatedId{0};

  proxy1->callMethodAsync("Notify")
      .onInterface(kNotificationsInterface)
      .withArguments(
          std::string{"originating-client"}, notifId, std::string{""}, std::string{"Updated Notification"},
          std::string{"New message"}, std::vector<std::string>{"inline-reply", "Reply"},
          std::map<std::string, sdbus::Variant>{}, int32_t{0}
      )
      .uponReplyInvoke([&](std::optional<sdbus::Error> error, uint32_t id) {
        TEST_CHECK(!error.has_value());
        updatedId = id;
        replaceDone = true;
      });

  TEST_CHECK(drainUntil(bus, [&] { return replaceDone.load(); }));
  TEST_CHECK(updatedId == notifId);

  client1Replied = false;
  client1TokenReceived = false;
  client2Replied = false;
  client2TokenReceived = false;

  TEST_CHECK(manager.invokeInlineReply(updatedId, "Second confidential reply", "token-88", true));

  TEST_CHECK(drainUntil(bus, [&] { return client1Replied.load() && client1TokenReceived.load(); }));
  TEST_CHECK(client1ReplyId == updatedId);
  TEST_CHECK(client1ReplyText == "Second confidential reply");
  TEST_CHECK(client1Token == "token-88");

  for (int i = 0; i < 50; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(2ms);
  }
  TEST_CHECK(!client2Replied);
  TEST_CHECK(!client2TokenReceived);

  // 4. Client 2 sends its own notification; replies must go ONLY to Client 2
  std::atomic<bool> client2NotifyDone{false};
  uint32_t client2NotifId{0};

  proxy2->callMethodAsync("Notify")
      .onInterface(kNotificationsInterface)
      .withArguments(
          std::string{"client-two"}, uint32_t{0}, std::string{""}, std::string{"Client 2 Notification"},
          std::string{"Ping"}, std::vector<std::string>{"inline-reply", "Reply"},
          std::map<std::string, sdbus::Variant>{}, int32_t{0}
      )
      .uponReplyInvoke([&](std::optional<sdbus::Error> error, uint32_t id) {
        TEST_CHECK(!error.has_value());
        client2NotifId = id;
        client2NotifyDone = true;
      });

  TEST_CHECK(drainUntil(bus, [&] { return client2NotifyDone.load(); }));
  TEST_CHECK(client2NotifId != 0);

  client1Replied = false;
  client1TokenReceived = false;
  client2Replied = false;
  client2TokenReceived = false;

  TEST_CHECK(manager.invokeInlineReply(client2NotifId, "Reply for client 2 only", "token-c2", true));

  TEST_CHECK(drainUntil(bus, [&] { return client2Replied.load() && client2TokenReceived.load(); }));
  TEST_CHECK(client2ReplyId == client2NotifId);
  TEST_CHECK(client2ReplyText == "Reply for client 2 only");
  TEST_CHECK(client2Token == "token-c2");
  TEST_CHECK(!client1Replied);
  TEST_CHECK(!client1TokenReceived);

  // 5. Two different connections send identical notifications within duplicate window
  std::atomic<bool> dup1Done{false};
  std::atomic<bool> dup2Done{false};
  uint32_t dup1Id{0};
  uint32_t dup2Id{0};

  proxy1->callMethodAsync("Notify")
      .onInterface(kNotificationsInterface)
      .withArguments(
          std::string{"shared-client"}, uint32_t{0}, std::string{""}, std::string{"Identical Summary"},
          std::string{"Identical Body"}, std::vector<std::string>{"inline-reply", "Reply"},
          std::map<std::string, sdbus::Variant>{}, int32_t{0}
      )
      .uponReplyInvoke([&](std::optional<sdbus::Error> error, uint32_t id) {
        TEST_CHECK(!error.has_value());
        dup1Id = id;
        dup1Done = true;
      });

  proxy2->callMethodAsync("Notify")
      .onInterface(kNotificationsInterface)
      .withArguments(
          std::string{"shared-client"}, uint32_t{0}, std::string{""}, std::string{"Identical Summary"},
          std::string{"Identical Body"}, std::vector<std::string>{"inline-reply", "Reply"},
          std::map<std::string, sdbus::Variant>{}, int32_t{0}
      )
      .uponReplyInvoke([&](std::optional<sdbus::Error> error, uint32_t id) {
        TEST_CHECK(!error.has_value());
        dup2Id = id;
        dup2Done = true;
      });

  TEST_CHECK(drainUntil(bus, [&] { return dup1Done.load() && dup2Done.load(); }));
  TEST_CHECK(dup1Id != 0);
  TEST_CHECK(dup2Id != 0);
  TEST_CHECK(dup1Id != dup2Id);

  // Reply to client 1's notification reaches only client 1
  client1Replied = false;
  client1TokenReceived = false;
  client2Replied = false;
  client2TokenReceived = false;

  TEST_CHECK(manager.invokeInlineReply(dup1Id, "Reply to dup 1", "token-dup1", true));
  TEST_CHECK(drainUntil(bus, [&] { return client1Replied.load() && client1TokenReceived.load(); }));
  TEST_CHECK(client1ReplyId == dup1Id);
  TEST_CHECK(client1ReplyText == "Reply to dup 1");
  TEST_CHECK(client1Token == "token-dup1");

  for (int i = 0; i < 50; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(2ms);
  }
  TEST_CHECK(!client2Replied);
  TEST_CHECK(!client2TokenReceived);

  // Reply to client 2's notification reaches only client 2
  client1Replied = false;
  client1TokenReceived = false;
  client2Replied = false;
  client2TokenReceived = false;

  TEST_CHECK(manager.invokeInlineReply(dup2Id, "Reply to dup 2", "token-dup2", true));
  TEST_CHECK(drainUntil(bus, [&] { return client2Replied.load() && client2TokenReceived.load(); }));
  TEST_CHECK(client2ReplyId == dup2Id);
  TEST_CHECK(client2ReplyText == "Reply to dup 2");
  TEST_CHECK(client2Token == "token-dup2");

  for (int i = 0; i < 50; ++i) {
    bus.processPendingEvents();
    std::this_thread::sleep_for(2ms);
  }
  TEST_CHECK(!client1Replied);
  TEST_CHECK(!client1TokenReceived);

  client1->leaveEventLoop();
  client2->leaveEventLoop();
  return 0;
}
