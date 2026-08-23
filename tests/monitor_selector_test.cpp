#include "config/config_types.h"
#include "tests/test_check.h"
#include "wayland/wayland_connection.h"

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
    TEST_CHECK(outputMatchesSelector("eDP-1", output));
    TEST_CHECK(outputMatchesSelector("BOE", output));
    TEST_CHECK(outputMatchesSelector("0x0BCA", output));
    TEST_CHECK(!outputMatchesSelector("DP-1", output));
    TEST_CHECK(!outputMatchesSelector("0x0", output));
    TEST_CHECK(!outputMatchesSelector("HDMI-A-1", output));
  }

  // make/model/serial are independent events; description tokens the fields omit must still match.
  {
    const auto output = makeOutput("DP-2", "Acme UltraDisplay DP-2", "Acme");
    TEST_CHECK(outputMatchesSelector("UltraDisplay", output));
    TEST_CHECK(outputMatchesSelector("Acme", output));
    TEST_CHECK(outputMatchesSelector("DP-2", output));
    TEST_CHECK(!outputMatchesSelector("ABC123", output));
  }

  // Serial without model.
  {
    const auto output = makeOutput("DP-3", "Acme UltraDisplay DP-3", "Acme", "", "8VXYZ12");
    TEST_CHECK(outputMatchesSelector("8VXYZ12", output));
    TEST_CHECK(outputMatchesSelector("UltraDisplay", output));
  }

  // Complete metadata, description omitting the serial.
  {
    const auto output = makeOutput("DP-4", "Dell Inc. U2723QE", "Dell Inc.", "U2723QE", "8VXYZ12");
    TEST_CHECK(outputMatchesSelector("8VXYZ12", output));
    TEST_CHECK(outputMatchesSelector("U2723QE", output));
    TEST_CHECK(outputMatchesSelector("Dell", output));
    TEST_CHECK(outputMatchesSelector("DP-4", output));
    TEST_CHECK(!outputMatchesSelector("8VXYZ", output));
    TEST_CHECK(!outputMatchesSelector("ZZZZZZZ", output));
  }

  // Identical panels told apart by serial alone.
  {
    const auto first = makeOutput("DP-1", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00001");
    const auto second = makeOutput("DP-2", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00002");
    TEST_CHECK(outputMatchesSelector("SN00001", first));
    TEST_CHECK(!outputMatchesSelector("SN00001", second));
    TEST_CHECK(outputMatchesSelector("SN00002", second));
  }

  // Selectors never span two fields, so matching cannot depend on a join order.
  {
    const auto output = makeOutput("DP-5", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00003");
    TEST_CHECK(!outputMatchesSelector("UltraDisplay SN00003", output));
  }

  {
    const auto output = makeOutput("DP-6", "", "", "", "");
    TEST_CHECK(outputMatchesSelector("DP-6", output));
    TEST_CHECK(!outputMatchesSelector("DP-7", output));
  }

  {
    const auto output = makeOutput("DP-7", "Acme UltraDisplay", "Acme", "UltraDisplay", "SN00004");
    TEST_CHECK(!outputMatchesSelector("", output));
  }

  return 0;
}
