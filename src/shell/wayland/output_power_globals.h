#pragma once

#include "wayland/wayland_extension.h"

class WaylandConnection;
struct zwlr_output_power_manager_v1;

// wlr-output-power-management client: powers every output off or on (DPMS) without removing it from the layout.
class OutputPowerGlobals final : public WaylandExtension {
public:
  explicit OutputPowerGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  bool available() const noexcept { return m_manager != nullptr; }

  // Asks for the power mode of every output. Returns false when the compositor has no manager or there is no output;
  // a request the compositor refuses (`failed`) isn't seen.
  [[nodiscard]] bool setMode(bool on);

private:
  WaylandConnection& m_connection;
  zwlr_output_power_manager_v1* m_manager = nullptr;
};
