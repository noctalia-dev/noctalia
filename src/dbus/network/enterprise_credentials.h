#pragma once

#include "dbus/network/network_manager_security.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Credentials and NM setting layout for 802.1X (WPA2/WPA3 Enterprise) Wi-Fi.
// Deliberately free of sdbus types: the mapping from what the user typed to what
// NetworkManager stores is the part worth testing, and it should be testable
// without a bus.
namespace network_enterprise {

  enum class EapMethod : std::uint8_t {
    Peap, // Protected EAP — the eduroam and Windows-domain default.
    Ttls, // Tunneled TLS.
  };

  // Inner (phase 2) authentication. Password-based methods only; EAP-in-EAP
  // inner methods use NM's separate "phase2-autheap" property and are out of
  // scope here.
  enum class Phase2Auth : std::uint8_t {
    MsChapV2,
    Pap,
    MsChap,
    Chap,
  };

  // Whether the password-based EAP methods modelled here can satisfy an AP with
  // this key management. The WPA3-Enterprise 192-bit suite is certificate-only:
  // NM refuses to write a profile without "802-1x.client-cert", so it needs
  // EAP-TLS rather than any password the user could type.
  [[nodiscard]] constexpr bool passwordAuthUsable(network_manager_security::KeyManagement kind) noexcept {
    return kind == network_manager_security::KeyManagement::Enterprise;
  }

  struct EnterpriseCredentials {
    EapMethod eap = EapMethod::Peap;
    Phase2Auth phase2 = Phase2Auth::MsChapV2;
    std::string identity;
    std::string anonymousIdentity; // Outer identity; omitted from the profile when empty.
    std::string password;
    std::string caCertPath;        // Absolute path; empty means use the system CA store.
    std::string domainSuffixMatch; // RADIUS server name suffix to require.

    bool operator==(const EnterpriseCredentials&) const = default;
  };

  enum class Validation : std::uint8_t {
    Ok,
    MissingIdentity,
    MissingPassword,
    UnsupportedPhase2,
    CaCertNotAbsolute,
  };

  [[nodiscard]] inline std::string_view eapName(EapMethod method) noexcept {
    switch (method) {
    case EapMethod::Ttls:
      return "ttls";
    case EapMethod::Peap:
      break;
    }
    return "peap";
  }

  [[nodiscard]] inline std::string_view phase2Name(Phase2Auth auth) noexcept {
    switch (auth) {
    case Phase2Auth::Pap:
      return "pap";
    case Phase2Auth::MsChap:
      return "mschap";
    case Phase2Auth::Chap:
      return "chap";
    case Phase2Auth::MsChapV2:
      break;
    }
    return "mschapv2";
  }

  // PEAP's tunnel carries MSCHAPv2 here; PAP/CHAP/MSCHAP are TTLS-only. Offering
  // an impossible pair produces a profile that fails at association time with an
  // error no user can act on, so it is rejected up front.
  [[nodiscard]] inline bool phase2Supported(EapMethod eap, Phase2Auth phase2) noexcept {
    if (eap == EapMethod::Peap) {
      return phase2 == Phase2Auth::MsChapV2;
    }
    return true;
  }

  [[nodiscard]] inline Validation validate(const EnterpriseCredentials& credentials) {
    if (credentials.identity.empty()) {
      return Validation::MissingIdentity;
    }
    if (credentials.password.empty()) {
      return Validation::MissingPassword;
    }
    if (!phase2Supported(credentials.eap, credentials.phase2)) {
      return Validation::UnsupportedPhase2;
    }
    if (!credentials.caCertPath.empty() && !credentials.caCertPath.starts_with('/')) {
      return Validation::CaCertNotAbsolute;
    }
    return Validation::Ok;
  }

  // NM stores certificate references as a byte array holding a "file://" URI
  // terminated by NUL; a blob without the scheme is read as inline DER/PEM data.
  [[nodiscard]] inline std::vector<std::uint8_t> caCertUriBytes(std::string_view path) {
    constexpr std::string_view kScheme = "file://";
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kScheme.size() + path.size() + 1U);
    for (const char ch : kScheme) {
      bytes.push_back(static_cast<std::uint8_t>(ch));
    }
    for (const char ch : path) {
      bytes.push_back(static_cast<std::uint8_t>(ch));
    }
    bytes.push_back(0U);
    return bytes;
  }

  // Plain mirror of NM's "802-1x" setting. Empty optional-ish fields are omitted
  // by the caller rather than written as empty strings, which NM treats as set.
  struct EapSetting {
    std::vector<std::string> eap;
    std::string identity;
    std::string anonymousIdentity;
    std::string phase2Auth;
    std::string password;
    bool systemCaCerts = false;
    std::vector<std::uint8_t> caCert;
    std::string domainSuffixMatch;

    bool operator==(const EapSetting&) const = default;
  };

  [[nodiscard]] inline EapSetting buildEapSetting(const EnterpriseCredentials& credentials) {
    EapSetting setting;
    setting.eap.emplace_back(eapName(credentials.eap));
    setting.identity = credentials.identity;
    setting.anonymousIdentity = credentials.anonymousIdentity;
    setting.phase2Auth = std::string(phase2Name(credentials.phase2));
    setting.password = credentials.password;
    setting.domainSuffixMatch = credentials.domainSuffixMatch;
    if (credentials.caCertPath.empty()) {
      setting.systemCaCerts = true;
    } else {
      setting.caCert = caCertUriBytes(credentials.caCertPath);
    }
    return setting;
  }

} // namespace network_enterprise
