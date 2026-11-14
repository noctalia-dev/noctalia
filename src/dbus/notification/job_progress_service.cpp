#include "dbus/notification/job_progress_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"

#include <algorithm>
#include <string>

namespace {
  constexpr Logger kLog("job-progress");
  constexpr auto kService = "org.kde.JobViewServer";
  constexpr auto kServerPath = "/JobViewServer";
  constexpr auto kServerInterface = "org.kde.JobViewServerV2";
  constexpr auto kViewInterface = "org.kde.JobViewV3";
}

JobProgressService::JobProgressService(SessionBus& bus, JobProgressManager& manager, NotificationManager& notifications)
    : m_bus(bus), m_manager(manager), m_notifications(notifications) {
  m_server = sdbus::createObject(m_bus.connection(), sdbus::ObjectPath{kServerPath});
  m_server->addVTable(
      sdbus::registerMethod("requestView").withInputParamNames("desktopEntry", "capabilities", "hints")
          .withOutputParamNames("trackerPath").implementedAs([this, object = m_server.get()](
              const std::string& desktopEntry, std::int32_t capabilities,
              const std::map<std::string, sdbus::Variant>& registrationHints) {
            const std::string sender = object->getCurrentlyProcessedMessage().getSender();
            if (desktopEntry.empty() || desktopEntry.size() > 256 || capabilities < 0 || registrationHints.size() > 64)
              throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.InvalidArgs"}, "invalid job registration");
            const auto id = m_manager.add(sender, desktopEntry, static_cast<std::uint32_t>(capabilities));
            if (!registrationHints.empty() && !m_manager.update(id, registrationHints)) {
              m_manager.remove(id);
              throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.InvalidArgs"}, "invalid initial job state");
            }
            const auto path = std::string("/org/kde/notificationmanager/jobs/JobView_") + std::to_string(id);
            std::shared_ptr<sdbus::IObject> view{sdbus::createObject(m_bus.connection(), sdbus::ObjectPath{path}).release()};
            const std::weak_ptr<sdbus::IObject> weakView = view;
            view->addVTable(
                sdbus::registerMethod("update").withInputParamNames("properties").implementedAs(
                    [this, id, weakView](const std::map<std::string, sdbus::Variant>& properties) {
                      const auto viewObject = weakView.lock();
                      const auto* job = m_manager.find(id);
                      if (job == nullptr || viewObject == nullptr || viewObject->getCurrentlyProcessedMessage().getSender() != job->owner
                          || !m_manager.update(id, properties))
                        throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "invalid job update");
                    }),
                sdbus::registerMethod("terminate").withInputParamNames("errorCode", "errorMessage", "hints")
                    .implementedAs([this, id, weakView](std::uint32_t code, const std::string& message,
                                                                  const std::map<std::string, sdbus::Variant>& finalHints) {
                      const auto viewObject = weakView.lock();
                      const auto* job = m_manager.find(id);
                      if (job == nullptr || viewObject == nullptr || viewObject->getCurrentlyProcessedMessage().getSender() != job->owner || message.size() > 4096 || finalHints.size() > 64)
                        throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "invalid job termination");
                      if (!m_manager.finish(id, code, message)) return;
                      const auto* finished = m_manager.find(id);
                      if (finished != nullptr) {
                        const bool cancelled = code == 1;
                        const bool detailedFileOperation = finished->desktopEntry == "org.nythera.FileManager"
                            && (finished->title.ends_with(" completed") || finished->title.ends_with(" cancelled")
                                || finished->title.ends_with(" failed"));
                        const std::string summary = detailedFileOperation ? finished->title
                            : code == 0 ? i18n::tr("notifications.internal.job-completed")
                                : cancelled ? i18n::tr("notifications.internal.job-cancelled")
                                            : i18n::tr("notifications.internal.job-failed");
                        NotificationRequest notification{
                            .appName = finished->desktopEntry,
                            .summary = summary,
                            .body = message.empty() ? finished->infoMessage.empty() ? finished->title : finished->infoMessage : message,
                            .urgency = code != 0 && !cancelled ? Urgency::Critical : Urgency::Normal,
                            .timeout = kDefaultNotificationTimeout,
                            .origin = NotificationOrigin::Internal,
                            .persistInHistory = code != 0 && !cancelled,
                        };
                        m_notifications.addOrReplace(std::move(notification));
                        m_manager.remove(id);
                        std::erase_if(m_views, [id](const auto& entry) { return entry.first == id; });
                      }
                    }),
                sdbus::registerSignal("suspendRequested"), sdbus::registerSignal("resumeRequested"),
                sdbus::registerSignal("cancelRequested")
            ).forInterface(kViewInterface);
            m_views.emplace_back(id, std::move(view));
            return sdbus::ObjectPath{path};
          })
  ).forInterface(kServerInterface);

  auto dbus = sdbus::createProxy(m_bus.connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
  std::uint32_t reply = 0;
  try {
    dbus->callMethod("RequestName").onInterface("org.freedesktop.DBus")
        .withArguments(std::string{kService}, std::uint32_t{4}).storeResultsTo(reply);
  } catch (const sdbus::Error& e) {
    m_server->unregister();
    m_server.reset();
    kLog.warn("KDE job service unavailable: {}", e.what());
    return;
  }
  if (reply != 1 && reply != 4) {
    m_server->unregister();
    m_server.reset();
    kLog.info("KDE job service already provided by another process");
    return;
  }
  m_owned = true;
  m_manager.setControlCallback(&JobProgressService::requestThunk, this);
  m_busProxy = sdbus::createProxy(m_bus.connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
  m_busProxy->uponSignal("NameOwnerChanged").onInterface("org.freedesktop.DBus").call(
      [this](const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
        clientGone(name, oldOwner, newOwner);
      }
  );
  kLog.info("providing KDE job progress service");
}

JobProgressService::~JobProgressService() {
  m_manager.setControlCallback(nullptr, nullptr);
  for (auto& [id, view] : m_views) { (void)id; if (view) view->unregister(); }
  if (m_server) m_server->unregister();
  if (m_owned) {
    try { m_bus.connection().releaseName(sdbus::ServiceName{kService}); }
    catch (const sdbus::Error& e) { kLog.debug("job service release failed: {}", e.what()); }
  }
}

void JobProgressService::request(std::uint32_t id, const char* signal) {
  const JobProgress* job = m_manager.find(id);
  if (job == nullptr || !m_owned) return;
  auto it = std::ranges::find_if(m_views, [id](const auto& entry) { return entry.first == id; });
  if (it == m_views.end()) return;
  const std::string name(signal);
  it->second->emitSignal(name).onInterface(kViewInterface);
}

void JobProgressService::requestThunk(void* context, std::uint32_t id, JobProgressControl control) {
  const char* signal = control == JobProgressControl::Cancel ? "cancelRequested"
      : control == JobProgressControl::Suspend ? "suspendRequested" : "resumeRequested";
  static_cast<JobProgressService*>(context)->request(id, signal);
}

void JobProgressService::clientGone(const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
  if (!name.starts_with(':') || oldOwner.empty() || !newOwner.empty()) return;
  m_manager.removeOwner(name);
  std::erase_if(m_views, [this](const auto& entry) { return m_manager.find(entry.first) == nullptr; });
}
