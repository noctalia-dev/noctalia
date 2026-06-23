#include "shell/bar/widgets/sysmon_widget.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/graph.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <vector>

namespace {

  [[nodiscard]] std::string displaySysmonLabel(const std::string& raw, bool verticalBar) {
    if (!verticalBar || raw.size() < 2 || raw.back() != '%') {
      return raw;
    }
    return raw.substr(0, raw.size() - 1);
  }

  // The gauge track is a dimmed version of the gauge fill so it inherits the
  // fill's readability against whatever sits behind it (e.g. a custom capsule).
  [[nodiscard]] ColorSpec gaugeTrackColor(const ColorSpec& fill) {
    ColorSpec track = fill;
    track.alpha *= 0.3f;
    return track;
  }

  constexpr float kGraphLineWidth = 0.75f;
  constexpr auto kSamplePublishSlack = std::chrono::milliseconds(20);
  constexpr auto kSampleRetryDelay = std::chrono::milliseconds(25);
  constexpr auto kInitialSampleRetryDelay = std::chrono::milliseconds(250);
  constexpr double kBytesPerMb = 1000.0 * 1000.0;

  [[nodiscard]] std::chrono::steady_clock::duration historyInterval(const SystemMonitorService* monitor) {
    return monitor != nullptr ? monitor->historySampleInterval()
                              : std::chrono::steady_clock::duration{std::chrono::seconds(1)};
  }

  [[nodiscard]] double gradientFactor(double value, double activityThreshold, double criticalThreshold) {
    // Tint the value sits at right when it first crosses the activity threshold, so "active" is
    // immediately visible rather than fading in from nothing.
    constexpr double kActivityOnset = 0.25;
    const double clampedValue = std::max(value, 0.0);
    const double clampedCritical = std::max(criticalThreshold, 0.0);
    if (clampedCritical <= 0.0 || clampedValue <= 0.0) {
      return 0.0;
    }

    const double clampedActivity = std::clamp(activityThreshold, 0.0, clampedCritical);
    // Below the activity threshold: stay at the default color.
    if (clampedValue <= clampedActivity) {
      return 0.0;
    }
    // At or above the critical threshold: full highlight.
    if (clampedValue >= clampedCritical) {
      return 1.0;
    }
    // Between the two: jump to the onset tint, then ramp the rest of the way to full.
    const double t = (clampedValue - clampedActivity) / (clampedCritical - clampedActivity);
    return kActivityOnset + (1.0 - kActivityOnset) * t;
  }

  bool needsCpuTemp(SysmonStat stat) { return stat == SysmonStat::CpuTemp; }
  bool needsGpuTemp(SysmonStat stat) { return stat == SysmonStat::GpuTemp; }
  bool needsGpuUsage(SysmonStat stat) { return stat == SysmonStat::GpuUsage; }
  bool needsGpuVram(SysmonStat stat) { return stat == SysmonStat::GpuVram; }

  std::string statDisplayName(SysmonStat stat) {
    switch (stat) {
    case SysmonStat::CpuUsage:
      return i18n::tr("bar.widgets.sysmon.cpu");
    case SysmonStat::CpuTemp:
      return i18n::tr("bar.widgets.sysmon.cpu-temp");
    case SysmonStat::GpuTemp:
      return i18n::tr("bar.widgets.sysmon.gpu-temp");
    case SysmonStat::GpuUsage:
      return i18n::tr("bar.widgets.sysmon.gpu-usage");
    case SysmonStat::GpuVram:
      return i18n::tr("bar.widgets.sysmon.gpu-vram");
    case SysmonStat::RamUsed:
    case SysmonStat::RamPct:
      return i18n::tr("bar.widgets.sysmon.ram");
    case SysmonStat::SwapPct:
      return i18n::tr("bar.widgets.sysmon.swap");
    case SysmonStat::DiskPct:
      return i18n::tr("bar.widgets.sysmon.disk");
    case SysmonStat::NetRx:
      return i18n::tr("bar.widgets.sysmon.download");
    case SysmonStat::NetTx:
      return i18n::tr("bar.widgets.sysmon.upload");
    }
    return i18n::tr("bar.widgets.sysmon.system");
  }

} // namespace

SysmonWidget::SysmonWidget(SystemMonitorService* monitor, ConfigService& configService, SysmonWidgetOptions options)
    : m_monitor(monitor), m_stat(options.stat), m_displayMode(options.displayMode),
      m_highlightColor(options.highlightColor), m_configService(configService), m_showLabel(options.showLabel),
      m_labelMinWidth(options.labelMinWidth), m_diskPath(std::move(options.diskPath)),
      m_networkInterface(std::move(options.networkInterface)), m_glyphOverride(std::move(options.glyph)) {
  if (m_monitor != nullptr) {
    if (needsCpuTemp(m_stat)) {
      m_monitor->retainCpuTemp();
    }
    if (needsGpuTemp(m_stat)) {
      m_monitor->retainGpuTemp();
    }
    if (needsGpuUsage(m_stat)) {
      m_monitor->retainGpuUsage();
    }
    if (needsGpuVram(m_stat)) {
      m_monitor->retainGpuVram();
    }
    if (m_stat == SysmonStat::DiskPct && !m_diskPath.empty()) {
      m_monitor->retainDiskPath(m_diskPath);
    }
  }
}

SysmonWidget::~SysmonWidget() {
  if (m_monitor != nullptr) {
    if (needsCpuTemp(m_stat)) {
      m_monitor->releaseCpuTemp();
    }
    if (needsGpuTemp(m_stat)) {
      m_monitor->releaseGpuTemp();
    }
    if (needsGpuUsage(m_stat)) {
      m_monitor->releaseGpuUsage();
    }
    if (needsGpuVram(m_stat)) {
      m_monitor->releaseGpuVram();
    }
    if (m_stat == SysmonStat::DiskPct && !m_diskPath.empty()) {
      m_monitor->releaseDiskPath(m_diskPath);
    }
  }
}

void SysmonWidget::create() {
  auto container = std::make_unique<InputArea>();
  container->setOnClick([this](const InputArea::PointerData& /*data*/) {
    requestPanelToggle("control-center", "system");
  });

  container->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = m_glyphOverride.empty() ? glyphName(m_stat) : m_glyphOverride,
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  if (m_displayMode == SysmonDisplayMode::Graph) {
    m_chartBg = static_cast<Box*>(container->addChild(ui::box()));

    auto graph = std::make_unique<Graph>();
    graph->setLineWidth(kGraphLineWidth * m_contentScale);
    graph->setFillOpacity(0.15f);
    m_graph = static_cast<Graph*>(m_chartBg->addChild(std::move(graph)));
  }

  if (m_displayMode == SysmonDisplayMode::Gauge) {
    const ColorSpec base = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
    m_gauge = static_cast<ProgressBar*>(container->addChild(
        ui::progressBar({
            .fill = base,
            .track = gaugeTrackColor(base),
            .progress = 0.0f,
        })
    ));
  }

  if (m_displayMode == SysmonDisplayMode::Text || m_showLabel) {
    container->addChild(
        ui::label({
            .out = &m_label,
            .fontSize = Style::fontSizeBody * m_contentScale,
            .fontFamily = labelFontFamily(),
            .minWidth = m_labelMinWidth > 0.0f ? std::optional<float>{m_labelMinWidth * m_contentScale}
                                               : std::optional<float>{},
            .fontWeight = labelFontWeight(),
        })
    );
  }

  setRoot(std::move(container));

  syncVisualPalette();
  m_paletteConn = paletteChanged().connect([this]() {
    syncVisualPalette();
    requestRedraw();
  });
}

void SysmonWidget::syncVisualPalette() {
  if (m_chartBg != nullptr) {
    RoundedRectStyle bgStyle;
    bgStyle.fill = colorForRole(ColorRole::SurfaceVariant);
    bgStyle.radius = Style::scaledRadiusSm();
    bgStyle.softness = 0.5f;
    m_chartBg->setStyle(bgStyle);
  }
  if (m_graph != nullptr) {
    m_graph->setColor(currentValueColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))));
  }
  if (m_gauge != nullptr) {
    const ColorSpec base = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
    m_gauge->setTrack(gaugeTrackColor(base));
  }
  syncValueColor();
}

void SysmonWidget::syncValueColor() {
  const Color valueColor = currentValueColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  if (m_glyph != nullptr) {
    const Color iconColor = m_widgetIconColor.has_value() ? resolveColorSpec(m_widgetIconColor.value()) : valueColor;
    m_glyph->setColor(iconColor);
  }
  if (m_label != nullptr) {
    m_label->setColor(valueColor);
  }
  if (m_graph != nullptr) {
    m_graph->setColor(valueColor);
  }
  if (m_gauge != nullptr) {
    m_gauge->setFill(valueColor);
  }
}

Color SysmonWidget::currentValueColor(ColorSpec baseColor) {
  const Color base = resolveColorSpec(baseColor);
  const Color highlight = resolveColorSpec(m_highlightColor);
  const auto [activityThreshold, criticalThreshold] = currentThresholds();
  const auto factor = static_cast<float>(gradientFactor(currentGradientValue(), activityThreshold, criticalThreshold));
  return lerpColor(base, highlight, factor);
}

std::pair<double, double> SysmonWidget::currentThresholds() const {
  const auto& monitorConfig = m_configService.config().system.monitor;
  switch (m_stat) {
  case SysmonStat::CpuUsage:
    return {monitorConfig.cpuUsageActivityThreshold, monitorConfig.cpuUsageCriticalThreshold};
  case SysmonStat::CpuTemp:
    return {monitorConfig.cpuTempActivityThreshold, monitorConfig.cpuTempCriticalThreshold};
  case SysmonStat::GpuTemp:
    return {monitorConfig.gpuTempActivityThreshold, monitorConfig.gpuTempCriticalThreshold};
  case SysmonStat::GpuUsage:
    return {monitorConfig.gpuUsageActivityThreshold, monitorConfig.gpuUsageCriticalThreshold};
  case SysmonStat::GpuVram:
    return {monitorConfig.gpuVramActivityThreshold, monitorConfig.gpuVramCriticalThreshold};
  case SysmonStat::RamUsed:
  case SysmonStat::RamPct:
    return {monitorConfig.ramPctActivityThreshold, monitorConfig.ramPctCriticalThreshold};
  case SysmonStat::SwapPct:
    return {monitorConfig.swapPctActivityThreshold, monitorConfig.swapPctCriticalThreshold};
  case SysmonStat::DiskPct:
    return {monitorConfig.diskPctActivityThreshold, monitorConfig.diskPctCriticalThreshold};
  case SysmonStat::NetRx:
    return {monitorConfig.netRxActivityThreshold, monitorConfig.netRxCriticalThreshold};
  case SysmonStat::NetTx:
    return {monitorConfig.netTxActivityThreshold, monitorConfig.netTxCriticalThreshold};
  }
  return {monitorConfig.cpuUsageActivityThreshold, monitorConfig.cpuUsageCriticalThreshold};
}

double SysmonWidget::currentGradientValue() {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    return 0.0;
  }

  if (m_stat == SysmonStat::DiskPct) {
    return std::max(static_cast<double>(m_monitor->diskUsagePercent(m_diskPath)), 0.0);
  }

  const auto stats = m_monitor->latest();
  switch (m_stat) {
  case SysmonStat::CpuUsage:
    return std::max(stats.cpuUsagePercent, 0.0);
  case SysmonStat::CpuTemp:
    return stats.cpuTempC.value_or(0.0);
  case SysmonStat::GpuTemp:
    return stats.gpuTempC.value_or(0.0);
  case SysmonStat::GpuUsage:
    return stats.gpuUsagePercent.value_or(0.0);
  case SysmonStat::GpuVram:
    if (stats.gpuVramUsedBytes.has_value() && stats.gpuVramTotalBytes.has_value() && *stats.gpuVramTotalBytes > 0) {
      return 100.0 * static_cast<double>(*stats.gpuVramUsedBytes) / static_cast<double>(*stats.gpuVramTotalBytes);
    }
    return 0.0;
  case SysmonStat::RamUsed:
  case SysmonStat::RamPct:
    return std::max(stats.ramUsagePercent, 0.0);
  case SysmonStat::SwapPct:
    if (stats.swapTotalMb > 0) {
      return 100.0 * static_cast<double>(stats.swapUsedMb) / static_cast<double>(stats.swapTotalMb);
    }
    return 0.0;
  case SysmonStat::NetRx:
    return std::max(m_monitor->netRxBytesPerSec(m_networkInterface) / kBytesPerMb, 0.0);
  case SysmonStat::NetTx:
    return std::max(m_monitor->netTxBytesPerSec(m_networkInterface) / kBytesPerMb, 0.0);
  case SysmonStat::DiskPct:
    return 0.0;
  }
  return 0.0;
}

bool SysmonWidget::syncLabelText(const std::string& raw) {
  if (m_label == nullptr) {
    return false;
  }

  if (raw == m_lastRawValue && m_isVerticalBar == m_lastLabelVertical) {
    return false;
  }

  m_lastRawValue = raw;
  m_lastLabelVertical = m_isVerticalBar;
  m_label->setText(displaySysmonLabel(raw, m_isVerticalBar));
  requestRedraw();
  return true;
}

void SysmonWidget::syncGaugeProgress(double normalized) {
  if (m_gauge == nullptr) {
    return;
  }

  const float fillAxis = m_isVerticalBar ? m_gauge->width() : m_gauge->height();
  const float progress = (fillAxis > 0.0f && normalized * fillAxis < 1.0f) ? 0.0f : static_cast<float>(normalized);
  m_gauge->setProgress(progress);
  requestRedraw();
}

void SysmonWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }
  const bool isVerticalBar = containerHeight > containerWidth;
  const bool orientationChanged = m_isVerticalBar != isVerticalBar;
  m_isVerticalBar = isVerticalBar;

  syncVisualPalette();
  m_glyph->measure(renderer);
  const float glyphH = m_glyph->height();
  const float gap = Style::spaceXs * m_contentScale;
  const bool verticalBar = m_isVerticalBar;

  if (m_label != nullptr) {
    if (orientationChanged || m_lastRawValue.empty()) {
      syncLabelText(m_lastRawValue.empty() ? formatValue() : m_lastRawValue);
    }
    m_label->setFontSize((verticalBar ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->measure(renderer);
  }
  const float labelW = m_label != nullptr ? m_label->width() : 0.0f;
  const float labelH = m_label != nullptr ? m_label->height() : 0.0f;

  if (m_displayMode == SysmonDisplayMode::Gauge && m_gauge != nullptr) {
    const float baseSize = Style::fontSizeBody * m_contentScale;
    const float gaugeStem = std::round(baseSize * 0.85f);
    const float gaugeThickness = std::max(3.0f, roundf(baseSize * 0.3f));

    if (verticalBar) {
      m_gauge->setOrientation(ProgressBarOrientation::Horizontal);
      const float trackW = std::max(m_glyph->width(), gaugeStem);
      const float trackH = gaugeThickness;
      m_gauge->setRadius(trackH / 2.0f);
      float contentW = std::max(m_glyph->width(), trackW);
      if (m_label != nullptr)
        contentW = std::max(contentW, labelW);
      m_glyph->setPosition((contentW - m_glyph->width()) * 0.5f, 0.0f);
      m_gauge->setPosition(std::round((contentW - trackW) * 0.5f), glyphH + gap);
      m_gauge->setSize(trackW, trackH);
      float totalH = glyphH + gap + trackH;
      if (m_label != nullptr) {
        m_label->setPosition((contentW - labelW) * 0.5f, totalH + gap);
        totalH += gap + labelH;
      }
      rootNode->setSize(contentW, totalH);
    } else {
      m_gauge->setOrientation(ProgressBarOrientation::Vertical);
      const float gaugeW = gaugeThickness;
      const float gaugeH = gaugeStem;
      m_gauge->setRadius(gaugeW / 2.0f);
      float contentH = std::max(glyphH, gaugeH);
      if (m_label != nullptr)
        contentH = std::max(contentH, labelH);
      const float gaugeY = std::round((contentH - gaugeH) * 0.5f);
      m_glyph->setPosition(0.0f, (contentH - glyphH) * 0.5f);
      m_gauge->setPosition(m_glyph->width() + gap, gaugeY);
      m_gauge->setSize(gaugeW, gaugeH);
      float totalW = m_gauge->x() + gaugeW;
      if (m_label != nullptr) {
        m_label->setPosition(totalW + gap, (contentH - labelH) * 0.5f);
        totalW = m_label->x() + labelW;
      }
      rootNode->setSize(totalW, contentH);
    }
    syncGaugeProgress(currentNormalized());
    syncValueColor();
    return;
  }

  if (m_displayMode == SysmonDisplayMode::Graph && m_chartBg != nullptr) {
    const float chartW =
        verticalBar ? std::min(50.0f * m_contentScale, std::max(1.0f, containerWidth)) : 50.0f * m_contentScale;

    if (verticalBar) {
      float contentW = std::max(m_glyph->width(), chartW);
      if (m_label != nullptr)
        contentW = std::max(contentW, labelW);
      m_glyph->setPosition((contentW - m_glyph->width()) * 0.5f, 0.0f);
      const float chartY = glyphH + gap;
      m_chartBg->setPosition(std::round((contentW - chartW) * 0.5f), chartY);
      m_chartBg->setSize(chartW, glyphH);

      if (m_graph != nullptr) {
        m_graph->setPosition(0.0f, 0.0f);
        m_graph->setSize(chartW, glyphH);
      }

      float totalH = chartY + glyphH;
      if (m_label != nullptr) {
        m_label->setPosition((contentW - labelW) * 0.5f, totalH + gap);
        totalH += gap + labelH;
      }
      rootNode->setSize(contentW, totalH);
    } else {
      float contentH = glyphH;
      if (m_label != nullptr)
        contentH = std::max(contentH, labelH);
      m_glyph->setPosition(0.0f, (contentH - glyphH) * 0.5f);
      m_chartBg->setPosition(m_glyph->width() + gap, std::round((contentH - glyphH) * 0.5f));
      m_chartBg->setSize(chartW, glyphH);

      if (m_graph != nullptr) {
        m_graph->setPosition(0.0f, 0.0f);
        m_graph->setSize(chartW, glyphH);
      }

      float totalW = m_chartBg->x() + chartW;
      if (m_label != nullptr) {
        m_label->setPosition(totalW + gap, (contentH - labelH) * 0.5f);
        totalW = m_label->x() + labelW;
      }
      rootNode->setSize(totalW, contentH);
    }
  } else if (m_label != nullptr && verticalBar) {
    const float contentW = std::max(m_glyph->width(), labelW);
    m_glyph->setPosition((contentW - m_glyph->width()) * 0.5f, 0.0f);
    m_label->setPosition((contentW - labelW) * 0.5f, glyphH + gap);
    rootNode->setSize(contentW, glyphH + gap + labelH);
  } else if (m_label != nullptr) {
    const float contentH = std::max(glyphH, labelH);
    m_glyph->setPosition(0.0f, (contentH - glyphH) * 0.5f);
    m_label->setPosition(m_glyph->width() + gap, (contentH - labelH) * 0.5f);
    rootNode->setSize(m_label->x() + labelW, contentH);
  } else {
    m_glyph->setPosition(0.0f, 0.0f);
    rootNode->setSize(m_glyph->width(), glyphH);
  }
}

void SysmonWidget::doUpdate(Renderer& renderer) {
  if (m_glyph == nullptr) {
    return;
  }

  const std::string value = formatValue();
  syncValueColor();
  if (m_label != nullptr) {
    m_label->setFontSize((m_isVerticalBar ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    if (syncLabelText(value)) {
      m_label->measure(renderer);
    }
  }

  if (auto* rootNode = root(); rootNode != nullptr) {
    static_cast<InputArea*>(rootNode)->setTooltip(std::vector<TooltipRow>{{statDisplayName(m_stat), value}});
  }

  if (m_displayMode == SysmonDisplayMode::Gauge) {
    syncGaugeProgress(currentNormalized());
    return;
  }

  if (m_displayMode == SysmonDisplayMode::Graph) {
    if (m_monitor != nullptr && m_monitor->isRunning()) {
      updateGraph(renderer);
      scheduleNextUpdate(m_monitor->latest().sampledAt);
    } else {
      clearGraph();
    }
  }
}

void SysmonWidget::onFrameTick(float deltaMs) {
  (void)deltaMs;
  if (m_graph == nullptr || m_scrollProgress >= 1.0f) {
    m_redrawLimiter.reset();
    return;
  }
  if (!m_redrawLimiter.shouldStep([this]() { requestRedraw(); })) {
    return;
  }
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  m_graph->setScroll(m_scrollProgress);
  if (m_scrollProgress < 1.0f) {
    requestRedraw();
  } else {
    m_redrawLimiter.reset();
  }
}

bool SysmonWidget::needsFrameTick() const {
  return m_displayMode == SysmonDisplayMode::Graph && m_scrollProgress < 1.0f;
}

void SysmonWidget::scheduleNextUpdate(std::chrono::steady_clock::time_point latestSampleAt) {
  if (latestSampleAt == std::chrono::steady_clock::time_point{}) {
    m_updateTimer.start(kInitialSampleRetryDelay, [this]() { requestUpdate(); });
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const auto nextExpectedAt = latestSampleAt + historyInterval(m_monitor) + kSamplePublishSlack;
  const auto delay = now < nextExpectedAt ? std::chrono::duration_cast<std::chrono::milliseconds>(nextExpectedAt - now)
                                          : kSampleRetryDelay;
  m_updateTimer.start(delay, [this]() { requestUpdate(); });
}

void SysmonWidget::clearGraph() {
  if (m_graph == nullptr || !m_graphInitialized) {
    return;
  }

  m_graph->setValues({});
  m_graphInitialized = false;
  m_lastSampleAt = {};
  m_scrollProgress = 1.0f;
  requestRedraw();
}

void SysmonWidget::updateGraph(Renderer& renderer) {
  if (m_graph == nullptr || m_monitor == nullptr || !m_monitor->isRunning()) {
    return;
  }

  const auto latestSampleAt = m_monitor->latest().sampledAt;
  const bool newData = latestSampleAt != m_lastSampleAt;
  if (!newData && m_graphInitialized) {
    return;
  }

  std::vector<float> data;
  if (m_stat == SysmonStat::DiskPct) {
    data = m_monitor->diskHistory(m_diskPath, kHistorySamples);
    if (data.size() < 4) {
      return;
    }
    for (float& sample : data) {
      sample = std::clamp(sample / 100.0f, 0.0f, 1.0f);
    }
  } else {
    const auto hist = m_monitor->history(kHistorySamples);
    if (hist.size() < 4) {
      return;
    }
    data.resize(hist.size());
    for (std::size_t i = 0; i < hist.size(); ++i) {
      data[i] = static_cast<float>(
          std::clamp(normalizedFromStats(m_stat, hist[i], m_tempMin, m_tempMax, m_networkInterface), 0.0, 1.0)
      );
    }
  }

  m_graph->setValues(std::move(data));
  m_graph->sync(renderer);
  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  m_graph->setScroll(m_scrollProgress);
  requestRedraw();
}

float SysmonWidget::scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const {
  if (sampledAt == std::chrono::steady_clock::time_point{}) {
    return 1.0f;
  }

  const auto sampleInterval = historyInterval(m_monitor);
  if (sampleInterval.count() <= 0) {
    return 1.0f;
  }

  const auto elapsed = std::chrono::steady_clock::now() - sampledAt;
  const auto clamped = std::clamp(elapsed, std::chrono::steady_clock::duration::zero(), sampleInterval);
  return std::chrono::duration<float>(clamped).count() / std::chrono::duration<float>(sampleInterval).count();
}

double SysmonWidget::normalizedFromStats(
    SysmonStat stat, const SystemStats& stats, double& tempMin, double& tempMax, std::string_view networkInterface
) {
  switch (stat) {
  case SysmonStat::CpuUsage:
    return stats.cpuUsagePercent / 100.0;

  case SysmonStat::CpuTemp:
    if (stats.cpuTempC.has_value()) {
      const double temp = *stats.cpuTempC;
      tempMin = std::min(tempMin, temp);
      tempMax = std::max(tempMax, temp);
      const double range = tempMax - tempMin;
      if (range <= 0.0) {
        return 0.5;
      }
      return std::clamp((temp - tempMin) / range, 0.0, 1.0);
    }
    return 0.0;

  case SysmonStat::GpuTemp:
    if (stats.gpuTempC.has_value()) {
      const double temp = *stats.gpuTempC;
      tempMin = std::min(tempMin, temp);
      tempMax = std::max(tempMax, temp);
      const double range = tempMax - tempMin;
      if (range <= 0.0) {
        return 0.5;
      }
      return std::clamp((temp - tempMin) / range, 0.0, 1.0);
    }
    return 0.0;

  case SysmonStat::GpuUsage:
    if (stats.gpuUsagePercent.has_value()) {
      return *stats.gpuUsagePercent / 100.0;
    }
    return 0.0;

  case SysmonStat::GpuVram:
    if (stats.gpuVramUsedBytes.has_value() && stats.gpuVramTotalBytes.has_value() && *stats.gpuVramTotalBytes > 0) {
      return static_cast<double>(*stats.gpuVramUsedBytes) / static_cast<double>(*stats.gpuVramTotalBytes);
    }
    return 0.0;

  case SysmonStat::RamUsed:
    if (stats.ramTotalMb > 0) {
      return static_cast<double>(stats.ramUsedMb) / static_cast<double>(stats.ramTotalMb);
    }
    return 0.0;

  case SysmonStat::RamPct:
    return stats.ramUsagePercent / 100.0;

  case SysmonStat::SwapPct:
    if (stats.swapTotalMb > 0) {
      return static_cast<double>(stats.swapUsedMb) / static_cast<double>(stats.swapTotalMb);
    }
    return 0.0;

  case SysmonStat::NetRx: {
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

  case SysmonStat::NetTx: {
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

  case SysmonStat::DiskPct:
    return 0.0;
  }
  return 0.0;
}

double SysmonWidget::currentNormalized() {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    return 0.0;
  }

  if (m_stat == SysmonStat::DiskPct) {
    return std::clamp(static_cast<double>(m_monitor->diskUsagePercent(m_diskPath)) / 100.0, 0.0, 1.0);
  }

  return std::clamp(
      normalizedFromStats(m_stat, m_monitor->latest(), m_tempMin, m_tempMax, m_networkInterface), 0.0, 1.0
  );
}

std::string SysmonWidget::formatValue() const {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    return "--";
  }

  if (m_stat == SysmonStat::DiskPct) {
    return std::format("{:.0f}%", m_monitor->diskUsagePercent(m_diskPath));
  }

  const auto stats = m_monitor->latest();

  switch (m_stat) {
  case SysmonStat::CpuUsage:
    return std::format("{:.0f}%", stats.cpuUsagePercent);

  case SysmonStat::CpuTemp:
    if (stats.cpuTempC.has_value()) {
      return std::format("{:.0f}°C", *stats.cpuTempC);
    }
    return "--";

  case SysmonStat::GpuTemp:
    if (stats.gpuTempC.has_value()) {
      return std::format("{:.0f}°C", *stats.gpuTempC);
    }
    return "--";

  case SysmonStat::GpuUsage:
    if (stats.gpuUsagePercent.has_value()) {
      return std::format("{:.0f}%", *stats.gpuUsagePercent);
    }
    return "--";

  case SysmonStat::GpuVram:
    if (stats.gpuVramUsedBytes.has_value() && stats.gpuVramTotalBytes.has_value() && *stats.gpuVramTotalBytes > 0) {
      return std::format(
          "{:.0f}%",
          100.0 * static_cast<double>(*stats.gpuVramUsedBytes) / static_cast<double>(*stats.gpuVramTotalBytes)
      );
    }
    return "--";

  case SysmonStat::RamUsed:
    return FormatUnits::formatBinaryMib(stats.ramUsedMb);

  case SysmonStat::RamPct:
    return std::format("{:.0f}%", stats.ramUsagePercent);

  case SysmonStat::SwapPct:
    if (stats.swapTotalMb > 0) {
      return std::format(
          "{:.0f}%", 100.0 * static_cast<double>(stats.swapUsedMb) / static_cast<double>(stats.swapTotalMb)
      );
    }
    return "--";

  case SysmonStat::NetRx:
    return FormatUnits::formatDecimalBytesPerSecond(m_monitor->netRxBytesPerSec(m_networkInterface));

  case SysmonStat::NetTx:
    return FormatUnits::formatDecimalBytesPerSecond(m_monitor->netTxBytesPerSec(m_networkInterface));

  case SysmonStat::DiskPct:
    break; // handled above
  }

  return "--";
}

const char* SysmonWidget::glyphName(SysmonStat stat) {
  switch (stat) {
  case SysmonStat::CpuUsage:
    return "cpu-usage";
  case SysmonStat::CpuTemp:
    return "cpu-temperature";
  case SysmonStat::GpuTemp:
    return "temperature";
  case SysmonStat::GpuUsage:
    return "gpu-usage";
  case SysmonStat::GpuVram:
    return "memory";
  case SysmonStat::RamUsed:
  case SysmonStat::RamPct:
    return "memory";
  case SysmonStat::SwapPct:
  case SysmonStat::DiskPct:
    return "storage";
  case SysmonStat::NetRx:
    return "download";
  case SysmonStat::NetTx:
    return "upload";
  }
  return "cpu-usage";
}
