#pragma once

#include "app/poll_source.h"
#include "core/timer_manager.h"

class TimerPollSource final : public PollSource {
public:
  int pollTimeoutMs() const override { return TimerManager::instance().pollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {
    TimerManager::instance().tick();
  }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}
};
