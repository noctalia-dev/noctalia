#include "render/core/image_source_log.h"

#include <cstdio>
#include <print>
#include <string>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "image_source_log_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  bool ok = true;

  ok = check(ImageSourceLog::describe("/tmp/icon.png") == "/tmp/icon.png", "regular paths should pass through") && ok;

  const std::string dataUri = "data:image/png;base64,abcdef";
  const std::string dataDescription = ImageSourceLog::describe(dataUri);
  ok =
      check(dataDescription == "data:image/png;base64 (payload=6 bytes, uri=28 bytes)", "data URI should be summarized")
      && ok;
  ok = check(!dataDescription.contains("abcdef"), "data URI payload should not be logged") && ok;

  const std::string longPath(800, 'x');
  const std::string longPathDescription = ImageSourceLog::describe(longPath);
  ok = check(longPathDescription.size() < longPath.size(), "long sources should be shortened") && ok;
  ok = check(longPathDescription.contains("original=800 bytes"), "long source summary should include original size")
      && ok;

  const std::string malformedDataUri = "data:" + std::string(160, 'h');
  const std::string malformedDescription = ImageSourceLog::describe(malformedDataUri);
  ok = check(malformedDescription.contains("malformed"), "malformed data URI should be identified") && ok;
  ok = check(malformedDescription.contains("original=160 bytes"), "long data URI header should be shortened") && ok;

  return ok ? 0 : 1;
}
