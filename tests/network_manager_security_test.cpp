#include "dbus/network/network_manager_security.h"
#include "dbus/network/network_types.h"
#include "tests/test_check.h"

namespace {

  using network_manager_security::KeyManagement;

  // Representative rsnFlags as NetworkManager reports them, cipher bits included
  // so the key-management probes are exercised against realistic input.
  constexpr std::uint32_t kCiphers = 0x00000008U | 0x00000080U; // PAIR_CCMP | GROUP_CCMP
  constexpr std::uint32_t kWpa2Personal = kCiphers | 0x00000100U;
  constexpr std::uint32_t kWpa3Personal = kCiphers | 0x00000400U;
  constexpr std::uint32_t kWpa2Enterprise = kCiphers | 0x00000200U;
  constexpr std::uint32_t kWpa3Enterprise192 = kCiphers | 0x00000200U | 0x00002000U;

} // namespace

int main() {
  TEST_CHECK(!network_manager_security::supportsSae(0U));
  TEST_CHECK(!network_manager_security::supportsSae(0x00000100U));
  TEST_CHECK(network_manager_security::supportsSae(0x00000400U));
  TEST_CHECK(network_manager_security::supportsSae(0x00000500U));

  TEST_CHECK(!network_manager_security::supportsEnterprise(0U));
  TEST_CHECK(!network_manager_security::supportsEnterprise(kWpa2Personal));
  TEST_CHECK(!network_manager_security::supportsEnterprise(kWpa3Personal));
  TEST_CHECK(network_manager_security::supportsEnterprise(0x00000200U));
  TEST_CHECK(network_manager_security::supportsEnterprise(0x00002000U));
  TEST_CHECK(network_manager_security::supportsEnterprise(kWpa2Enterprise));

  TEST_CHECK(!network_manager_security::supportsSuiteB192(kWpa2Enterprise));
  TEST_CHECK(network_manager_security::supportsSuiteB192(kWpa3Enterprise192));

  TEST_CHECK(network_manager_security::keyManagementFor(0U) == KeyManagement::Psk);
  TEST_CHECK(network_manager_security::keyManagementFor(kWpa2Personal) == KeyManagement::Psk);
  TEST_CHECK(network_manager_security::keyManagementFor(kWpa3Personal) == KeyManagement::Sae);
  TEST_CHECK(network_manager_security::keyManagementFor(kWpa2Enterprise) == KeyManagement::Enterprise);
  TEST_CHECK(network_manager_security::keyManagementFor(kWpa3Enterprise192) == KeyManagement::EnterpriseSuiteB);

  // Priority. An AP advertising 802.1X needs EAP credentials, so enterprise must
  // win over SAE and PSK no matter which other key-mgmt bits are also set —
  // classifying such an AP as personal is what makes it prompt for a password
  // that can never authenticate.
  TEST_CHECK(
      network_manager_security::keyManagementFor(kCiphers | 0x00000200U | 0x00000400U) == KeyManagement::Enterprise
  );
  TEST_CHECK(
      network_manager_security::keyManagementFor(kCiphers | 0x00000200U | 0x00000100U) == KeyManagement::Enterprise
  );
  // Suite-B is not negotiable down to plain wpa-eap, so it outranks 802_1X.
  TEST_CHECK(
      network_manager_security::keyManagementFor(kCiphers | 0x00002000U | 0x00000400U)
      == KeyManagement::EnterpriseSuiteB
  );

  TEST_CHECK(network_manager_security::keyManagementName(KeyManagement::Psk) == "wpa-psk");
  TEST_CHECK(network_manager_security::keyManagementName(KeyManagement::Sae) == "sae");
  TEST_CHECK(network_manager_security::keyManagementName(KeyManagement::Enterprise) == "wpa-eap");
  TEST_CHECK(network_manager_security::keyManagementName(KeyManagement::EnterpriseSuiteB) == "wpa-eap-suite-b-192");

  TEST_CHECK(!network_manager_security::isEnterprise(KeyManagement::Psk));
  TEST_CHECK(!network_manager_security::isEnterprise(KeyManagement::Sae));
  TEST_CHECK(network_manager_security::isEnterprise(KeyManagement::Enterprise));
  TEST_CHECK(network_manager_security::isEnterprise(KeyManagement::EnterpriseSuiteB));

  // A default-constructed AP stays personal, so nothing starts out asking for
  // EAP credentials before its RSN flags have been read.
  const AccessPointInfo accessPoint;
  TEST_CHECK(accessPoint.keyManagement == KeyManagement::Psk);
  TEST_CHECK(!accessPoint.isEnterprise());

  AccessPointInfo enterpriseAp;
  enterpriseAp.keyManagement = network_manager_security::keyManagementFor(kWpa2Enterprise);
  TEST_CHECK(enterpriseAp.isEnterprise());
  TEST_CHECK(enterpriseAp != accessPoint);

  return 0;
}
