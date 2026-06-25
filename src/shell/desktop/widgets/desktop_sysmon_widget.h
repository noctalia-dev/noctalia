#pragma once

#include "core/frame_rate_limiter.h"
#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct SystemStats;
class ConfigService;
class Glyph;
class Graph;
class Label;
class ProgressBar;
class SystemMonitorService;

enum class DesktopSysmonStat : std::uint8_t {
  CpuUsage,
  CpuTemp,
  GpuTemp,
  GpuUsage,
  GpuVram,
  RamPct,
  SwapPct,
  NetRx,
  NetTx
};

enum class DesktopSysmonDisplayMode : std::uint8_t { Graph, Gauge };

enum class DesktopSysmonGaugeLayout : std::uint8_t { Horizontal, Vertical };

class DesktopSysmonWidget : public DesktopWidget {
public:
  struct Options {
    DesktopSysmonStat stat = DesktopSysmonStat::CpuUsage;
    std::optional<DesktopSysmonStat> stat2;
    DesktopSysmonDisplayMode displayMode = DesktopSysmonDisplayMode::Graph;
    DesktopSysmonGaugeLayout gaugeLayout = DesktopSysmonGaugeLayout::Horizontal;
    ColorSpec lineColor = colorSpecFromRole(ColorRole::Primary);
    ColorSpec lineColor2 = colorSpecFromRole(ColorRole::Secondary);
    ColorSpec highlightColor = colorSpecFromRole(ColorRole::Error);
    std::string networkInterface;
    bool showLabel = true;
    float labelMinWidth = 0.0f;
    bool shadow = true;
    ConfigService* config = nullptr;
  };

  DesktopSysmonWidget(SystemMonitorService* monitor, Options options);
  ~DesktopSysmonWidget() override;

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;
  [[nodiscard]] bool needsFrameTick() const override;
  void onFrameTick(float deltaMs, Renderer& renderer) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;
  void onFontFamilyChanged(const std::string& family, Renderer& renderer) override;

  void layoutGraphMode(Renderer& renderer);
  void layoutGaugeMode(Renderer& renderer);
  void syncGaugeProgress(double normalized);
  void syncValueColor();
  [[nodiscard]] std::string formatValueFor(DesktopSysmonStat stat) const;
  void syncLabel();
  void clearGraph();
  void updateGraph(Renderer& renderer);
  [[nodiscard]] float scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const;
  [[nodiscard]] double currentNormalized() const;
  [[nodiscard]] Color currentValueColor(ColorSpec baseColor) const;
  [[nodiscard]] double currentGradientValue() const;
  [[nodiscard]] std::pair<double, double> currentThresholds() const;
  [[nodiscard]] static double normalizedFromStats(
      DesktopSysmonStat stat, const SystemStats& stats, double& tempMin, double& tempMax,
      std::string_view networkInterface
  );
  [[nodiscard]] static const char* glyphName(DesktopSysmonStat stat);

  SystemMonitorService* m_monitor;
  ConfigService* m_config = nullptr;
  DesktopSysmonStat m_stat;
  std::optional<DesktopSysmonStat> m_stat2;
  DesktopSysmonDisplayMode m_displayMode;
  DesktopSysmonGaugeLayout m_gaugeLayout;
  ColorSpec m_lineColor;
  ColorSpec m_lineColor2;
  ColorSpec m_highlightColor;
  std::string m_networkInterface;
  bool m_showLabel;
  float m_labelMinWidth;
  bool m_shadow;

  Glyph* m_glyph = nullptr;
  Glyph* m_glyph2 = nullptr;
  Label* m_label = nullptr;
  Label* m_label2 = nullptr;
  Graph* m_graph = nullptr;
  ProgressBar* m_gauge = nullptr;

  bool m_graphInitialized = false;
  float m_scrollProgress = 1.0f;
  FrameRateLimiter m_redrawLimiter{std::chrono::milliseconds{200}};
  std::chrono::steady_clock::time_point m_lastSampleAt{};
  std::string m_lastRawValue;
  std::string m_lastRawValue2;

  mutable double m_tempMin1 = 30.0;
  mutable double m_tempMax1 = 80.0;
  mutable double m_tempMin2 = 30.0;
  mutable double m_tempMax2 = 80.0;
  mutable double m_gaugeTempMin = 30.0;
  mutable double m_gaugeTempMax = 80.0;
};
