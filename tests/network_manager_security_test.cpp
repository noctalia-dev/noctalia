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

  const AccessPointInfo accessPoint;
  TEST_CHECK(!accessPoint.supportsSae);

  return 0;
}
