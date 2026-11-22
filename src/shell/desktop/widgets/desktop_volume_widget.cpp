#include "shell/desktop/widgets/desktop_volume_widget.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "pipewire/audio_glyphs.h"
#include "pipewire/pipewire_service.h"
#include "volume_widget.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <utility>

DesktopVolumeWidget::DesktopVolumeWidget(PipeWireService* audio, Options options)
    : m_audio(audio), m_config(options.config), m_glyphOverride(std::move(options.glyph)),
      m_fillColor(options.fillColor), m_trackColor(options.trackColor), m_input(options.input),
      m_showDevice(options.showDevice), m_shadow(options.shadow),
      m_scrollStep(std::clamp(static_cast<float>(options.scrollStepPercent) / 100.0F, 0.01F, 0.25F)) {}

void DesktopVolumeWidget::create() {
  auto widget = std::make_unique<DesktopVolume>();
  m_widget = widget.get();
  widget->setOnScroll([this](float steps) {
    const auto* node = currentNode();
    if (node == nullptr || steps == 0.0F) {
      return;
    }
    const float value = std::clamp(node->volume - steps * m_scrollStep, 0.0F, maxVolume());
    if (m_input) {
      m_audio->setSourceVolume(node->id, value);
    } else {
      m_audio->setSinkVolume(node->id, value);
    }
  });
  widget->setOnToggleMute([this] {
    const auto* node = currentNode();
    if (node == nullptr) {
      return;
    }
    if (m_input) {
      m_audio->setSourceMuted(node->id, !node->muted);
    } else {
      m_audio->setSinkMuted(node->id, !node->muted);
    }
  });
  setRoot(std::move(widget));
  syncAppearance();
}
void DesktopVolumeWidget::layout(Renderer& renderer) {
  // Boxed tiles fill the box directly. Aspect-fit letterboxing leaves a sharp inner content
  // rect inside the rounded background and looks broken when the user resizes freely.
  if (boxInnerWidth() <= 0.0F || boxInnerHeight() <= 0.0F) {
    DesktopWidget::layout(renderer);
    return;
  }

  UiPhaseScope layoutPhase(UiPhase::Layout);
  m_inLayout = true;
  struct InLayoutReset {
    bool& flag;
    ~InLayoutReset() { flag = false; }
  } inLayoutReset{m_inLayout};

  onFontFamilyChanged(m_fontFamily, renderer);
  m_contentScale = m_baseScale;
  doLayout(renderer);
  applyBackground();
}
bool DesktopVolumeWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& /*allSettings*/, Renderer& renderer
) {
  if (key == "glyph") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_glyphOverride = *v;
      syncState(renderer, true);
      return true;
    }
    return false;
  }
  if (key == "fill_color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_fillColor = colorSpecFromConfigString(*v, key);
      syncAppearance();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "track_color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_trackColor = colorSpecFromConfigString(*v, key);
      syncAppearance();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "device") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      const bool input = *v == "input";
      if (input != m_input) {
        m_input = input;
        syncState(renderer, true);
      }
      return true;
    }
    return false;
  }
  if (key == "show_device") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showDevice = *v;
      syncAppearance();
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      syncAppearance();
      requestRedraw();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, {}, renderer);
}
void DesktopVolumeWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  if (m_widget != nullptr) {
    m_widget->setFontFamily(family);
  }
}
void DesktopVolumeWidget::syncAppearance() {
  if (m_widget != nullptr) {
    m_widget->setAppearance(m_fillColor, m_trackColor, m_showDevice, m_shadow);
  }
}
void DesktopVolumeWidget::doUpdate(Renderer& renderer) { syncState(renderer, false); }
void DesktopVolumeWidget::doLayout(Renderer& renderer) {
  if (m_widget == nullptr) {
    return;
  }
  syncState(renderer, false);
  m_widget->setContentScale(contentScale());
  m_widget->setViewport(boxInnerWidth(), boxInnerHeight(), backgroundRadius());
  m_widget->layout(renderer);
}
void DesktopVolumeWidget::syncState(Renderer& renderer, bool forceLayout) {
  float volume = 0.0F;
  bool muted = false;
  std::string deviceName;

  if (const auto* node = currentNode(); node != nullptr) {
    volume = node->volume;
    muted = node->muted;
    deviceName = audioDeviceLabel(*node);
  }

  const float displayVolume = muted ? 0.0F : std::clamp(volume, 0.0F, 1.0F);
  const int percent = static_cast<int>(std::round(std::max(0.0F, volume) * 100.0F));
  const std::string percentText = std::format("{}%", percent);
  const std::string statusText = muted
      ? i18n::tr("desktop-widgets.volume.muted")
      : i18n::tr(m_input ? "desktop-widgets.volume.mic" : "desktop-widgets.volume.volume");
  const std::string glyphName = resolveGlyphName(volume, muted);

  bool changed = forceLayout;
  if (percentText != m_lastPercentText) {
    m_lastPercentText = percentText;

    changed = true;
  }
  if (statusText != m_lastStatusText) {
    m_lastStatusText = statusText;

    changed = true;
  }
  if (deviceName != m_lastDeviceText) {
    m_lastDeviceText = std::move(deviceName);

    changed = true;
  }
  if (glyphName != m_lastGlyphName) {
    m_lastGlyphName = glyphName;

    changed = true;
  }

  const bool mutedChanged = muted != m_lastMuted;
  m_lastMuted = muted;
  if (std::abs(displayVolume - m_fillProgress) > 0.0005F || mutedChanged) {
    m_fillProgress = displayVolume;

    requestRedraw();
  }

  if (m_widget != nullptr) {
    m_widget->setData(
        {.glyph = m_lastGlyphName,
         .percent = m_lastPercentText,
         .status = m_lastStatusText,
         .device = m_lastDeviceText,
         .progress = m_fillProgress}
    );
  }
  if (changed && !forceLayout && !isLayingOut()) {
    requestLayout();
  }

  (void)renderer;
}
std::string DesktopVolumeWidget::resolveGlyphName(float volume, bool muted) const {
  if (!m_glyphOverride.empty()) {
    return m_glyphOverride;
  }
  return audioVolumeGlyph(volume, muted, m_input);
}
const AudioNode* DesktopVolumeWidget::currentNode() const noexcept {
  if (m_audio == nullptr) {
    return nullptr;
  }
  return m_input ? m_audio->defaultSource() : m_audio->defaultSink();
}
float DesktopVolumeWidget::maxVolume() const {
  if (m_config != nullptr) {
    return maxAudioVolume(m_config->config().audio);
  }
  return 1.0F;
}
