#include "password/systemd_password_agent.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("systemd_password_agent");

  constexpr Inotify::WatchMask mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM;

  void sendAskPasswordReply(const fs::path& socket, const std::string& payload) {
    if (socket.empty()) {
      kLog.warn("query has no socket, reply dropped");
      return;
    }
    const std::string socketStr = socket.string();
    const std::size_t nameLen = socketStr.size();
    if (nameLen == 0 || nameLen >= sizeof(sockaddr_un::sun_path)) {
      kLog.warn("bad socket path '{}'", socketStr);
      return;
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      kLog.warn("socket failed: {}", std::strerror(errno));
      return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socketStr.data(), nameLen);
    const auto addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + nameLen + 1);
    if (::sendto(fd, payload.data(), payload.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&addr), addrLen) < 0) {
      kLog.warn("send to '{}' failed: {}", socketStr, std::strerror(errno));
    }
    ::close(fd);
  }

  bool canAnswerQuery(const SystemdPasswordQuery& query) {
    if (query.socket.empty()) {
      return false;
    }
    if (::geteuid() == 0) {
      return true;
    }
    return ::access(query.socket.c_str(), W_OK) == 0;
  }

  std::uint64_t monotonicUsecs() {
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
      return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
  }

  bool pidAlive(pid_t pid) {
    if (pid == 0) {
      return true;
    } else if (pid < 0) {
      return false;
    } else if (::kill(pid, 0) == 0) {
      return true;
    }
    // EPERM means the process exists but we may not signal it; only ESRCH is dead.
    return errno != ESRCH;
  }

  bool queryLive(const SystemdPasswordQuery& query) {
    if (query.not_after != 0 && monotonicUsecs() >= query.not_after) {
      return false;
    }
    return pidAlive(query.pid);
  }

} // namespace

bool parseBool(std::string_view value) { return value == "1"; }

std::optional<uint64_t> parseUInt64(std::string_view value) {
  uint64_t res;
  auto [_, ec] = std::from_chars(value.data(), value.data() + value.size(), res);
  if (ec == std::errc()) {
    // Parsing successful
    return res;
  } else {
    return std::nullopt;
  }
}

SystemdPasswordAgent::~SystemdPasswordAgent() = default;

void SystemdPasswordAgent::setStateCallback(StateCallback callback) { m_stateCallback = std::move(callback); }

void SystemdPasswordAgent::emitStateCallback() {
  if (m_stateCallback != nullptr) {
    m_stateCallback();
  }
}

void SystemdPasswordAgent::reconcilePending() {
  if (m_pending != nullptr) {
    const auto it =
        std::ranges::find_if(m_queries, [&key = m_pendingKey](const auto& query) { return query.first == key; });
    if (it != m_queries.end()) {
      m_pending = &it->second;
      return;
    }
  }
  if (m_queries.empty()) {
    m_pending = nullptr;
    m_pendingKey.clear();
    return;
  }
  const auto it = m_queries.begin();
  m_pendingKey = it->first;
  m_pending = &it->second;
}

std::uint64_t SystemdPasswordAgent::eraseQuery(const fs::path& key) {
  return std::erase_if(m_queries, [&key](const auto& query) { return query.first == key; });
}

bool SystemdPasswordAgent::addQuery(const fs::path& path, SystemdPasswordQuery&& query) {
  auto it = std::ranges::find_if(m_queries, [&path](const auto& item) { return item.first == path; });

  if (it == m_queries.end()) {
    m_queries.emplace_back(path, query);
    return true;
  }
  return false;
}

bool SystemdPasswordAgent::hasPendingRequest() const noexcept { return m_pending != nullptr; }

SystemdPasswordQuery SystemdPasswordAgent::pendingRequest() const {
  if (m_pending != nullptr) {
    return *m_pending;
  }
  return SystemdPasswordQuery{};
}

void SystemdPasswordAgent::submitResponse(const std::string& response) {
  if (m_pending == nullptr) {
    return;
  }
  const fs::path socket = m_pending->socket;
  eraseQuery(m_pendingKey);
  reconcilePending();
  emitStateCallback();
  sendAskPasswordReply(socket, "+" + response);
}

void SystemdPasswordAgent::cancelRequest() {
  if (m_pending == nullptr) {
    return;
  }
  const fs::path socket = m_pending->socket;
  eraseQuery(m_pendingKey);
  reconcilePending();
  emitStateCallback();
  sendAskPasswordReply(socket, "-");
}

std::optional<SystemdPasswordQuery> SystemdPasswordAgent::parsePasswordQuery(const fs::path& filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    kLog.debug("failed to open systemd password query file '{}'", filepath.string());
    return std::nullopt;
  }

  std::optional<SystemdPasswordQuery> res;
  res.emplace();
  auto& query = res.value();
  query.ask_file = filepath;

  bool inAsk = false;

  std::string line;
  while (std::getline(file, line)) {
    // Strip trailing whitespace/carriage return
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }

    if (line.empty() || line[0] == '#') {
      continue;
    }

    if (line[0] == '[') {
      if (line == "[Ask]") {
        inAsk = true;
      } else {
        inAsk = false;
      }
      continue;
    }

    if (!inAsk) {
      continue;
    }

    auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }

    std::string_view key(line.data(), eq);
    std::string_view value(line.data() + eq + 1, line.size() - eq - 1);

    if (key == "Message") {
      query.message = std::string(value);
    } else if (key == "Icon") {
      query.icon = fs::path(value);
    } else if (key == "Socket") {
      query.socket = fs::path(value).lexically_normal();
    } else if (key == "PID") {
      auto parsed = parseUInt64(value);
      if (parsed.has_value()) {
        constexpr auto kPidMax = static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max());
        if (*parsed == 0 || *parsed > kPidMax) {
          kLog.debug("ignoring out-of-range PID: {}", value);
        } else {
          query.pid = static_cast<pid_t>(*parsed);
        }
      } else {
        kLog.debug("failed to parse PID: {}", value);
      }
    } else if (key == "Echo") {
      query.echo = parseBool(value);
    } else if (key == "Silent") {
      query.silent = parseBool(value);
    } else if (key == "AcceptCached") {
      query.accept_cached = parseBool(value);
    } else if (key == "NotAfter") {
      auto not_after = parseUInt64(value);
      if (not_after.has_value()) {
        query.not_after = *not_after;
      } else {
        kLog.debug("failed to parse NotAfter: {}", value);
      }
    }
  }

  if (!queryLive(query)) {
    return std::nullopt;
  }
  return res;
}

void SystemdPasswordAgent::start() {
  for (const auto& dir : m_paths) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
      fs::create_directories(dir, ec);
    }
    auto wd = m_inotify.watch(dir, mask);
    if (!wd.has_value()) {
      kLog.info("systemd password agent inactive for '{}'", dir.string());
      continue;
    }
    m_watches.emplace_back(*wd, dir);
    ec.clear();
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file(ec) || ec) {
        continue;
      }
      if (entry.path().filename().string().starts_with("ask.")) {
        if (auto query = parsePasswordQuery(entry.path())) {
          if (canAnswerQuery(*query)) {
            addQuery(entry.path(), std::move(*query));
          } else {
            kLog.info("not querying '{}', lacking privileges to reply", entry.path().string());
          }
        }
      }
    }
    if (ec) {
      kLog.debug("systemd password agent scan of '{}' failed: {}", dir.string(), ec.message());
    }
  }
  reconcilePending();
  emitStateCallback();
}

void SystemdPasswordAgent::processEvents() {
  const bool hadPending = m_pending != nullptr;
  const fs::path previousKey = m_pendingKey;
  m_inotify.drain([this](const inotify_event* event) {
    if (event == nullptr || event->len == 0) {
      return;
    }
    const auto watchIt =
        std::ranges::find_if(m_watches, [wd = event->wd](const auto& watch) { return watch.first == wd; });
    if (watchIt == m_watches.end()) {
      return;
    }
    const std::string_view name(event->name, strnlen(event->name, event->len));
    if (name.empty() || !name.starts_with("ask.")) {
      return;
    }
    const fs::path key = watchIt->second / name;
    if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
      eraseQuery(key);
      return;
    }
    if ((event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) == 0) {
      return;
    }
    if (auto query = parsePasswordQuery(key)) {
      if (canAnswerQuery(*query) && fs::is_regular_file(key)) {
        addQuery(key, std::move(*query));
      } else {
        eraseQuery(key);
        kLog.info("not querying '{}', lacking privileges to reply", key.string());
      }
    } else {
      eraseQuery(key);
    }
  });

  reconcilePending();
  const bool hasPending = m_pending != nullptr;
  if (hadPending != hasPending || (hasPending && m_pendingKey != previousKey)) {
    emitStateCallback();
  }
}

int SystemdPasswordAgent::nextExpiryTimeoutMs() const {
  std::uint64_t now = monotonicUsecs();
  // now==0 means CLOCK_MONOTONIC unavailable; fall back to no timed wake.
  if (now == 0) {
    return -1;
  }
  bool found = false;
  std::uint64_t minMs = 0;
  for (const auto& [_, query] : m_queries) {
    if (query.not_after == 0) {
      continue;
    }
    if (now >= query.not_after) {
      return 0;
    }
    const std::uint64_t ms = (query.not_after - now + 999ULL) / 1000ULL;
    if (!found || ms < minMs) {
      minMs = ms;
      found = true;
    }
  }
  if (!found) {
    return -1;
  }
  constexpr auto kMaxInt = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
  return static_cast<int>(std::min(minMs, kMaxInt));
}

bool SystemdPasswordAgent::sweepExpired() {
  const bool hadPending = m_pending != nullptr;
  const fs::path previousKey = m_pendingKey;
  std::erase_if(m_queries, [](const auto& entry) {
    std::error_code ec;
    if (!fs::is_regular_file(entry.first, ec) || ec) {
      return true;
    }
    return !queryLive(entry.second);
  });
  reconcilePending();
  const bool hasPending = m_pending != nullptr;
  const bool changed = hadPending != hasPending || (hasPending && m_pendingKey != previousKey);
  if (changed) {
    emitStateCallback();
  }
  return changed;
}
