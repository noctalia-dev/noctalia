#include "pipewire/audio_media_class.h"

#include <print>
#include <string>

namespace {

  bool expectNormalized(const char* input, const char* expected, const char* message) {
    std::string mediaClass(input);
    normalizeAudioMediaClass(mediaClass);
    if (mediaClass != expected) {
      std::println(stderr, "audio_media_class_test: {}: expected '{}', got '{}'", message, expected, mediaClass);
      return false;
    }
    return true;
  }

  bool expectTracked(const char* mediaClass, bool expected, const char* message) {
    if (isTrackedNodeClass(mediaClass) != expected) {
      std::println(stderr, "audio_media_class_test: {}: expected tracked={}", message, expected);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;

  ok = expectNormalized("Audio/Sink", "Audio/Sink", "plain sink") && ok;
  ok = expectNormalized("Audio/Source", "Audio/Source", "plain source") && ok;
  ok = expectNormalized("Audio/Sink/Virtual", "Audio/Sink", "virtual sink collapses") && ok;
  ok = expectNormalized("Audio/Source/Virtual", "Audio/Source", "virtual source collapses") && ok;
  ok = expectNormalized("Audio/Sink/Internal", "Audio/Sink/Internal", "internal sink keeps its class") && ok;
  ok = expectNormalized("Audio/Source/Internal", "Audio/Source/Internal", "internal source keeps its class") && ok;
  ok = expectNormalized("Stream/Output/Audio", "Stream/Output/Audio", "stream class untouched") && ok;

  ok = expectTracked("Audio/Sink", true, "plain sink tracked") && ok;
  ok = expectTracked("Audio/Source", true, "plain source tracked") && ok;
  ok = expectTracked("Stream/Input/Audio", true, "audio capture stream tracked") && ok;
  ok = expectTracked("Video/Source", true, "video source tracked") && ok;
  ok = expectTracked("Audio/Sink/Internal", false, "split-sink internal endpoint hidden") && ok;
  ok = expectTracked("Audio/Source/Internal", false, "internal source hidden") && ok;
  ok = expectTracked("Video/Source/Internal", false, "internal video source hidden") && ok;
  ok = expectTracked("Audio/Duplex", false, "unknown class untracked") && ok;

  return ok ? 0 : 1;
}
