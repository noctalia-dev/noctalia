#include "core/deferred_call.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace {

  std::mutex& deferredMutex() {
    static std::mutex mutex;
    return mutex;
  }

  std::array<int, 2>& wakePipe() {
    static std::array<int, 2> fds{-1, -1};
    return fds;
  }

  void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }

  void setCloseOnExec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
      (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }

  bool ensureWakePipe() {
    auto& fds = wakePipe();
    if (fds[0] >= 0 && fds[1] >= 0) {
      return true;
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
      return false;
    }
    setNonBlocking(pipeFds[0]);
    setNonBlocking(pipeFds[1]);
    setCloseOnExec(pipeFds[0]);
    setCloseOnExec(pipeFds[1]);
    fds = {pipeFds[0], pipeFds[1]};
    return true;
  }

  void wakeMainLoop() {
    std::scoped_lock lock(deferredMutex());
    if (!ensureWakePipe()) {
      return;
    }
    constexpr std::uint8_t byte = 1;
    const int fd = wakePipe()[1];
    for (;;) {
      const ssize_t n = ::write(fd, &byte, sizeof(byte));
      if (n >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      if (errno != EINTR) {
        return;
      }
    }
  }

} // namespace

std::vector<std::function<void()>>& DeferredCall::queue() {
  static std::vector<std::function<void()>> q;
  return q;
}

void DeferredCall::callLater(std::function<void()> fn) {
  {
    std::scoped_lock lock(deferredMutex());
    queue().push_back(std::move(fn));
  }
  wakeMainLoop();
}

std::vector<std::function<void()>> DeferredCall::takePending() {
  std::scoped_lock lock(deferredMutex());
  auto pending = std::move(queue());
  queue().clear();
  return pending;
}

void DeferredCall::drainWakeFd() {
  const int fd = wakeFd();
  if (fd < 0) {
    return;
  }

  std::array<std::uint8_t, 64> bytes{};
  for (;;) {
    const ssize_t n = ::read(fd, bytes.data(), bytes.size());
    if (n > 0) {
      continue;
    }
    if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    if (errno != EINTR) {
      return;
    }
  }
}

int DeferredCall::wakeFd() {
  std::scoped_lock lock(deferredMutex());
  return ensureWakePipe() ? wakePipe()[0] : -1;
}
