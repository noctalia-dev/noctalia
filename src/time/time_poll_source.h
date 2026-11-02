#pragma once

#include "app/poll_source.h"
#include "time/time_service.h"

class TimePollSource final : public PollSource {
public:
  explicit TimePollSource(TimeService& time) : m_time(time) {}

  int pollTimeoutMs() const override { return m_time.pollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_time.tick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  TimeService& m_time;
};
