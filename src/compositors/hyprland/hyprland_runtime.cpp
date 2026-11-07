#include "compositors/hyprland/hyprland_runtime.h"

#include "compositors/hyprland/hyprland_event_handler.h"
#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

namespace compositors::hyprland {

  namespace {

    constexpr Logger kLog("hyprland_runtime");
    constexpr auto kRequestTimeout = std::chrono::milliseconds(500);

    [[nodiscard]] bool waitForRequestSocket(int fd, short events, std::chrono::steady_clock::time_point deadline) {
      while (true) {
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          return false;
        }
        pollfd socket{.fd = fd, .events = events, .revents = 0};
        const int result = ::poll(&socket, 1, static_cast<int>(remaining.count()));
        if (result > 0) {
          if ((socket.revents & (POLLERR | POLLNVAL)) != 0) {
            return false;
          }
          return (socket.revents & events) != 0 || (events == POLLIN && (socket.revents & POLLHUP) != 0);
        }
        if (result == 0 || errno != EINTR) {
          return false;
        }
      }
    }

  } // namespace

  HyprlandRuntime::HyprlandRuntime() {
    ensureResolved();
    updateConfigProvider();
  }
  bool HyprlandRuntime::available() const { return m_eventSocketFd >= 0; }

  bool HyprlandRuntime::configIsLua() const {
    ensureResolved();
    if (!m_configIsLua.has_value()) {
      updateConfigProvider();
    }
    return m_configIsLua.value_or(false);
  }

  const std::string& HyprlandRuntime::requestSocketPath() const {
    ensureResolved();
    return m_socketPaths.request;
  }

  const std::string& HyprlandRuntime::eventSocketPath() const {
    ensureResolved();
    return m_socketPaths.event;
  }

  bool HyprlandRuntime::connectSocket() {
    if (m_socketPaths.event.empty()) {
      return false;
    }

    cleanup();

    m_eventSocketFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_eventSocketFd < 0) {
      kLog.warn("failed to create hyprland IPC socket: {}", std::strerror(errno));
      return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPaths.event.size() >= sizeof(addr.sun_path)) {
      kLog.warn("hyprland IPC socket path too long");
      cleanup();
      return false;
    }
    std::memcpy(addr.sun_path, m_socketPaths.event.c_str(), m_socketPaths.event.size() + 1);

    if (::connect(m_eventSocketFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      kLog.warn("failed to connect to hyprland IPC {}: {}", m_socketPaths.event, std::strerror(errno));
      cleanup();
      return false;
    }

    const int flags = ::fcntl(m_eventSocketFd, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(m_eventSocketFd, F_SETFL, flags | O_NONBLOCK);
    }

    kLog.info("connected to hyprland IPC at {}", m_socketPaths.event);
    return true;
  }

  void HyprlandRuntime::dispatchPoll(short revents) {
    if (m_eventSocketFd < 0) {
      return;
    }
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      kLog.warn("hyprland IPC disconnected");
      cleanup();
      for (const auto& eventHandler : m_eventHandlers) {
        eventHandler->notifyChanged();
      }
      return;
    }
    if ((revents & POLLIN) != 0) {
      readSocket();
    }
  }

  void HyprlandRuntime::cleanup() {
    if (m_eventSocketFd >= 0) {
      ::close(m_eventSocketFd);
      m_eventSocketFd = -1;
    }
    m_readBuffer.clear();
    for (const auto& eventHandler : m_eventHandlers) {
      eventHandler->notifyCleanup();
    }
  }

  std::optional<std::string> HyprlandRuntime::request(std::string_view command) const {
    ensureResolved();
    if (m_socketPaths.request.empty() || command.empty()) {
      return std::nullopt;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      kLog.debug("failed to create request socket: {}", std::strerror(errno));
      return std::nullopt;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPaths.request.size() >= sizeof(addr.sun_path)) {
      kLog.debug("request socket path too long");
      ::close(fd);
      return std::nullopt;
    }
    std::memcpy(addr.sun_path, m_socketPaths.request.c_str(), m_socketPaths.request.size() + 1);

    const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      if (errno != EINPROGRESS || !waitForRequestSocket(fd, POLLOUT, deadline)) {
        kLog.debug("failed to connect to request socket {}", m_socketPaths.request);
        ::close(fd);
        return std::nullopt;
      }
      int socketError = 0;
      socklen_t socketErrorSize = sizeof(socketError);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize) < 0 || socketError != 0) {
        kLog.debug("failed to connect to request socket {}", m_socketPaths.request);
        ::close(fd);
        return std::nullopt;
      }
    }

    std::size_t offset = 0;
    while (offset < command.size()) {
      if (!waitForRequestSocket(fd, POLLOUT, deadline)) {
        kLog.debug("request write timed out or disconnected");
        ::close(fd);
        return std::nullopt;
      }
      const ssize_t written = ::send(fd, command.data() + offset, command.size() - offset, MSG_NOSIGNAL);
      if (written <= 0) {
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
          continue;
        }
        kLog.debug("failed to write request: {}", written < 0 ? std::strerror(errno) : "short write");
        ::close(fd);
        return std::nullopt;
      }
      offset += static_cast<std::size_t>(written);
    }

    ::shutdown(fd, SHUT_WR);

    std::string response;
    char buffer[4096];
    while (true) {
      if (!waitForRequestSocket(fd, POLLIN, deadline)) {
        kLog.debug("request read timed out or disconnected");
        ::close(fd);
        return std::nullopt;
      }
      const ssize_t read = ::recv(fd, buffer, sizeof(buffer), 0);
      if (read > 0) {
        response.append(buffer, buffer + read);
        continue;
      }
      if (read == 0) {
        break;
      }
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      kLog.debug("failed to read response: {}", std::strerror(errno));
      ::close(fd);
      return std::nullopt;
    }

    ::close(fd);
    return response;
  }

  std::optional<nlohmann::json> HyprlandRuntime::requestJson(std::string_view command) const {
    const auto response = request(command);
    if (!response.has_value() || response->empty()) {
      return std::nullopt;
    }
    try {
      return nlohmann::json::parse(*response);
    } catch (const nlohmann::json::exception& e) {
      kLog.warn("failed to parse hyprland response for {}: {}", command, e.what());
      return std::nullopt;
    }
  }

  void HyprlandRuntime::refresh() {
    m_socketPaths = {};
    m_resolved = false;
    m_configIsLua.reset();
    resolveSocketPaths();
  }

  void HyprlandRuntime::ensureResolved() const {
    if (!m_resolved) {
      resolveSocketPaths();
    }
  }

  void HyprlandRuntime::resolveSocketPaths() const {
    m_resolved = true;
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature == nullptr || signature[0] == '\0') {
      return;
    }

    std::string hyprDir;
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir != nullptr && runtimeDir[0] != '\0') {
      hyprDir = std::string(runtimeDir) + "/hypr/" + signature;
    }

    std::error_code ec;
    if (hyprDir.empty() || !std::filesystem::is_directory(hyprDir, ec)) {
      hyprDir = std::string("/tmp/hypr/") + signature;
    }

    ec.clear();
    if (!std::filesystem::is_directory(hyprDir, ec)) {
      return;
    }

    m_socketPaths = IpcSocketPaths{
        .request = hyprDir + "/.socket.sock",
        .event = hyprDir + "/.socket2.sock",
    };
  }

  // Leaves the provider unknown when Hyprland does not answer, so the next configIsLua() call asks again.
  void HyprlandRuntime::updateConfigProvider() const {
    const auto response = request("j/status");
    if (!response.has_value()) {
      kLog.debug("hyprland config provider unknown: j/status got no response");
      return;
    }
    const auto json = nlohmann::json::parse(*response, nullptr, false);
    m_configIsLua = json.is_object() && json.value("configProvider", "") == "lua";
  }

  void HyprlandRuntime::registerEventHandler(HyprlandEventHandler* handler) { m_eventHandlers.push_back(handler); }

  void HyprlandRuntime::unregisterEventHandler(HyprlandEventHandler* handler) { std::erase(m_eventHandlers, handler); }

  void HyprlandRuntime::handleEvent(std::string_view line) {
    const auto split = line.find(">>");
    if (split == std::string_view::npos) {
      return;
    }

    const std::string_view event = line.substr(0, split);
    const std::string_view data = line.substr(split + 2);

    for (const auto& eventHandler : m_eventHandlers) {
      eventHandler->handleEvent(event, data);
    }
  }

  void HyprlandRuntime::readSocket() {
    if (m_eventSocketFd < 0) {
      kLog.warn("Event socket is not opened");
      return;
    }
    char buffer[4096];
    while (true) {
      const ssize_t n = ::recv(m_eventSocketFd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (n > 0) {
        m_readBuffer.insert(m_readBuffer.end(), buffer, buffer + n);
        continue;
      }
      if (n == 0) {
        cleanup();
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      kLog.warn("failed to read from hyprland IPC: {}", std::strerror(errno));
      cleanup();
      return;
    }

    parseMessages();
  }

  void HyprlandRuntime::parseMessages() {
    while (true) {
      auto it = std::ranges::find(m_readBuffer, '\n');
      if (it == m_readBuffer.end()) {
        return;
      }
      std::string line(m_readBuffer.begin(), it);
      m_readBuffer.erase(m_readBuffer.begin(), it + 1);
      if (!line.empty()) {
        handleEvent(line);
      }
    }
  }

} // namespace compositors::hyprland
