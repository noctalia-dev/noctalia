#include "shell/desktop/widgets/desktop_sysmon_widget.h"

#include "config/config_service.h"
#include "sysmon_widget.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

namespace {
  constexpr double kBytesPerMb = 1000.0 * 1000.0;
  constexpr double kFreqFallbackCeilingMhz = 5000.0;

  double gradientFactor(double value, double activityThreshold, double criticalThreshold) {
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
  bool needsGpuVram(DesktopSysmonStat stat) {
    return stat == DesktopSysmonStat::GpuVram || stat == DesktopSysmonStat::GpuVramUsed;
  }

} // namespace

DesktopSysmonWidget::DesktopSysmonWidget(SystemMonitorService* monitor, Options options)
    : m_monitor(monitor), m_config(options.config), m_stat(options.stat), m_stat2(options.stat2),
      m_displayMode(options.displayMode), m_gaugeLayout(options.gaugeLayout), m_lineColor(options.lineColor),
      m_lineColor2(options.lineColor2), m_highlightColor(options.highlightColor),
      m_networkInterface(std::move(options.networkInterface)), m_networkSpeedUnit(options.networkSpeedUnit),
      m_networkSpeedLabelStyle(options.networkSpeedLabelStyle),
      m_networkSpeedDecimalPlaces(options.networkSpeedDecimalPlaces), m_showLabel(options.showLabel),
      m_labelMinWidth(options.labelMinWidth), m_shadow(options.shadow) {
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
    if (m_stat2.has_value() && needsCpuTemp(*m_stat2)) {
      m_monitor->retainCpuTemp();
    }
    if (m_stat2.has_value() && needsGpuTemp(*m_stat2)) {
      m_monitor->retainGpuTemp();
    }
    if (m_stat2.has_value() && needsGpuUsage(*m_stat2)) {
      m_monitor->retainGpuUsage();
    }
    if (m_stat2.has_value() && needsGpuVram(*m_stat2)) {
      m_monitor->retainGpuVram();
    }
  }
}

DesktopSysmonWidget::~DesktopSysmonWidget() {
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
    if (m_stat2.has_value() && needsCpuTemp(*m_stat2)) {
      m_monitor->releaseCpuTemp();
    }
    if (m_stat2.has_value() && needsGpuTemp(*m_stat2)) {
      m_monitor->releaseGpuTemp();
    }
    if (m_stat2.has_value() && needsGpuUsage(*m_stat2)) {
      m_monitor->releaseGpuUsage();
    }
    if (m_stat2.has_value() && needsGpuVram(*m_stat2)) {
      m_monitor->releaseGpuVram();
    }
  }
}

void DesktopSysmonWidget::create() {
  auto widget = std::make_unique<DesktopSysmon>(DesktopSysmon::Options{
      .glyph = glyphName(m_stat),
      .glyph2 = m_stat2.has_value() ? glyphName(*m_stat2) : "",
      .displayMode = m_displayMode,
      .gaugeLayout = m_gaugeLayout,
      .lineColor = m_lineColor,
      .lineColor2 = m_lineColor2,
      .showLabel = m_showLabel,
      .labelMinWidth = m_labelMinWidth,
      .shadow = m_shadow
  });
  m_widget = widget.get();
  setRoot(std::move(widget));
}

bool DesktopSysmonWidget::needsFrameTick() const {
  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    return true;
  }
  return m_scrollProgress < 1.0F;
}

void DesktopSysmonWidget::onFrameTick(float deltaMs, Renderer& renderer) {
  (void)deltaMs;
  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    if (!m_redrawLimiter.shouldStep([this] { requestFrameTick(); })) {
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

  if (!m_redrawLimiter.shouldStep([this] { requestFrameTick(); })) {
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
  if (m_widget != nullptr) {
    m_widget->setScroll(m_scrollProgress);
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
      syncAppearance();
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "network_speed_unit") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_networkSpeedUnit = FormatUnits::decimalByteRateUnitFromString(*v);
      syncLabel();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "network_speed_compact") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_networkSpeedLabelStyle = *v ? FormatUnits::ByteRateLabelStyle::Compact : FormatUnits::ByteRateLabelStyle::Full;
      syncLabel();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "network_speed_decimal_places") {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      const auto decimalPlaces = std::clamp(
          *v, static_cast<std::int64_t>(FormatUnits::kMinCompactByteRateDecimalPlaces),
          static_cast<std::int64_t>(FormatUnits::kMaxCompactByteRateDecimalPlaces)
      );
      m_networkSpeedDecimalPlaces = static_cast<int>(decimalPlaces);
      syncLabel();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_lineColor = colorSpecFromConfigString(*v, key);
      syncAppearance();
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
      syncAppearance();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopSysmonWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  if (m_widget != nullptr) {
    m_widget->setFontFamily(family);
  }
}

void DesktopSysmonWidget::doLayout(Renderer& renderer) {
  if (m_widget == nullptr) {
    return;
  }
  syncAppearance();
  m_widget->setContentScale(contentScale());
  m_widget->layout(renderer);
  if (m_displayMode == DesktopSysmonDisplayMode::Gauge) {
    syncGaugeProgress(currentNormalized());
    syncValueColor();
  }
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
  if (m_widget == nullptr) {
    return;
  }
  m_widget->setGauge(normalized, currentValueColor(m_lineColor));
  requestRedraw();
}

void DesktopSysmonWidget::syncValueColor() {
  if (m_widget != nullptr) {
    m_widget->setGauge(currentNormalized(), currentValueColor(m_lineColor));
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
  return lerpHsvChromaWeighted(base, highlight, factor);
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
  case DesktopSysmonStat::CpuFreq:
    return {monitorConfig.cpuFreqActivityThreshold, monitorConfig.cpuFreqCriticalThreshold};
  case DesktopSysmonStat::GpuTemp:
    return {monitorConfig.gpuTempActivityThreshold, monitorConfig.gpuTempCriticalThreshold};
  case DesktopSysmonStat::GpuUsage:
    return {monitorConfig.gpuUsageActivityThreshold, monitorConfig.gpuUsageCriticalThreshold};
  case DesktopSysmonStat::GpuVram:
  case DesktopSysmonStat::GpuVramUsed:
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
  case DesktopSysmonStat::CpuFreq:
    return stats.cpuFreqAvailable ? stats.cpuFreqMhz / 1000.0 : 0.0;
  case DesktopSysmonStat::GpuTemp:
    return stats.gpuTempC.value_or(0.0);
  case DesktopSysmonStat::GpuUsage:
    return stats.gpuUsagePercent.value_or(0.0);
  case DesktopSysmonStat::GpuVram:
  case DesktopSysmonStat::GpuVramUsed:
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
  if (!m_showLabel || m_widget == nullptr) {
    return;
  }
  auto text = formatValueFor(m_stat);
  auto text2 = m_stat2.has_value() ? formatValueFor(*m_stat2) : std::string{};
  if (text != m_lastRawValue || text2 != m_lastRawValue2) {
    m_lastRawValue = std::move(text);
    m_lastRawValue2 = std::move(text2);
    m_widget->setText(m_lastRawValue, m_lastRawValue2);
    requestRedraw();
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
      if (range <= 0.0) {
        return 0.5;
      }
      return std::clamp((temp - tempMin) / range, 0.0, 1.0);
    }
    return 0.0;

  case DesktopSysmonStat::CpuFreq: {
    const double maxMhz = stats.cpuMaxFreqMhz.value_or(0.0);
    return stats.cpuFreqAvailable
        ? std::clamp(stats.cpuFreqMhz / (maxMhz > 0.0 ? maxMhz : kFreqFallbackCeilingMhz), 0.0, 1.0)
        : 0.0;
  }

  case DesktopSysmonStat::GpuTemp:
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

  case DesktopSysmonStat::GpuUsage:
    if (stats.gpuUsagePercent.has_value()) {
      return *stats.gpuUsagePercent / 100.0;
    }
    return 0.0;

  case DesktopSysmonStat::GpuVram:
  case DesktopSysmonStat::GpuVramUsed:
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
    const double value = networkInterface.empty() ? stats.netRxBytesPerSec : [&stats, networkInterface] {
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
    const double value = networkInterface.empty() ? stats.netTxBytesPerSec : [&stats, networkInterface] {
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
    return std::format("{:.0F}%", stats.cpuUsagePercent);

  case DesktopSysmonStat::CpuTemp:
    if (stats.cpuTempC.has_value()) {
      return std::format("{:.0F}°C", *stats.cpuTempC);
    }
    return "--";

  case DesktopSysmonStat::CpuFreq:
    if (stats.cpuFreqAvailable) {
      return std::format("{:.1f} GHz", stats.cpuFreqMhz / 1000.0);
    }
    return "--";

  case DesktopSysmonStat::GpuTemp:
    if (stats.gpuTempC.has_value()) {
      return std::format("{:.0F}°C", *stats.gpuTempC);
    }
    return "--";

  case DesktopSysmonStat::GpuUsage:
    if (stats.gpuUsagePercent.has_value()) {
      return std::format("{:.0F}%", *stats.gpuUsagePercent);
    }
    return "--";

  case DesktopSysmonStat::GpuVram:
    if (stats.gpuVramUsedBytes.has_value() && stats.gpuVramTotalBytes.has_value() && *stats.gpuVramTotalBytes > 0) {
      return std::format(
          "{:.0F}%",
          100.0 * static_cast<double>(*stats.gpuVramUsedBytes) / static_cast<double>(*stats.gpuVramTotalBytes)
      );
    }
    return "--";

  case DesktopSysmonStat::GpuVramUsed:
    if (stats.gpuVramUsedBytes.has_value()) {
      return FormatUnits::formatBinaryBytesAsGib(*stats.gpuVramUsedBytes);
    }
    return "--";

  case DesktopSysmonStat::RamPct:
    return std::format("{:.0F}%", stats.ramUsagePercent);

  case DesktopSysmonStat::SwapPct:
    if (stats.swapTotalMb > 0) {
      return std::format(
          "{:.0F}%", 100.0 * static_cast<double>(stats.swapUsedMb) / static_cast<double>(stats.swapTotalMb)
      );
    }
    return "--";

  case DesktopSysmonStat::NetRx:
    return FormatUnits::formatDecimalBytesPerSecond(
        m_monitor->netRxBytesPerSec(m_networkInterface), m_networkSpeedUnit, m_networkSpeedLabelStyle,
        m_networkSpeedDecimalPlaces
    );

  case DesktopSysmonStat::NetTx:
    return FormatUnits::formatDecimalBytesPerSecond(
        m_monitor->netTxBytesPerSec(m_networkInterface), m_networkSpeedUnit, m_networkSpeedLabelStyle,
        m_networkSpeedDecimalPlaces
    );
  }

  return "--";
}

void DesktopSysmonWidget::clearGraph() {
  if (m_widget == nullptr || !m_graphInitialized) {
    return;
  }

  m_widget->clearHistory();
  m_graphInitialized = false;
  m_lastSampleAt = {};
  m_scrollProgress = 1.0F;
  requestRedraw();
}

void DesktopSysmonWidget::updateGraph(Renderer& renderer) {
  if (m_widget == nullptr || m_monitor == nullptr || !m_monitor->isRunning()) {
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
  std::vector<float> data2;

  if (m_stat2.has_value()) {
    data2.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      data2[i] = static_cast<float>(
          std::clamp(normalizedFromStats(*m_stat2, hist[i], m_tempMin2, m_tempMax2, m_networkInterface), 0.0, 1.0)
      );
    }
  }

  m_widget->setHistory(renderer, std::move(data1), std::move(data2));
  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);
  m_widget->setScroll(m_scrollProgress);
  requestRedraw();
}

float DesktopSysmonWidget::scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const {
  if (sampledAt == std::chrono::steady_clock::time_point{}) {
    return 1.0F;
  }

  const auto sampleInterval = m_monitor != nullptr ? m_monitor->historySampleInterval()
                                                   : std::chrono::steady_clock::duration{std::chrono::seconds(1)};
  if (sampleInterval.count() <= 0) {
    return 1.0F;
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
  case DesktopSysmonStat::CpuFreq:
    return "performance";
  case DesktopSysmonStat::GpuTemp:
    return "temperature";
  case DesktopSysmonStat::GpuUsage:
    return "gpu-usage";
  case DesktopSysmonStat::GpuVram:
  case DesktopSysmonStat::GpuVramUsed:
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

void DesktopSysmonWidget::syncAppearance() {
  if (m_widget != nullptr) {
    m_widget->setAppearance(m_gaugeLayout, m_lineColor, m_lineColor2, m_labelMinWidth, m_shadow);
  }
}
