#include "pipewire/audio_glyphs.h"
#include "tests/test_check.h"

#include <cstring>

int main() {
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.0F, false, false), "volume-mute") == 0);
  // Speaker volume displayed as 0%.
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.004F, false, false), "volume-mute") == 0);
  // Speaker volume displayed above 0%.
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.006F, false, false), "volume-low") == 0);
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.39F, false, false), "volume-low") == 0);
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.4F, false, false), "volume-high") == 0);
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.8F, true, false), "volume-mute") == 0);
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.0F, false, true), "microphone-mute") == 0);
  TEST_CHECK(std::strcmp(audioVolumeGlyph(0.8F, true, true), "microphone-mute") == 0);

  return 0;
}
