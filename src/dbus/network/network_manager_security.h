#pragma once

#include <cstdint>
#include <string_view>

namespace network_manager_security {

  // NM80211ApSecurityFlags subset (see libnm/nm-dbus-interface.h). Only the
  // key-management bits matter here; cipher bits are not inspected.
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtPsk = 0x00000100U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmt8021X = 0x00000200U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtSae = 0x00000400U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtEapSuiteB192 = 0x00002000U;

  enum class KeyManagement : std::uint8_t {
    Psk,              // wpa-psk        — WPA/WPA2 Personal
    Sae,              // sae            — WPA3 Personal
    Enterprise,       // wpa-eap        — WPA2/WPA3 Enterprise (802.1X)
    EnterpriseSuiteB, // wpa-eap-suite-b-192 — WPA3 Enterprise 192-bit (CNSA)
  };

  [[nodiscard]] constexpr bool supportsSae(std::uint32_t rsnFlags) noexcept {
    return (rsnFlags & kNm80211ApSecKeyMgmtSae) != 0U;
  }

  [[nodiscard]] constexpr bool supportsSuiteB192(std::uint32_t rsnFlags) noexcept {
    return (rsnFlags & kNm80211ApSecKeyMgmtEapSuiteB192) != 0U;
  }

  // True when the AP asks for 802.1X/EAP credentials rather than a pre-shared key.
  [[nodiscard]] constexpr bool supportsEnterprise(std::uint32_t rsnFlags) noexcept {
    return (rsnFlags & (kNm80211ApSecKeyMgmt8021X | kNm80211ApSecKeyMgmtEapSuiteB192)) != 0U;
  }

  // Enterprise outranks SAE: an AP advertising 802.1X needs EAP credentials, and
  // no PSK the user could type would authenticate against it. Suite-B outranks
  // plain enterprise because its 192-bit suite is not negotiable down to wpa-eap.
  [[nodiscard]] constexpr KeyManagement keyManagementFor(std::uint32_t rsnFlags) noexcept {
    if (supportsSuiteB192(rsnFlags)) {
      return KeyManagement::EnterpriseSuiteB;
    }
    if (supportsEnterprise(rsnFlags)) {
      return KeyManagement::Enterprise;
    }
    if (supportsSae(rsnFlags)) {
      return KeyManagement::Sae;
    }
    return KeyManagement::Psk;
  }

  [[nodiscard]] constexpr std::string_view keyManagementName(KeyManagement kind) noexcept {
    switch (kind) {
    case KeyManagement::Sae:
      return "sae";
    case KeyManagement::Enterprise:
      return "wpa-eap";
    case KeyManagement::EnterpriseSuiteB:
      return "wpa-eap-suite-b-192";
    case KeyManagement::Psk:
      break;
    }
    return "wpa-psk";
  }

  [[nodiscard]] constexpr bool isEnterprise(KeyManagement kind) noexcept {
    return kind == KeyManagement::Enterprise || kind == KeyManagement::EnterpriseSuiteB;
  }

} // namespace network_manager_security
