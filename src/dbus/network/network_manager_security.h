#pragma once

#include <cstdint>
#include <string_view>

namespace network_manager_security {

  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtSae = 0x00000400U;

  [[nodiscard]] constexpr bool supportsSae(std::uint32_t rsnFlags) noexcept {
    return (rsnFlags & kNm80211ApSecKeyMgmtSae) != 0U;
  }

  [[nodiscard]] constexpr std::string_view keyManagement(bool supportsSae) noexcept {
    return supportsSae ? "sae" : "wpa-psk";
  }

  [[nodiscard]] constexpr bool shouldRestartCellularAuthentication(
      std::string_view connectionType, std::uint32_t activeConnectionState, std::uint32_t deviceState
  ) noexcept {
    constexpr std::uint32_t kActiveConnectionStateActivating = 1;
    constexpr std::uint32_t kDeviceStateNeedAuth = 60;
    return connectionType == "gsm"
        && activeConnectionState == kActiveConnectionStateActivating
        && deviceState == kDeviceStateNeedAuth;
  }

} // namespace network_manager_security
