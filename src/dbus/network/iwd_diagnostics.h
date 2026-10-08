#pragma once

#include <cstdint>
#include <map>
#include <sdbus-c++/Types.h>
#include <string>

struct NetworkState;

namespace iwd_diagnostics {

  [[nodiscard]] std::uint8_t signalToPercent(std::int16_t dBm);
  void applyStationDiagnostics(NetworkState& next, const std::map<std::string, sdbus::Variant>& diagnostics);

} // namespace iwd_diagnostics
