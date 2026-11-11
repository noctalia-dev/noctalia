#include "compositors/sway/sway_runtime.h"

#include "compositors/sway/sway_event_handler.h"
#include "core/log.h"
#include "system/process.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace compositors::sway {

  namespace {

    constexpr Logger kLog("sway_runtime");
    constexpr std::string_view kIpcMagic = "i3-ipc";
    constexpr std::size_t kIpcHeaderSize = kIpcMagic.size() + (2 * sizeof(std::uint32_t));
    // The union of the event types the handlers consume: workspaces and windows for the workspace
    // backend, input for keyboard layout switches.
    constexpr std::string_view kSubscribedEvents = R"(["workspace","window","input"])";

  } // namespace

  SwayRuntime::~SwayRuntime() {
    if (m_eventSocketFd >= 0) {
      ::close(m_eventSocketFd);
    }
  }

  const std::string& SwayRuntime::socketPath() const {
    ensureResolved();
    return m_socketPath;
  }

  const std::string& SwayRuntime::msgCommand() const {
    ensureResolved();
    return m_msgCommand;
  }

  const std::string& SwayRuntime::outputCommand() const {
    ensureResolved();
    return m_outputCommand;
  }

  bool SwayRuntime::hasMsgCommand() const {
    ensureResolved();
    return !m_msgCommand.empty();
  }

  bool SwayRuntime::hasOutputCommand() const {
    ensureResolved();
    return !m_outputCommand.empty();
  }

  void SwayRuntime::refresh() {
    m_socketPath.clear();
    m_msgCommand.clear();
    m_outputCommand.clear();
    m_resolved = true;
    resolveSocketPath();
    resolveCommands();
  }

  bool SwayRuntime::connectSocket() {
    const std::string& path = socketPath();
    if (path.empty()) {
      return false;
    }

    cleanup();

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      kLog.warn("sway IPC socket path too long");
      return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    m_eventSocketFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_eventSocketFd < 0) {
      kLog.warn("failed to create sway IPC socket: {}", std::strerror(errno));
      return false;
    }

    if (::connect(m_eventSocketFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      kLog.warn("failed to connect to sway IPC {}: {}", path, std::strerror(errno));
      cleanup();
      return false;
    }

    sendMessage(ipc::kSubscribe, kSubscribedEvents);
    if (!connected()) {
      return false;
    }
    kLog.info("connected to sway IPC at {}", path);
    return true;
  }

  void SwayRuntime::sendMessage(std::uint32_t type, std::string_view payload) {
    if (m_eventSocketFd < 0) {
      return;
    }

    const auto payloadLength = static_cast<std::uint32_t>(payload.size());
    std::string message;
    message.reserve(kIpcHeaderSize + payload.size());
    message.append(kIpcMagic);
    message.append(reinterpret_cast<const char*>(&payloadLength), sizeof(payloadLength));
    message.append(reinterpret_cast<const char*>(&type), sizeof(type));
    message.append(payload);

    std::size_t offset = 0;
    while (offset < message.size()) {
      const ssize_t written = ::send(m_eventSocketFd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        kLog.warn("failed to send sway IPC message: {}", std::strerror(errno));
        cleanup();
        return;
      }
      offset += static_cast<std::size_t>(written);
    }
  }

  void SwayRuntime::cleanup() {
    if (m_eventSocketFd >= 0) {
      ::close(m_eventSocketFd);
      m_eventSocketFd = -1;
    }
    m_readBuffer.clear();
    for (auto* handler : m_eventHandlers) {
      handler->handleCleanup();
    }
  }

  void SwayRuntime::registerEventHandler(SwayEventHandler* handler) { m_eventHandlers.push_back(handler); }

  void SwayRuntime::unregisterEventHandler(SwayEventHandler* handler) { std::erase(m_eventHandlers, handler); }

  void SwayRuntime::dispatchPoll(short revents) {
    if (m_eventSocketFd < 0) {
      return;
    }
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      kLog.warn("sway IPC disconnected");
      disconnect();
      return;
    }
    if ((revents & POLLIN) != 0) {
      readSocket();
    }
  }

  void SwayRuntime::disconnect() {
    cleanup();
    for (auto* handler : m_eventHandlers) {
      handler->handleDisconnect();
    }
  }

  void SwayRuntime::readSocket() {
    char buffer[8192];
    while (true) {
      const ssize_t n = ::recv(m_eventSocketFd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (n > 0) {
        m_readBuffer.insert(m_readBuffer.end(), buffer, buffer + n);
        continue;
      }
      if (n == 0) {
        kLog.warn("sway IPC closed the connection");
        disconnect();
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      kLog.warn("failed to read from sway IPC: {}", std::strerror(errno));
      disconnect();
      return;
    }

    parseMessages();
  }

  void SwayRuntime::parseMessages() {
    while (m_eventSocketFd >= 0 && m_readBuffer.size() >= kIpcHeaderSize) {
      if (!std::equal(kIpcMagic.begin(), kIpcMagic.end(), m_readBuffer.begin())) {
        kLog.warn("invalid sway IPC frame header");
        disconnect();
        return;
      }

      std::uint32_t payloadLength = 0;
      std::uint32_t type = 0;
      std::memcpy(&payloadLength, m_readBuffer.data() + kIpcMagic.size(), sizeof(payloadLength));
      std::memcpy(&type, m_readBuffer.data() + kIpcMagic.size() + sizeof(payloadLength), sizeof(type));
      if (m_readBuffer.size() < kIpcHeaderSize + payloadLength) {
        break;
      }

      const auto payloadBegin = m_readBuffer.begin() + static_cast<std::ptrdiff_t>(kIpcHeaderSize);
      const auto payloadEnd = payloadBegin + static_cast<std::ptrdiff_t>(payloadLength);
      m_frame.assign(payloadBegin, payloadEnd);
      m_readBuffer.erase(m_readBuffer.begin(), payloadEnd);
      for (auto* handler : m_eventHandlers) {
        handler->handleMessage(type, m_frame);
      }
    }

    for (auto* handler : m_eventHandlers) {
      handler->handleBatchEnd();
    }
  }

  void SwayRuntime::ensureResolved() const {
    if (!m_resolved) {
      m_resolved = true;
      resolveSocketPath();
      resolveCommands();
    }
  }

  void SwayRuntime::resolveSocketPath() const {
    if (const char* swaySock = std::getenv("SWAYSOCK"); swaySock != nullptr && swaySock[0] != '\0') {
      m_socketPath = swaySock;
      return;
    }
    if (const char* i3Sock = std::getenv("I3SOCK"); i3Sock != nullptr && i3Sock[0] != '\0') {
      m_socketPath = i3Sock;
    }
  }

  void SwayRuntime::resolveCommands() const {
    if (process::commandExists("swaymsg")) {
      m_msgCommand = "swaymsg";
    } else if (process::commandExists("i3-msg")) {
      m_msgCommand = "i3-msg";
    }

    if (process::commandExists("scrollmsg")) {
      m_outputCommand = "scrollmsg";
    } else {
      m_outputCommand = m_msgCommand;
    }
  }

} // namespace compositors::sway
