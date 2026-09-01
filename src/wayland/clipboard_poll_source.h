#pragma once

#include "app/poll_source.h"
#include "wayland/clipboard_service.h"

class ClipboardPollSource final : public PollSource {
public:
  explicit ClipboardPollSource(ClipboardService& clipboard) : m_clipboard(clipboard) {}

  // A NULL selection queues an adoption but leaves no fd to poll on, so without
  // this the main loop would never dispatch us and the shell would keep the
  // stored payload while the live selection stays empty.
  [[nodiscard]] int pollTimeoutMs() const override { return m_clipboard.hasPendingOrphanAdopt() ? 0 : -1; }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    m_clipboard.dispatchPollEvents(fds, startIdx, m_pollFdCount);
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override { m_pollFdCount = m_clipboard.addPollFds(fds); }

private:
  ClipboardService& m_clipboard;
  std::size_t m_pollFdCount = 0;
};
