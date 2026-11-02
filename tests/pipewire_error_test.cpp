#include "pipewire/pipewire_error.h"
#include "test_check.h"

#include <cerrno>
#include <pipewire/core.h>

int main() {
  using noctalia::pipewire::classifyError;
  using noctalia::pipewire::ErrorDisposition;

  TEST_CHECK(classifyError(PW_ID_CORE, -EPIPE) == ErrorDisposition::Reconnect);
  TEST_CHECK(classifyError(PW_ID_CORE, -ENOENT) == ErrorDisposition::StaleObject);
  TEST_CHECK(classifyError(37, -ESTALE) == ErrorDisposition::StaleObject);
  TEST_CHECK(classifyError(37, -EPIPE) == ErrorDisposition::Report);
  TEST_CHECK(classifyError(PW_ID_CORE, -EACCES) == ErrorDisposition::Report);
}
