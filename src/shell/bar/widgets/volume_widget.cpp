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
#include <string_view>
#include <utility>

VolumeWidget::VolumeWidget(PipeWireService* audio, EasyEffectsService* easyEffects, Options options)
    : m_audio(audio), m_easyEffects(easyEffects), m_showLabel(options.showLabel), m_target(options.device),
      m_muteColor(options.muteColor), m_glyphOverride(std::move(options.glyph)),
      m_muteGlyphOverride(std::move(options.muteGlyph)),
      m_effectsProfileGlyphs(std::move(options.effectsProfileGlyphs)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)) {}

void VolumeWidget::create() {
  auto area = ui::inputArea({});

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = glyphName(1.0F, false),
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = m_showLabel,
      })
  );

  setRoot(std::move(area));
}

void VolumeWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if ((m_glyph == nullptr && m_image == nullptr) || m_label == nullptr || rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  syncState(renderer);

  const float iconW = m_image != nullptr ? m_image->width() : m_glyph->width();
  const float iconH = m_image != nullptr ? m_image->height() : m_glyph->height();
  if (m_label->visible()) {
    m_label->measure(renderer);
  }

  const bool labelVisible = m_label->visible();
  if (m_isVertical && labelVisible) {
    const float w = std::max(iconW, m_label->width());
    if (m_image != nullptr) {
      m_image->setPosition(std::round((w - iconW) * 0.5F), 0.0F);
    } else {
      m_glyph->setPosition(std::round((w - iconW) * 0.5F), 0.0F);
    }
    m_label->setPosition(std::round((w - m_label->width()) * 0.5F), iconH);
    rootNode->setSize(w, iconH + m_label->height());
  } else {
    const float h = labelVisible ? std::max(iconH, m_label->height()) : iconH;
    if (m_image != nullptr) {
      m_image->setPosition(0.0F, std::round((h - iconH) * 0.5F));
    } else {
      m_glyph->setPosition(0.0F, std::round((h - iconH) * 0.5F));
    }
    float totalWidth = iconW;
    if (labelVisible) {
      m_label->setPosition(iconW + Style::spaceXs, std::round((h - m_label->height()) * 0.5F));
      totalWidth = m_label->x() + m_label->width();
    }
    rootNode->setSize(totalWidth, h);
  }
}

void VolumeWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void VolumeWidget::syncState(Renderer& renderer) {
  if (m_audio == nullptr || (m_glyph == nullptr && m_image == nullptr) || m_label == nullptr) {
    return;
  }

  const auto* node = m_target == VolumeWidgetTarget::Input ? m_audio->defaultSource() : m_audio->defaultSink();
  float volume = node != nullptr ? node->volume : 0.0F;
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

  if (m_image != nullptr) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale,
        muted ? m_muteColor : widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
  } else {
    m_glyph->setGlyph(glyphName(volume, muted, effectsProfile));
    m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    m_glyph->setColor(muted ? m_muteColor : widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
  }

  m_label->setVisible(m_showLabel);
  if (m_showLabel) {
    int pct = static_cast<int>(std::round(volume * 100.0F));
    m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setText(m_isVertical ? std::to_string(pct) : std::to_string(pct) + "%");
    m_label->setColor(muted ? m_muteColor : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  if (auto* rootNode = root(); rootNode != nullptr) {
    auto* area = static_cast<InputArea*>(rootNode);
    if (node != nullptr) {
      int pct = static_cast<int>(std::round(volume * 100.0F));
      std::vector<TooltipRow> rows;
      rows.push_back(
          {i18n::tr(m_target == VolumeWidgetTarget::Input ? "bar.widgets.volume.mic" : "bar.widgets.volume.volume"),
           std::to_string(pct) + "%"}
      );
      if (std::string deviceLabel = audioDeviceLabel(*node); !deviceLabel.empty()) {
        rows.push_back({i18n::tr("bar.widgets.volume.device"), std::move(deviceLabel), TextEllipsize::Middle});
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

std::string VolumeWidget::glyphName(float volume, bool muted, const std::string& effectsProfile) const {
  const char* dynamicGlyph = audioVolumeGlyph(volume, muted, m_target == VolumeWidgetTarget::Input);
  const std::string_view muteGlyph = m_target == VolumeWidgetTarget::Input ? "microphone-mute" : "volume-mute";
  const bool usingMuteGlyph = std::string_view{dynamicGlyph} == muteGlyph;
  if (usingMuteGlyph && !m_muteGlyphOverride.empty()) {
    return m_muteGlyphOverride;
  }
  if (!usingMuteGlyph && !effectsProfile.empty()) {
    const auto profileGlyph = m_effectsProfileGlyphs.find(effectsProfile);
    if (profileGlyph != m_effectsProfileGlyphs.end() && !profileGlyph->second.empty()) {
      return profileGlyph->second;
    }
  }
  if (!usingMuteGlyph && !m_glyphOverride.empty()) {
    return m_glyphOverride;
  }
  return dynamicGlyph;
}
