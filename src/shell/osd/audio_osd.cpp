#include "shell/osd/audio_osd.h"

#include "pipewire/pipewire_service.h"
#include "pipewire/sound_player.h"
#include "shell/osd/osd_overlay.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {
  constexpr auto kVolumeSoundCooldown = std::chrono::milliseconds(70);
  constexpr auto kSuppressInputOsdAfterOutput = std::chrono::milliseconds(500);
  constexpr float kVolumeChangeEpsilon = 0.003f;

  [[nodiscard]] bool volumeChanged(float a, float b) { return std::abs(a - b) > kVolumeChangeEpsilon; }

  OsdContent makeOutputContent(float volume, bool muted) {
    const int percent = static_cast<int>(std::round(std::max(0.0f, volume) * 100.0f));
    return OsdContent{
        .kind = OsdKind::Volume,
        .icon = audioVolumeGlyph(volume, muted, false),
        .value = std::to_string(percent) + "%",
        .progress = std::clamp(volume, 0.0f, 1.0f),
        .overLimit = percent > 100,
        .inactive = muted,
    };
  }

  OsdContent makeOutputNameContent(std::string name, bool muted) {
    return OsdContent{
        .kind = OsdKind::Volume,
        .icon = audioVolumeGlyph(1.0f, muted, false),
        .value = std::move(name),
        .showProgress = false,
        .inactive = muted,
    };
  }

  OsdContent makeInputContent(float volume, bool muted) {
    const int percent = static_cast<int>(std::round(std::max(0.0f, volume) * 100.0f));
    return OsdContent{
        .kind = OsdKind::Microphone,
        .icon = audioVolumeGlyph(volume, muted, true),
        .value = std::to_string(percent) + "%",
        .progress = std::clamp(volume, 0.0f, 1.0f),
        .overLimit = percent > 100,
        .inactive = muted,
    };
  }

  OsdContent makeInputNameContent(std::string name, bool muted) {
    return OsdContent{
        .kind = OsdKind::Microphone,
        .icon = audioVolumeGlyph(1.0f, muted, true),
        .value = std::move(name),
        .showProgress = false,
        .inactive = muted,
    };
  }

} // namespace

void AudioOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }
void AudioOsd::setSoundPlayer(SoundPlayer* soundPlayer) { m_soundPlayer = soundPlayer; }

void AudioOsd::primeFromService(const PipeWireService& service) {
  if (const auto* sink = service.defaultSink(); sink != nullptr) {
    m_lastSinkId = sink->id;
    m_lastSinkVolume = sink->volume;
    m_lastSinkPercent = static_cast<int>(std::round(std::max(0.0f, sink->volume) * 100.0f));
    m_lastSinkMuted = sink->muted;
  }

  if (const auto* source = service.defaultSource(); source != nullptr) {
    m_lastSourceId = source->id;
    m_lastSourceVolume = source->volume;
    m_lastSourcePercent = static_cast<int>(std::round(std::max(0.0f, source->volume) * 100.0f));
    m_lastSourceMuted = source->muted;
  }
}

void AudioOsd::suppressFor(std::chrono::milliseconds duration) {
  m_suppressUntil = std::chrono::steady_clock::now() + duration;
}

void AudioOsd::showOutput(std::uint32_t sinkId, float volume, bool muted, bool playFeedback) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  m_suppressAutoInputOsdUntil = now + kSuppressInputOsdAfterOutput;
  if (m_overlay != nullptr) {
    m_overlay->show(makeOutputContent(volume, muted));
    m_currentKind = OsdKind::Volume;
  }
  if (playFeedback && m_soundPlayer != nullptr && now - m_lastSoundAt >= kVolumeSoundCooldown) {
    m_soundPlayer->play("volume-change");
    m_lastSoundAt = now;
  }
  m_lastSinkId = sinkId;
  m_lastSinkVolume = volume;
  m_lastSinkPercent = static_cast<int>(std::round(std::max(0.0f, volume) * 100.0f));
  m_lastSinkMuted = muted;
}

void AudioOsd::showOutputName(std::string name, bool muted) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  m_suppressAutoInputOsdUntil = now + kSuppressInputOsdAfterOutput;
  if (m_overlay != nullptr) {
    m_overlay->show(makeOutputNameContent(std::move(name), muted));
    m_currentKind = OsdKind::Volume;
  }
}

void AudioOsd::showInput(std::uint32_t sourceId, float volume, bool muted, bool playFeedback) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  if (m_overlay != nullptr) {
    m_overlay->show(makeInputContent(volume, muted));
    m_currentKind = OsdKind::Microphone;
  }
  if (playFeedback && m_soundPlayer != nullptr && now - m_lastSoundAt >= kVolumeSoundCooldown) {
    m_soundPlayer->play("volume-change");
    m_lastSoundAt = now;
  }
  m_lastSourceId = sourceId;
  m_lastSourceVolume = volume;
  m_lastSourcePercent = static_cast<int>(std::round(std::max(0.0f, volume) * 100.0f));
  m_lastSourceMuted = muted;
}

void AudioOsd::showInputName(std::string name, bool muted) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  if (m_overlay != nullptr) {
    m_overlay->show(makeInputNameContent(std::move(name), muted));
    m_currentKind = OsdKind::Microphone;
  }
}

void AudioOsd::showOutputValue(float volume, bool muted) {
  if (m_overlay == nullptr) {
    return;
  }
  m_overlay->show(makeOutputContent(volume, muted));
  m_currentKind = OsdKind::Volume;
}

void AudioOsd::showInputValue(float volume, bool muted) {
  if (m_overlay == nullptr) {
    return;
  }
  m_overlay->show(makeInputContent(volume, muted));
  m_currentKind = OsdKind::Microphone;
}

void AudioOsd::onAudioStateChanged(const PipeWireService& service) {
  const auto* sink = service.defaultSink();
  const auto* source = service.defaultSource();

  const std::uint32_t sinkId = sink != nullptr ? sink->id : 0;
  const std::string sinkName = sink != nullptr ? audioDeviceLabel(*sink) : "";
  const float sinkVolume = sink != nullptr ? sink->volume : 0.0f;
  const int sinkPercent = sink != nullptr ? static_cast<int>(std::round(std::max(0.0f, sinkVolume) * 100.0f)) : 0;
  const bool sinkMuted = sink != nullptr ? sink->muted : false;

  const std::uint32_t sourceId = source != nullptr ? source->id : 0;
  const std::string sourceName = source != nullptr ? audioDeviceLabel(*source) : "";
  const float sourceVolume = source != nullptr ? source->volume : 0.0f;
  const int sourcePercent = source != nullptr ? static_cast<int>(std::round(std::max(0.0f, sourceVolume) * 100.0f)) : 0;
  const bool sourceMuted = source != nullptr ? source->muted : false;

  const auto now = std::chrono::steady_clock::now();

  // The bar samples committed audio state live on every refresh; the OSD is a frozen snapshot fed
  // through suppressed/optimistic event paths, so the two can end up showing different mute icons.
  // Whenever committed mute changes for the node the OSD is currently displaying, force its icon
  // back in sync in place. Suppression only gates popping up / switching OSDs, not correcting the
  // one already on screen; volume stays optimistic, only the mute state is reconciled.
  if (m_overlay != nullptr && m_overlay->isVisible()) {
    if (sink != nullptr && m_currentKind == OsdKind::Volume && sinkMuted != m_lastSinkMuted) {
      m_overlay->show(makeOutputContent(sinkVolume, sinkMuted));
    } else if (source != nullptr && m_currentKind == OsdKind::Microphone && sourceMuted != m_lastSourceMuted) {
      m_overlay->show(makeInputContent(sourceVolume, sourceMuted));
    }
  }

  if (now < m_suppressUntil) {
    m_lastSinkId = sinkId;
    m_lastSinkVolume = sinkVolume;
    m_lastSinkPercent = sinkPercent;
    m_lastSinkMuted = sinkMuted;
    m_lastSourceId = sourceId;
    m_lastSourceVolume = sourceVolume;
    m_lastSourcePercent = sourcePercent;
    m_lastSourceMuted = sourceMuted;
    return;
  }

  const bool sinkVolumeChanged = sink != nullptr && volumeChanged(sinkVolume, m_lastSinkVolume);
  const bool sinkMuteChanged = sink != nullptr && sinkMuted != m_lastSinkMuted;
  const bool sinkRouteChanged = sink != nullptr && sinkId != m_lastSinkId;
  const bool sinkDisappeared = sink == nullptr && m_lastSinkId != 0;
  const bool sourceVolumeChanged = source != nullptr && volumeChanged(sourceVolume, m_lastSourceVolume);
  const bool sourceMuteChanged = source != nullptr && sourceMuted != m_lastSourceMuted;
  const bool sourceRouteChanged = source != nullptr && sourceId != m_lastSourceId;

  if (sinkRouteChanged || sinkDisappeared) {
    m_suppressAutoInputOsdUntil = now + std::chrono::milliseconds(400);
  }

  if (m_overlay != nullptr) {
    if (sinkRouteChanged) {
      showOutputName(sinkName, sinkMuted);
    } else if (sinkVolumeChanged || sinkMuteChanged) {
      // Passive PipeWire updates: click only on real volume changes while unmuted.
      showOutput(sink->id, sink->volume, sinkMuted, sinkVolumeChanged && !sinkMuted);
    } else if (sourceRouteChanged && now >= m_suppressAutoInputOsdUntil) {
      showInputName(sourceName, sourceMuted);
    } else if ((sourceVolumeChanged || sourceMuteChanged) && now >= m_suppressAutoInputOsdUntil) {
      showInput(source->id, source->volume, sourceMuted, sourceVolumeChanged && !sourceMuted);
    }
  }

  m_lastSinkId = sinkId;
  m_lastSinkVolume = sinkVolume;
  m_lastSinkPercent = sinkPercent;
  m_lastSinkMuted = sinkMuted;
  m_lastSourceId = sourceId;
  m_lastSourceVolume = sourceVolume;
  m_lastSourcePercent = sourcePercent;
  m_lastSourceMuted = sourceMuted;
}
