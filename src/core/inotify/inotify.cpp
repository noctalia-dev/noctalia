#include "core/inotify/inotify.h"

#include "core/log.h"

#include <optional>
#include <sys/inotify.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("inotify");
}

Inotify::Inotify() {
  m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (m_inotifyFd < 0)
    kLog.warn("inotify_init1 failed");
}

Inotify::~Inotify() {
  if (m_inotifyFd < 0)
    return;
  for (auto& wd : m_watchDescriptors)
    inotify_rm_watch(m_inotifyFd, wd);
  ::close(m_inotifyFd);
}

std::optional<int> Inotify::watch(const std::filesystem::path& path, Inotify::WatchMask mask) noexcept {
  if (m_inotifyFd < 0)
    return std::nullopt;

  auto dir = path.string();

  int wd = inotify_add_watch(m_inotifyFd, dir.c_str(), mask);
  if (wd < 0) {
    kLog.warn("failed to watch directory '{}'", dir);
    return std::nullopt;
  }
  m_watchDescriptors.insert(wd);

  return wd;
}

void Inotify::unwatch(int wd) {
  if (m_inotifyFd >= 0)
    inotify_rm_watch(m_inotifyFd, wd);
  m_watchDescriptors.erase(wd);
}

void Inotify::drain(std::optional<Callback> global_callback) noexcept {
  if (m_inotifyFd < 0)
    return;

  alignas(inotify_event) char buf[4096];

  while (true) {
    const ssize_t n = ::read(m_inotifyFd, buf, sizeof(buf));
    if (n <= 0)
      break;

    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(n)) {
      const auto* event = reinterpret_cast<inotify_event*>(buf + offset);

      if ((event->mask & IN_IGNORED) != 0) {
        // watch was removed somehow => remove watch id
        m_watchDescriptors.erase(event->wd);
      } else if (
          global_callback.has_value() && ((event->mask & IN_Q_OVERFLOW) != 0 || m_watchDescriptors.contains(event->wd))
      ) {
        (*global_callback)(event);
      }

      offset += sizeof(inotify_event) + event->len;
    }
  }
}
