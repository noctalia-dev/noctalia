#include "dbus/notification/job_progress_service.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {
  using namespace std::chrono_literals;
  constexpr auto kService = "org.kde.JobViewServer";

  void requestName(sdbus::IConnection& connection, std::uint32_t expected) {
    auto dbus = sdbus::createProxy(connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                                  sdbus::ObjectPath{"/org/freedesktop/DBus"});
    std::uint32_t reply = 0;
    dbus->callMethod("RequestName").onInterface("org.freedesktop.DBus")
        .withArguments(std::string{kService}, std::uint32_t{4}).storeResultsTo(reply);
    assert(reply == expected);
  }

  void releaseName(sdbus::IConnection& connection) {
    auto dbus = sdbus::createProxy(connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                                  sdbus::ObjectPath{"/org/freedesktop/DBus"});
    std::uint32_t reply = 0;
    dbus->callMethod("ReleaseName").onInterface("org.freedesktop.DBus")
        .withArguments(std::string{kService}).storeResultsTo(reply);
    assert(reply == 1);
  }

  bool servicePresent(sdbus::IConnection& connection) {
    auto dbus = sdbus::createProxy(connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                                  sdbus::ObjectPath{"/org/freedesktop/DBus"});
    try {
      std::string owner;
      dbus->callMethod("GetNameOwner").onInterface("org.freedesktop.DBus")
          .withArguments(std::string{kService}).storeResultsTo(owner);
      return !owner.empty();
    } catch (const sdbus::Error&) {
      return false;
    }
  }

  std::shared_ptr<sdbus::IConnection> clientConnection() {
    auto connection = sdbus::createSessionBusConnection();
    connection->enterEventLoopAsync();
    return connection;
  }

  sdbus::ObjectPath registerJob(sdbus::IConnection& connection, std::int32_t capabilities,
                                std::string desktopEntry = "org.example.JobProgressTest") {
    auto server = sdbus::createProxy(connection, sdbus::ServiceName{kService}, sdbus::ObjectPath{"/JobViewServer"});
    sdbus::ObjectPath path;
    server->callMethod("requestView").onInterface("org.kde.JobViewServerV2")
        .withArguments(std::move(desktopEntry), capabilities,
                       std::map<std::string, sdbus::Variant>{}).storeResultsTo(path);
    return path;
  }

  void sendUpdate(sdbus::IConnection& connection, const sdbus::ObjectPath& path,
                  std::map<std::string, sdbus::Variant> properties) {
    auto view = sdbus::createProxy(connection, sdbus::ServiceName{kService}, path);
    view->callMethod("update").onInterface("org.kde.JobViewV3").withArguments(properties);
  }

  void terminate(sdbus::IConnection& connection, const sdbus::ObjectPath& path,
                 std::uint32_t code, std::string message) {
    auto view = sdbus::createProxy(connection, sdbus::ServiceName{kService}, path);
    view->callMethod("terminate").onInterface("org.kde.JobViewV3")
        .withArguments(code, message, std::map<std::string, sdbus::Variant>{});
  }

  bool waitFor(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) return true;
      std::this_thread::sleep_for(10ms);
    }
    return predicate();
  }
}

int main() {
  SessionBus bus;
  NotificationManager notifications;
  JobProgressManager jobs;

  auto incumbent = sdbus::createSessionBusConnection();
  requestName(*incumbent, 1);
  {
    JobProgressService conflicted(bus, jobs, notifications);
    assert(!conflicted.ownsService());
  }
  releaseName(*incumbent);

  auto service = std::make_unique<JobProgressService>(bus, jobs, notifications);
  assert(service->ownsService());
  bus.connection().enterEventLoopAsync();

  auto inspector = clientConnection();
  auto introspection = sdbus::createProxy(*inspector, sdbus::ServiceName{kService}, sdbus::ObjectPath{"/JobViewServer"});
  std::string xml;
  introspection->callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(xml);
  assert(xml.find("org.kde.JobViewServerV2") != std::string::npos);
  assert(xml.find("requestView") != std::string::npos);

  auto client1 = clientConnection();
  auto client2 = clientConnection();
  const auto path1 = registerJob(*client1, 3, "org.nythera.FileManager");
  const auto path2 = registerJob(*client2, 1, "org.nythera.FileManager");
  assert(path1 != path2);
  assert(waitFor([&] { return jobs.jobs().size() == 2; }));

  auto viewInspector = sdbus::createProxy(*inspector, sdbus::ServiceName{kService}, path1);
  std::string viewXml;
  viewInspector->callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(viewXml);
  assert(viewXml.find("org.kde.JobViewV3") != std::string::npos);
  assert(viewXml.find("cancelRequested") != std::string::npos);
  assert(viewXml.find("suspendRequested") != std::string::npos);

  const auto id1 = jobs.jobs()[0].id;
  const auto id2 = jobs.jobs()[1].id;
  std::atomic<unsigned> addedNotifications{0};
  std::mutex notificationMutex;
  std::string latestSummary;
  std::string latestBody;
  notifications.addEventCallback([&](const Notification& notification, NotificationEvent event) {
    if (event != NotificationEvent::Added) return;
    addedNotifications.fetch_add(1);
    if (notification.appName == "org.nythera.FileManager") {
      std::lock_guard lock(notificationMutex);
      latestSummary = notification.summary;
      latestBody = notification.body;
    }
  });
  const auto notificationId = notifications.addInternal("Test", "Ordinary notification", "Independent lifecycle");
  const auto historySize = notifications.history().size();
  assert(notifications.close(notificationId, CloseReason::Dismissed));
  assert(jobs.find(id1) != nullptr && jobs.find(id2) != nullptr);

  bool crossOwnerDenied = false;
  try {
    sendUpdate(*client2, path1, {{"percent", sdbus::Variant(std::uint32_t{88})}});
  } catch (const sdbus::Error&) {
    crossOwnerDenied = true;
  }
  assert(crossOwnerDenied);
  assert(jobs.find(id1)->percent == std::nullopt);

  for (std::uint32_t percent = 0; percent <= 100; percent += 10) {
    sendUpdate(*client1, path1, {
        {"title", sdbus::Variant(std::string{"Copying test payload"})},
        {"infoMessage", sdbus::Variant(std::string{"synthetic transfer"})},
        {"destUrl", sdbus::Variant(std::string{"file:///tmp/nythera-test-destination"})},
        {"percent", sdbus::Variant(percent)},
        {"processedBytes", sdbus::Variant(std::uint64_t{percent * 1024U})},
        {"totalBytes", sdbus::Variant(std::uint64_t{102400})},
        {"speed", sdbus::Variant(std::uint64_t{8192})},
        {"elapsedTime", sdbus::Variant(std::int64_t{percent + 1})},
        {"processedFiles", sdbus::Variant(std::uint64_t{percent / 10U})},
        {"totalFiles", sdbus::Variant(std::uint64_t{10})},
        {"processedItems", sdbus::Variant(std::uint64_t{percent / 10U})},
        {"totalItems", sdbus::Variant(std::uint64_t{10})},
        {"descriptionLabel1", sdbus::Variant(std::string{"From"})},
        {"descriptionValue1", sdbus::Variant(std::string{"/tmp/source"})},
        {"descriptionLabel2", sdbus::Variant(std::string{"To"})},
        {"descriptionValue2", sdbus::Variant(std::string{"/tmp/destination"})},
    });
  }
  assert(waitFor([&] { return jobs.find(id1) != nullptr && jobs.find(id1)->percent == 100; }));
  assert(jobs.find(id1)->processedBytes == 102400);
  assert(jobs.find(id1)->totalFiles == 10 && jobs.find(id1)->speed == 8192);
  assert(jobs.find(id2)->percent == std::nullopt);

  std::mutex signalMutex;
  std::condition_variable signalCondition;
  std::vector<std::string> receivedSignals;
  std::atomic<unsigned> otherClientCancelSignals{0};
  auto signalProxy = sdbus::createProxy(*client1, sdbus::ServiceName{kService}, path1);
  const auto recordSignal = [&](const char* name) {
    signalProxy->uponSignal(name).onInterface("org.kde.JobViewV3").call([&, name] {
      {
        std::lock_guard lock(signalMutex);
        receivedSignals.emplace_back(name);
      }
      signalCondition.notify_all();
    });
  };
  recordSignal("cancelRequested");
  recordSignal("suspendRequested");
  recordSignal("resumeRequested");
  auto secondSignalProxy = sdbus::createProxy(*client2, sdbus::ServiceName{kService}, path2);
  secondSignalProxy->uponSignal("cancelRequested").onInterface("org.kde.JobViewV3").call([&] {
    otherClientCancelSignals.fetch_add(1);
  });
  jobs.request(id1, JobProgressControl::Suspend);
  assert(jobs.update(id1, {{"suspended", sdbus::Variant(true)}})); // Client acknowledges Pause.
  jobs.request(id1, JobProgressControl::Resume);
  assert(jobs.update(id1, {{"suspended", sdbus::Variant(false)}})); // Client acknowledges Resume.
  jobs.request(id1, JobProgressControl::Cancel);
  jobs.request(id2, JobProgressControl::Suspend); // Capability absent: no signal.
  jobs.request(id2, JobProgressControl::Cancel);
  {
    std::unique_lock lock(signalMutex);
    assert(signalCondition.wait_for(lock, 2s, [&] { return receivedSignals.size() == 3; }));
  }
  assert(waitFor([&] { return otherClientCancelSignals.load() == 1; }));
  assert(receivedSignals[0] == "suspendRequested");
  assert(receivedSignals[1] == "resumeRequested");
  assert(receivedSignals[2] == "cancelRequested");

  sendUpdate(*client1, path1, {
      {"title", sdbus::Variant(std::string{"Copy completed"})},
      {"infoMessage", sdbus::Variant(std::string{"3 files copied to Documents"})},
  });
  terminate(*client1, path1, 0, "3 files copied to Documents");
  assert(waitFor([&] { return jobs.find(id1) == nullptr; }));
  {
    std::lock_guard lock(notificationMutex);
    assert(latestSummary == "Copy completed");
    assert(latestBody == "3 files copied to Documents");
  }
  assert(notifications.history().size() == historySize);
  assert(addedNotifications.load() == 2); // Ordinary notification plus successful job completion.
  bool terminalPathRemoved = false;
  try {
    std::string terminalXml;
    viewInspector->callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(terminalXml);
  } catch (const sdbus::Error&) {
    terminalPathRemoved = true;
  }
  assert(terminalPathRemoved);
  sendUpdate(*client2, path2, {
      {"title", sdbus::Variant(std::string{"Move cancelled"})},
      {"infoMessage", sdbus::Variant(std::string{"Transfer to Documents was cancelled"})},
  });
  terminate(*client2, path2, 1, "Transfer to Documents was cancelled");
  assert(waitFor([&] { return jobs.find(id2) == nullptr; }));
  {
    std::lock_guard lock(notificationMutex);
    assert(latestSummary == "Move cancelled");
    assert(latestBody == "Transfer to Documents was cancelled");
  }
  assert(notifications.history().size() == historySize);
  assert(addedNotifications.load() == 3); // Cancelled job completion also emitted a normal notification.

  auto failingClient = clientConnection();
  const auto failingPath = registerJob(*failingClient, 0, "org.nythera.FileManager");
  assert(waitFor([&] { return jobs.jobs().size() == 1; }));
  sendUpdate(*failingClient, failingPath, {
      {"title", sdbus::Variant(std::string{"Copy failed"})},
      {"infoMessage", sdbus::Variant(std::string{"Permission denied for \"secret.txt\""})},
  });
  terminate(*failingClient, failingPath, 500, "Permission denied for \"secret.txt\"");
  assert(waitFor([&] { return jobs.jobs().empty(); }));
  {
    std::lock_guard lock(notificationMutex);
    assert(latestSummary == "Copy failed");
    assert(latestBody == "Permission denied for \"secret.txt\"");
  }
  assert(notifications.history().size() == historySize + 1);
  assert(notifications.history().back().notification.persistInHistory);
  assert(addedNotifications.load() == 4);

  auto disconnectingClient = clientConnection();
  (void)registerJob(*disconnectingClient, 1);
  assert(waitFor([&] { return jobs.jobs().size() == 1; }));
  disconnectingClient.reset();
  assert(waitFor([&] { return jobs.jobs().empty(); }));

  service.reset();
  auto restartedService = std::make_unique<JobProgressService>(bus, jobs, notifications);
  assert(restartedService->ownsService());
  auto reconnectingClient = clientConnection();
  (void)registerJob(*reconnectingClient, 1);
  assert(waitFor([&] { return jobs.jobs().size() == 1; }));
  restartedService.reset();
  assert(waitFor([&] { return !servicePresent(*inspector); }));
  return 0;
}
