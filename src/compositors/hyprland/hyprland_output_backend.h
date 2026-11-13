#pragma once

#include "compositors/hyprland/hyprland_event_handler.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace compositors::hyprland {
  class HyprlandRuntime;
} // namespace compositors::hyprland

class HyprlandOutputBackend final : public compositors::hyprland::HyprlandEventHandler {
public:
  explicit HyprlandOutputBackend(compositors::hyprland::HyprlandRuntime& runtime);
  std::optional<std::string> focusedOutputName() const;
  std::unordered_map<std::string, bool> outputPowerStates() const;

  void handleEvent(std::string_view event, std::string_view data) override;
  void notifyCleanup() override;

private:
  // Seeded from j/monitors on first use, then kept current by focusedmonv2 events.
  mutable std::optional<std::string> m_focusedOutput;
  mutable bool m_focusedOutputKnown = false;
};

namespace compositors::hyprland {

  [[nodiscard]] bool setOutputPower(HyprlandRuntime& runtime, bool on);

  // Focus `connectorName` so newly spawned clients land on that monitor.
  [[nodiscard]] bool focusOutput(HyprlandRuntime& runtime, std::string_view connectorName);

  // Move `windowSelector` (e.g. address:0x…) to the active workspace on `connectorName`.
  [[nodiscard]] bool
  moveWindowToOutput(HyprlandRuntime& runtime, std::string_view windowSelector, std::string_view connectorName);

} // namespace compositors::hyprland
