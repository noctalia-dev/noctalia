#pragma once

#include <cstdint>
#include <string_view>

namespace network_manager_security {

  // NM80211ApSecurityFlags subset (see libnm/nm-dbus-interface.h). Only the
  // key-management bits matter here; cipher bits are not inspected.
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtPsk = 0x00000100U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmt8021X = 0x00000200U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtSae = 0x00000400U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtOwe = 0x00000800U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtOweTm = 0x00001000U;
  inline constexpr std::uint32_t kNm80211ApSecKeyMgmtEapSuiteB192 = 0x00002000U;

  enum class KeyManagement : std::uint8_t {
    Psk,              // wpa-psk: WPA/WPA2 Personal
    Sae,              // sae: WPA3 Personal
    Owe,              // owe: Enhanced Open (passwordless)
    Enterprise,       // wpa-eap: WPA2/WPA3 Enterprise (802.1X)
    EnterpriseSuiteB, // wpa-eap-suite-b-192: WPA3 Enterprise 192-bit (CNSA)
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

  // Pure OWE / OWE-TM only. Ignored when SAE, PSK, or 802.1X is also advertised so
  // transitional APs still prompt for a password / EAP credentials.
  [[nodiscard]] constexpr bool supportsOwe(std::uint32_t rsnFlags) noexcept {
    constexpr std::uint32_t kPassworded = kNm80211ApSecKeyMgmtPsk
        | kNm80211ApSecKeyMgmt8021X
        | kNm80211ApSecKeyMgmtSae
        | kNm80211ApSecKeyMgmtEapSuiteB192;
    if ((rsnFlags & kPassworded) != 0U) {
      return false;
    }
    return (rsnFlags & (kNm80211ApSecKeyMgmtOwe | kNm80211ApSecKeyMgmtOweTm)) != 0U;
  }

  // Enterprise outranks SAE: an AP advertising 802.1X needs EAP credentials, and
  // no PSK the user could type would authenticate against it. Suite-B outranks
  // plain enterprise because its 192-bit suite is not negotiable down to wpa-eap.
  // OWE is last among secured modes: only pure Enhanced Open maps here.
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
    if (supportsOwe(rsnFlags)) {
      return KeyManagement::Owe;
    }
    return KeyManagement::Psk;
  }

  [[nodiscard]] constexpr std::string_view keyManagementName(KeyManagement kind) noexcept {
    switch (kind) {
    case KeyManagement::Sae:
      return "sae";
    case KeyManagement::Owe:
      return "owe";
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

  // Secured APs that still need a password / EAP prompt. OWE is encrypted but
  // passwordless, so it is excluded.
  [[nodiscard]] constexpr bool requiresPsk(bool secured, KeyManagement kind) noexcept {
    return secured && kind != KeyManagement::Owe;
  }

} // namespace network_manager_security
