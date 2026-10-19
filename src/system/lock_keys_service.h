#pragma once

#include "wayland/wayland_seat.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class WaylandConnection;

class LockKeysService {
public:
  using LockKeysState = WaylandSeat::LockKeysState;
  using ChangeCallback = std::function<void(const LockKeysState& previous, const LockKeysState& current)>;

  explicit LockKeysService(WaylandConnection& wayland);

  [[nodiscard]] LockKeysState state() const noexcept;
  void setChangeCallback(ChangeCallback callback);
  // notify=false re-reads state without reporting a change, for resyncing
  // after a period where nothing was listening.
  void refreshNow(bool notify = true);

private:
  [[nodiscard]] LockKeysState readCurrentState();
  // nullopt when no cached LED reads; with requireAll, also when any one fails.
  [[nodiscard]] std::optional<LockKeysState> readCachedSysfsState(bool requireAll);
  void discoverSysfsLeds();
  [[nodiscard]] bool hasCachedSysfsLeds() const noexcept;

  WaylandConnection& m_wayland;
  LockKeysState m_state;
  bool m_hasState = false;
  ChangeCallback m_changeCallback;
  std::vector<std::filesystem::path> m_capsLockPaths;
  std::vector<std::filesystem::path> m_numLockPaths;
  std::vector<std::filesystem::path> m_scrollLockPaths;
};
