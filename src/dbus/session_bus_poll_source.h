#pragma once

#include "app/poll_source.h"
#include "dbus/session_bus.h"

class SessionBusPollSource final : public PollSource {
public:
  explicit SessionBusPollSource(SessionBus& bus) : m_bus(bus) {}

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
  SessionBus& m_bus;
};
