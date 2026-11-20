#include "compositors/mango/mango_runtime.h"

#include "compositors/unix_socket.h"

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

namespace compositors::mango {

  namespace {

    bool isSocketPath(const std::string& path) {
      struct stat st{};
      return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    }

  } // namespace

  bool MangoRuntime::available() const {
    ensureResolved();
    return !m_socketPath.empty();
  }

  const std::string& MangoRuntime::socketPath() const {
    ensureResolved();
    return m_socketPath;
  }

  std::optional<nlohmann::json> MangoRuntime::request(std::string_view command) const {
    ensureResolved();
    if (m_socketPath.empty() || command.empty()) {
      return std::nullopt;
    }

    const int fd = connectUnixSocket(m_socketPath, 0, kIpcRequestTimeout);
    if (fd < 0) {
      return std::nullopt;
    }

    std::string payload(command);
    payload.push_back('\n');
    if (!sendAll(fd, payload)) {
      ::close(fd);
      return std::nullopt;
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
      return std::nullopt;
    }

    ::close(fd);

    const std::size_t newline = response.find('\n');
    if (newline != std::string::npos) {
      response.resize(newline);
    }
    if (response.empty()) {
      return std::nullopt;
    }

    try {
      return std::make_optional(nlohmann::json::parse(response));
    } catch (const nlohmann::json::exception&) {
      return std::nullopt;
    }
  }

  bool MangoRuntime::dispatch(std::string_view command) const {
    std::string requestText = "dispatch ";
    requestText += command;
    const auto response = request(requestText);
    if (!response.has_value() || !response->is_object()) {
      return false;
    }
    const auto successIt = response->find("success");
    return successIt != response->end() && successIt->is_boolean() && successIt->get<bool>();
  }

  void MangoRuntime::refresh() {
    m_socketPath.clear();
    m_resolved = false;
    resolveSocketPath();
  }

  void MangoRuntime::ensureResolved() const {
    if (!m_resolved) {
      resolveSocketPath();
    }
  }

  void MangoRuntime::resolveSocketPath() const {
    m_resolved = true;
    if (const char* signature = std::getenv("MANGO_INSTANCE_SIGNATURE");
        signature != nullptr && signature[0] != '\0' && isSocketPath(signature)) {
      m_socketPath = signature;
    }
  }

} // namespace compositors::mango
