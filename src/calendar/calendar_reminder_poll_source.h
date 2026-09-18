#pragma once

#include "app/poll_source.h"
#include "calendar/calendar_reminder_monitor.h"

class CalendarReminderPollSource final : public PollSource {
public:
  explicit CalendarReminderPollSource(CalendarReminderMonitor& monitor) : m_monitor(monitor) {}

  [[nodiscard]] int pollTimeoutMs() const override { return m_monitor.pollTimeoutMs(); }
  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_monitor.tick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  CalendarReminderMonitor& m_monitor;
};
