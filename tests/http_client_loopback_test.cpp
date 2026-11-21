#include "net/http_client.h"
#include "tests/test_check.h"

int main() {
  TEST_CHECK(isLoopbackUrl("http://127.0.0.1:8087/status"));
  TEST_CHECK(isLoopbackUrl("http://127.1.2.3/"));
  TEST_CHECK(isLoopbackUrl("https://LOCALHOST/path?q=1"));
  TEST_CHECK(isLoopbackUrl("http://[::1]:8080/"));
  TEST_CHECK(isLoopbackUrl("http://user:pass@localhost/"));

  TEST_CHECK(!isLoopbackUrl("http://localhost.example.com/"));
  TEST_CHECK(!isLoopbackUrl("http://127.0.0.1.nip.io/"));
  TEST_CHECK(!isLoopbackUrl("http://127.0.0.1@example.com/"));
  TEST_CHECK(!isLoopbackUrl("http://10.0.0.1/"));
  TEST_CHECK(!isLoopbackUrl("http://[::ffff:8.8.8.8]/"));
  TEST_CHECK(!isLoopbackUrl("ftp://127.0.0.1/"));
  TEST_CHECK(!isLoopbackUrl("file:///etc/hosts"));
  TEST_CHECK(!isLoopbackUrl("127.0.0.1:8087/status"));
  TEST_CHECK(!isLoopbackUrl(""));
  return 0;
}
