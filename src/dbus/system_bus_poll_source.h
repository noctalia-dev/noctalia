#pragma once

#include "app/poll_source.h"
#include "dbus/system_bus.h"

class SystemBusPollSource final : public PollSource {
public:
  explicit SystemBusPollSource(SystemBus& bus) : m_bus(bus) {}

  int pollTimeoutMs() const override {
    if (m_bus.hasPendingEvents()) {
      return 0;
    }
    const int t = m_bus.getPollData().getPollTimeout();
    return t;
  }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_bus.processPendingEvents(); }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    auto pd = m_bus.getPollData();
    fds.push_back({.fd = pd.fd, .events = pd.events, .revents = 0});
    fds.push_back({.fd = pd.eventFd, .events = POLLIN, .revents = 0});
  }

private:
  SystemBus& m_bus;
};
