#pragma once

#include "compositors/compositor_platform.h"
#include "core/poll_source.h"

class KeyboardLayoutPollSource final : public PollSource {
public:
  explicit KeyboardLayoutPollSource(CompositorPlatform& platform) : m_platform(platform) {}

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    m_platform.dispatchKeyboardLayoutPoll(fds, startIdx);
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override { m_platform.addKeyboardLayoutPollFds(fds); }

private:
  CompositorPlatform& m_platform;
};
