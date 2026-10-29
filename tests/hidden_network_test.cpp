#include "dbus/network/hidden_network.h"
#include "tests/test_check.h"

#include <string>

int main() {
  using network_hidden::Validation;

  TEST_CHECK(network_hidden::validateSsid("") == Validation::MissingSsid);
  TEST_CHECK(network_hidden::validateSsid("home") == Validation::Ok);
  TEST_CHECK(network_hidden::validateSsid(std::string(32, 'a')) == Validation::Ok);
  TEST_CHECK(network_hidden::validateSsid(std::string(33, 'a')) == Validation::SsidTooLong);
  // The limit is in bytes, so a multi-byte name counts each octet.
  TEST_CHECK(network_hidden::validateSsid(std::string(11, '\xc3') + std::string(11, '\xa9')) == Validation::Ok);
  TEST_CHECK(
      network_hidden::validateSsid(std::string(17, '\xc3') + std::string(17, '\xa9')) == Validation::SsidTooLong
  );

  const auto bytes = network_hidden::ssidBytes("caf\xc3\xa9");
  TEST_CHECK(bytes.size() == 5);
  TEST_CHECK(bytes[0] == 'c');
  TEST_CHECK(bytes[3] == 0xc3);
  TEST_CHECK(bytes[4] == 0xa9);
  TEST_CHECK(network_hidden::ssidBytes("").empty());

  return 0;
}
