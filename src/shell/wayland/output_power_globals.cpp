#include "shell/wayland/output_power_globals.h"

#include "wayland/wayland_connection.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"

OutputPowerGlobals::OutputPowerGlobals(WaylandConnection& connection) : m_connection(connection) {}

void OutputPowerGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t /*version*/
) {
  if (interface != zwlr_output_power_manager_v1_interface.name) {
    return;
  }
  m_manager = static_cast<zwlr_output_power_manager_v1*>(
      wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1)
  );
}

void OutputPowerGlobals::releaseGlobals() {
  if (m_manager != nullptr) {
    zwlr_output_power_manager_v1_destroy(m_manager);
    m_manager = nullptr;
  }
}

bool OutputPowerGlobals::setMode(bool on) {
  if (m_manager == nullptr) {
    return false;
  }
  const auto mode = on ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF;
  bool any = false;
  for (const auto& output : m_connection.outputs()) {
    if (output.output == nullptr) {
      continue;
    }
    // Released right away, as wlopm does: compositors may give an output's power control to one client at a time.
    auto* power = zwlr_output_power_manager_v1_get_output_power(m_manager, output.output);
    zwlr_output_power_v1_set_mode(power, mode);
    zwlr_output_power_v1_destroy(power);
    any = true;
  }
  return any;
}
