#pragma once

#include "shell/desktop/desktop_widget.h"
#include "ui/palette.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

struct SystemStats;

class Glyph;
class GraphNode;
class Label;
class SystemMonitorService;

enum class DesktopSysmonStat : std::uint8_t { CpuUsage, CpuTemp, GpuTemp, GpuVram, RamPct, SwapPct, NetRx, NetTx };

class DesktopSysmonWidget : public DesktopWidget {
public:
  DesktopSysmonWidget(
      SystemMonitorService* monitor, DesktopSysmonStat stat, std::optional<DesktopSysmonStat> stat2,
      ColorSpec lineColor, ColorSpec lineColor2, bool showLabel, bool shadow
  );
  ~DesktopSysmonWidget() override;

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;
  [[nodiscard]] bool needsFrameTick() const override { return true; }
  void onFrameTick(float deltaMs, Renderer& renderer) override;

private:
  void doLayout(Renderer& renderer) override;
  void doUpdate(Renderer& renderer) override;

  [[nodiscard]] std::string formatValueFor(DesktopSysmonStat stat) const;
  void syncLabel();
  void clearGraph();
  void updateGraph(Renderer& renderer);
  [[nodiscard]] float scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const;
  [[nodiscard]] static double
  normalizedFromStats(DesktopSysmonStat stat, const SystemStats& stats, double& tempMin, double& tempMax);
  [[nodiscard]] static const char* glyphName(DesktopSysmonStat stat);

  SystemMonitorService* m_monitor;
  DesktopSysmonStat m_stat;
  std::optional<DesktopSysmonStat> m_stat2;
  ColorSpec m_lineColor;
  ColorSpec m_lineColor2;
  bool m_showLabel;
  bool m_shadow;

  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  GraphNode* m_graphNode = nullptr;

  bool m_graphInitialized = false;
  float m_scrollProgress = 1.0f;
  std::chrono::steady_clock::time_point m_lastSampleAt{};
  std::string m_lastRawValue;

  mutable double m_tempMin1 = 30.0;
  mutable double m_tempMax1 = 80.0;
  mutable double m_tempMin2 = 30.0;
  mutable double m_tempMax2 = 80.0;
};
