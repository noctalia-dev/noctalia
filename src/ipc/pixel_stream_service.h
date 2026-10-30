#pragma once

#include "app/poll_source.h"

#include <vector>

class PixelStreamClient;

// Polls the companion sockets of every live PixelStreamClient and drives their reconnects.
// Clients register while their owning control exists.
class PixelStreamService final : public PollSource {
public:
  PixelStreamService();
  ~PixelStreamService() override;
  PixelStreamService(const PixelStreamService&) = delete;
  PixelStreamService& operator=(const PixelStreamService&) = delete;

  // Null before the application creates the service and after it is destroyed.
  [[nodiscard]] static PixelStreamService* current() noexcept;

  void add(PixelStreamClient& client);
  void remove(PixelStreamClient& client);

  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  static PixelStreamService* s_instance;

  std::vector<PixelStreamClient*> m_clients;
  // Clients whose fd was appended by the last doAddPollFds, in fd order. A client removed
  // before dispatch is nulled rather than erased so the indices stay aligned.
  std::vector<PixelStreamClient*> m_polled;
};
