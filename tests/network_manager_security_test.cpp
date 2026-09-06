#include "dbus/network/network_manager_security.h"
#include "dbus/network/network_types.h"
#include "tests/test_check.h"

int main() {
  TEST_CHECK(!network_manager_security::supportsSae(0U));
  TEST_CHECK(!network_manager_security::supportsSae(0x00000100U));
  TEST_CHECK(network_manager_security::supportsSae(0x00000400U));
  TEST_CHECK(network_manager_security::supportsSae(0x00000500U));

  TEST_CHECK(network_manager_security::keyManagement(false) == "wpa-psk");
  TEST_CHECK(network_manager_security::keyManagement(true) == "sae");

  TEST_CHECK(network_manager_security::shouldRestartCellularAuthentication("gsm", 1U, 60U));
  TEST_CHECK(!network_manager_security::shouldRestartCellularAuthentication("vpn", 1U, 60U));
  TEST_CHECK(!network_manager_security::shouldRestartCellularAuthentication("gsm", 2U, 60U));
  TEST_CHECK(!network_manager_security::shouldRestartCellularAuthentication("gsm", 1U, 30U));

  const AccessPointInfo accessPoint;
  TEST_CHECK(!accessPoint.supportsSae);

  return 0;
}
