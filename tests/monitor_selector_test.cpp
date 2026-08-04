#include "config/config_types.h"
#include "wayland/wayland_connection.h"

#include <cassert>
#include <string>

namespace {

  WaylandOutput makeOutput(
      std::string connectorName, std::string description, std::string make = {}, std::string model = {},
      std::string serialNumber = {}
  ) {
    WaylandOutput output;
    output.connectorName = std::move(connectorName);
    output.description = std::move(description);
    output.make = std::move(make);
    output.model = std::move(model);
    output.serialNumber = std::move(serialNumber);
    return output;
  }

} // namespace

int main() {
  // Compositor without wlr-output-management.
  {
    const auto output = makeOutput("eDP-1", "BOE 0x0BCA eDP-1");
    assert(outputMatchesSelector("eDP-1", output));
    assert(outputMatchesSelector("BOE", output));
    assert(outputMatchesSelector("0x0BCA", output));
    assert(!outputMatchesSelector("DP-1", output));
    assert(!outputMatchesSelector("0x0", output));
    assert(!outputMatchesSelector("HDMI-A-1", output));
  }

  // make/model/serial are independent events; description tokens the fields omit must still match.
  {
    const auto output = makeOutput("DP-2", "Acme UltraDisplay DP-2", "Acme");
    assert(outputMatchesSelector("UltraDisplay", output));
    assert(outputMatchesSelector("Acme", output));
    assert(outputMatchesSelector("DP-2", output));
    assert(!outputMatchesSelector("ABC123", output));
  }

  // Serial without model.
  {
    const auto output = makeOutput("DP-3", "Acme UltraDisplay DP-3", "Acme", "", "8VXYZ12");
    assert(outputMatchesSelector("8VXYZ12", output));
    assert(outputMatchesSelector("UltraDisplay", output));
  }

  // Complete metadata, description omitting the serial.
  {
    const auto output = makeOutput("DP-4", "Dell Inc. U2723QE", "Dell Inc.", "U2723QE", "8VXYZ12");
    assert(outputMatchesSelector("8VXYZ12", output));
    assert(outputMatchesSelector("U2723QE", output));
    assert(outputMatchesSelector("Dell", output));
    assert(outputMatchesSelector("DP-4", output));
    assert(!outputMatchesSelector("8VXYZ", output));
    assert(!outputMatchesSelector("ZZZZZZZ", output));
  }

  // Identical panels told apart by serial alone.
  {
    const auto first = makeOutput("DP-1", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00001");
    const auto second = makeOutput("DP-2", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00002");
    assert(outputMatchesSelector("SN00001", first));
    assert(!outputMatchesSelector("SN00001", second));
    assert(outputMatchesSelector("SN00002", second));
  }

  // Selectors never span two fields, so matching cannot depend on a join order.
  {
    const auto output = makeOutput("DP-5", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00003");
    assert(!outputMatchesSelector("UltraDisplay SN00003", output));
  }

  {
    const auto output = makeOutput("DP-6", "", "", "", "");
    assert(outputMatchesSelector("DP-6", output));
    assert(!outputMatchesSelector("DP-7", output));
  }

  {
    const auto output = makeOutput("DP-7", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00004");
    assert(!outputMatchesSelector("", output));
  }

  return 0;
}
