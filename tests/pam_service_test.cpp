#include "auth/pam_authenticator.h"
#include "test_check.h"

#include <cstdlib>

int main() {
  TEST_CHECK(::unsetenv("NOCTALIA_PAM_SERVICE") == 0);
  TEST_CHECK(PamAuthenticator::configuredService() == "login");

  TEST_CHECK(::setenv("NOCTALIA_PAM_SERVICE", "noctalia-test", 1) == 0);
  TEST_CHECK(PamAuthenticator::configuredService() == "noctalia-test");

  TEST_CHECK(::setenv("NOCTALIA_PAM_SERVICE", "", 1) == 0);
  TEST_CHECK(PamAuthenticator::configuredService() == "login");

  return EXIT_SUCCESS;
}
