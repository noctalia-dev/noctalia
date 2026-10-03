#include "system/lock_keys_service.h"

#include "wayland/wayland_connection.h"

#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace {

  namespace fs = std::filesystem;

  constexpr int kSysfsFailureRescanThreshold = 3;

  bool readBrightness(const fs::path& path, bool& on) {
    std::ifstream file(path);
    if (!file.is_open()) {
      return false;
    }

    int brightness = 0;
    file >> brightness;
    if (!file.good() && !file.eof()) {
      return false;
    }

    on = brightness != 0;
    return true;
  }

} // namespace

LockKeysService::LockKeysService(WaylandConnection& wayland) : m_wayland(wayland) {}

LockKeysService::LockKeysState LockKeysService::state() const noexcept { return m_state; }

void LockKeysService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void LockKeysService::refreshNow() {
  const LockKeysState previous = m_state;
  const bool hadState = m_hasState;

  m_state = readCurrentState();
  m_hasState = true;

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

  auto readGroup = [&](const std::vector<fs::path>& paths, bool LockKeysState::* field) {
    for (const auto& path : paths) {
      bool on = false;
      if (!readBrightness(path, on)) {
        hadFailure = true;
        continue;
      }
      readAny = true;
      state.*field = state.*field || on;
    }
  };

  readGroup(m_capsLockPaths, &LockKeysState::capsLock);
  readGroup(m_numLockPaths, &LockKeysState::numLock);
  readGroup(m_scrollLockPaths, &LockKeysState::scrollLock);

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
  m_capsLockPaths.clear();
  m_numLockPaths.clear();
  m_scrollLockPaths.clear();
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
    std::vector<fs::path>* target = nullptr;
    if (kind == "capslock") {
      target = &m_capsLockPaths;
    } else if (kind == "numlock") {
      target = &m_numLockPaths;
    } else if (kind == "scrolllock") {
      target = &m_scrollLockPaths;
    }

    if (target == nullptr) {
      continue;
    }

    const fs::path brightnessPath = it->path() / "brightness";
    bool on = false;
    if (readBrightness(brightnessPath, on)) {
      target->push_back(brightnessPath);
    }
  }
}

bool LockKeysService::hasCachedSysfsLeds() const noexcept {
  return !m_capsLockPaths.empty() || !m_numLockPaths.empty() || !m_scrollLockPaths.empty();
}
