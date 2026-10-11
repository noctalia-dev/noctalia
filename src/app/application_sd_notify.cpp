/* SPDX-License-Identifier: MIT-0 */

/* Code extracted from `man 3 sd_notify` and modified for use in this project. */

/* Implement the systemd notify protocol without external dependencies.
 * Supports both readiness notification on startup and on reloading,
 * according to the protocol defined at:
 * https://www.freedesktop.org/software/systemd/man/latest/sd_notify.html
 * This protocol is guaranteed to be stable as per:
 * https://systemd.io/PORTABILITY_AND_STABILITY/ */

#include "application.h"
#include "core/log.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("app");

  int notify(const char* message) {
    union sockaddr_union {
      struct sockaddr sa;
      struct sockaddr_un sun;
    } socket_addr;
    memset(&socket_addr, 0, sizeof(socket_addr));
    socket_addr.sun.sun_family = AF_UNIX;

    /* Verify the argument first */
    if (!message)
      return -EINVAL;

    size_t message_length = strlen(message);
    if (message_length == 0)
      return -EINVAL;

    /* If the variable is not set, the protocol is a noop */
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (!socket_path)
      return 0; /* Not set? Nothing to do */

    /* Only AF_UNIX is supported, with path or abstract sockets */
    if (socket_path[0] != '/' && socket_path[0] != '@')
      return -EAFNOSUPPORT;

    size_t path_length = strlen(socket_path);
    /* Ensure there is room for NUL byte */
    if (path_length >= sizeof(socket_addr.sun.sun_path))
      return -E2BIG;

    memcpy(socket_addr.sun.sun_path, socket_path, path_length);

    /* Support for abstract socket */
    if (socket_addr.sun.sun_path[0] == '@')
      socket_addr.sun.sun_path[0] = 0;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return -errno;

    if (connect(fd, &socket_addr.sa, offsetof(struct sockaddr_un, sun_path) + static_cast<socklen_t>(path_length))
        != 0) {
      close(fd);
      return -errno;
    }

    ssize_t written = write(fd, message, message_length);
    if (written != static_cast<ssize_t>(message_length)) {
      close(fd);
      return written < 0 ? -errno : -EPROTO;
    }

    close(fd);
    unsetenv("NOTIFY_SOCKET");
    return 1; /* Notified! */
  }

} // namespace

bool Application::systemdNotifyReady(void) {
  int ret = notify("READY=1");
  if (ret < 0) {
    kLog.error("systemd notification failed with return code {}", ret);
    return false;
  } else if (ret == 0) {
    kLog.info("NOTIFY_SOCKET not found. Skipping systemd notification.");
    return true;
  } else {
    kLog.info("systemd notification sent successfully.");
    return true;
  }
}
