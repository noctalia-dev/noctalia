#pragma once

#include "core/frame_rate_limiter.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <string>
#include <vector>

class Flex;
class Glyph;
class Graph;
class Label;
class SystemMonitorService;

class SystemTab : public Tab {
public:
  explicit SystemTab(SystemMonitorService* monitor);
  ~SystemTab() override;

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  void setActive(bool active) override;
  void onFrameTick(float deltaMs) override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  void updateGraphs(Renderer& renderer);
  void syncLabels();
  void updateGpuVisibility();
  [[nodiscard]] float scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const;

  SystemMonitorService* m_monitor;
  bool m_active = false;
  bool m_graphInitialized = false;
  bool m_gpuVisible = false;
  float m_scrollProgress = 1.0f;
  std::chrono::steady_clock::time_point m_lastSampleAt;
  FrameRateLimiter m_redrawLimiter{std::chrono::milliseconds{200}};

  double m_cpuTempMin = 30.0;
  double m_cpuTempMax = 80.0;
  double m_gpuTempMin = 30.0;
  double m_gpuTempMax = 80.0;
  double m_netPeak = 0.0;

  Flex* m_root = nullptr;

  Graph* m_cpuGraph = nullptr;
  Graph* m_ramGraph = nullptr;
  Graph* m_gpuGraph = nullptr;
  Graph* m_netGraph = nullptr;

  Flex* m_cpuCard = nullptr;
  Flex* m_ramCard = nullptr;
  Flex* m_gpuCard = nullptr;
  Flex* m_netCard = nullptr;

  Flex* m_cpuLegend = nullptr;
  Flex* m_ramLegend = nullptr;
  Flex* m_gpuLegend = nullptr;
  Flex* m_netLegend = nullptr;

  Glyph* m_cpuPctIcon = nullptr;
  Label* m_cpuPctLabel = nullptr;
  Glyph* m_cpuTempIcon = nullptr;
  Label* m_cpuTempLabel = nullptr;
  Flex* m_gpuTempGroup = nullptr;
  Glyph* m_gpuTempIcon = nullptr;
  Label* m_gpuTempLabel = nullptr;
  Flex* m_gpuUsageGroup = nullptr;
  Glyph* m_gpuUsageIcon = nullptr;
  Label* m_gpuUsageLabel = nullptr;
  Flex* m_gpuVramGroup = nullptr;
  Glyph* m_gpuVramIcon = nullptr;
  Label* m_gpuVramLabel = nullptr;
  Glyph* m_ramIcon = nullptr;
  Label* m_ramLabel = nullptr;
  Glyph* m_rxIcon = nullptr;
  Label* m_rxLabel = nullptr;
  Glyph* m_txIcon = nullptr;
  Label* m_txLabel = nullptr;

  // System card: cpu, gpu, distro, kernel, wm, uptime + os age
  static constexpr int kSystemLines = 6;
  Label* m_systemLines[kSystemLines] = {};

  // Resources card: load, memory, swap (hidden when no swap), then up to four discovered physical disks.
  static constexpr int kResourcesLines = 2;
  Label* m_resourcesLines[kResourcesLines] = {};
  Flex* m_swapRow = nullptr;
  Label* m_swapLabel = nullptr;
  std::vector<std::string> m_diskMountPoints;
  std::vector<Label*> m_diskLabels;
};
