#include "ipc/pixel_stream_client.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("pixel-stream");

  enum : std::uint32_t {
    kHello = 1,
    kRequest = 2,
  };

  enum : std::uint32_t {
    kBuffer = 1,
    kFrame = 2,
  };

  constexpr std::size_t kBufferMessageSize = 16;
  constexpr std::size_t kFrameMessageSize = 8;
  // Bounds one dispatch so a flooding companion cannot starve the main loop.
  constexpr int kMaxMessagesPerDispatch = 16;
  constexpr std::size_t kMaxFdsPerMessage = 4;

  std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
  }

  void writeLe32(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
  }

  // Descriptors received with one message; closes whatever the handler did not consume.
  struct ReceivedFds {
    std::array<int, kMaxFdsPerMessage> fds{};
    std::size_t count = 0;

    ReceivedFds() = default;
    ReceivedFds(const ReceivedFds&) = delete;
    ReceivedFds& operator=(const ReceivedFds&) = delete;
    ~ReceivedFds() {
      for (std::size_t i = 0; i < count; ++i) {
        ::close(fds[i]);
      }
    }
  };
} // namespace

PixelStreamClient::~PixelStreamClient() { disconnect(); }

bool PixelStreamClient::setPath(std::string path) {
  if (path == m_path) {
    return false;
  }
  disconnect();
  m_path = std::move(path);
  m_retryAt = {};
  return true;
}

std::optional<std::chrono::steady_clock::time_point> PixelStreamClient::retryAt() const {
  if (connected() || m_path.empty()) {
    return std::nullopt;
  }
  return m_retryAt;
}

void PixelStreamClient::tick(std::chrono::steady_clock::time_point now) {
  if (!connected() && !m_path.empty() && now >= m_retryAt) {
    connect();
  }
}

void PixelStreamClient::connect() {
  m_retryAt = std::chrono::steady_clock::now() + kRetryInterval;
  sockaddr_un address{};
  if (m_path.front() != '/' || m_path.size() >= sizeof(address.sun_path) || m_path.contains('\0')) {
    // Retried like any other failure, so log at debug level to avoid repeating a warning.
    kLog.debug("'{}' is not a usable socket path", m_path);
    return;
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);

  const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    kLog.warn("socket() failed: {}", std::strerror(errno));
    return;
  }
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    // The companion not running (yet) is the normal case; retry quietly.
    kLog.debug("connect '{}' failed: {}", m_path, std::strerror(errno));
    ::close(fd);
    return;
  }
  ucred peer{};
  socklen_t size = sizeof(peer);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid()) {
    kLog.warn("refusing '{}': companion runs as another user", m_path);
    ::close(fd);
    return;
  }
  m_fd = fd;
  if (send(kHello, kProtocolVersion) && send(kRequest, 0)) {
    m_requested = true;
    kLog.debug("connected '{}'", m_path);
  }
}

void PixelStreamClient::disconnect() {
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
  for (Buffer& buffer : m_buffers) {
    unmap(buffer);
  }
  m_frameBuffer.reset();
  m_requested = false;
  m_retryAt = std::chrono::steady_clock::now() + kRetryInterval;
}

void PixelStreamClient::fail(std::string_view reason) {
  kLog.warn("'{}': {}; reconnecting", m_path, reason);
  disconnect();
  notify();
}

void PixelStreamClient::reconnect() {
  if (m_path.empty()) {
    return;
  }
  disconnect();
  m_retryAt = {};
}

bool PixelStreamClient::send(std::uint32_t type, std::uint32_t argument) {
  std::array<std::uint8_t, 8> message{};
  writeLe32(message.data(), type);
  writeLe32(message.data() + 4, argument);
  if (::send(m_fd, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL)
      != static_cast<ssize_t>(message.size())) {
    // One outstanding request never fills the socket buffer; any failure means the peer is gone.
    fail("write failed");
    return false;
  }
  return true;
}

void PixelStreamClient::dispatch(short revents) {
  if (!connected()) {
    return;
  }
  if ((revents & (POLLERR | POLLNVAL)) != 0) {
    fail("socket error");
    return;
  }
  if ((revents & (POLLIN | POLLHUP)) == 0) {
    return;
  }
  for (int i = 0; i < kMaxMessagesPerDispatch && readMessage(); ++i) {
  }
}

bool PixelStreamClient::readMessage() {
  std::array<std::uint8_t, kBufferMessageSize + 1> payload{};
  alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)> control{};
  iovec vector{.iov_base = payload.data(), .iov_len = payload.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_control = control.data();
  message.msg_controllen = control.size();

  const ssize_t count = ::recvmsg(m_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  if (count < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return false;
    }
    fail("read failed");
    return false;
  }

  ReceivedFds fds;
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
      continue;
    }
    const std::size_t received = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (std::size_t i = 0; i < received && fds.count < fds.fds.size(); ++i) {
      std::memcpy(&fds.fds[fds.count++], CMSG_DATA(header) + i * sizeof(int), sizeof(int));
    }
  }

  if (count == 0) {
    kLog.debug("'{}' closed by companion", m_path);
    disconnect();
    notify();
    return false;
  }
  if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    fail("oversized message");
    return false;
  }

  const auto size = static_cast<std::size_t>(count);
  const std::uint32_t type = size >= 4 ? readLe32(payload.data()) : 0;
  if (type == kBuffer && size == kBufferMessageSize && fds.count == 1) {
    attachBuffer(readLe32(payload.data() + 4), readLe32(payload.data() + 8), readLe32(payload.data() + 12), fds.fds[0]);
    return connected();
  }
  if (type == kFrame && size == kFrameMessageSize && fds.count == 0) {
    const std::uint32_t id = readLe32(payload.data() + 4);
    if (!m_requested) {
      fail("FRAME without a pending REQUEST");
      return false;
    }
    if (id >= kMaxBuffers || m_buffers[id].data == nullptr) {
      fail("FRAME names an unattached buffer");
      return false;
    }
    m_requested = false;
    m_frameBuffer = id;
    notify();
    return connected();
  }
  fail("malformed message");
  return false;
}

void PixelStreamClient::attachBuffer(std::uint32_t id, std::uint32_t width, std::uint32_t height, int fd) {
  if (id >= kMaxBuffers) {
    fail("BUFFER id out of range");
    return;
  }
  if (m_frameBuffer == id) {
    fail("BUFFER replaces the buffer the shell is reading");
    return;
  }
  if (width == 0
      || height == 0
      || width > kMaxDimension
      || height > kMaxDimension
      || std::size_t{width} * height > kMaxPixels) {
    fail("BUFFER dimensions out of range");
    return;
  }
  const std::size_t bytes = std::size_t{width} * height * 4;
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::size_t>(info.st_size) < bytes) {
    fail("BUFFER file is smaller than its pixels");
    return;
  }
  // A shrinkable mapping could SIGBUS the shell; require the companion to seal it.
  const int seals = ::fcntl(fd, F_GET_SEALS);
  if (seals < 0 || (seals & F_SEAL_SHRINK) == 0) {
    fail("BUFFER memfd is not sealed against shrinking");
    return;
  }
  void* data = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    fail("BUFFER mmap failed");
    return;
  }
  Buffer& buffer = m_buffers[id];
  unmap(buffer);
  buffer = Buffer{.data = data, .size = bytes, .width = static_cast<int>(width), .height = static_cast<int>(height)};
}

void PixelStreamClient::unmap(Buffer& buffer) {
  if (buffer.data != nullptr) {
    ::munmap(buffer.data, buffer.size);
  }
  buffer = Buffer{};
}

std::optional<PixelStreamClient::Frame> PixelStreamClient::frame() const {
  if (!m_frameBuffer.has_value()) {
    return std::nullopt;
  }
  const Buffer& buffer = m_buffers[*m_frameBuffer];
  return Frame{.pixels = static_cast<const std::uint8_t*>(buffer.data), .width = buffer.width, .height = buffer.height};
}

void PixelStreamClient::releaseFrame() {
  if (!m_frameBuffer.has_value()) {
    return;
  }
  m_frameBuffer.reset();
  if (connected() && send(kRequest, 0)) {
    m_requested = true;
  }
}

void PixelStreamClient::notify() {
  if (m_onChange) {
    m_onChange();
  }
}
