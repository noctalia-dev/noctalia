#pragma once

#include "core/deferred_call.h"
#include "core/poll_source.h"

#include <poll.h>
#include <vector>

class DeferredCallPollSource final : public PollSource {
public:
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (startIdx < fds.size() && (fds[startIdx].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
      DeferredCall::drainWakeFd();
    }

    auto pending = DeferredCall::takePending();
    for (auto& fn : pending) {
      if (fn) {
        fn();
      }
    }
  }

private:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    const int fd = DeferredCall::wakeFd();
    if (fd >= 0) {
      fds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
    }
  }
};
