#pragma once

#include "app/poll_source.h"
#include "wayland/wayland_connection.h"

class KeyRepeatPollSource final : public PollSource {
public:
  explicit KeyRepeatPollSource(WaylandConnection& wayland) : m_wayland(wayland) {}

  int pollTimeoutMs() const override { return m_wayland.repeatPollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_wayland.repeatTick(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  WaylandConnection& m_wayland;
};
