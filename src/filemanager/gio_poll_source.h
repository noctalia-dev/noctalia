#pragma once

#include "app/poll_source.h"

#include <glib.h>
#include <vector>

class GioPollSource final : public PollSource {
public:
  explicit GioPollSource(GMainContext* context = nullptr);
  ~GioPollSource() override;
  GioPollSource(const GioPollSource&) = delete;
  GioPollSource& operator=(const GioPollSource&) = delete;

  [[nodiscard]] int pollTimeoutMs() const override { return m_timeoutMs; }
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;
  void finishPoll() override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  GMainContext* m_context = nullptr;
  int m_priority = 0;
  int m_timeoutMs = -1;
  bool m_preparedReady = false;
  bool m_contextAcquired = false;
  std::vector<GPollFD> m_fds;
};
