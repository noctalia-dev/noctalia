#include "pipewire/audio_media_class.h"

#include <algorithm>
#include <array>

namespace {
  constexpr auto kTrackedNodeClasses = std::to_array<std::string_view>({
      "Audio/Sink",
      "Audio/Source",
      "Stream/Output/Audio",
      "Stream/Input/Audio",
  });
} // namespace

bool isInternalAudioMediaClass(std::string_view mediaClass) { return mediaClass.ends_with("/Internal"); }

void normalizeAudioMediaClass(std::string& mediaClass) {
  if (isInternalAudioMediaClass(mediaClass)) {
    return;
  }
  if (mediaClass.starts_with("Audio/Sink")) {
    mediaClass = "Audio/Sink";
  } else if (mediaClass.starts_with("Audio/Source")) {
    mediaClass = "Audio/Source";
  }
}

bool isTrackedNodeClass(std::string_view mediaClass) {
  if (isInternalAudioMediaClass(mediaClass)) {
    return false;
  }
  return std::ranges::contains(kTrackedNodeClasses, mediaClass) || mediaClass.contains("Video");
}
