#include "sysmon_widget.h"

#include "render/core/renderer.h"
#include "render/text/glyph_registry.h"
#include "ui/builders.h"
#include "ui/controls/progress_bar.h"
#include "ui/style.h"
#include "visuals/graph.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
  constexpr float kBaseWidth = 180.0F;
  constexpr float kBaseHeight = 80.0F;
  constexpr float kGraphLineWidth = 0.75F;
  ColorSpec gaugeTrackColor(const ColorSpec& fill) {
    ColorSpec track = fill;
    track.alpha *= 0.3F;
    return track;
  }

  struct GlyphInkBounds {
    float left = 0.0F;
    float right = 0.0F;
    float width() const noexcept { return right - left; }
  };

  GlyphInkBounds glyphHorizontalInkBounds(float boxWidth, const TextMetrics& metrics) {
    const float halfSpan = (metrics.right - metrics.left) * 0.5F;
    return {
        .left = boxWidth * 0.5F - halfSpan,
        .right = boxWidth * 0.5F + halfSpan,
    };
  }

  TextMetrics glyphMetricsFor(Renderer& renderer, const char* glyphName, float glyphSize) {
    const char32_t codepoint = GlyphRegistry::lookup(glyphName);
    if (codepoint == 0) {
      return {};
    }
    return renderer.measureGlyph(codepoint, glyphSize);
  }

} // namespace

DesktopSysmon::DesktopSysmon(const Options& options)
    : m_glyphName(options.glyph), m_hasSecond(!options.glyph2.empty()), m_displayMode(options.displayMode),
      m_gaugeLayout(options.gaugeLayout), m_lineColor(options.lineColor), m_lineColor2(options.lineColor2),
      m_showLabel(options.showLabel), m_labelMinWidth(options.labelMinWidth), m_shadow(options.shadow) {
  auto glyph = ui::glyph({
      .out = &m_glyph,
      .glyph = options.glyph,
  });
  addChild(std::move(glyph));

  if (m_displayMode == DesktopSysmonDisplayMode::Graph) {
    auto graph = std::make_unique<Graph>();
    graph->setLineWidth(kGraphLineWidth);
    graph->setFillOpacity(0.2F);
    m_graph = static_cast<Graph*>(addChild(std::move(graph)));

    if (m_hasSecond) {
      auto glyph2 = ui::glyph({
          .out = &m_glyph2,
          .glyph = options.glyph2,
      });
      addChild(std::move(glyph2));
    }
  } else {
    m_gauge = static_cast<ProgressBar*>(addChild(
        ui::progressBar({
            .fill = m_lineColor,
            .track = gaugeTrackColor(m_lineColor),
            .progress = 0.0F,
        })
    ));
  }

  if (m_showLabel) {
    const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, 0.5F);
    auto label = ui::label({
        .out = &m_label,
        .fontWeight = FontWeight::Medium,
        .minWidth = m_labelMinWidth > 0.0F ? std::optional<float>{m_labelMinWidth} : std::nullopt,
    });
    if (m_shadow) {
      label->setShadow(shadow, 0.0F, 1.0F);
    }
    addChild(std::move(label));

    if (m_displayMode == DesktopSysmonDisplayMode::Graph && m_hasSecond) {
      auto label2 = ui::label({
          .out = &m_label2,
          .fontWeight = FontWeight::Medium,
      });
      if (m_shadow) {
        label2->setShadow(shadow, 0.0F, 1.0F);
      }
      addChild(std::move(label2));
    }
  }

  m_valueColor = resolveColorSpec(m_lineColor);
}

void DesktopSysmon::setAppearance(
    DesktopSysmonGaugeLayout layout, const ColorSpec& color, const ColorSpec& color2, float minWidth, bool shadow
) {
  const bool layoutChanged = m_gaugeLayout != layout || m_labelMinWidth != minWidth || m_shadow != shadow;
  m_gaugeLayout = layout;
  m_lineColor = color;
  m_lineColor2 = color2;
  if (m_label != nullptr && m_labelMinWidth != minWidth) {
    m_label->setMinWidth(minWidth > 0.0F ? minWidth * m_contentScale : 0.0F);
  }
  m_labelMinWidth = minWidth;
  m_shadow = shadow;
  applyShadow();
  if (layoutChanged) {
    markLayoutDirty();
  }
}

void DesktopSysmon::setText(const std::string& text, const std::string& text2) {
  if (m_label != nullptr) {
    m_label->setText(text);
  }
  if (m_label2 != nullptr) {
    m_label2->setText(text2);
  }
}

void DesktopSysmon::setGauge(double progress, const Color& color) {
  m_progress = progress;
  m_valueColor = color;
  syncGaugeProgress();
  m_glyph->setColor(color);
  if (m_label != nullptr) {
    m_label->setColor(color);
  }
  if (m_gauge != nullptr) {
    m_gauge->setFill(color);
    m_gauge->setTrack(gaugeTrackColor(m_lineColor));
  }
}

void DesktopSysmon::setHistory(Renderer& renderer, std::vector<float> primary, std::vector<float> secondary) {
  if (m_graph == nullptr) {
    return;
  }
  m_graph->setValues(std::move(primary));
  m_graph->setValues2(std::move(secondary));
  m_graph->sync(renderer);
}

void DesktopSysmon::clearHistory() {
  if (m_graph != nullptr) {
    m_graph->setValues({});
    m_graph->setValues2({});
  }
}

void DesktopSysmon::setScroll(float progress) {
  if (m_graph != nullptr) {
    m_graph->setScroll(progress);
  }
}

void DesktopSysmon::setContentScale(float scale) {
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void DesktopSysmon::applyShadow() {
  const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, 0.5F);
  for (Glyph* glyph : {m_glyph, m_glyph2}) {
    if (glyph != nullptr) {
      if (m_shadow) {
        glyph->setShadow(shadow, 0.0F, 1.0F);
      } else {
        glyph->clearShadow();
      }
    }
  }
  for (Label* label : {m_label, m_label2}) {
    if (label != nullptr) {
      if (m_shadow) {
        label->setShadow(shadow, 0.0F, 1.0F);
      } else {
        label->clearShadow();
      }
    }
  }
}

void DesktopSysmon::setFontFamily(const std::string& family) {
  for (Label* label : {m_label, m_label2}) {
    if (label != nullptr) {
      label->setFontFamily(family);
    }
  }
}

void DesktopSysmon::syncGaugeProgress() {
  if (m_gauge == nullptr) {
    return;
  }

  const bool stacked = m_gaugeLayout == DesktopSysmonGaugeLayout::Vertical;
  const float fillAxis = stacked ? m_gauge->width() : m_gauge->height();
  const float progress = (fillAxis > 0.0F && m_progress * fillAxis < 1.0F) ? 0.0F : static_cast<float>(m_progress);
  m_gauge->setProgress(progress);
}

void DesktopSysmon::doLayout(Renderer& renderer) {
  if (m_glyph == nullptr) {
    return;
  }

  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    layoutGaugeMode(renderer);
    return;
  }
  layoutGraphMode(renderer);
}

void DesktopSysmon::layoutGaugeMode(Renderer& renderer) {
  const float scale = m_contentScale;
  const float gap = Style::spaceXs * scale;
  const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, 0.5F);
  const bool stacked = m_gaugeLayout == DesktopSysmonGaugeLayout::Vertical;

  m_glyph->setGlyphSize(Style::baseGlyphSize * scale);
  if (m_shadow) {
    m_glyph->setShadow(shadow, 0.0F, 1.0F);
  }
  m_glyph->measure(renderer);
  const float glyphH = m_glyph->height();

  if (m_label != nullptr) {
    m_label->setFontSize((stacked ? Style::fontSizeCaption : Style::fontSizeBody) * scale);
    m_label->setMinWidth(m_labelMinWidth > 0.0F ? m_labelMinWidth * scale : 0.0F);
    m_label->measure(renderer);
  }
  const float labelW = m_label != nullptr ? m_label->width() : 0.0F;
  const float labelH = m_label != nullptr ? m_label->height() : 0.0F;

  if (m_gauge == nullptr) {
    return;
  }

  const float baseSize = Style::fontSizeBody * scale;
  const float gaugeStem = std::round(baseSize * 0.85F);
  const float gaugeThickness = std::max(3.0F, roundf(baseSize * 0.3F));
  const float glyphSize = Style::baseGlyphSize * scale;
  const TextMetrics glyphMetrics = glyphMetricsFor(renderer, m_glyphName.c_str(), glyphSize);
  const GlyphInkBounds ink = glyphHorizontalInkBounds(m_glyph->width(), glyphMetrics);

  if (stacked) {
    m_gauge->setOrientation(ProgressBarOrientation::Horizontal);
    const float trackW = std::max(ink.width(), gaugeStem);
    const float trackH = gaugeThickness;
    m_gauge->setRadius(trackH / 2.0F);
    float contentW = trackW;
    if (m_label != nullptr) {
      contentW = std::max(contentW, labelW);
    }
    const float glyphX = std::round((contentW - ink.width()) * 0.5F - ink.left);
    m_glyph->setPosition(glyphX, 0.0F);
    m_gauge->setPosition(std::round((contentW - trackW) * 0.5F), glyphH + gap);
    m_gauge->setSize(trackW, trackH);
    float totalH = glyphH + gap + trackH;
    if (m_label != nullptr) {
      m_label->setPosition(std::round((contentW - labelW) * 0.5F), totalH + gap);
      totalH += gap + labelH;
    }
    setSize(contentW, totalH);
  } else {
    m_gauge->setOrientation(ProgressBarOrientation::Vertical);
    const float gaugeW = gaugeThickness;
    const float gaugeH = gaugeStem;
    m_gauge->setRadius(gaugeW / 2.0F);
    float contentH = std::max(glyphH, gaugeH);
    if (m_label != nullptr) {
      contentH = std::max(contentH, labelH);
    }
    const float gaugeY = std::round((contentH - gaugeH) * 0.5F);
    const float glyphX = std::round(std::max(0.0F, -ink.left));
    m_glyph->setPosition(glyphX, std::round((contentH - glyphH) * 0.5F));
    const float gaugeX = std::round(glyphX + ink.right + gap);
    m_gauge->setPosition(gaugeX, gaugeY);
    m_gauge->setSize(gaugeW, gaugeH);
    float totalW = gaugeX + gaugeW;
    if (m_label != nullptr) {
      m_label->setPosition(totalW + gap, std::round((contentH - labelH) * 0.5F));
      totalW = m_label->x() + labelW;
    }
    setSize(totalW, contentH);
  }

  setGauge(m_progress, m_valueColor);
}

void DesktopSysmon::layoutGraphMode(Renderer& renderer) {
  const float scale = m_contentScale;
  const float fontSize = Style::fontSizeCaption * scale;
  const float glyphSize = Style::baseGlyphSize * scale;
  const float groupGap = Style::spaceXs * scale;
  const float legendGap = Style::spaceMd * scale;
  const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, 0.5F);

  m_graph->setColor(m_lineColor);
  if (m_hasSecond) {
    m_graph->setColor2(m_lineColor2);
  }
  m_graph->setLineWidth(kGraphLineWidth * scale);

  auto measureGroup = [&](Glyph* glyph, Label* label, const ColorSpec& color, float& width, float& height) {
    glyph->setGlyphSize(glyphSize);
    glyph->setColor(color);
    if (m_shadow) {
      glyph->setShadow(shadow, 0.0F, 1.0F);
    }
    glyph->measure(renderer);
    width = glyph->width();
    height = glyph->height();
    if (label != nullptr) {
      label->setFontSize(fontSize);
      label->setColor(color);
      label->measure(renderer);
      width += groupGap + label->width();
      height = std::max(height, label->height());
    }
  };

  float w1 = 0.0F, h1 = 0.0F;
  measureGroup(m_glyph, m_label, m_lineColor, w1, h1);

  float w2 = 0.0F, h2 = 0.0F;
  if (m_glyph2 != nullptr) {
    measureGroup(m_glyph2, m_label2, m_lineColor2, w2, h2);
  }

  const float totalW = kBaseWidth * scale;
  const float chartH = kBaseHeight * scale;

  const float headerW = (m_glyph2 != nullptr) ? (w1 + legendGap + w2) : w1;
  const float headerH = std::max(h1, h2);
  const float contentW = std::max(totalW, headerW);

  m_graph->setPosition(0.0F, 0.0F);
  m_graph->setSize(contentW, chartH);
  m_graph->sync(renderer);

  const float headerY = chartH + Style::spaceSm * scale;
  float x = std::round((contentW - headerW) * 0.5F);

  auto placeGroup = [&](Glyph* glyph, Label* label) {
    glyph->setPosition(x, headerY + std::round((headerH - glyph->height()) * 0.5F));
    x += glyph->width();
    if (label != nullptr) {
      x += groupGap;
      label->setPosition(x, headerY + std::round((headerH - label->height()) * 0.5F));
      x += label->width();
    }
  };

  placeGroup(m_glyph, m_label);
  if (m_glyph2 != nullptr) {
    x += legendGap;
    placeGroup(m_glyph2, m_label2);
  }

  setSize(contentW, headerY + headerH);
}
