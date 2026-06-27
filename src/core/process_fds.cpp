#include "core/process_fds.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <format>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

  [[nodiscard]] bool isFdName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
      return false;
    }
    for (const char* p = name; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9') {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string rlimitValue(rlim_t value) {
    if (value == RLIM_INFINITY) {
      return "infinity";
    }
    return std::to_string(static_cast<unsigned long long>(value));
  }

  [[nodiscard]] std::string rlimitSummary() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
      return "rlimit_nofile=unavailable";
    }
    return std::format("rlimit_nofile={}/{}", rlimitValue(limit.rlim_cur), rlimitValue(limit.rlim_max));
  }

  [[nodiscard]] bool startsWith(std::string_view value, std::string_view prefix) { return value.starts_with(prefix); }

  [[nodiscard]] std::string bucketTarget(std::string target) {
    if (startsWith(target, "socket:")) {
      return "socket";
    }
    if (startsWith(target, "pipe:")) {
      return "pipe";
    }
    if (startsWith(target, "memfd:") || startsWith(target, "/memfd:")) {
      return "memfd";
    }
    if (startsWith(target, "anon_inode:")) {
      return target;
    }
    if (target.size() > 120) {
      target.resize(117);
      target += "...";
    }
    return target;
  }

  [[nodiscard]] std::string readFdTarget(const char* fdName) {
    const std::string path = std::string("/proc/self/fd/") + fdName;
    std::vector<char> buffer(512);
    while (true) {
      const ssize_t n = readlink(path.c_str(), buffer.data(), buffer.size() - 1);
      if (n < 0) {
        return std::format("readlink failed: {}", std::strerror(errno));
      }
      if (static_cast<std::size_t>(n) < buffer.size() - 1) {
        buffer[static_cast<std::size_t>(n)] = '\0';
        return std::string(buffer.data());
      }
      buffer.resize(buffer.size() * 2U);
      if (buffer.size() > 8192U) {
        return "<fd target too long>";
      }
    }
  }

} // namespace

std::string ProcessFds::raiseOpenFileLimit() {
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return std::format("RLIMIT_NOFILE getrlimit failed: {}", std::strerror(errno));
  }

  const rlim_t previous = limit.rlim_cur;
  if (limit.rlim_cur >= limit.rlim_max) {
    return std::format("RLIMIT_NOFILE already at hard limit ({})", rlimitValue(previous));
  }

  limit.rlim_cur = limit.rlim_max;
  if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return std::format(
        "RLIMIT_NOFILE setrlimit to {} failed: {} (soft limit stays {})", rlimitValue(limit.rlim_max),
        std::strerror(errno), rlimitValue(previous)
    );
  }

  return std::format("RLIMIT_NOFILE soft limit raised {} -> {}", rlimitValue(previous), rlimitValue(limit.rlim_max));
}

std::string ProcessFds::describeOpenFileDescriptors(std::size_t maxTargets) {
  const std::string limit = rlimitSummary();
  DIR* dir = opendir("/proc/self/fd");
  if (dir == nullptr) {
    return std::format("open_fds=unavailable (opendir /proc/self/fd failed: {}), {}", std::strerror(errno), limit);
  }

  std::size_t count = 0;
  std::unordered_map<std::string, std::size_t> targetCounts;
  const int directoryFd = dirfd(dir);
  while (dirent* entry = readdir(dir)) {
    if (!isFdName(entry->d_name)) {
      continue;
    }
    char* end = nullptr;
    const long fdNumber = std::strtol(entry->d_name, &end, 10);
    if (end != nullptr && *end == '\0' && fdNumber == directoryFd) {
      continue;
    }
    ++count;
    ++targetCounts[bucketTarget(readFdTarget(entry->d_name))];
  }
  closedir(dir);

  std::vector<std::pair<std::string, std::size_t>> targets;
  targets.reserve(targetCounts.size());
  for (auto& [target, targetCount] : targetCounts) {
    targets.emplace_back(target, targetCount);
  }
  std::ranges::sort(targets, [](const auto& lhs, const auto& rhs) {
    if (lhs.second != rhs.second) {
      return lhs.second > rhs.second;
    }
    return lhs.first < rhs.first;
  });

  std::string out = std::format("open_fds={}, {}", count, limit);
  if (!targets.empty() && maxTargets > 0) {
    out += ", top_fd_targets=[";
    const std::size_t n = std::min(maxTargets, targets.size());
    for (std::size_t i = 0; i < n; ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += std::format("{}={}", targets[i].first, targets[i].second);
    }
    out += "]";
  }
  return out;
}
