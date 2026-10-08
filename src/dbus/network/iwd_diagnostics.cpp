#include "dbus/network/iwd_diagnostics.h"

#include "dbus/network/network_types.h"

#include <optional>

namespace {

  using VariantMap = std::map<std::string, sdbus::Variant>;

  template <typename T> std::optional<T> variantGet(const sdbus::Variant& value) {
    try {
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  std::optional<std::uint32_t> frequencyMhzProp(const VariantMap& props) {
    const auto it = props.find("Frequency");
    if (it == props.end()) {
      return std::nullopt;
    }
    if (const auto u32 = variantGet<std::uint32_t>(it->second); u32.has_value() && *u32 > 0) {
      return u32;
    }
    if (const auto u16 = variantGet<std::uint16_t>(it->second); u16.has_value() && *u16 > 0) {
      return static_cast<std::uint32_t>(*u16);
    }
    if (const auto i32 = variantGet<std::int32_t>(it->second); i32.has_value() && *i32 > 0) {
      return static_cast<std::uint32_t>(*i32);
    }
    return std::nullopt;
  }

  std::optional<std::int16_t> rssiDbmProp(const VariantMap& props) {
    const auto it = props.find("RSSI");
    if (it == props.end()) {
      return std::nullopt;
    }
    if (const auto i16 = variantGet<std::int16_t>(it->second)) {
      return i16;
    }
    if (const auto i32 = variantGet<std::int32_t>(it->second)) {
      return static_cast<std::int16_t>(*i32);
    }
    return std::nullopt;
  }

} // namespace

namespace iwd_diagnostics {

  std::uint8_t signalToPercent(std::int16_t dBm) {
    if (dBm <= -100) {
      return 0;
    }
    if (dBm >= -50) {
      return 100;
    }
    return static_cast<std::uint8_t>(2 * (dBm + 100));
  }

  void applyStationDiagnostics(NetworkState& next, const std::map<std::string, sdbus::Variant>& diagnostics) {
    if (const auto freq = frequencyMhzProp(diagnostics); freq.has_value()) {
      next.frequencyMhz = *freq;
    }
    if (next.signalStrength == 0) {
      if (const auto rssi = rssiDbmProp(diagnostics); rssi.has_value()) {
        next.signalStrength = signalToPercent(*rssi);
      }
    }
  }

} // namespace iwd_diagnostics
