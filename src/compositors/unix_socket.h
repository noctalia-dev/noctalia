#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace compositors {

  // Upper bound for one synchronous request/reply exchange with a compositor IPC socket.
  inline constexpr auto kIpcRequestTimeout = std::chrono::milliseconds(500);

  // Creates an AF_UNIX stream socket (SOCK_CLOEXEC plus socketFlags) and connects it to path. A nonzero ioTimeout
  // sets SO_SNDTIMEO and SO_RCVTIMEO before connecting. Returns the fd, or -1 with errno set.
  [[nodiscard]] int connectUnixSocket(
      const std::string& path, int socketFlags = 0, std::chrono::milliseconds ioTimeout = std::chrono::milliseconds{0}
  );

  // Sends all of data with MSG_NOSIGNAL, retrying on EINTR. Any other failure, including EAGAIN, returns false.
  [[nodiscard]] bool sendAll(int fd, std::string_view data);

} // namespace compositors
