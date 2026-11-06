#include "app/single_instance_lock.h"

#include "core/log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("instance");
} // namespace

SingleInstanceLock::~SingleInstanceLock() { release(); }

bool SingleInstanceLock::tryAcquire() {
  m_path = resolveLockPath();

  const int fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    // The lock file is unusable (e.g. read-only runtime dir). Degrade to running
    // unguarded rather than refusing to start — losing the single-instance
    // guarantee is better than a shell that won't launch.
    kLog.warn("could not open lock file {}: {}; running without single-instance guard", m_path, std::strerror(errno));
    return true;
  }

  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) {
      // Another live instance holds the lock.
      ::close(fd);
      return false;
    }
    // Unexpected flock failure — degrade to unguarded, same rationale as above.
    kLog.warn("flock failed on {}: {}; running without single-instance guard", m_path, std::strerror(errno));
    ::close(fd);
    return true;
  }

  m_fd = fd;
  return true;
}

void SingleInstanceLock::release() noexcept {
  if (m_fd >= 0) {
    // Closing the fd releases the flock. The lock file itself is intentionally
    // left in place: unlinking it would let a racing instance flock a different
    // inode than a third instance later creates, defeating the lock.
    ::close(m_fd);
    m_fd = -1;
  }
}

std::string SingleInstanceLock::resolveLockPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] == '\0') {
    runtime = "/tmp";
  }
  const char* display = std::getenv("WAYLAND_DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    display = "wayland-0";
  }
  return std::string(runtime) + "/noctalia-" + display + ".lock";
}
