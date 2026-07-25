#pragma once

#include "core/frame_rate_limiter.h"
#include "core/timer_manager.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"
#include "shell/tooltip/tooltip_content.h"
#include "system/format_units.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Box;
class ConfigService;
class Glyph;
class Graph;
class Image;
class Label;
class ProgressBar;
class SystemMonitorService;
struct SystemStats;

enum class SysmonStat {
  CpuUsage,
  CpuTemp,
  GpuTemp,
  GpuUsage,
  GpuVram,
  RamUsed,
  RamPct,
  SwapPct,
  DiskUsedPct,
  DiskUsed,
  DiskFreePct,
  DiskFree,
  NetRx,
  NetTx
};
enum class SysmonDisplayMode { Text, Graph, Gauge, None };
enum class SysmonGlyphPosition { Before, After };

struct SysmonWidgetOptions {
  SysmonStat stat = SysmonStat::CpuUsage;
  std::string diskPath = "/";
  SysmonDisplayMode displayMode = SysmonDisplayMode::Gauge;
  ColorSpec highlightColor = colorSpecFromRole(ColorRole::Error);
  std::string networkInterface;
  FormatUnits::DecimalByteRateUnit networkSpeedUnit = FormatUnits::DecimalByteRateUnit::Auto;
  FormatUnits::ByteRateLabelStyle networkSpeedLabelStyle = FormatUnits::ByteRateLabelStyle::Full;
  bool showLabel = true;
  float labelMinWidth = 0.0f;
  std::string glyph;
  WidgetCustomImage customImage;
  bool showUnits = true;
  SysmonGlyphPosition glyphPosition = SysmonGlyphPosition::After;
};

class SysmonWidget : public Widget {
public:
  SysmonWidget(SystemMonitorService* monitor, ConfigService& configService, SysmonWidgetOptions options);
  ~SysmonWidget() override;

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool needsFrameTick() const override;
  bool syncLabelText(const std::string& raw);
  void syncGaugeProgress(double normalized);
  [[nodiscard]] std::string formatValue() const;
  [[nodiscard]] double currentNormalized();
  [[nodiscard]] static const char* glyphName(SysmonStat stat);
  void scheduleNextUpdate(std::chrono::steady_clock::time_point latestSampleAt);
  void clearGraph();
  void syncVisualPalette();
  void syncValueColor();
  void syncIcon(Renderer& renderer);
  void updateGraph(Renderer& renderer);
  [[nodiscard]] float iconWidth() const;
  [[nodiscard]] float iconHeight() const;
  void setIconPosition(float x, float y);
  [[nodiscard]] float scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const;
  [[nodiscard]] Color currentValueColor(ColorSpec baseColor);
  [[nodiscard]] double currentGradientValue();
  [[nodiscard]] std::pair<double, double> currentThresholds() const;
  [[nodiscard]] std::optional<std::string> formatValueFor(SysmonStat stat, const SystemStats& stats) const;
  [[nodiscard]] bool statAvailableForTooltip(SysmonStat stat, const SystemStats& stats) const;
  [[nodiscard]] std::vector<TooltipRow> buildTooltipRows(const std::string& currentValue) const;
  [[nodiscard]] static double normalizedFromStats(
      SysmonStat stat, const SystemStats& stats, double& tempMin, double& tempMax, std::string_view networkInterface
  );

  SystemMonitorService* m_monitor;
  SysmonStat m_stat;
  SysmonDisplayMode m_displayMode;
  ColorSpec m_highlightColor = colorSpecFromRole(ColorRole::Error);
  ConfigService& m_configService;
  bool m_showLabel;
  float m_labelMinWidth = 0.0f;
  std::string m_diskPath;
  std::string m_networkInterface;
  FormatUnits::DecimalByteRateUnit m_networkSpeedUnit = FormatUnits::DecimalByteRateUnit::Auto;
  FormatUnits::ByteRateLabelStyle m_networkSpeedLabelStyle = FormatUnits::ByteRateLabelStyle::Full;
  std::string m_glyphOverride;
  WidgetCustomImage m_customImage;
  bool m_showUnits;
  SysmonGlyphPosition m_glyphPosition;
  std::string m_lastRawValue;
  bool m_isVerticalBar = false;
  bool m_lastLabelVertical = false;

  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;
  Flex* m_containerRow = nullptr;

  static constexpr int kHistorySamples = 30;
  bool m_graphInitialized = false;
  std::chrono::steady_clock::time_point m_lastSampleAt;
  double m_tempMin = 30.0;
  double m_tempMax = 80.0;
  Box* m_chartBg = nullptr;
  Graph* m_graph = nullptr;
  float m_scrollProgress = 1.0f;
  Timer m_updateTimer;
  FrameRateLimiter m_redrawLimiter{std::chrono::milliseconds{200}};

  // Gauge mode
  ProgressBar* m_gauge = nullptr;

  Signal<>::ScopedConnection m_paletteConn;
};
