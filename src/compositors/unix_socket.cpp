#include "compositors/unix_socket.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace compositors {

  int connectUnixSocket(const std::string& path, int socketFlags, std::chrono::milliseconds ioTimeout) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
      errno = path.empty() ? ENOENT : ENAMETOOLONG;
      return -1;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | socketFlags, 0);
    if (fd < 0) {
      return -1;
    }

    bool ok = true;
    if (ioTimeout.count() > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(ioTimeout);
      const timeval tv{
          .tv_sec = static_cast<time_t>(seconds.count()),
          .tv_usec = static_cast<suseconds_t>(std::chrono::microseconds(ioTimeout - seconds).count()),
      };
      ok = ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0
          && ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    }
    // Linux AF_UNIX connect completes immediately or fails (EAGAIN when nonblocking and the backlog is full).
    if (!ok || ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
      const int savedErrno = errno;
      ::close(fd);
      errno = savedErrno;
      return -1;
    }
    return fd;
  }

  bool sendAll(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const ssize_t written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(written);
    }
    return true;
  }

  bool waitForSocket(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    while (true) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
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

} // namespace compositors
