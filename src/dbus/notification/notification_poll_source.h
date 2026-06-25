#pragma once

#include "app/poll_source.h"
#include "notification/notification_manager.h"

class NotificationDBusHost;

class NotificationPollSource final : public PollSource {
public:
  explicit NotificationPollSource(NotificationManager& manager) : m_manager(manager) {}

  void setDbusService(NotificationDBusHost* dbus) { m_dbus = dbus; }

  [[nodiscard]] int pollTimeoutMs() const override { return m_manager.nextExpiryTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override;

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  NotificationManager& m_manager;
  NotificationDBusHost* m_dbus = nullptr;
};
