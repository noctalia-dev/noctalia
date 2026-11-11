#pragma once

#include "core/poll_source.h"
#include "ipc/ipc_service.h"

class IpcPollSource final : public PollSource {
public:
  explicit IpcPollSource(IpcService& ipc) : m_ipc(ipc) {}

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (m_ipc.listenFd() >= 0 && (fds[startIdx].revents & POLLIN) != 0) {
      m_ipc.dispatch();
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (m_ipc.listenFd() >= 0) {
      fds.push_back({.fd = m_ipc.listenFd(), .events = POLLIN, .revents = 0});
    }
  }

private:
  IpcService& m_ipc;
};
