#pragma once

#include "config/config_service.h"
#include "core/poll_source.h"

class ConfigPollSource final : public PollSource {
public:
  explicit ConfigPollSource(ConfigService& config) : m_config(config) {}

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (m_config.watchFd() >= 0 && (fds[startIdx].revents & POLLIN) != 0) {
      m_config.checkReload();
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (m_config.watchFd() >= 0) {
      fds.push_back({.fd = m_config.watchFd(), .events = POLLIN, .revents = 0});
    }
  }

private:
  ConfigService& m_config;
};
