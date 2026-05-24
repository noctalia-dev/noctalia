#include "compositors/triad/triad_runtime.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace compositors::triad {

  struct TriadRuntime::IpcReply {
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

  namespace {

    [[nodiscard]] bool isSocketPath(const std::string& path) {
      struct stat st{};
      return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    }

    [[nodiscard]] nlohmann::json triadRequest(std::string_view requestName) {
      return nlohmann::json{
          {"triad", nlohmann::json{{"version", 1}, {"request", requestName}}},
      };
    }

  } // namespace

  bool TriadRuntime::available() const {
    ensureResolved();
    return !m_socketPath.empty();
  }

  const std::string& TriadRuntime::socketPath() const {
    ensureResolved();
    return m_socketPath;
  }

  std::optional<nlohmann::json> TriadRuntime::requestJson(std::string_view requestName) const {
    return requestPayload(triadRequest(requestName));
  }

  std::optional<nlohmann::json> TriadRuntime::requestPayload(const nlohmann::json& payload) const {
    auto serialized = payload.dump();
    serialized.push_back('\n');
    const auto reply = request(serialized);
    if (!reply.json.has_value() || !reply.json->is_object()) {
      return std::nullopt;
    }
    const auto okIt = reply.json->find("ok");
    if (okIt != reply.json->end() && okIt->is_boolean() && !okIt->get<bool>()) {
      return std::nullopt;
    }
    return reply.json;
  }

  bool TriadRuntime::requestAction(std::string_view action, nlohmann::json args) const {
    if (!args.is_object()) {
      args = nlohmann::json::object();
    }
    args["version"] = 1;
    args["request"] = "action";
    args["action"] = action;
    const auto response = requestPayload(nlohmann::json{{"triad", std::move(args)}});
    return response.has_value();
  }

  TriadRuntime::IpcReply TriadRuntime::request(std::string_view payload) const {
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
        if (response.find('\n') != std::string::npos) {
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

  void TriadRuntime::refresh() {
    m_socketPath.clear();
    m_resolved = false;
    resolveSocketPath();
  }

  void TriadRuntime::ensureResolved() const {
    if (!m_resolved) {
      resolveSocketPath();
    }
  }

  void TriadRuntime::resolveSocketPath() const {
    m_resolved = true;

    if (const char* socketPath = std::getenv("TRIAD_SOCKET"); socketPath != nullptr && socketPath[0] != '\0') {
      m_socketPath = socketPath;
      return;
    }

    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      return;
    }

    const std::string fallback = std::string(runtimeDir) + "/triad.sock";
    if (isSocketPath(fallback)) {
      m_socketPath = fallback;
    }
  }

} // namespace compositors::triad
