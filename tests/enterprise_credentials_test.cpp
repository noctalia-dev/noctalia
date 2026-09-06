#include "dbus/network/enterprise_credentials.h"
#include "tests/test_check.h"

#include <string>

namespace {

  using network_enterprise::EapMethod;
  using network_enterprise::EnterpriseCredentials;
  using network_enterprise::Phase2Auth;
  using network_enterprise::Validation;

  // A credential set that passes validation, used as the base for negative cases.
  EnterpriseCredentials validCredentials() {
    EnterpriseCredentials credentials;
    credentials.eap = EapMethod::Peap;
    credentials.phase2 = Phase2Auth::MsChapV2;
    credentials.identity = "user@example.edu";
    credentials.password = "secret";
    credentials.domainSuffixMatch = "example.edu";
    return credentials;
  }

  std::string toString(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

} // namespace

int main() {
  TEST_CHECK(network_enterprise::eapName(EapMethod::Peap) == "peap");
  TEST_CHECK(network_enterprise::eapName(EapMethod::Ttls) == "ttls");
  TEST_CHECK(network_enterprise::phase2Name(Phase2Auth::MsChapV2) == "mschapv2");
  TEST_CHECK(network_enterprise::phase2Name(Phase2Auth::Pap) == "pap");
  TEST_CHECK(network_enterprise::phase2Name(Phase2Auth::MsChap) == "mschap");
  TEST_CHECK(network_enterprise::phase2Name(Phase2Auth::Chap) == "chap");

  // PEAP tunnels MSCHAPv2 only; TTLS takes the password-based set.
  TEST_CHECK(network_enterprise::phase2Supported(EapMethod::Peap, Phase2Auth::MsChapV2));
  TEST_CHECK(!network_enterprise::phase2Supported(EapMethod::Peap, Phase2Auth::Pap));
  TEST_CHECK(!network_enterprise::phase2Supported(EapMethod::Peap, Phase2Auth::Chap));
  TEST_CHECK(network_enterprise::phase2Supported(EapMethod::Ttls, Phase2Auth::Pap));
  TEST_CHECK(network_enterprise::phase2Supported(EapMethod::Ttls, Phase2Auth::MsChapV2));

  // Only plain wpa-eap is satisfiable with a password. Offering the form for a
  // Suite-B AP would build a profile NM bounces for a missing client certificate.
  using network_manager_security::KeyManagement;
  TEST_CHECK(network_enterprise::passwordAuthUsable(KeyManagement::Enterprise));
  TEST_CHECK(!network_enterprise::passwordAuthUsable(KeyManagement::EnterpriseSuiteB));
  TEST_CHECK(!network_enterprise::passwordAuthUsable(KeyManagement::Psk));
  TEST_CHECK(!network_enterprise::passwordAuthUsable(KeyManagement::Sae));

  TEST_CHECK(network_enterprise::validate(validCredentials()) == Validation::Ok);

  {
    auto credentials = validCredentials();
    credentials.identity.clear();
    TEST_CHECK(network_enterprise::validate(credentials) == Validation::MissingIdentity);
  }
  {
    auto credentials = validCredentials();
    credentials.password.clear();
    TEST_CHECK(network_enterprise::validate(credentials) == Validation::MissingPassword);
  }
  {
    auto credentials = validCredentials();
    credentials.phase2 = Phase2Auth::Pap; // still PEAP
    TEST_CHECK(network_enterprise::validate(credentials) == Validation::UnsupportedPhase2);
  }
  {
    auto credentials = validCredentials();
    credentials.caCertPath = "certs/ca.pem";
    TEST_CHECK(network_enterprise::validate(credentials) == Validation::CaCertNotAbsolute);
  }

  // Both server-trust anchors are optional, and either one alone is accepted.
  {
    auto credentials = validCredentials();
    credentials.domainSuffixMatch.clear();
    credentials.caCertPath.clear();
    TEST_CHECK(network_enterprise::validate(credentials) == Validation::Ok);

    credentials.caCertPath = "/etc/ssl/certs/ca-certificates.crt";
    TEST_CHECK(network_enterprise::validate(credentials) == Validation::Ok);
  }

  // ca-cert is a byte array holding a NUL-terminated file:// URI. Dropping the
  // scheme makes NM read the path as inline certificate data; dropping the NUL
  // makes it reject the value outright.
  {
    const auto bytes = network_enterprise::caCertUriBytes("/etc/ssl/certs/ca-certificates.crt");
    TEST_CHECK(bytes.back() == 0U);
    TEST_CHECK(toString(bytes) == std::string("file:///etc/ssl/certs/ca-certificates.crt") + '\0');
    TEST_CHECK(bytes.size() == std::string("file:///etc/ssl/certs/ca-certificates.crt").size() + 1U);
  }

  {
    const auto setting = network_enterprise::buildEapSetting(validCredentials());
    TEST_CHECK(setting.eap.size() == 1U);
    TEST_CHECK(setting.eap.front() == "peap");
    TEST_CHECK(setting.identity == "user@example.edu");
    TEST_CHECK(setting.phase2Auth == "mschapv2");
    TEST_CHECK(setting.password == "secret");
    TEST_CHECK(setting.domainSuffixMatch == "example.edu");
    TEST_CHECK(setting.systemCaCerts);
    TEST_CHECK(setting.caCert.empty());
    TEST_CHECK(setting.anonymousIdentity.empty());
  }

  // An explicit CA path pins that file instead, and must not also request the
  // system store — the two anchors are mutually exclusive in the profile.
  {
    auto credentials = validCredentials();
    credentials.caCertPath = "/etc/ssl/certs/radius-ca.pem";
    credentials.anonymousIdentity = "anonymous@example.edu";
    credentials.eap = EapMethod::Ttls;
    credentials.phase2 = Phase2Auth::Pap;

    const auto setting = network_enterprise::buildEapSetting(credentials);
    TEST_CHECK(setting.eap.front() == "ttls");
    TEST_CHECK(setting.phase2Auth == "pap");
    TEST_CHECK(setting.anonymousIdentity == "anonymous@example.edu");
    TEST_CHECK(!setting.systemCaCerts);
    TEST_CHECK(toString(setting.caCert) == std::string("file:///etc/ssl/certs/radius-ca.pem") + '\0');
  }

  return 0;
}
