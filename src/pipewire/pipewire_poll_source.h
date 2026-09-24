#pragma once

#include "app/poll_source.h"
#include "pipewire/pipewire_service.h"

class PipeWirePollSource final : public PollSource {
public:
  explicit PipeWirePollSource(PipeWireService& service) : m_service(service) {}

  [[nodiscard]] int pollTimeoutMs() const override { return m_service.pollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_service.dispatch(); }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    int pwFd = m_service.fd();
    if (pwFd >= 0) {
      fds.push_back({.fd = pwFd, .events = POLLIN, .revents = 0});
    }
  }

private:
  PipeWireService& m_service;
};
