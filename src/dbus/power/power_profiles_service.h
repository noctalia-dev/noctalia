#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SystemBus;
class IpcService;

namespace sdbus {
  class IProxy;
}

[[nodiscard]] std::string profileLabel(std::string_view profile);
[[nodiscard]] std::string_view profileGlyphName(std::string_view profile);

/// Canonical low-power to high-power ordering of the well-known profiles.
[[nodiscard]] std::span<const std::string_view> powerProfileOrder();

struct PowerProfilesState {
  std::string activeProfile;
  std::vector<std::string> profiles;
  std::string performanceInhibited;

  bool operator==(const PowerProfilesState&) const = default;
};

enum class PowerProfilesChangeOrigin : std::uint8_t {
  External,
  Noctalia,
};

class PowerProfilesService {
public:
  using ChangeCallback = std::function<void(const PowerProfilesState&, PowerProfilesChangeOrigin)>;
  using StateFeedbackCallback = std::function<void(std::string_view profile)>;

  explicit PowerProfilesService(SystemBus& bus);
  ~PowerProfilesService();

  void setChangeCallback(ChangeCallback callback);
  void refresh();

  [[nodiscard]] bool hasStateSnapshot() const noexcept { return m_hasStateSnapshot; }
  [[nodiscard]] const PowerProfilesState& state() const noexcept { return m_state; }
  [[nodiscard]] const std::string& activeProfile() const noexcept { return m_state.activeProfile; }
  [[nodiscard]] const std::vector<std::string>& profiles() const noexcept { return m_state.profiles; }

  [[nodiscard]] bool setActiveProfile(std::string_view profile);
  /// Step through profiles in canonical power order, wrapping at the ends.
  /// direction >= 0 moves toward performance, direction < 0 toward power-saver.
  /// Returns false when no active profile is known and UPower returned no profile list,
  /// or when the D-Bus set dispatch fails synchronously.
  [[nodiscard]] bool cycleActiveProfile(int direction = 1);

  void registerIpc(IpcService& ipc, StateFeedbackCallback stateFeedback = {});

private:
  [[nodiscard]] PowerProfilesChangeOrigin consumeActiveProfileChangeOrigin(std::string_view profile);
  void emitChangedIfNeeded(PowerProfilesState next, bool stateSnapshot);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::shared_ptr<int> m_lifetimeToken = std::make_shared<int>(0);
  PowerProfilesState m_state;
  std::optional<std::string> m_pendingLocalActiveProfile;
  ChangeCallback m_changeCallback;
  bool m_hasStateSnapshot = false;
  bool m_refreshInFlight = false;
  bool m_refreshQueued = false;
};
