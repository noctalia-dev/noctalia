#include "shell/desktop/widgets/desktop_volume_widget.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "pipewire/audio_glyphs.h"
#include "pipewire/pipewire_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>

namespace {

  constexpr float kBaseWidth = 220.0f;
  constexpr float kBaseHeight = 120.0f;
  constexpr float kTextGap = 12.0f;
  constexpr float kShadowAlpha = 0.55f;
  constexpr float kShadowOffset = 1.5f;
  constexpr float kTrackAlphaScale = 0.45f;
  constexpr float kMinTextWidth = 72.0f;

  float percentFontSize(float layoutScale) { return Style::fontSizeTitle * 1.35f * layoutScale; }
  float statusFontSize(float layoutScale) { return Style::fontSizeBody * layoutScale; }
  float deviceFontSize(float layoutScale) { return Style::fontSizeCaption * layoutScale; }

} // namespace

DesktopVolumeWidget::DesktopVolumeWidget(PipeWireService* audio, Options options)
    : m_audio(audio), m_config(options.config), m_glyphOverride(std::move(options.glyph)),
      m_fillColor(options.fillColor), m_trackColor(options.trackColor), m_input(options.input),
      m_showDevice(options.showDevice), m_shadow(options.shadow),
      m_scrollStep(std::clamp(static_cast<float>(options.scrollStepPercent) / 100.0f, 0.01f, 0.25f)) {}

void DesktopVolumeWidget::create() {
  auto area = std::make_unique<InputArea>();
  // Whole card: scroll only. Mute is on the glyph hit target below.
  area->setAcceptedButtons(0);
  area->setOnAxis([this](const InputArea::PointerData& data) {
    const auto* node = currentNode();
    if (node == nullptr) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0f) {
      return;
    }
    const float newValue = std::clamp(node->volume - steps * m_scrollStep, 0.0f, maxVolume());
    if (m_input) {
      m_audio->setSourceVolume(node->id, newValue);
    } else {
      m_audio->setSinkVolume(node->id, newValue);
    }
  });

  area->addChild(
      ui::label({
          .out = &m_percentLabel,
          .fontSize = percentFontSize(contentScale()),
          .fontWeight = FontWeight::Bold,
          .color = m_fillColor,
          .textAlign = TextAlign::Start,
      })
  );
  area->addChild(
      ui::label({
          .out = &m_statusLabel,
          .fontSize = statusFontSize(contentScale()),
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .textAlign = TextAlign::Start,
      })
  );
  area->addChild(
      ui::label({
          .out = &m_deviceLabel,
          .fontSize = deviceFontSize(contentScale()),
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
          .textAlign = TextAlign::Start,
          .ellipsize = TextEllipsize::End,
          .visible = m_showDevice,
      })
  );

  auto glyphHit = std::make_unique<InputArea>();
  glyphHit->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
  glyphHit->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
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
  glyphHit->addChild(
      ui::glyph({
          .out = &m_trackGlyph,
          .glyph = "volume-high",
          .color = m_trackColor,
      })
  );
  auto fillClip = std::make_unique<Node>();
  fillClip->setClipChildren(true);
  m_fillClip = glyphHit->addChild(std::move(fillClip));
  m_fillClip->addChild(
      ui::glyph({
          .out = &m_fillGlyph,
          .glyph = "volume-high",
          .color = m_fillColor,
      })
  );
  m_glyphHit = static_cast<InputArea*>(area->addChild(std::move(glyphHit)));

  m_area = area.get();
  setRoot(std::move(area));
  applyColors();
  applyShadow();
}

void DesktopVolumeWidget::layout(Renderer& renderer) {
  // Boxed tiles fill the box directly. Aspect-fit letterboxing leaves a sharp inner content
  // rect inside the rounded background and looks broken when the user resizes freely.
  if (boxInnerWidth() <= 0.0f || boxInnerHeight() <= 0.0f) {
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
      applyColors();
      applyShadow();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "track_color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_trackColor = colorSpecFromConfigString(*v, key);
      applyColors();
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
      if (m_deviceLabel != nullptr) {
        m_deviceLabel->setVisible(m_showDevice);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      requestRedraw();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, {}, renderer);
}

void DesktopVolumeWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_percentLabel != nullptr) {
    m_percentLabel->setFontFamily(family);
  }
  if (m_statusLabel != nullptr) {
    m_statusLabel->setFontFamily(family);
  }
  if (m_deviceLabel != nullptr) {
    m_deviceLabel->setFontFamily(family);
  }
}

void DesktopVolumeWidget::doUpdate(Renderer& renderer) { syncState(renderer, false); }

void DesktopVolumeWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr
      || m_percentLabel == nullptr
      || m_statusLabel == nullptr
      || m_deviceLabel == nullptr
      || m_glyphHit == nullptr
      || m_trackGlyph == nullptr
      || m_fillClip == nullptr
      || m_fillGlyph == nullptr) {
    return;
  }

  syncState(renderer, false);

  const float innerW = boxInnerWidth();
  const float innerH = boxInnerHeight();
  const bool boxed = innerW > 0.0f && innerH > 0.0f;
  const float width = boxed ? innerW : kBaseWidth * contentScale();
  const float height = boxed ? innerH : kBaseHeight * contentScale();
  // Keep content clear of the outer rounded clip (padding alone is not enough near corners).
  const float edgeInset = boxed ? std::round(std::max({4.0f, height * 0.06f, backgroundRadius() * 0.35f})) : 0.0f;
  const float layoutW = std::max(1.0f, width - 2.0f * edgeInset);
  const float layoutH = std::max(1.0f, height - 2.0f * edgeInset);
  const float layoutScale = std::max(0.05f, layoutH / kBaseHeight);
  const float gap = kTextGap * layoutScale;

  float glyphBox = layoutH * 0.85f;
  const float minTextW = kMinTextWidth * layoutScale;
  glyphBox = std::min(glyphBox, std::max(layoutH * 0.35f, layoutW - minTextW - gap));
  glyphBox = std::round(glyphBox);
  m_glyphBox = glyphBox;
  const float textMaxW = std::max(1.0f, layoutW - glyphBox - gap);

  m_percentLabel->setFontSize(percentFontSize(layoutScale));
  m_percentLabel->setMaxWidth(textMaxW);
  m_percentLabel->measure(renderer);

  m_statusLabel->setFontSize(statusFontSize(layoutScale));
  m_statusLabel->setMaxWidth(textMaxW);
  m_statusLabel->measure(renderer);

  const bool showDevice = m_showDevice && !m_lastDeviceText.empty();
  m_deviceLabel->setVisible(showDevice);
  float deviceH = 0.0f;
  if (showDevice) {
    m_deviceLabel->setFontSize(deviceFontSize(layoutScale));
    m_deviceLabel->setMaxWidth(textMaxW);
    m_deviceLabel->setMaxLines(2);
    m_deviceLabel->measure(renderer);
    deviceH = m_deviceLabel->height();
  }

  const float textBlockH =
      m_percentLabel->height() + m_statusLabel->height() + (showDevice ? Style::spaceXs * layoutScale + deviceH : 0.0f);
  const float textY = edgeInset + std::max(0.0f, (layoutH - textBlockH) * 0.5f);

  m_percentLabel->setPosition(edgeInset, textY);
  m_statusLabel->setPosition(edgeInset, textY + m_percentLabel->height());
  if (showDevice) {
    m_deviceLabel->setPosition(
        edgeInset, textY + m_percentLabel->height() + m_statusLabel->height() + Style::spaceXs * layoutScale
    );
  }

  // Same square box + glyph size for track and fill so ink centers stay identical.
  m_trackGlyph->setGlyphSize(glyphBox);
  m_trackGlyph->setSize(glyphBox, glyphBox);
  m_trackGlyph->measure(renderer);
  m_fillGlyph->setGlyphSize(glyphBox);
  m_fillGlyph->setSize(glyphBox, glyphBox);
  m_fillGlyph->measure(renderer);

  const float glyphW = glyphBox;
  const float glyphH = glyphBox;
  const float glyphX = edgeInset + layoutW - glyphW;
  const float glyphY = edgeInset + (layoutH - glyphH) * 0.5f;

  m_glyphHit->setPosition(glyphX, glyphY);
  m_glyphHit->setFrameSize(glyphW, glyphH);
  m_trackGlyph->setPosition(0.0f, 0.0f);
  updateFillClip();

  root()->setSize(width, height);
  if (m_area != nullptr) {
    m_area->setFrameSize(width, height);
  }

  applyColors();
  applyShadow();
}

void DesktopVolumeWidget::syncState(Renderer& renderer, bool forceLayout) {
  float volume = 0.0f;
  bool muted = false;
  std::string deviceName;

  if (const auto* node = currentNode(); node != nullptr) {
    volume = node->volume;
    muted = node->muted;
    deviceName = audioDeviceLabel(*node);
  }

  const float displayVolume = muted ? 0.0f : std::clamp(volume, 0.0f, 1.0f);
  const int percent = static_cast<int>(std::round(std::max(0.0f, volume) * 100.0f));
  const std::string percentText = std::format("{}%", percent);
  const std::string statusText = muted
      ? i18n::tr("desktop-widgets.volume.muted")
      : i18n::tr(m_input ? "desktop-widgets.volume.mic" : "desktop-widgets.volume.volume");
  const std::string glyphName = resolveGlyphName(volume, muted);

  bool changed = forceLayout;
  if (percentText != m_lastPercentText) {
    m_lastPercentText = percentText;
    if (m_percentLabel != nullptr) {
      m_percentLabel->setText(m_lastPercentText);
    }
    changed = true;
  }
  if (statusText != m_lastStatusText) {
    m_lastStatusText = statusText;
    if (m_statusLabel != nullptr) {
      m_statusLabel->setText(m_lastStatusText);
    }
    changed = true;
  }
  if (deviceName != m_lastDeviceText) {
    m_lastDeviceText = std::move(deviceName);
    if (m_deviceLabel != nullptr) {
      m_deviceLabel->setText(m_lastDeviceText);
    }
    changed = true;
  }
  if (glyphName != m_lastGlyphName) {
    m_lastGlyphName = glyphName;
    if (m_trackGlyph != nullptr) {
      m_trackGlyph->setGlyph(m_lastGlyphName);
    }
    if (m_fillGlyph != nullptr) {
      m_fillGlyph->setGlyph(m_lastGlyphName);
    }
    changed = true;
  }

  const bool mutedChanged = muted != m_lastMuted;
  m_lastMuted = muted;
  if (std::abs(displayVolume - m_fillProgress) > 0.0005f || mutedChanged) {
    m_fillProgress = displayVolume;
    updateFillClip();
    requestRedraw();
  }

  if (changed) {
    if (!forceLayout && !isLayingOut()) {
      requestLayout();
    }
  }
  (void)renderer;
}

void DesktopVolumeWidget::updateFillClip() {
  if (m_fillClip == nullptr || m_fillGlyph == nullptr || m_glyphBox <= 0.0f) {
    return;
  }
  // Full-size fill glyph, revealed through a bottom-anchored clip (same as ProgressBar).
  const float progress = std::clamp(m_fillProgress, 0.0f, 1.0f);
  const float clipH = m_glyphBox * progress;
  const float clipY = m_glyphBox - clipH;
  m_fillClip->setPosition(0.0f, clipY);
  m_fillClip->setFrameSize(m_glyphBox, clipH);
  m_fillGlyph->setPosition(0.0f, -clipY);
}

void DesktopVolumeWidget::applyColors() {
  if (m_percentLabel != nullptr) {
    m_percentLabel->setColor(m_fillColor);
  }
  if (m_statusLabel != nullptr) {
    m_statusLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  }
  if (m_deviceLabel != nullptr) {
    m_deviceLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
  }
  if (m_trackGlyph != nullptr) {
    ColorSpec track = m_trackColor;
    track.alpha *= kTrackAlphaScale;
    m_trackGlyph->setColor(track);
  }
  if (m_fillGlyph != nullptr) {
    m_fillGlyph->setColor(m_fillColor);
  }
}

void DesktopVolumeWidget::applyShadow() {
  const Color shadowColor(0.0f, 0.0f, 0.0f, kShadowAlpha);
  const auto applyLabel = [&](Label* label) {
    if (label == nullptr) {
      return;
    }
    if (m_shadow) {
      label->setShadow(shadowColor, kShadowOffset, kShadowOffset);
    } else {
      label->clearShadow();
    }
  };
  applyLabel(m_percentLabel);
  applyLabel(m_statusLabel);
  applyLabel(m_deviceLabel);

  if (m_trackGlyph != nullptr) {
    if (m_shadow) {
      m_trackGlyph->setShadow(shadowColor, kShadowOffset, kShadowOffset);
    } else {
      m_trackGlyph->clearShadow();
    }
  }
  if (m_fillGlyph != nullptr) {
    m_fillGlyph->clearShadow();
  }
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
  return 1.0f;
}
