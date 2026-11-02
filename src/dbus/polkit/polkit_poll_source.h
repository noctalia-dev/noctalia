#pragma once

#include "app/poll_source.h"
#include "dbus/polkit/polkit_agent.h"

class PolkitPollSource final : public PollSource {
public:
  explicit PolkitPollSource(PolkitAgent& agent) : m_agent(agent) {}

  int pollTimeoutMs() const override { return m_agent.pollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override { m_agent.dispatch(fds, startIdx); }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override { m_agent.addPollFds(fds); }

private:
  PolkitAgent& m_agent;
};
