#pragma once

#include "app/poll_source.h"
#include "system/lock_keys_service.h"

class LockKeysPollSource final : public PollSource {
public:
  explicit LockKeysPollSource(LockKeysService& service) : m_service(service) {}

  int pollTimeoutMs() const override { return m_service.pollTimeoutMs(); }
  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override { m_service.dispatchPoll(); }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  LockKeysService& m_service;
};
