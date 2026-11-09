#include "pipewire/audio_media_class.h"
#include "tests/test_check.h"

#include <string>

namespace {

  std::string normalized(const char* input) {
    std::string mediaClass(input);
    normalizeAudioMediaClass(mediaClass);
    return mediaClass;
  }

} // namespace

int main() {
  TEST_CHECK(normalized("Audio/Sink") == "Audio/Sink");
  TEST_CHECK(normalized("Audio/Source") == "Audio/Source");
  TEST_CHECK(normalized("Audio/Sink/Virtual") == "Audio/Sink");
  TEST_CHECK(normalized("Audio/Source/Virtual") == "Audio/Source");
  TEST_CHECK(normalized("Audio/Sink/Internal") == "Audio/Sink/Internal");
  TEST_CHECK(normalized("Audio/Source/Internal") == "Audio/Source/Internal");
  TEST_CHECK(normalized("Stream/Output/Audio") == "Stream/Output/Audio");

  TEST_CHECK(isTrackedNodeClass("Audio/Sink"));
  TEST_CHECK(isTrackedNodeClass("Audio/Source"));
  TEST_CHECK(isTrackedNodeClass("Stream/Input/Audio"));
  TEST_CHECK(isTrackedNodeClass("Video/Source"));
  // split-sink internal endpoint hidden
  TEST_CHECK(!isTrackedNodeClass("Audio/Sink/Internal"));
  TEST_CHECK(!isTrackedNodeClass("Audio/Source/Internal"));
  TEST_CHECK(!isTrackedNodeClass("Video/Source/Internal"));
  TEST_CHECK(!isTrackedNodeClass("Audio/Duplex"));

  return 0;
}
