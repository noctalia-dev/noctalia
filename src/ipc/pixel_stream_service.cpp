#include "ipc/pixel_stream_service.h"

#include "ipc/pixel_stream_client.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

PixelStreamService* PixelStreamService::s_instance = nullptr;

PixelStreamService::PixelStreamService() { s_instance = this; }

PixelStreamService::~PixelStreamService() {
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

PixelStreamService* PixelStreamService::current() noexcept { return s_instance; }

void PixelStreamService::add(PixelStreamClient& client) {
  if (!std::ranges::contains(m_clients, &client)) {
    m_clients.push_back(&client);
  }
}

void PixelStreamService::remove(PixelStreamClient& client) {
  std::erase(m_clients, &client);
  std::ranges::replace(m_polled, &client, nullptr);
}

int PixelStreamService::pollTimeoutMs() const {
  std::optional<std::chrono::steady_clock::time_point> earliest;
  for (const PixelStreamClient* client : m_clients) {
    if (const auto retryAt = client->retryAt(); retryAt.has_value()) {
      earliest = earliest.has_value() ? std::min(*earliest, *retryAt) : *retryAt;
    }
  }
  if (!earliest.has_value()) {
    return -1;
  }
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(*earliest - std::chrono::steady_clock::now()).count();
  return static_cast<int>(std::max<std::int64_t>(0, remaining));
}

void PixelStreamService::doAddPollFds(std::vector<pollfd>& fds) {
  m_polled.clear();
  for (PixelStreamClient* client : m_clients) {
    if (client->connected()) {
      fds.push_back({.fd = client->fd(), .events = POLLIN, .revents = 0});
      m_polled.push_back(client);
    }
  }
}

void PixelStreamService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  // Callbacks only mark controls dirty, but a client can still be removed mid-loop, so every
  // step re-checks registration instead of trusting a snapshot.
  for (std::size_t i = 0; i < m_polled.size() && startIdx + i < fds.size(); ++i) {
    PixelStreamClient* client = m_polled[i];
    if (client != nullptr && client->fd() == fds[startIdx + i].fd && fds[startIdx + i].revents != 0) {
      client->dispatch(fds[startIdx + i].revents);
    }
  }
  m_polled.clear();

  const auto now = std::chrono::steady_clock::now();
  const std::vector<PixelStreamClient*> clients = m_clients;
  for (PixelStreamClient* client : clients) {
    if (std::ranges::contains(m_clients, client)) {
      client->tick(now);
    }
  }
}
