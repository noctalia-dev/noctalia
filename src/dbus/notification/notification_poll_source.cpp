#include "dbus/notification/notification_poll_source.h"

#include "dbus/notification/notification_dbus_host.h"

void NotificationPollSource::dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) {
  if (m_dbus != nullptr) {
    m_dbus->processExpired();
  } else {
    m_manager.processExpired();
  }
}
