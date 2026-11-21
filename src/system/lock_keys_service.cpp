#include "system/lock_keys_service.h"

#include "wayland/wayland_connection.h"

#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  namespace fs = std::filesystem;

  constexpr auto kRefreshInterval = std::chrono::milliseconds(200);
  constexpr int kSysfsFailureRescanThreshold = 3;

  // sysfs regenerates the attribute on every read from offset 0.
  bool readBrightness(int fd, bool& on) {
    char buf[16];
    const ssize_t n = ::pread(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      return false;
    }

    int brightness = 0;
    const auto [ptr, ec] = std::from_chars(buf, buf + n, brightness);
    if (ec != std::errc{} || ptr == buf) {
      return false;
    }

    on = brightness != 0;
    return true;
  }

} // namespace

LockKeysService::LockKeysService(WaylandConnection& wayland) : m_wayland(wayland) {}

LockKeysService::~LockKeysService() { closeSysfsLeds(); }

LockKeysService::LockKeysState LockKeysService::state() const noexcept { return m_state; }

void LockKeysService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

int LockKeysService::pollTimeoutMs() const {
  if (m_nextRefreshAt == std::chrono::steady_clock::time_point{}) {
    return 0;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_nextRefreshAt <= now) {
    return 0;
  }

  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(m_nextRefreshAt - now).count());
}

void LockKeysService::dispatchPoll() {
  if (m_nextRefreshAt != std::chrono::steady_clock::time_point{}
      && std::chrono::steady_clock::now() < m_nextRefreshAt) {
    return;
  }

  refreshNow();
}

void LockKeysService::refreshNow() {
  const LockKeysState previous = m_state;
  const bool hadState = m_hasState;

  m_state = readCurrentState();
  m_hasState = true;
  m_nextRefreshAt = std::chrono::steady_clock::now() + kRefreshInterval;

  if (hadState && previous != m_state && m_changeCallback) {
    m_changeCallback(previous, m_state);
  }
}

LockKeysService::LockKeysState LockKeysService::readCurrentState() {
  if (!m_sysfsDiscovered) {
    discoverSysfsLeds();
  }

  if (auto sysfsState = readCachedSysfsState(); sysfsState.has_value()) {
    return *sysfsState;
  }

  return m_wayland.keyboardLockKeysState();
}

std::optional<LockKeysService::LockKeysState> LockKeysService::readCachedSysfsState() {
  if (!hasCachedSysfsLeds()) {
    return std::nullopt;
  }

  LockKeysState state;
  bool readAny = false;
  bool hadFailure = false;

  auto readGroup = [&](const std::vector<int>& fds, bool LockKeysState::* field) {
    for (const int fd : fds) {
      bool on = false;
      if (!readBrightness(fd, on)) {
        hadFailure = true;
        continue;
      }
      readAny = true;
      state.*field = state.*field || on;
    }
  };

  readGroup(m_capsLockFds, &LockKeysState::capsLock);
  readGroup(m_numLockFds, &LockKeysState::numLock);
  readGroup(m_scrollLockFds, &LockKeysState::scrollLock);

  if (!readAny) {
    ++m_sysfsReadFailures;
    if (m_sysfsReadFailures >= kSysfsFailureRescanThreshold) {
      discoverSysfsLeds();
    }
    return std::nullopt;
  }

  if (hadFailure) {
    ++m_sysfsReadFailures;
    if (m_sysfsReadFailures >= kSysfsFailureRescanThreshold) {
      discoverSysfsLeds();
    }
  } else {
    m_sysfsReadFailures = 0;
  }

  return state;
}

void LockKeysService::discoverSysfsLeds() {
  closeSysfsLeds();
  m_sysfsReadFailures = 0;
  m_sysfsDiscovered = true;

  const fs::path ledsDir{"/sys/class/leds"};
  std::error_code ec;
  if (!fs::is_directory(ledsDir, ec)) {
    return;
  }

  for (fs::directory_iterator it(ledsDir, ec), end; it != end && !ec; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    const std::size_t sep = name.find("::");
    if (sep == std::string::npos) {
      continue;
    }

    const std::string kind = name.substr(sep + 2);
    std::vector<int>* target = nullptr;
    if (kind == "capslock") {
      target = &m_capsLockFds;
    } else if (kind == "numlock") {
      target = &m_numLockFds;
    } else if (kind == "scrolllock") {
      target = &m_scrollLockFds;
    }

    if (target == nullptr) {
      continue;
    }

    const fs::path brightnessPath = it->path() / "brightness";
    const int fd = ::open(brightnessPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    bool on = false;
    if (!readBrightness(fd, on)) {
      ::close(fd);
      continue;
    }
    target->push_back(fd);
  }
}

bool LockKeysService::hasCachedSysfsLeds() const noexcept {
  return !m_capsLockFds.empty() || !m_numLockFds.empty() || !m_scrollLockFds.empty();
}

void LockKeysService::closeSysfsLeds() noexcept {
  for (auto* fds : {&m_capsLockFds, &m_numLockFds, &m_scrollLockFds}) {
    for (const int fd : *fds) {
      ::close(fd);
    }
    fds->clear();
  }
}
