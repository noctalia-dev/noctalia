#pragma once

#include <poll.h>
#include <vector>

class PollSource {
public:
  virtual ~PollSource() = default;

  // Append any fds this source wants polled. Returns the starting index.
  std::size_t addPollFds(std::vector<pollfd>& fds) {
    auto start = fds.size();
    doAddPollFds(fds);
    return start;
  }

  // Minimum timeout this source needs before its next dispatch, or -1 for none.
  // The loop treats it as an absolute deadline from when it is first advertised.
  virtual int pollTimeoutMs() const { return -1; }

  // Called after poll() returns. startIdx is what addPollFds returned.
  virtual void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) = 0;

protected:
  virtual void doAddPollFds(std::vector<pollfd>& fds) = 0;
};
