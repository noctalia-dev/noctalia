#include "compositors/umbriel/umbriel_runtime.h"

#include "compositors/umbriel/umbriel_event_handler.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace compositors::umbriel {

  namespace {

    constexpr Logger kLog("umbriel_runtime");
    constexpr auto kReconnectInitial = std::chrono::seconds(2);
    constexpr auto kReconnectMax = std::chrono::seconds(30);
    constexpr std::size_t kReadBufferMaxBytes = 1024U * 1024U;
    constexpr std::string_view kEventStreamRequest =
        R"({"cmd":"subscribe","events":["windows","overview","keyboard_layout"]})";

    [[nodiscard]] bool writeAll(int fd, std::string_view data) {
      std::size_t offset = 0;
      while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written <= 0) {
          if (written < 0 && errno == EINTR) {
            continue;
          }
          return false;
        }
        offset += static_cast<std::size_t>(written);
      }
      return true;
    }

    [[nodiscard]] bool isSocketPath(const std::string& path) {
      struct stat st{};
      return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    }

  } // namespace

  struct UmbrielRuntime::IpcReply {
    enum class Status {
      Unavailable,
      WriteFailed,
      ReadFailed,
      NoResponse,
      InvalidJson,
      Replied,
    };

    Status status = Status::Unavailable;
    std::optional<nlohmann::json> json;
  };

  bool UmbrielRuntime::available() const {
    ensureResolved();
    return !m_socketPath.empty();
  }

  std::optional<nlohmann::json>
  UmbrielRuntime::requestCommand(std::string_view cmd, std::optional<std::string_view> arg) const {
    nlohmann::json request = nlohmann::json::object();
    request["cmd"] = cmd;
    if (arg.has_value()) {
      request["arg"] = *arg;
    }
    auto payload = request.dump();
    payload.push_back('\n');

    const auto reply = this->request(payload);
    if (!reply.json.has_value() || !reply.json->is_object()) {
      return std::nullopt;
    }
    if (reply.json->contains("err") || !reply.json->contains("ok")) {
      return std::nullopt;
    }
    return reply.json;
  }

  bool UmbrielRuntime::requestAction(std::string_view action) const {
    return requestCommand("msg", action).has_value();
  }

  UmbrielRuntime::IpcReply UmbrielRuntime::request(std::string_view payload) const {
    ensureResolved();
    if (m_socketPath.empty() || payload.empty()) {
      return {IpcReply::Status::Unavailable, std::nullopt};
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return {IpcReply::Status::Unavailable, std::nullopt};
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
      ::close(fd);
      return {IpcReply::Status::Unavailable, std::nullopt};
    }
    std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      return {IpcReply::Status::Unavailable, std::nullopt};
    }

    std::size_t offset = 0;
    while (offset < payload.size()) {
      const ssize_t written = ::write(fd, payload.data() + offset, payload.size() - offset);
      if (written <= 0) {
        if (written < 0 && errno == EINTR) {
          continue;
        }
        ::close(fd);
        return {IpcReply::Status::WriteFailed, std::nullopt};
      }
      offset += static_cast<std::size_t>(written);
    }

    std::string response;
    char buffer[4096];
    while (true) {
      const ssize_t count = ::read(fd, buffer, sizeof(buffer));
      if (count > 0) {
        response.append(buffer, static_cast<std::size_t>(count));
        if (response.contains('\n')) {
          break;
        }
        continue;
      }
      if (count == 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return {IpcReply::Status::ReadFailed, std::nullopt};
    }

    ::close(fd);

    const std::size_t newline = response.find('\n');
    if (newline != std::string::npos) {
      response.resize(newline);
    }
    if (response.empty()) {
      return {IpcReply::Status::NoResponse, std::nullopt};
    }

    try {
      return {IpcReply::Status::Replied, nlohmann::json::parse(response)};
    } catch (const nlohmann::json::exception&) {
      return {IpcReply::Status::InvalidJson, std::nullopt};
    }
  }

  void UmbrielRuntime::refresh() {
    m_socketPath.clear();
    m_resolved = false;
    resolveSocketPath();
  }

  UmbrielRuntime::~UmbrielRuntime() { closeSocket(false); }

  void UmbrielRuntime::cleanup() {
    closeSocket(false);
    m_readBuffer.clear();
    m_reconnectBackoff = kReconnectInitial;
    notifyStreamReset();
  }

  void UmbrielRuntime::registerEventHandler(UmbrielEventHandler* handler) {
    if (handler == nullptr || std::ranges::contains(m_eventHandlers, handler)) {
      return;
    }
    m_eventHandlers.push_back(handler);
    if (available()) {
      connectIfNeeded();
    }
  }

  void UmbrielRuntime::unregisterEventHandler(UmbrielEventHandler* handler) { std::erase(m_eventHandlers, handler); }

  void UmbrielRuntime::dispatchEvent(std::string_view event, const nlohmann::json& data) const {
    for (auto* handler : m_eventHandlers) {
      if (handler != nullptr) {
        handler->handleEvent(event, data);
      }
    }
  }

  void UmbrielRuntime::notifyStreamReset() const {
    for (auto* handler : m_eventHandlers) {
      if (handler != nullptr) {
        handler->handleStreamReset();
      }
    }
  }

  int UmbrielRuntime::pollTimeoutMs() const noexcept {
    if (m_eventSocketFd >= 0 || !available()) {
      return -1;
    }
    if (m_nextReconnectAt.time_since_epoch().count() == 0) {
      return 0;
    }
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(m_nextReconnectAt - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::max<std::int64_t>(0, remaining));
  }

  void UmbrielRuntime::dispatchPoll(short revents) {
    if (!available()) {
      return;
    }
    if (m_eventSocketFd < 0) {
      connectIfNeeded();
      return;
    }
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      closeSocket(true);
      return;
    }
    if ((revents & POLLIN) != 0) {
      readSocket();
    }
  }

  void UmbrielRuntime::connectIfNeeded() {
    ensureResolved();
    if (m_eventSocketFd >= 0 || m_socketPath.empty()) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_nextReconnectAt.time_since_epoch().count() != 0 && now < m_nextReconnectAt) {
      return;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      scheduleReconnect();
      return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
      kLog.warn("umbriel socket path too long");
      ::close(fd);
      scheduleReconnect();
      return;
    }
    std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      scheduleReconnect();
      return;
    }

    std::string subscribe(kEventStreamRequest);
    subscribe.push_back('\n');
    if (!writeAll(fd, subscribe)) {
      ::close(fd);
      scheduleReconnect();
      return;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    m_eventSocketFd = fd;
    m_nextReconnectAt = {};
    m_reconnectBackoff = kReconnectInitial;
    m_readBuffer.clear();
    kLog.debug("connected to umbriel event stream");
  }

  void UmbrielRuntime::closeSocket(bool scheduleReconnectFlag) {
    if (m_eventSocketFd >= 0) {
      ::close(m_eventSocketFd);
      m_eventSocketFd = -1;
    }

    if (scheduleReconnectFlag) {
      scheduleReconnect();
    } else {
      m_nextReconnectAt = {};
    }
  }

  void UmbrielRuntime::scheduleReconnect() {
    const auto now = std::chrono::steady_clock::now();
    m_nextReconnectAt = now + m_reconnectBackoff;
    const auto doubled = m_reconnectBackoff * 2;
    m_reconnectBackoff = std::min(doubled, kReconnectMax);
  }

  void UmbrielRuntime::readSocket() {
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t readBytes = ::read(m_eventSocketFd, buffer.data(), buffer.size());
      if (readBytes > 0) {
        m_readBuffer.insert(m_readBuffer.end(), buffer.begin(), buffer.begin() + readBytes);
        if (m_readBuffer.size() > kReadBufferMaxBytes) {
          kLog.warn("umbriel event stream read buffer exceeded {} bytes; reconnecting", kReadBufferMaxBytes);
          closeSocket(true);
          m_readBuffer.clear();
          return;
        }
        continue;
      }

      if (readBytes == 0) {
        closeSocket(true);
        return;
      }

      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      closeSocket(true);
      return;
    }

    parseMessages();
  }

  void UmbrielRuntime::parseMessages() {
    auto lineStart = m_readBuffer.begin();
    for (auto it = m_readBuffer.begin(); it != m_readBuffer.end(); ++it) {
      if (*it != '\n') {
        continue;
      }

      std::string line(lineStart, it);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (!line.empty() && !handleMessage(line)) {
        m_readBuffer.clear();
        return;
      }

      lineStart = std::next(it);
    }

    if (lineStart != m_readBuffer.begin()) {
      m_readBuffer.erase(m_readBuffer.begin(), lineStart);
    }
  }

  bool UmbrielRuntime::handleMessage(std::string_view line) {
    nlohmann::json json;
    try {
      json = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception& e) {
      kLog.warn("failed to parse umbriel event stream message: {}", e.what());
      return true;
    }

    if (!json.is_object()) {
      return true;
    }

    if (json.contains("err")) {
      kLog.warn("umbriel event stream returned an error, reconnecting");
      closeSocket(true);
      return false;
    }

    const auto eventIt = json.find("event");
    if (eventIt == json.end() || !eventIt->is_string()) {
      return true;
    }

    dispatchEvent(eventIt->get_ref<const std::string&>(), json.value("data", nlohmann::json()));
    return true;
  }

  void UmbrielRuntime::ensureResolved() const {
    if (!m_resolved) {
      resolveSocketPath();
    }
  }

  void UmbrielRuntime::resolveSocketPath() const {
    m_resolved = true;

    if (const char* socketPath = std::getenv("UMBRIEL_SOCKET"); socketPath != nullptr && socketPath[0] != '\0') {
      m_socketPath = socketPath;
      return;
    }

    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0' || waylandDisplay == nullptr || waylandDisplay[0] == '\0') {
      return;
    }

    const std::string fallback = std::string(runtimeDir) + "/umbriel-" + waylandDisplay + ".sock";
    if (isSocketPath(fallback)) {
      m_socketPath = fallback;
    }
  }

} // namespace compositors::umbriel
