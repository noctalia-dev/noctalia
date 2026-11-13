#include "render/core/image_source_log.h"
#include "tests/test_check.h"

#include <string>

int main() {
  TEST_CHECK(ImageSourceLog::describe("/tmp/icon.png") == "/tmp/icon.png");

  const std::string dataUri = "data:image/png;base64,abcdef";
  const std::string dataDescription = ImageSourceLog::describe(dataUri);
  TEST_CHECK(dataDescription == "data:image/png;base64 (payload=6 bytes, uri=28 bytes)");
  TEST_CHECK(!dataDescription.contains("abcdef"));

  const std::string longPath(800, 'x');
  const std::string longPathDescription = ImageSourceLog::describe(longPath);
  TEST_CHECK(longPathDescription.size() < longPath.size());
  TEST_CHECK(longPathDescription.contains("original=800 bytes"));

  const std::string malformedDataUri = "data:" + std::string(160, 'h');
  const std::string malformedDescription = ImageSourceLog::describe(malformedDataUri);
  TEST_CHECK(malformedDescription.contains("malformed"));
  TEST_CHECK(malformedDescription.contains("original=160 bytes"));

  return 0;
}
