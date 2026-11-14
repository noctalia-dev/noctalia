#pragma once

#include "notification/job_progress.h"

#include <memory>
#include <utility>
#include <vector>
#include <sdbus-c++/sdbus-c++.h>

class SessionBus;
class NotificationManager;

class JobProgressService {
public:
  JobProgressService(SessionBus& bus, JobProgressManager& manager, NotificationManager& notifications);
  ~JobProgressService();
  [[nodiscard]] bool ownsService() const noexcept { return m_owned; }
  void request(std::uint32_t id, const char* signal);
  static void requestThunk(void* context, std::uint32_t id, JobProgressControl control);
private:
  void clientGone(const std::string& name, const std::string& oldOwner, const std::string& newOwner);
  SessionBus& m_bus;
  JobProgressManager& m_manager;
  NotificationManager& m_notifications;
  std::unique_ptr<sdbus::IObject> m_server;
  std::unique_ptr<sdbus::IProxy> m_busProxy;
  std::vector<std::pair<std::uint32_t, std::shared_ptr<sdbus::IObject>>> m_views;
  bool m_owned = false;
};
