#include "compositors/umbriel/umbriel_audio_provider.h"

#include "compositors/umbriel/umbriel_runtime.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace compositors::umbriel {
  namespace {
    constexpr auto kHeartbeat = std::chrono::milliseconds(100);
    constexpr auto kMinInterval = std::chrono::nanoseconds(16666667);
    constexpr auto kReplyTimeout = std::chrono::milliseconds(200);
    constexpr std::size_t kMaxPacket = 4096;
  } // namespace

  UmbrielAudioProvider::UmbrielAudioProvider(UmbrielRuntime& runtime, SnapshotSource source)
      : m_runtime(runtime), m_source(std::move(source)) {}
  UmbrielAudioProvider::~UmbrielAudioProvider() { disconnect(); }

  void UmbrielAudioProvider::setEnabled(bool enabled) {
    if (m_enabled == enabled) {
      return;
    }
    m_enabled = enabled;
    disconnect();
    m_due = m_lastSentAt + kMinInterval;
    m_backoff = std::chrono::milliseconds(100);
  }

  void UmbrielAudioProvider::notifyChanged() {
    m_dirty = true;
    if (m_fd >= 0 && !m_awaitingReply && m_output.empty()) {
      m_due = std::min(m_due, m_lastSentAt + kMinInterval);
    }
  }

  void UmbrielAudioProvider::disconnect() {
    if (m_fd >= 0) {
      close(m_fd);
    }
    m_fd = -1;
    m_awaitingReply = false;
    m_output.clear();
    m_written = 0;
    m_input.clear();
    m_due = Clock::now() + m_backoff;
    m_backoff = std::min(m_backoff * 2, std::chrono::milliseconds(2000));
  }

  bool UmbrielAudioProvider::connectSocket() {
    const auto& path = m_runtime.socketPath();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
      return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    m_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    // Linux AF_UNIX connect completes immediately or fails (including EAGAIN).
    return m_fd >= 0 && connect(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
  }

  void UmbrielAudioProvider::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_fd >= 0) {
      const short events = m_output.empty() ? POLLIN : POLLIN | POLLOUT;
      fds.push_back({m_fd, events, 0});
    }
  }

  int UmbrielAudioProvider::pollTimeoutMs() const {
    if (!m_enabled) {
      return -1;
    }
    return static_cast<int>(
        std::max(std::chrono::milliseconds(0), std::chrono::ceil<std::chrono::milliseconds>(m_due - Clock::now()))
            .count()
    );
  }

  bool UmbrielAudioProvider::readReply() {
    std::array<char, kMaxPacket> buffer{};
    const auto size = recv(m_fd, buffer.data(), buffer.size(), 0);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      return true;
    }
    if (size <= 0 || !m_awaitingReply || m_input.size() + static_cast<std::size_t>(size) > kMaxPacket) {
      return false;
    }
    m_input.append(buffer.data(), static_cast<std::size_t>(size));
    const auto newline = m_input.find('\n');
    if (newline == std::string::npos) {
      return m_input.size() < kMaxPacket;
    }
    const auto reply = nlohmann::json::parse(m_input, nullptr, false);
    if (newline != m_input.size() - 1 || reply.is_discarded() || reply != nlohmann::json{{"ok", true}}) {
      return false;
    }
    m_input.clear();
    m_awaitingReply = false;
    m_backoff = std::chrono::milliseconds(100);
    m_due = m_lastSentAt + (m_dirty ? kMinInterval : kHeartbeat);
    return true;
  }

  bool UmbrielAudioProvider::writeSnapshot() {
    const auto size = send(m_fd, m_output.data() + m_written, m_output.size() - m_written, MSG_NOSIGNAL);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      return true;
    }
    if (size <= 0) {
      return false;
    }
    m_written += static_cast<std::size_t>(size);
    if (m_written == m_output.size()) {
      m_output.clear();
      m_written = 0;
      m_awaitingReply = true;
      m_lastSentAt = Clock::now();
    }
    return true;
  }

  void UmbrielAudioProvider::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (!m_enabled) {
      return;
    }
    const auto now = Clock::now();
    const short events = m_fd >= 0 && startIdx < fds.size() && fds[startIdx].fd == m_fd ? fds[startIdx].revents : 0;
    if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0
        || ((events & POLLIN) != 0 && !readReply())
        || ((m_awaitingReply || !m_output.empty()) && now >= m_due)) {
      disconnect();
      return;
    }
    if (m_awaitingReply) {
      return;
    }
    if (!m_output.empty()) {
      if ((events & POLLOUT) == 0) {
        return;
      }
      if (m_written != 0 || !m_dirty) {
        if (!writeSnapshot()) {
          disconnect();
        }
        return;
      }
    } else if (now < m_due) {
      return;
    }
    if (m_fd < 0 && !connectSocket()) {
      disconnect();
      return;
    }
    const auto bands = m_source();
    if (!std::ranges::all_of(bands, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; })) {
      disconnect();
      return;
    }
    if (m_output.empty()) {
      m_due = now + kReplyTimeout;
    }
    m_output = nlohmann::json{{"cmd", "effect-audio"}, {"version", 2}, {"bands", bands}}.dump() + '\n';
    m_dirty = false;
    if (m_output.size() > kMaxPacket || !writeSnapshot()) {
      disconnect();
    }
  }
} // namespace compositors::umbriel
