#pragma once

#include "app/poll_source.h"
#include "compositors/compositor_platform.h"

class WorkspacePollSource final : public PollSource {
public:
  explicit WorkspacePollSource(CompositorPlatform& platform) : m_platform(platform) {}

  int pollTimeoutMs() const override { return m_platform.workspacePollTimeoutMs(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    m_platform.dispatchWorkspacePoll(fds, startIdx);
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override { (void)m_platform.addWorkspacePollFds(fds); }

private:
  CompositorPlatform& m_platform;
};
