#include "app/service_notify.h"

#include "core/log.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("notify");
} // namespace

void ServiceNotifier::takeSocketFromEnvironment() {
  if (const char* path = std::getenv("NOTIFY_SOCKET"); path != nullptr) {
    m_socketPath = path;
  }
  ::unsetenv("NOTIFY_SOCKET");
}

void ServiceNotifier::notifyReady() {
  if (m_socketPath.empty()) {
    return;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (m_socketPath.size() >= sizeof(addr.sun_path) || (m_socketPath[0] != '/' && m_socketPath[0] != '@')) {
    kLog.warn("ignoring invalid NOTIFY_SOCKET {}", m_socketPath);
    return;
  }
  std::memcpy(addr.sun_path, m_socketPath.data(), m_socketPath.size());
  if (m_socketPath[0] == '@') {
    addr.sun_path[0] = '\0';
  }
  const auto addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + m_socketPath.size());

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    kLog.warn("failed to create notify socket: {}", std::strerror(errno));
    return;
  }
  constexpr std::string_view kReady = "READY=1";
  if (::sendto(fd, kReady.data(), kReady.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&addr), addrLen) < 0) {
    kLog.warn("failed to send READY=1 to {}: {}", m_socketPath, std::strerror(errno));
  }
  ::close(fd);
}
