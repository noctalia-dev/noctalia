#include "shell/bar/widgets/sysmon_widget.h"

#include "render/core/renderer.h"
#include "render/scene/graph_node.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
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

  constexpr float kGraphLineWidth = 0.75f;
  constexpr auto kSamplePublishSlack = std::chrono::milliseconds(20);
  constexpr auto kSampleRetryDelay = std::chrono::milliseconds(25);
  constexpr auto kInitialSampleRetryDelay = std::chrono::milliseconds(250);

  [[nodiscard]] std::chrono::steady_clock::duration historyInterval(const SystemMonitorService* monitor) {
    return monitor != nullptr ? monitor->historySampleInterval()
                              : std::chrono::steady_clock::duration{std::chrono::seconds(1)};
  }

  bool needsCpuTemp(SysmonStat stat) { return stat == SysmonStat::CpuTemp; }
  bool needsGpuTemp(SysmonStat stat) { return stat == SysmonStat::GpuTemp; }
  bool needsGpuVram(SysmonStat stat) { return stat == SysmonStat::GpuVram; }

  const char* statDisplayName(SysmonStat stat) {
    switch (stat) {
    case SysmonStat::CpuUsage:
      return "CPU";
    case SysmonStat::CpuTemp:
      return "CPU Temp";
    case SysmonStat::GpuTemp:
      return "GPU Temp";
    case SysmonStat::GpuVram:
      return "GPU VRAM";
    case SysmonStat::RamUsed:
    case SysmonStat::RamPct:
      return "RAM";
    case SysmonStat::SwapPct:
      return "Swap";
    case SysmonStat::DiskPct:
      return "Disk";
    case SysmonStat::NetRx:
      return "Download";
    case SysmonStat::NetTx:
      return "Upload";
    }
    return "System";
  }

} // namespace

SysmonWidget::SysmonWidget(
    SystemMonitorService* monitor, wl_output* /*output*/, SysmonStat stat, std::string diskPath,
    SysmonDisplayMode displayMode, bool showLabel, float labelMinWidth
)
    : m_monitor(monitor), m_stat(stat), m_displayMode(displayMode), m_showLabel(showLabel),
      m_labelMinWidth(labelMinWidth), m_diskPath(std::move(diskPath)) {
  if (m_monitor != nullptr) {
    if (needsCpuTemp(m_stat)) {
      m_monitor->retainCpuTemp();
    }
    if (needsGpuTemp(m_stat)) {
      m_monitor->retainGpuTemp();
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
          .glyph = glyphName(m_stat),
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  if (m_displayMode == SysmonDisplayMode::Graph) {
    m_chartBg = static_cast<Box*>(container->addChild(ui::box()));

    auto graph = std::make_unique<GraphNode>();
    graph->setLineWidth(kGraphLineWidth * m_contentScale);
    graph->setGraphFillOpacity(0.15f);
    m_graphNode = static_cast<GraphNode*>(m_chartBg->addChild(std::move(graph)));
  }

  if (m_displayMode == SysmonDisplayMode::Gauge) {
    m_gauge = static_cast<ProgressBar*>(container->addChild(
        ui::progressBar({
            .fill = colorSpecFromRole(ColorRole::Primary),
            .track = colorSpecFromRole(ColorRole::OnSurface, 0.25f),
            .progress = 0.0f,
        })
    ));
  }

  if (m_displayMode == SysmonDisplayMode::Text || m_showLabel) {
    container->addChild(
        ui::label({
            .out = &m_label,
            .fontSize = Style::fontSizeBody * m_contentScale,
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
  if (m_graphNode != nullptr) {
    m_graphNode->setLineColor1(colorForRole(ColorRole::Primary));
  }
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
  m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  const float glyphH = m_glyph->height();
  const float gap = Style::spaceXs * m_contentScale;
  const bool verticalBar = m_isVerticalBar;

  if (m_label != nullptr) {
    if (orientationChanged || m_lastRawValue.empty()) {
      syncLabelText(m_lastRawValue.empty() ? formatValue() : m_lastRawValue);
    }
    m_label->setFontSize((verticalBar ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
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
      m_glyph->setPosition(std::round((contentW - m_glyph->width()) * 0.5f), 0.0f);
      m_gauge->setPosition(std::round((contentW - trackW) * 0.5f), glyphH + gap);
      m_gauge->setSize(trackW, trackH);
      float totalH = glyphH + gap + trackH;
      if (m_label != nullptr) {
        m_label->setPosition(std::round((contentW - labelW) * 0.5f), totalH + gap);
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
      m_glyph->setPosition(0.0f, std::round((contentH - glyphH) * 0.5f));
      m_gauge->setPosition(m_glyph->width() + gap, gaugeY);
      m_gauge->setSize(gaugeW, gaugeH);
      float totalW = m_gauge->x() + gaugeW;
      if (m_label != nullptr) {
        m_label->setPosition(totalW + gap, std::round((contentH - labelH) * 0.5f));
        totalW = m_label->x() + labelW;
      }
      rootNode->setSize(totalW, contentH);
    }
    syncGaugeProgress(currentNormalized());
    return;
  }

  if (m_displayMode == SysmonDisplayMode::Graph && m_chartBg != nullptr) {
    const float chartW =
        verticalBar ? std::min(50.0f * m_contentScale, std::max(1.0f, containerWidth)) : 50.0f * m_contentScale;

    if (verticalBar) {
      float contentW = std::max(m_glyph->width(), chartW);
      if (m_label != nullptr)
        contentW = std::max(contentW, labelW);
      m_glyph->setPosition(std::round((contentW - m_glyph->width()) * 0.5f), 0.0f);
      const float chartY = glyphH + gap;
      m_chartBg->setPosition(std::round((contentW - chartW) * 0.5f), chartY);
      m_chartBg->setSize(chartW, glyphH);

      if (m_graphNode != nullptr) {
        m_graphNode->setPosition(0.0f, 0.0f);
        m_graphNode->setSize(chartW, glyphH);
      }

      float totalH = chartY + glyphH;
      if (m_label != nullptr) {
        m_label->setPosition(std::round((contentW - labelW) * 0.5f), totalH + gap);
        totalH += gap + labelH;
      }
      rootNode->setSize(contentW, totalH);
    } else {
      float contentH = glyphH;
      if (m_label != nullptr)
        contentH = std::max(contentH, labelH);
      m_glyph->setPosition(0.0f, std::round((contentH - glyphH) * 0.5f));
      m_chartBg->setPosition(m_glyph->width() + gap, std::round((contentH - glyphH) * 0.5f));
      m_chartBg->setSize(chartW, glyphH);

      if (m_graphNode != nullptr) {
        m_graphNode->setPosition(0.0f, 0.0f);
        m_graphNode->setSize(chartW, glyphH);
      }

      float totalW = m_chartBg->x() + chartW;
      if (m_label != nullptr) {
        m_label->setPosition(totalW + gap, std::round((contentH - labelH) * 0.5f));
        totalW = m_label->x() + labelW;
      }
      rootNode->setSize(totalW, contentH);
    }
  } else if (m_label != nullptr && verticalBar) {
    const float contentW = std::max(m_glyph->width(), labelW);
    m_glyph->setPosition(std::round((contentW - m_glyph->width()) * 0.5f), 0.0f);
    m_label->setPosition(std::round((contentW - labelW) * 0.5f), glyphH + gap);
    rootNode->setSize(contentW, glyphH + gap + labelH);
  } else if (m_label != nullptr) {
    const float contentH = std::max(glyphH, labelH);
    m_glyph->setPosition(0.0f, std::round((contentH - glyphH) * 0.5f));
    m_label->setPosition(m_glyph->width() + gap, std::round((contentH - labelH) * 0.5f));
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
  if (m_graphNode == nullptr || m_scrollProgress >= 1.0f) {
    return;
  }
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  m_graphNode->setScroll1(m_scrollProgress);
  if (m_scrollProgress < 1.0f) {
    requestRedraw();
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
  if (m_graphNode == nullptr || !m_graphInitialized) {
    return;
  }

  m_graphNode->setCount1(0.0f);
  m_graphInitialized = false;
  m_lastSampleAt = {};
  m_scrollProgress = 1.0f;
  requestRedraw();
}

void SysmonWidget::updateGraph(Renderer& renderer) {
  if (m_graphNode == nullptr || m_monitor == nullptr || !m_monitor->isRunning()) {
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
      data[i] = static_cast<float>(std::clamp(normalizedFromStats(m_stat, hist[i], m_tempMin, m_tempMax), 0.0, 1.0));
    }
  }

  const int n = static_cast<int>(data.size());
  const int texSize = n + 1;
  data.push_back(
      std::clamp(
          data[static_cast<std::size_t>(n - 1)] +
              (data[static_cast<std::size_t>(n - 1)] - data[static_cast<std::size_t>(n - 2)]) * 0.5f,
          0.0f, 1.0f
      )
  );

  m_graphNode->setData(renderer.textureManager(), data.data(), texSize, nullptr, 0);
  m_graphNode->setCount1(static_cast<float>(n));
  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  m_graphNode->setScroll1(m_scrollProgress);
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

double SysmonWidget::normalizedFromStats(SysmonStat stat, const SystemStats& stats, double& tempMin, double& tempMax) {
  switch (stat) {
  case SysmonStat::CpuUsage:
    return stats.cpuUsagePercent / 100.0;

  case SysmonStat::CpuTemp:
    if (stats.cpuTempC.has_value()) {
      const double temp = *stats.cpuTempC;
      if (temp < tempMin) {
        tempMin = temp;
      }
      if (temp > tempMax) {
        tempMax = temp;
      }
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
      if (temp < tempMin) {
        tempMin = temp;
      }
      if (temp > tempMax) {
        tempMax = temp;
      }
      const double range = tempMax - tempMin;
      if (range <= 0.0) {
        return 0.5;
      }
      return std::clamp((temp - tempMin) / range, 0.0, 1.0);
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
    if (stats.netRxBytesPerSec > tempMax)
      tempMax = stats.netRxBytesPerSec;
    return tempMax > 0.0 ? std::clamp(stats.netRxBytesPerSec / tempMax, 0.0, 1.0) : 0.0;
  }

  case SysmonStat::NetTx: {
    if (stats.netTxBytesPerSec > tempMax)
      tempMax = stats.netTxBytesPerSec;
    return tempMax > 0.0 ? std::clamp(stats.netTxBytesPerSec / tempMax, 0.0, 1.0) : 0.0;
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

  return std::clamp(normalizedFromStats(m_stat, m_monitor->latest(), m_tempMin, m_tempMax), 0.0, 1.0);
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
    return FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec);

  case SysmonStat::NetTx:
    return FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec);

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
