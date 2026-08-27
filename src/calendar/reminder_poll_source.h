#pragma once

#include "app/poll_source.h"
#include "calendar/reminder_scheduler.h"

class ReminderPollSource final : public PollSource {
public:
  explicit ReminderPollSource(CalendarReminderScheduler& scheduler) : m_scheduler(scheduler) {}

  [[nodiscard]] int pollTimeoutMs() const override { return m_scheduler.pollTimeoutMs(); }
  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_scheduler.tick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  CalendarReminderScheduler& m_scheduler;
};
