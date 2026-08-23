#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <sys/inotify.h>

class Inotify {
public:
  using WatchMask = std::uint32_t;
  using Callback = std::function<void(const inotify_event*)>;

  Inotify();
  ~Inotify();

  Inotify(const Inotify&) = delete;
  Inotify& operator=(const Inotify&) = delete;

  struct WatchEntry {
    int wd;
    std::filesystem::path path;
    WatchMask mask;
  };

  std::optional<int> watch(const std::filesystem::path& path, WatchMask mask) noexcept;

  [[nodiscard]] int fd() const noexcept { return m_inotifyFd; }

  void unwatch(int wd);

  void drain(std::optional<Callback> callback = std::nullopt) noexcept;

private:
  int m_inotifyFd = -1;
  std::set<int> m_watchDescriptors;
};
