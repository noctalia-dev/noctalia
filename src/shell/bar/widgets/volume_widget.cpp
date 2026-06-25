#include "shell/bar/widgets/volume_widget.h"

#include "config/config_types.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/easyeffects_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

  const char* volumeGlyphName(float volume, bool muted, VolumeWidgetTarget target) {
    if (target == VolumeWidgetTarget::Input) {
      return muted ? "microphone-mute" : "microphone";
    }

    if (muted || volume <= 0.0f) {
      return "volume-mute";
    }
    if (volume < 0.4f) {
      return "volume-low";
    }
    return "volume-high";
  }

} // namespace

VolumeWidget::VolumeWidget(
    PipeWireService* audio, EasyEffectsService* easyEffects, const Config* config, wl_output* /*output*/,
    bool showLabel, VolumeWidgetTarget target, int scrollStepPercent, ColorSpec muteColor
)
    : m_audio(audio), m_easyEffects(easyEffects), m_config(config), m_showLabel(showLabel),
      m_scrollStep(static_cast<float>(scrollStepPercent) / 100.0f), m_target(target), m_muteColor(muteColor) {}

void VolumeWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      requestPanelToggle("control-center", "audio");
      return;
    }
    if (data.button != BTN_RIGHT || m_audio == nullptr) {
      return;
    }
    const auto* node = m_target == VolumeWidgetTarget::Input ? m_audio->defaultSource() : m_audio->defaultSink();
    if (node == nullptr) {
      return;
    }
    if (m_target == VolumeWidgetTarget::Input) {
      m_audio->setSourceMuted(node->id, !node->muted);
    } else {
      m_audio->setSinkMuted(node->id, !node->muted);
    }
  });
  area->setOnAxis([this](const InputArea::PointerData& data) {
    if (m_audio == nullptr) {
      return;
    }
    const auto* node = m_target == VolumeWidgetTarget::Input ? m_audio->defaultSource() : m_audio->defaultSink();
    if (node == nullptr) {
      return;
    }
    const float delta = data.scrollDelta(1.0f) > 0 ? -m_scrollStep : m_scrollStep;
    const float maxVolume = (m_config != nullptr && m_config->audio.enableOverdrive) ? 1.5f : 1.0f;
    const float newValue = std::clamp(node->volume + delta, 0.0f, maxVolume);
    if (m_target == VolumeWidgetTarget::Input) {
      m_audio->setSourceVolume(node->id, newValue);
    } else {
      m_audio->setSinkVolume(node->id, newValue);
    }
  });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "volume-high",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontFamily = labelFontFamily(),
          .fontWeight = labelFontWeight(),
          .visible = m_showLabel,
      })
  );

  setRoot(std::move(area));
}

void VolumeWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (m_glyph == nullptr || m_label == nullptr || rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);

  m_glyph->measure(renderer);
  if (m_label->visible()) {
    m_label->measure(renderer);
  }

  const bool labelVisible = m_label->visible();
  if (m_isVertical && labelVisible) {
    const float w = std::max(m_glyph->width(), m_label->width());
    m_glyph->setPosition(std::round((w - m_glyph->width()) * 0.5f), 0.0f);
    m_label->setPosition(std::round((w - m_label->width()) * 0.5f), m_glyph->height());
    rootNode->setSize(w, m_glyph->height() + m_label->height());
  } else {
    const float h = labelVisible ? std::max(m_glyph->height(), m_label->height()) : m_glyph->height();
    m_glyph->setPosition(0.0f, std::round((h - m_glyph->height()) * 0.5f));
    float totalWidth = m_glyph->width();
    if (labelVisible) {
      m_label->setPosition(m_glyph->width() + Style::spaceXs, std::round((h - m_label->height()) * 0.5f));
      totalWidth = m_label->x() + m_label->width();
    }
    rootNode->setSize(totalWidth, h);
  }
}

void VolumeWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void VolumeWidget::syncState(Renderer& renderer) {
  if (m_audio == nullptr || m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  const auto* node = m_target == VolumeWidgetTarget::Input ? m_audio->defaultSource() : m_audio->defaultSink();
  float volume = node != nullptr ? node->volume : 0.0f;
  bool muted = node != nullptr ? node->muted : false;
  const auto kind =
      m_target == VolumeWidgetTarget::Input ? AudioEffectsProfileKind::Input : AudioEffectsProfileKind::Output;
  const std::string effectsProfile =
      m_easyEffects != nullptr ? m_easyEffects->activeEffectsProfile(kind) : std::string{};

  if (volume == m_lastVolume
      && muted == m_lastMuted
      && m_isVertical == m_lastVertical
      && effectsProfile == m_lastEffectsProfile) {
    return;
  }

  m_lastVolume = volume;
  m_lastMuted = muted;
  m_lastVertical = m_isVertical;
  m_lastEffectsProfile = effectsProfile;

  m_glyph->setGlyph(volumeGlyphName(volume, muted, m_target));
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(muted ? m_muteColor : widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);

  m_label->setVisible(m_showLabel);
  if (m_showLabel) {
    int pct = static_cast<int>(std::round(volume * 100.0f));
    m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setText(m_isVertical ? std::to_string(pct) : std::to_string(pct) + "%");
    m_label->setColor(muted ? m_muteColor : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  if (auto* rootNode = root(); rootNode != nullptr) {
    auto* area = static_cast<InputArea*>(rootNode);
    if (node != nullptr) {
      int pct = static_cast<int>(std::round(volume * 100.0f));
      std::vector<TooltipRow> rows;
      rows.push_back(
          {i18n::tr(m_target == VolumeWidgetTarget::Input ? "bar.widgets.volume.mic" : "bar.widgets.volume.volume"),
           std::to_string(pct) + "%"}
      );
      if (!node->description.empty()) {
        rows.push_back({i18n::tr("bar.widgets.volume.device"), node->description});
      }
      if (!effectsProfile.empty()) {
        rows.push_back({i18n::tr("control-center.audio.effects-profile"), effectsProfile});
      }
      area->setTooltip(std::move(rows));
    } else {
      area->clearTooltip();
    }
  }

  requestRedraw();
}
