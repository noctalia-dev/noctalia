#pragma once

#include "core/poll_source.h"
#include "system/brightness_service.h"

class BrightnessPollSource final : public PollSource {
public:
  explicit BrightnessPollSource(BrightnessService& service) : m_service(service) {}

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (m_service.watchFd() >= 0 && (fds[startIdx].revents & POLLIN) != 0) {
      m_service.dispatchWatch();
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (m_service.watchFd() >= 0) {
      fds.push_back({.fd = m_service.watchFd(), .events = POLLIN, .revents = 0});
    }
  }

private:
  BrightnessService& m_service;
};
