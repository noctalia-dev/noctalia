#include "pipewire/audio_glyphs.h"

#include <algorithm>
#include <cmath>

namespace {

  [[nodiscard]] int displayedVolumePercent(float volume) {
    return static_cast<int>(std::round(std::max(0.0f, volume) * 100.0f));
  }

} // namespace

const char* audioVolumeGlyph(float volume, bool muted, bool isInput) {
  if (isInput) {
    return (muted || displayedVolumePercent(volume) == 0) ? "microphone-mute" : "microphone";
  }
  if (muted || displayedVolumePercent(volume) == 0) {
    return "volume-mute";
  }
  if (volume < 0.4f) {
    return "volume-low";
  }
  return "volume-high";
}
