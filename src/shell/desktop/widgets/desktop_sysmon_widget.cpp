#include "shell/desktop/widgets/desktop_sysmon_widget.h"

#include "config/config_service.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/text/glyph_registry.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/graph.h"
#include "ui/controls/progress_bar.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

namespace {

  constexpr float kBaseWidth = 180.0f;
  constexpr float kBaseHeight = 80.0f;
  constexpr float kGraphLineWidth = 0.75f;
  constexpr double kBytesPerMb = 1000.0 * 1000.0;

  [[nodiscard]] ColorSpec gaugeTrackColor(const ColorSpec& fill) {
    ColorSpec track = fill;
    track.alpha *= 0.3f;
    return track;
  }

  [[nodiscard]] double gradientFactor(double value, double activityThreshold, double criticalThreshold) {
    constexpr double kActivityOnset = 0.25;
    const double clampedValue = std::max(value, 0.0);
    const double clampedCritical = std::max(criticalThreshold, 0.0);
    if (clampedCritical <= 0.0 || clampedValue <= 0.0) {
      return 0.0;
    }

    const double clampedActivity = std::clamp(activityThreshold, 0.0, clampedCritical);
    if (clampedValue <= clampedActivity) {
      return 0.0;
    }
    if (clampedValue >= clampedCritical) {
      return 1.0;
    }
    const double t = (clampedValue - clampedActivity) / (clampedCritical - clampedActivity);
    return kActivityOnset + (1.0 - kActivityOnset) * t;
  }

  bool needsCpuTemp(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::CpuTemp; }
  bool needsGpuTemp(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::GpuTemp; }
  bool needsGpuUsage(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::GpuUsage; }
  bool needsGpuVram(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::GpuVram; }

  struct GlyphInkBounds {
    float left = 0.0f;
    float right = 0.0f;
    [[nodiscard]] float width() const noexcept { return right - left; }
  };

  [[nodiscard]] GlyphInkBounds glyphHorizontalInkBounds(float boxWidth, const TextMetrics& metrics) {
    const float halfSpan = (metrics.right - metrics.left) * 0.5f;
    return {
        .left = boxWidth * 0.5f - halfSpan,
        .right = boxWidth * 0.5f + halfSpan,
    };
  }

  [[nodiscard]] TextMetrics glyphMetricsFor(Renderer& renderer, const char* glyphName, float glyphSize) {
    const char32_t codepoint = GlyphRegistry::lookup(glyphName);
    if (codepoint == 0) {
      return {};
    }
    return renderer.measureGlyph(codepoint, glyphSize);
  }

} // namespace

DesktopSysmonWidget::DesktopSysmonWidget(SystemMonitorService* monitor, Options options)
    : m_monitor(monitor), m_config(options.config), m_stat(options.stat), m_stat2(options.stat2),
      m_displayMode(options.displayMode), m_gaugeLayout(options.gaugeLayout), m_lineColor(options.lineColor),
      m_lineColor2(options.lineColor2), m_highlightColor(options.highlightColor),
      m_networkInterface(std::move(options.networkInterface)), m_showLabel(options.showLabel),
      m_labelMinWidth(options.labelMinWidth), m_shadow(options.shadow) {
  if (m_monitor != nullptr) {
    if (needsCpuTemp(m_stat))
      m_monitor->retainCpuTemp();
    if (needsGpuTemp(m_stat))
      m_monitor->retainGpuTemp();
    if (needsGpuUsage(m_stat))
      m_monitor->retainGpuUsage();
    if (needsGpuVram(m_stat))
      m_monitor->retainGpuVram();
    if (m_stat2.has_value() && needsCpuTemp(*m_stat2))
      m_monitor->retainCpuTemp();
    if (m_stat2.has_value() && needsGpuTemp(*m_stat2))
      m_monitor->retainGpuTemp();
    if (m_stat2.has_value() && needsGpuUsage(*m_stat2))
      m_monitor->retainGpuUsage();
    if (m_stat2.has_value() && needsGpuVram(*m_stat2))
      m_monitor->retainGpuVram();
  }
}

DesktopSysmonWidget::~DesktopSysmonWidget() {
  if (m_monitor != nullptr) {
    if (needsCpuTemp(m_stat))
      m_monitor->releaseCpuTemp();
    if (needsGpuTemp(m_stat))
      m_monitor->releaseGpuTemp();
    if (needsGpuUsage(m_stat))
      m_monitor->releaseGpuUsage();
    if (needsGpuVram(m_stat))
      m_monitor->releaseGpuVram();
    if (m_stat2.has_value() && needsCpuTemp(*m_stat2))
      m_monitor->releaseCpuTemp();
    if (m_stat2.has_value() && needsGpuTemp(*m_stat2))
      m_monitor->releaseGpuTemp();
    if (m_stat2.has_value() && needsGpuUsage(*m_stat2))
      m_monitor->releaseGpuUsage();
    if (m_stat2.has_value() && needsGpuVram(*m_stat2))
      m_monitor->releaseGpuVram();
  }
}

void DesktopSysmonWidget::create() {
  auto rootNode = std::make_unique<Node>();

  auto glyph = ui::glyph({
      .out = &m_glyph,
      .glyph = glyphName(m_stat),
  });
  rootNode->addChild(std::move(glyph));

  if (m_displayMode == DesktopSysmonDisplayMode::Graph) {
    auto graph = std::make_unique<Graph>();
    graph->setLineWidth(kGraphLineWidth);
    graph->setFillOpacity(0.2f);
    m_graph = static_cast<Graph*>(rootNode->addChild(std::move(graph)));

    if (m_stat2.has_value()) {
      auto glyph2 = ui::glyph({
          .out = &m_glyph2,
          .glyph = glyphName(*m_stat2),
      });
      rootNode->addChild(std::move(glyph2));
    }
  } else {
    m_gauge = static_cast<ProgressBar*>(rootNode->addChild(
        ui::progressBar({
            .fill = m_lineColor,
            .track = gaugeTrackColor(m_lineColor),
            .progress = 0.0f,
        })
    ));
  }

  if (m_showLabel) {
    const Color shadow{0.0f, 0.0f, 0.0f, 0.5f};
    auto label = ui::label({
        .out = &m_label,
        .minWidth = m_labelMinWidth > 0.0f ? std::optional<float>{m_labelMinWidth} : std::nullopt,
        .fontWeight = FontWeight::Medium,
    });
    if (m_shadow) {
      label->setShadow(shadow, 0.0f, 1.0f);
    }
    rootNode->addChild(std::move(label));

    if (m_displayMode == DesktopSysmonDisplayMode::Graph && m_stat2.has_value()) {
      auto label2 = ui::label({
          .out = &m_label2,
          .fontWeight = FontWeight::Medium,
      });
      if (m_shadow) {
        label2->setShadow(shadow, 0.0f, 1.0f);
      }
      rootNode->addChild(std::move(label2));
    }
  }

  setRoot(std::move(rootNode));
}

bool DesktopSysmonWidget::needsFrameTick() const {
  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    return true;
  }
  return m_scrollProgress < 1.0f;
}

void DesktopSysmonWidget::onFrameTick(float deltaMs, Renderer& renderer) {
  (void)deltaMs;
  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    if (!m_redrawLimiter.shouldStep([this]() { requestFrameTick(); })) {
      return;
    }
    if (m_monitor != nullptr) {
      syncLabel();
      syncGaugeProgress(currentNormalized());
      syncValueColor();
    }
    requestRedraw();
    return;
  }

  if (!m_redrawLimiter.shouldStep([this]() { requestFrameTick(); })) {
    return;
  }
  if (m_monitor != nullptr) {
    if (m_monitor->isRunning()) {
      const auto latestSampleAt = m_monitor->latest().sampledAt;
      if (latestSampleAt != std::chrono::steady_clock::time_point{} && latestSampleAt != m_lastSampleAt) {
        updateGraph(renderer);
        syncLabel();
      }
    } else {
      clearGraph();
      syncLabel();
    }
  }

  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  if (m_graph != nullptr) {
    m_graph->setScroll(m_scrollProgress);
  }
  requestRedraw();
}

bool DesktopSysmonWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "display" || key == "stat" || key == "stat2" || key == "show_label") {
    (void)value;
    (void)allSettings;
    (void)renderer;
    return false;
  }
  if (key == "gauge_layout") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_gaugeLayout = *v == "vertical" ? DesktopSysmonGaugeLayout::Vertical : DesktopSysmonGaugeLayout::Horizontal;
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "highlight_color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_highlightColor = colorSpecFromConfigString(*v, key);
      syncValueColor();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "label_min_width") {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_labelMinWidth = static_cast<float>(*v);
      if (m_label != nullptr) {
        m_label->setMinWidth(m_labelMinWidth > 0.0f ? m_labelMinWidth * m_contentScale : 0.0f);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_lineColor = colorSpecFromConfigString(*v, key);
      if (m_gauge != nullptr) {
        m_gauge->setTrack(gaugeTrackColor(m_lineColor));
      }
      syncValueColor();
      if (m_displayMode == DesktopSysmonDisplayMode::Graph) {
        layout(renderer);
      } else {
        requestRedraw();
      }
      return true;
    }
    return false;
  }
  if (key == "color2") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_lineColor2 = colorSpecFromConfigString(*v, key);
      layout(renderer);
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      const Color shadow{0.0f, 0.0f, 0.0f, 0.5f};
      for (Glyph* glyph : {m_glyph, m_glyph2}) {
        if (glyph != nullptr) {
          if (m_shadow)
            glyph->setShadow(shadow, 0.0f, 1.0f);
          else
            glyph->clearShadow();
        }
      }
      for (Label* label : {m_label, m_label2}) {
        if (label != nullptr) {
          if (m_shadow)
            label->setShadow(shadow, 0.0f, 1.0f);
          else
            label->clearShadow();
        }
      }
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopSysmonWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  for (Label* label : {m_label, m_label2}) {
    if (label != nullptr) {
      label->setFontFamily(family);
    }
  }
}

void DesktopSysmonWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_glyph == nullptr) {
    return;
  }

  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    layoutGaugeMode(renderer);
    return;
  }
  layoutGraphMode(renderer);
}

void DesktopSysmonWidget::layoutGaugeMode(Renderer& renderer) {
  const float scale = m_contentScale;
  const float gap = Style::spaceXs * scale;
  const Color shadow{0.0f, 0.0f, 0.0f, 0.5f};
  const bool stacked = m_gaugeLayout == DesktopSysmonGaugeLayout::Vertical;

  m_glyph->setGlyphSize(Style::baseGlyphSize * scale);
  if (m_shadow) {
    m_glyph->setShadow(shadow, 0.0f, 1.0f);
  }
  m_glyph->measure(renderer);
  const float glyphH = m_glyph->height();

  if (m_label != nullptr) {
    m_label->setFontSize((stacked ? Style::fontSizeCaption : Style::fontSizeBody) * scale);
    m_label->setMinWidth(m_labelMinWidth > 0.0f ? m_labelMinWidth * scale : 0.0f);
    m_label->measure(renderer);
  }
  const float labelW = m_label != nullptr ? m_label->width() : 0.0f;
  const float labelH = m_label != nullptr ? m_label->height() : 0.0f;

  if (m_gauge == nullptr) {
    return;
  }

  const float baseSize = Style::fontSizeBody * scale;
  const float gaugeStem = std::round(baseSize * 0.85f);
  const float gaugeThickness = std::max(3.0f, roundf(baseSize * 0.3f));
  const float glyphSize = Style::baseGlyphSize * scale;
  const TextMetrics glyphMetrics = glyphMetricsFor(renderer, glyphName(m_stat), glyphSize);
  const GlyphInkBounds ink = glyphHorizontalInkBounds(m_glyph->width(), glyphMetrics);

  if (stacked) {
    m_gauge->setOrientation(ProgressBarOrientation::Horizontal);
    const float trackW = std::max(ink.width(), gaugeStem);
    const float trackH = gaugeThickness;
    m_gauge->setRadius(trackH / 2.0f);
    float contentW = trackW;
    if (m_label != nullptr) {
      contentW = std::max(contentW, labelW);
    }
    const float glyphX = std::round((contentW - ink.width()) * 0.5f - ink.left);
    m_glyph->setPosition(glyphX, 0.0f);
    m_gauge->setPosition(std::round((contentW - trackW) * 0.5f), glyphH + gap);
    m_gauge->setSize(trackW, trackH);
    float totalH = glyphH + gap + trackH;
    if (m_label != nullptr) {
      m_label->setPosition(std::round((contentW - labelW) * 0.5f), totalH + gap);
      totalH += gap + labelH;
    }
    root()->setSize(contentW, totalH);
  } else {
    m_gauge->setOrientation(ProgressBarOrientation::Vertical);
    const float gaugeW = gaugeThickness;
    const float gaugeH = gaugeStem;
    m_gauge->setRadius(gaugeW / 2.0f);
    float contentH = std::max(glyphH, gaugeH);
    if (m_label != nullptr) {
      contentH = std::max(contentH, labelH);
    }
    const float gaugeY = std::round((contentH - gaugeH) * 0.5f);
    const float glyphX = std::round(std::max(0.0f, -ink.left));
    m_glyph->setPosition(glyphX, std::round((contentH - glyphH) * 0.5f));
    const float gaugeX = std::round(glyphX + ink.right + gap);
    m_gauge->setPosition(gaugeX, gaugeY);
    m_gauge->setSize(gaugeW, gaugeH);
    float totalW = gaugeX + gaugeW;
    if (m_label != nullptr) {
      m_label->setPosition(totalW + gap, std::round((contentH - labelH) * 0.5f));
      totalW = m_label->x() + labelW;
    }
    root()->setSize(totalW, contentH);
  }

  syncGaugeProgress(currentNormalized());
  syncValueColor();
}

void DesktopSysmonWidget::layoutGraphMode(Renderer& renderer) {
  const float scale = m_contentScale;
  const float fontSize = Style::fontSizeBody * scale;
  const float glyphSize = Style::baseGlyphSize * scale;
  const float groupGap = Style::spaceXs * scale;
  const float legendGap = Style::spaceMd * scale;
  const Color shadow{0.0f, 0.0f, 0.0f, 0.5f};

  m_graph->setColor(m_lineColor);
  if (m_stat2.has_value()) {
    m_graph->setColor2(m_lineColor2);
  }
  m_graph->setLineWidth(kGraphLineWidth * scale);

  auto measureGroup = [&](Glyph* glyph, Label* label, const ColorSpec& color, float& width, float& height) {
    glyph->setGlyphSize(glyphSize);
    glyph->setColor(color);
    if (m_shadow) {
      glyph->setShadow(shadow, 0.0f, 1.0f);
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

  float w1 = 0.0f, h1 = 0.0f;
  measureGroup(m_glyph, m_label, m_lineColor, w1, h1);

  float w2 = 0.0f, h2 = 0.0f;
  if (m_glyph2 != nullptr) {
    measureGroup(m_glyph2, m_label2, m_lineColor2, w2, h2);
  }

  const float totalW = kBaseWidth * scale;
  const float chartH = kBaseHeight * scale;

  const float headerW = (m_glyph2 != nullptr) ? (w1 + legendGap + w2) : w1;
  const float headerH = std::max(h1, h2);
  const float contentW = std::max(totalW, headerW);

  m_graph->setPosition(0.0f, 0.0f);
  m_graph->setSize(contentW, chartH);
  m_graph->sync(renderer);

  const float headerY = chartH + Style::spaceSm * scale;
  float x = std::round((contentW - headerW) * 0.5f);

  auto placeGroup = [&](Glyph* glyph, Label* label) {
    glyph->setPosition(x, headerY + std::round((headerH - glyph->height()) * 0.5f));
    x += glyph->width();
    if (label != nullptr) {
      x += groupGap;
      label->setPosition(x, headerY + std::round((headerH - label->height()) * 0.5f));
      x += label->width();
    }
  };

  placeGroup(m_glyph, m_label);
  if (m_glyph2 != nullptr) {
    x += legendGap;
    placeGroup(m_glyph2, m_label2);
  }

  root()->setSize(contentW, headerY + headerH);
}

void DesktopSysmonWidget::doUpdate(Renderer& renderer) {
  if (m_monitor == nullptr) {
    return;
  }

  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    syncLabel();
    syncGaugeProgress(currentNormalized());
    syncValueColor();
    return;
  }

  if (m_monitor->isRunning()) {
    updateGraph(renderer);
  } else {
    clearGraph();
  }
  syncLabel();
}

void DesktopSysmonWidget::syncGaugeProgress(double normalized) {
  if (m_gauge == nullptr) {
    return;
  }

  const bool stacked = m_gaugeLayout == DesktopSysmonGaugeLayout::Vertical;
  const float fillAxis = stacked ? m_gauge->width() : m_gauge->height();
  const float progress = (fillAxis > 0.0f && normalized * fillAxis < 1.0f) ? 0.0f : static_cast<float>(normalized);
  m_gauge->setProgress(progress);
  requestRedraw();
}

void DesktopSysmonWidget::syncValueColor() {
  const Color valueColor = currentValueColor(m_lineColor);
  if (m_glyph != nullptr) {
    m_glyph->setColor(valueColor);
  }
  if (m_label != nullptr) {
    m_label->setColor(valueColor);
  }
  if (m_gauge != nullptr) {
    m_gauge->setFill(valueColor);
    m_gauge->setTrack(gaugeTrackColor(m_lineColor));
  }
}

Color DesktopSysmonWidget::currentValueColor(ColorSpec baseColor) const {
  const Color base = resolveColorSpec(baseColor);
  if (m_config == nullptr) {
    return base;
  }
  const Color highlight = resolveColorSpec(m_highlightColor);
  const auto [activityThreshold, criticalThreshold] = currentThresholds();
  const auto factor = static_cast<float>(gradientFactor(currentGradientValue(), activityThreshold, criticalThreshold));
  return lerpColor(base, highlight, factor);
}

std::pair<double, double> DesktopSysmonWidget::currentThresholds() const {
  if (m_config == nullptr) {
    return {0.0, 100.0};
  }
  const auto& monitorConfig = m_config->config().system.monitor;
  switch (m_stat) {
  case DesktopSysmonStat::CpuUsage:
    return {monitorConfig.cpuUsageActivityThreshold, monitorConfig.cpuUsageCriticalThreshold};
  case DesktopSysmonStat::CpuTemp:
    return {monitorConfig.cpuTempActivityThreshold, monitorConfig.cpuTempCriticalThreshold};
  case DesktopSysmonStat::GpuTemp:
    return {monitorConfig.gpuTempActivityThreshold, monitorConfig.gpuTempCriticalThreshold};
  case DesktopSysmonStat::GpuUsage:
    return {monitorConfig.gpuUsageActivityThreshold, monitorConfig.gpuUsageCriticalThreshold};
  case DesktopSysmonStat::GpuVram:
    return {monitorConfig.gpuVramActivityThreshold, monitorConfig.gpuVramCriticalThreshold};
  case DesktopSysmonStat::RamPct:
    return {monitorConfig.ramPctActivityThreshold, monitorConfig.ramPctCriticalThreshold};
  case DesktopSysmonStat::SwapPct:
    return {monitorConfig.swapPctActivityThreshold, monitorConfig.swapPctCriticalThreshold};
  case DesktopSysmonStat::NetRx:
    return {monitorConfig.netRxActivityThreshold, monitorConfig.netRxCriticalThreshold};
  case DesktopSysmonStat::NetTx:
    return {monitorConfig.netTxActivityThreshold, monitorConfig.netTxCriticalThreshold};
  }
  return {monitorConfig.cpuUsageActivityThreshold, monitorConfig.cpuUsageCriticalThreshold};
}

double DesktopSysmonWidget::currentGradientValue() const {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    return 0.0;
  }

  const auto stats = m_monitor->latest();
  switch (m_stat) {
  case DesktopSysmonStat::CpuUsage:
    return std::max(stats.cpuUsagePercent, 0.0);
  case DesktopSysmonStat::CpuTemp:
    return stats.cpuTempC.value_or(0.0);
  case DesktopSysmonStat::GpuTemp:
    return stats.gpuTempC.value_or(0.0);
  case DesktopSysmonStat::GpuUsage:
    return stats.gpuUsagePercent.value_or(0.0);
  case DesktopSysmonStat::GpuVram:
    if (stats.gpuVramUsedBytes.has_value() && stats.gpuVramTotalBytes.has_value() && *stats.gpuVramTotalBytes > 0) {
      return 100.0 * static_cast<double>(*stats.gpuVramUsedBytes) / static_cast<double>(*stats.gpuVramTotalBytes);
    }
    return 0.0;
  case DesktopSysmonStat::RamPct:
    return std::max(stats.ramUsagePercent, 0.0);
  case DesktopSysmonStat::SwapPct:
    if (stats.swapTotalMb > 0) {
      return 100.0 * static_cast<double>(stats.swapUsedMb) / static_cast<double>(stats.swapTotalMb);
    }
    return 0.0;
  case DesktopSysmonStat::NetRx:
    return std::max(m_monitor->netRxBytesPerSec(m_networkInterface) / kBytesPerMb, 0.0);
  case DesktopSysmonStat::NetTx:
    return std::max(m_monitor->netTxBytesPerSec(m_networkInterface) / kBytesPerMb, 0.0);
  }
  return 0.0;
}

double DesktopSysmonWidget::currentNormalized() const {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    return 0.0;
  }
  return std::clamp(
      normalizedFromStats(m_stat, m_monitor->latest(), m_gaugeTempMin, m_gaugeTempMax, m_networkInterface), 0.0, 1.0
  );
}

void DesktopSysmonWidget::syncLabel() {
  if (m_label == nullptr) {
    return;
  }

  std::string text = formatValueFor(m_stat);
  if (text != m_lastRawValue) {
    m_lastRawValue = text;
    m_label->setText(text);
    requestRedraw();
  }

  if (m_label2 != nullptr && m_stat2.has_value()) {
    std::string text2 = formatValueFor(*m_stat2);
    if (text2 != m_lastRawValue2) {
      m_lastRawValue2 = text2;
      m_label2->setText(text2);
      requestRedraw();
    }
  }
}

double DesktopSysmonWidget::normalizedFromStats(
    DesktopSysmonStat stat, const SystemStats& stats, double& tempMin, double& tempMax,
    std::string_view networkInterface
) {
  switch (stat) {
  case DesktopSysmonStat::CpuUsage:
    return stats.cpuUsagePercent / 100.0;

  case DesktopSysmonStat::CpuTemp:
    if (stats.cpuTempC.has_value()) {
      const double temp = *stats.cpuTempC;
      tempMin = std::min(tempMin, temp);
      tempMax = std::max(tempMax, temp);
      const double range = tempMax - tempMin;
      if (range <= 0.0)
        return 0.5;
      return std::clamp((temp - tempMin) / range, 0.0, 1.0);
    }
    return 0.0;

  case DesktopSysmonStat::GpuTemp:
    if (stats.gpuTempC.has_value()) {
      const double temp = *stats.gpuTempC;
      tempMin = std::min(tempMin, temp);
      tempMax = std::max(tempMax, temp);
      const double range = tempMax - tempMin;
      if (range <= 0.0)
        return 0.5;
      return std::clamp((temp - tempMin) / range, 0.0, 1.0);
    }
    return 0.0;

  case DesktopSysmonStat::GpuUsage:
    if (stats.gpuUsagePercent.has_value()) {
      return *stats.gpuUsagePercent / 100.0;
    }
    return 0.0;

  case DesktopSysmonStat::GpuVram:
    if (stats.gpuVramUsedBytes.has_value() && stats.gpuVramTotalBytes.has_value() && *stats.gpuVramTotalBytes > 0) {
      return static_cast<double>(*stats.gpuVramUsedBytes) / static_cast<double>(*stats.gpuVramTotalBytes);
    }
    return 0.0;

  case DesktopSysmonStat::RamPct:
    return stats.ramUsagePercent / 100.0;

  case DesktopSysmonStat::SwapPct:
    if (stats.swapTotalMb > 0) {
      return static_cast<double>(stats.swapUsedMb) / static_cast<double>(stats.swapTotalMb);
    }
    return 0.0;

  case DesktopSysmonStat::NetRx: {
    const double value = networkInterface.empty() ? stats.netRxBytesPerSec : [&stats, networkInterface]() {
      if (const auto it = stats.netThroughputByInterface.find(std::string(networkInterface));
          it != stats.netThroughputByInterface.end()) {
        return it->second.rxBytesPerSec;
      }
      return 0.0;
    }();
    tempMax = std::max(tempMax, value);
    return tempMax > 0.0 ? std::clamp(value / tempMax, 0.0, 1.0) : 0.0;
  }

  case DesktopSysmonStat::NetTx: {
    const double value = networkInterface.empty() ? stats.netTxBytesPerSec : [&stats, networkInterface]() {
      if (const auto it = stats.netThroughputByInterface.find(std::string(networkInterface));
          it != stats.netThroughputByInterface.end()) {
        return it->second.txBytesPerSec;
      }
      return 0.0;
    }();
    tempMax = std::max(tempMax, value);
    return tempMax > 0.0 ? std::clamp(value / tempMax, 0.0, 1.0) : 0.0;
  }
  }

  return 0.0;
}

std::string DesktopSysmonWidget::formatValueFor(DesktopSysmonStat stat) const {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    return "--";
  }

  const auto stats = m_monitor->latest();

  switch (stat) {
  case DesktopSysmonStat::CpuUsage:
    return std::format("{:.0f}%", stats.cpuUsagePercent);

  case DesktopSysmonStat::CpuTemp:
    if (stats.cpuTempC.has_value()) {
      return std::format("{:.0f}°C", *stats.cpuTempC);
    }
    return "--";

  case DesktopSysmonStat::GpuTemp:
    if (stats.gpuTempC.has_value()) {
      return std::format("{:.0f}°C", *stats.gpuTempC);
    }
    return "--";

  case DesktopSysmonStat::GpuUsage:
    if (stats.gpuUsagePercent.has_value()) {
      return std::format("{:.0f}%", *stats.gpuUsagePercent);
    }
    return "--";

  case DesktopSysmonStat::GpuVram:
    if (stats.gpuVramUsedBytes.has_value() && stats.gpuVramTotalBytes.has_value() && *stats.gpuVramTotalBytes > 0) {
      return std::format(
          "{:.0f}%",
          100.0 * static_cast<double>(*stats.gpuVramUsedBytes) / static_cast<double>(*stats.gpuVramTotalBytes)
      );
    }
    return "--";

  case DesktopSysmonStat::RamPct:
    return std::format("{:.0f}%", stats.ramUsagePercent);

  case DesktopSysmonStat::SwapPct:
    if (stats.swapTotalMb > 0) {
      return std::format(
          "{:.0f}%", 100.0 * static_cast<double>(stats.swapUsedMb) / static_cast<double>(stats.swapTotalMb)
      );
    }
    return "--";

  case DesktopSysmonStat::NetRx:
    return FormatUnits::formatDecimalBytesPerSecond(m_monitor->netRxBytesPerSec(m_networkInterface));

  case DesktopSysmonStat::NetTx:
    return FormatUnits::formatDecimalBytesPerSecond(m_monitor->netTxBytesPerSec(m_networkInterface));
  }

  return "--";
}

void DesktopSysmonWidget::clearGraph() {
  if (m_graph == nullptr || !m_graphInitialized) {
    return;
  }

  m_graph->setValues({});
  m_graph->setValues2({});
  m_graphInitialized = false;
  m_lastSampleAt = {};
  m_scrollProgress = 1.0f;
  requestRedraw();
}

void DesktopSysmonWidget::updateGraph(Renderer& renderer) {
  if (m_graph == nullptr || m_monitor == nullptr || !m_monitor->isRunning()) {
    return;
  }

  const auto hist = m_monitor->history();
  if (hist.size() < 4) {
    return;
  }

  const auto latestSampleAt = hist.back().sampledAt;
  const bool newData = latestSampleAt != m_lastSampleAt;
  if (!newData && m_graphInitialized) {
    return;
  }

  const auto n = hist.size();
  std::vector<float> data1(n);
  for (std::size_t i = 0; i < n; ++i) {
    data1[i] = static_cast<float>(
        std::clamp(normalizedFromStats(m_stat, hist[i], m_tempMin1, m_tempMax1, m_networkInterface), 0.0, 1.0)
    );
  }
  m_graph->setValues(std::move(data1));

  if (m_stat2.has_value()) {
    std::vector<float> data2(n);
    for (std::size_t i = 0; i < n; ++i) {
      data2[i] = static_cast<float>(
          std::clamp(normalizedFromStats(*m_stat2, hist[i], m_tempMin2, m_tempMax2, m_networkInterface), 0.0, 1.0)
      );
    }
    m_graph->setValues2(std::move(data2));
  }

  m_graph->sync(renderer);
  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  m_graph->setScroll(m_scrollProgress);
  requestRedraw();
}

float DesktopSysmonWidget::scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const {
  if (sampledAt == std::chrono::steady_clock::time_point{}) {
    return 1.0f;
  }

  const auto sampleInterval = m_monitor != nullptr ? m_monitor->historySampleInterval()
                                                   : std::chrono::steady_clock::duration{std::chrono::seconds(1)};
  if (sampleInterval.count() <= 0) {
    return 1.0f;
  }

  const auto elapsed = std::chrono::steady_clock::now() - sampledAt;
  const auto clamped = std::clamp(elapsed, std::chrono::steady_clock::duration::zero(), sampleInterval);
  return std::chrono::duration<float>(clamped).count() / std::chrono::duration<float>(sampleInterval).count();
}

const char* DesktopSysmonWidget::glyphName(DesktopSysmonStat stat) {
  switch (stat) {
  case DesktopSysmonStat::CpuUsage:
    return "cpu-usage";
  case DesktopSysmonStat::CpuTemp:
    return "cpu-temperature";
  case DesktopSysmonStat::GpuTemp:
    return "temperature";
  case DesktopSysmonStat::GpuUsage:
    return "gpu-usage";
  case DesktopSysmonStat::GpuVram:
    return "memory";
  case DesktopSysmonStat::RamPct:
    return "memory";
  case DesktopSysmonStat::SwapPct:
    return "storage";
  case DesktopSysmonStat::NetRx:
    return "download";
  case DesktopSysmonStat::NetTx:
    return "upload";
  }
  return "cpu-usage";
}
