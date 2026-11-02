#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace compositors::hyprland {
  class HyprlandRuntime;
} // namespace compositors::hyprland

class HyprlandOutputBackend {
public:
  explicit HyprlandOutputBackend(compositors::hyprland::HyprlandRuntime& runtime);
  [[nodiscard]] std::optional<std::string> focusedOutputName() const;
  [[nodiscard]] std::unordered_map<std::string, bool> outputPowerStates() const;

private:
  compositors::hyprland::HyprlandRuntime& m_runtime;
};

namespace compositors::hyprland {

  [[nodiscard]] bool setOutputPower(HyprlandRuntime& runtime, bool on);

  // Focus `connectorName` so newly spawned clients land on that monitor.
  [[nodiscard]] bool focusOutput(HyprlandRuntime& runtime, std::string_view connectorName);

  // Move `windowSelector` (e.g. address:0x…) to the active workspace on `connectorName`.
  [[nodiscard]] bool
  moveWindowToOutput(HyprlandRuntime& runtime, std::string_view windowSelector, std::string_view connectorName);

} // namespace compositors::hyprland
