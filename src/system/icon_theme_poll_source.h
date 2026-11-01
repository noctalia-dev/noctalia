#pragma once

#include "app/poll_source.h"

#include <functional>
#include <memory>

class IconThemePollSource final : public PollSource {
public:
  IconThemePollSource();
  ~IconThemePollSource() override;

  IconThemePollSource(const IconThemePollSource&) = delete;
  IconThemePollSource& operator=(const IconThemePollSource&) = delete;

  void setChangeCallback(std::function<void()> callback);
  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
