#include "compositors/sway/sway_keyboard_backend.h"

#include "compositors/sway/sway_runtime.h"
#include "core/log.h"
#include "system/process.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("keyboard_sway");

  constexpr std::string_view kIpcMagic = "i3-ipc";
  constexpr std::size_t kIpcHeaderSize = kIpcMagic.size() + (2 * sizeof(std::uint32_t));
  constexpr std::uint32_t kIpcSubscribe = 2;
  constexpr std::uint32_t kIpcInputEvent = 0x80000015U;

  constexpr auto kCurrentLayoutCacheTtl = std::chrono::seconds(1);

  struct CurrentLayoutCache {
    std::optional<std::string> value;
    std::chrono::steady_clock::time_point fetchedAt;
    bool valid = false;
  };

  CurrentLayoutCache& currentLayoutCache() {
    static CurrentLayoutCache cache;
    return cache;
  }

  void invalidateCurrentLayoutCache() { currentLayoutCache() = CurrentLayoutCache{}; }

  [[nodiscard]] std::optional<std::string> runAndCapture(const std::vector<std::string>& args) {
    if (args.empty() || args.front().empty()) {
      return std::nullopt;
    }

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) {
      return std::nullopt;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(pipefd[0]);
      ::close(pipefd[1]);
      return std::nullopt;
    }

    if (pid == 0) {
      ::close(pipefd[0]);
      ::dup2(pipefd[1], STDOUT_FILENO);
      ::close(pipefd[1]);

      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr);
      ::execvp(argv[0], argv.data());
      ::_exit(127);
    }

    ::close(pipefd[1]);
    std::string output;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = ::read(pipefd[0], buffer, sizeof(buffer))) > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(pipefd[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      return std::nullopt;
    }

    return output;
  }

  // The `change` values of a sway input event that can move a keyboard's active layout.
  [[nodiscard]] bool isLayoutChange(const std::string& payload) {
    try {
      const auto json = nlohmann::json::parse(payload);
      if (!json.is_object()) {
        return false;
      }
      const std::string change = json.value("change", "");
      return change == "xkb_layout" || change == "xkb_keymap";
    } catch (const nlohmann::json::exception&) {
      return false;
    }
  }

} // namespace

SwayKeyboardBackend::SwayKeyboardBackend(compositors::sway::SwayRuntime& runtime) : m_runtime(runtime) {}

SwayKeyboardBackend::~SwayKeyboardBackend() { cleanup(); }

bool SwayKeyboardBackend::isAvailable() const noexcept { return m_runtime.hasMsgCommand(); }

bool SwayKeyboardBackend::cycleLayout() const {
  if (!isAvailable()) {
    return false;
  }
  const bool ok = process::runSync({m_runtime.msgCommand(), "input", "type:keyboard", "xkb_switch_layout", "next"});
  if (ok) {
    invalidateCurrentLayoutCache();
  }
  return ok;
}

std::optional<KeyboardLayoutState> SwayKeyboardBackend::layoutState() const {
  const auto current = currentLayoutName();
  if (!current.has_value()) {
    return std::nullopt;
  }
  return KeyboardLayoutState{{*current}, 0};
}

std::optional<std::string> SwayKeyboardBackend::currentLayoutName() const {
  if (!isAvailable()) {
    return std::nullopt;
  }

  const auto now = std::chrono::steady_clock::now();
  auto& cache = currentLayoutCache();
  if (cache.valid && now - cache.fetchedAt < kCurrentLayoutCacheTtl) {
    return cache.value;
  }

  auto finish = [&](std::optional<std::string> value) {
    cache.value = std::move(value);
    cache.fetchedAt = now;
    cache.valid = true;
    return cache.value;
  };

  const auto payload = runAndCapture({m_runtime.msgCommand(), "-t", "get_inputs", "--raw"});
  if (!payload.has_value() || payload->empty()) {
    return finish(std::nullopt);
  }

  try {
    const auto json = nlohmann::json::parse(*payload);
    if (!json.is_array()) {
      return finish(std::nullopt);
    }
    for (const auto& input : json) {
      if (!input.is_object()) {
        continue;
      }
      if (input.value("type", "") != "keyboard") {
        continue;
      }
      const std::string layout = input.value("xkb_active_layout_name", "");
      if (!layout.empty()) {
        return finish(layout);
      }
    }
  } catch (const nlohmann::json::exception&) {
    return finish(std::nullopt);
  }

  return finish(std::nullopt);
}

bool SwayKeyboardBackend::connectSocket() {
  const std::string& path = m_runtime.socketPath();
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

  m_socketFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (m_socketFd < 0) {
    kLog.warn("failed to create sway IPC socket: {}", std::strerror(errno));
    return false;
  }

  if (::connect(m_socketFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    kLog.warn("failed to connect to sway IPC {}: {}", path, std::strerror(errno));
    cleanup();
    return false;
  }

  return sendMessage(kIpcSubscribe, R"(["input"])");
}

void SwayKeyboardBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void SwayKeyboardBackend::cleanup() {
  if (m_socketFd >= 0) {
    ::close(m_socketFd);
    m_socketFd = -1;
  }
  m_readBuffer.clear();
}

void SwayKeyboardBackend::disconnect() {
  cleanup();
  invalidateCurrentLayoutCache();
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void SwayKeyboardBackend::dispatchPoll(short revents) {
  if (m_socketFd < 0) {
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

bool SwayKeyboardBackend::sendMessage(std::uint32_t type, const std::string& payload) {
  if (m_socketFd < 0) {
    return false;
  }

  const auto payloadLength = static_cast<std::uint32_t>(payload.size());
  std::vector<char> message(kIpcHeaderSize + payload.size());
  std::memcpy(message.data(), kIpcMagic.data(), kIpcMagic.size());
  std::memcpy(message.data() + kIpcMagic.size(), &payloadLength, sizeof(payloadLength));
  std::memcpy(message.data() + kIpcMagic.size() + sizeof(payloadLength), &type, sizeof(type));
  std::memcpy(message.data() + kIpcHeaderSize, payload.data(), payload.size());

  std::size_t offset = 0;
  while (offset < message.size()) {
    const ssize_t written = ::send(m_socketFd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      kLog.warn("failed to send sway IPC message: {}", std::strerror(errno));
      cleanup();
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

void SwayKeyboardBackend::readSocket() {
  char buffer[4096];
  while (true) {
    const ssize_t n = ::recv(m_socketFd, buffer, sizeof(buffer), MSG_DONTWAIT);
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

void SwayKeyboardBackend::parseMessages() {
  while (m_socketFd >= 0 && m_readBuffer.size() >= kIpcHeaderSize) {
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
      return;
    }

    const auto payloadBegin = m_readBuffer.begin() + static_cast<std::ptrdiff_t>(kIpcHeaderSize);
    const auto payloadEnd = payloadBegin + static_cast<std::ptrdiff_t>(payloadLength);
    const std::string payload(payloadBegin, payloadEnd);
    m_readBuffer.erase(m_readBuffer.begin(), payloadEnd);
    handleMessage(type, payload);
  }
}

void SwayKeyboardBackend::handleMessage(std::uint32_t type, const std::string& payload) {
  if (type != kIpcInputEvent || !isLayoutChange(payload)) {
    return;
  }
  // Drop the cached name so the callback's refresh reads the layout sway just switched to.
  invalidateCurrentLayoutCache();
  if (m_changeCallback) {
    m_changeCallback();
  }
}
