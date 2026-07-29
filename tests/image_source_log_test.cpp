#include "render/core/image_source_log.h"

#include <cstdio>
#include <string>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "image_source_log_test: %s\n", message);
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
  ok = check(dataDescription.find("abcdef") == std::string::npos, "data URI payload should not be logged") && ok;

  const std::string longPath(800, 'x');
  const std::string longPathDescription = ImageSourceLog::describe(longPath);
  ok = check(longPathDescription.size() < longPath.size(), "long sources should be shortened") && ok;
  ok = check(
           longPathDescription.find("original=800 bytes") != std::string::npos,
           "long source summary should include original size"
       )
      && ok;

  const std::string malformedDataUri = "data:" + std::string(160, 'h');
  const std::string malformedDescription = ImageSourceLog::describe(malformedDataUri);
  ok = check(malformedDescription.find("malformed") != std::string::npos, "malformed data URI should be identified")
      && ok;
  ok = check(
           malformedDescription.find("original=160 bytes") != std::string::npos,
           "long data URI header should be shortened"
       )
      && ok;

  return ok ? 0 : 1;
}
