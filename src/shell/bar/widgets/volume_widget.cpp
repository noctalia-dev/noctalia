#include "shell/bar/widgets/volume_widget.h"

#include "config/config_types.h"
#include "pipewire/pipewire_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
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

  constexpr float kScrollStep = 0.05f;

} // namespace

VolumeWidget::VolumeWidget(
    PipeWireService* audio, const Config* config, wl_output* /*output*/, bool showLabel, VolumeWidgetTarget target
)
    : m_audio(audio), m_config(config), m_showLabel(showLabel), m_target(target) {}

void VolumeWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) { requestPanelToggle("control-center", "audio"); });
  area->setOnAxis([this](const InputArea::PointerData& data) {
    if (m_audio == nullptr) {
      return;
    }
    const auto* node = m_target == VolumeWidgetTarget::Input ? m_audio->defaultSource() : m_audio->defaultSink();
    if (node == nullptr) {
      return;
    }
    const float delta = data.scrollDelta(1.0f) > 0 ? -kScrollStep : kScrollStep;
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
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
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

  if (volume == m_lastVolume && muted == m_lastMuted && m_isVertical == m_lastVertical) {
    return;
  }

  m_lastVolume = volume;
  m_lastMuted = muted;
  m_lastVertical = m_isVertical;

  m_glyph->setGlyph(volumeGlyphName(volume, muted, m_target));
  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setColor(
      muted ? colorSpecFromRole(ColorRole::OnSurfaceVariant)
            : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
  );
  m_glyph->measure(renderer);

  m_label->setVisible(m_showLabel);
  if (m_showLabel) {
    int pct = static_cast<int>(std::round(volume * 100.0f));
    m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setText(m_isVertical ? std::to_string(pct) : std::to_string(pct) + "%");
    m_label->setColor(
        muted ? colorSpecFromRole(ColorRole::OnSurfaceVariant)
              : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
    );
    m_label->measure(renderer);
  }

  if (auto* rootNode = root(); rootNode != nullptr) {
    auto* area = static_cast<InputArea*>(rootNode);
    if (node != nullptr) {
      int pct = static_cast<int>(std::round(volume * 100.0f));
      std::vector<TooltipRow> rows;
      rows.push_back({m_target == VolumeWidgetTarget::Input ? "Mic" : "Volume", std::to_string(pct) + "%"});
      if (!node->description.empty()) {
        rows.push_back({"Device", node->description});
      }
      area->setTooltip(std::move(rows));
    } else {
      area->clearTooltip();
    }
  }

  requestRedraw();
}
