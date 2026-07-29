#pragma once

// Canonical bar/OSD glyph for a sink (isInput == false) or source (isInput == true) given its
// volume and effective mute. Single source of the speaker mute->slashed-icon rule and the
// volume-level thresholds so the bar widget and the OSD can never map the same state to different
// icons.
[[nodiscard]] const char* audioVolumeGlyph(float volume, bool muted, bool isInput);
