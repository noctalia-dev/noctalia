#include "shell/desktop/widgets/desktop_sysmon_widget.h"

#include "render/core/renderer.h"
#include "render/scene/graph_node.h"
#include "render/scene/node.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace {

  constexpr float kBaseWidth = 180.0f;
  constexpr float kBaseHeight = 80.0f;
  constexpr float kGraphLineWidth = 0.75f;
  bool needsCpuTemp(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::CpuTemp; }
  bool needsGpuTemp(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::GpuTemp; }
  bool needsGpuUsage(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::GpuUsage; }
  bool needsGpuVram(DesktopSysmonStat stat) { return stat == DesktopSysmonStat::GpuVram; }

} // namespace

DesktopSysmonWidget::DesktopSysmonWidget(
    SystemMonitorService* monitor, DesktopSysmonStat stat, std::optional<DesktopSysmonStat> stat2, ColorSpec lineColor,
    ColorSpec lineColor2, bool showLabel, bool shadow
)
    : m_monitor(monitor), m_stat(stat), m_stat2(stat2), m_lineColor(lineColor), m_lineColor2(lineColor2),
      m_showLabel(showLabel), m_shadow(shadow) {
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

  auto graph = std::make_unique<GraphNode>();
  graph->setLineWidth(kGraphLineWidth);
  graph->setGraphFillOpacity(0.2f);
  m_graphNode = static_cast<GraphNode*>(rootNode->addChild(std::move(graph)));

  if (m_showLabel) {
    auto label = ui::label({
        .out = &m_label,
        .fontWeight = FontWeight::Bold,
    });
    if (m_shadow) {
      label->setShadow(Color{0.0f, 0.0f, 0.0f, 0.5f}, 0.0f, 1.0f);
    }
    rootNode->addChild(std::move(label));
  }

  setRoot(std::move(rootNode));
}

void DesktopSysmonWidget::onFrameTick(float deltaMs, Renderer& renderer) {
  (void)deltaMs;
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
  if (m_graphNode != nullptr) {
    m_graphNode->setScroll1(m_scrollProgress);
    if (m_stat2.has_value()) {
      m_graphNode->setScroll2(m_scrollProgress);
    }
  }
  requestRedraw();
}

bool DesktopSysmonWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_lineColor = colorSpecFromConfigString(*v, key);
      layout(renderer);
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
      if (m_glyph != nullptr) {
        if (m_shadow)
          m_glyph->setShadow(shadow, 0.0f, 1.0f);
        else
          m_glyph->clearShadow();
      }
      if (m_label != nullptr) {
        if (m_shadow)
          m_label->setShadow(shadow, 0.0f, 1.0f);
        else
          m_label->clearShadow();
      }
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopSysmonWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_glyph == nullptr) {
    return;
  }

  const float scale = m_contentScale;
  const float fontSize = Style::fontSizeBody * scale;
  const float gap = Style::spaceSm * scale;

  m_graphNode->setLineColor1(resolveColorSpec(m_lineColor));
  if (m_stat2.has_value()) {
    m_graphNode->setLineColor2(resolveColorSpec(m_lineColor2));
  }
  m_graphNode->setLineWidth(kGraphLineWidth * scale);

  m_glyph->setGlyphSize(fontSize);
  m_glyph->setColor(colorForRole(ColorRole::OnSurface));
  if (m_shadow) {
    m_glyph->setShadow(Color{0.0f, 0.0f, 0.0f, 0.5f}, 0.0f, 1.0f);
  }
  m_glyph->measure(renderer);

  const float totalW = kBaseWidth * scale;
  const float chartH = kBaseHeight * scale;

  float headerW = m_glyph->width();
  float headerH = m_glyph->height();

  if (m_label != nullptr) {
    m_label->setFontSize(fontSize);
    m_label->setColor(colorForRole(ColorRole::OnSurface));
    m_label->measure(renderer);
    headerW += gap + m_label->width();
    headerH = std::max(headerH, m_label->height());
  }

  const float contentW = std::max(totalW, headerW);
  m_glyph->setPosition(0.0f, std::round((headerH - m_glyph->height()) * 0.5f));

  if (m_label != nullptr) {
    m_label->setPosition(m_glyph->width() + gap, std::round((headerH - m_label->height()) * 0.5f));
  }

  const float chartY = headerH + gap;
  m_graphNode->setPosition(0.0f, chartY);
  m_graphNode->setSize(contentW, chartH);

  root()->setSize(contentW, chartY + chartH);
}

void DesktopSysmonWidget::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_monitor == nullptr) {
    return;
  }

  if (m_monitor->isRunning()) {
    updateGraph(renderer);
  } else {
    clearGraph();
  }
  syncLabel();
}

void DesktopSysmonWidget::syncLabel() {
  if (m_label == nullptr) {
    return;
  }

  std::string text = formatValueFor(m_stat);
  if (m_stat2.has_value()) {
    text += " / " + formatValueFor(*m_stat2);
  }
  if (text != m_lastRawValue) {
    m_lastRawValue = text;
    m_label->setText(text);
    requestRedraw();
  }
}

double DesktopSysmonWidget::normalizedFromStats(
    DesktopSysmonStat stat, const SystemStats& stats, double& tempMin, double& tempMax
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

  case DesktopSysmonStat::NetRx:
    tempMax = std::max(tempMax, stats.netRxBytesPerSec);
    return tempMax > 0.0 ? std::clamp(stats.netRxBytesPerSec / tempMax, 0.0, 1.0) : 0.0;

  case DesktopSysmonStat::NetTx:
    tempMax = std::max(tempMax, stats.netTxBytesPerSec);
    return tempMax > 0.0 ? std::clamp(stats.netTxBytesPerSec / tempMax, 0.0, 1.0) : 0.0;
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
    return FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec);

  case DesktopSysmonStat::NetTx:
    return FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec);
  }

  return "--";
}

void DesktopSysmonWidget::clearGraph() {
  if (m_graphNode == nullptr || !m_graphInitialized) {
    return;
  }

  m_graphNode->setCount1(0.0f);
  m_graphNode->setCount2(0.0f);
  m_graphInitialized = false;
  m_lastSampleAt = {};
  m_scrollProgress = 1.0f;
  requestRedraw();
}

void DesktopSysmonWidget::updateGraph(Renderer& renderer) {
  if (m_graphNode == nullptr || m_monitor == nullptr || !m_monitor->isRunning()) {
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
  const int texSize = static_cast<int>(n + 1U);
  const auto last = n;
  const auto prev = n - 1U;
  const auto prev2 = n - 2U;

  std::vector<float> data1(n + 1U);
  for (std::size_t i = 0; i < n; ++i) {
    data1[i] = static_cast<float>(std::clamp(normalizedFromStats(m_stat, hist[i], m_tempMin1, m_tempMax1), 0.0, 1.0));
  }
  const float last1 = data1[prev];
  const float prev1 = data1[prev2];
  data1[last] = std::clamp(last1 + (last1 - prev1) * 0.5f, 0.0f, 1.0f);

  if (m_stat2.has_value()) {
    std::vector<float> data2(n + 1U);
    for (std::size_t i = 0; i < n; ++i) {
      data2[i] =
          static_cast<float>(std::clamp(normalizedFromStats(*m_stat2, hist[i], m_tempMin2, m_tempMax2), 0.0, 1.0));
    }
    const float last2 = data2[prev];
    const float previous2 = data2[prev2];
    data2[last] = std::clamp(last2 + (last2 - previous2) * 0.5f, 0.0f, 1.0f);

    m_graphNode->setData(renderer.textureManager(), data1.data(), texSize, data2.data(), texSize);
    m_graphNode->setCount2(static_cast<float>(n));
  } else {
    m_graphNode->setData(renderer.textureManager(), data1.data(), texSize, nullptr, 0);
  }

  m_graphNode->setCount1(static_cast<float>(n));
  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  m_graphNode->setScroll1(m_scrollProgress);
  if (m_stat2.has_value()) {
    m_graphNode->setScroll2(m_scrollProgress);
  }
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
