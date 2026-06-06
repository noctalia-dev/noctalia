#include "shell/control_center/system_tab.h"

#include "i18n/i18n.h"
#include "render/scene/graph_node.h"
#include "shell/panel/panel_manager.h"
#include "system/distro_info.h"
#include "system/format_units.h"
#include "system/hardware_info.h"
#include "system/system_monitor_service.h"
#include "time/time_format.h"
#include "ui/builders.h"

#include <algorithm>
#include <format>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kGraphLineWidth = 0.75f;
  constexpr float kGraphFillOpacity = 0.15f;
  constexpr double kNetMinScaleBps = 10000.0;

  Flex* makeHeaderRow(Flex& parent, const std::string& title, float scale) {
    Flex* ptr = nullptr;
    auto row = ui::row(
        {.out = &ptr, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .fontWeight = FontWeight::Bold,
            .flexGrow = 1.0f,
        })
    );
    parent.addChild(std::move(row));
    return ptr;
  }

  Label* makeValueLabel(Flex& parent, float scale) {
    Label* ptr = nullptr;
    parent.addChild(
        ui::label({
            .out = &ptr,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    return ptr;
  }

  Flex* makeIconLabel(Flex& parent, const char* glyphName, float scale, Glyph** outIcon = nullptr) {
    Flex* ptr = nullptr;
    auto group = ui::row(
        {.out = &ptr, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::glyph({
            .out = outIcon,
            .glyph = glyphName,
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    parent.addChild(std::move(group));
    return ptr;
  }

  GraphNode* addGraph(Flex& parent) {
    auto graph = std::make_unique<GraphNode>();
    graph->setGraphFillOpacity(kGraphFillOpacity);
    auto* ptr = graph.get();
    parent.addChild(std::move(graph));
    return ptr;
  }

  std::string formatMemoryUsedTotal(const SystemStats& stats) {
    if (stats.ramTotalMb == 0) {
      return memoryTotalLabel();
    }
    return FormatUnits::formatBinaryMibUsageAsGib(stats.ramUsedMb, stats.ramTotalMb);
  }

  std::string formatGpuVramUsed(const SystemStats& stats) {
    if (!stats.gpuVramUsedBytes.has_value()) {
      return "--";
    }
    return FormatUnits::formatBinaryBytesAsGib(*stats.gpuVramUsedBytes);
  }

  Flex* makeInfoCard(
      Flex& parent, const std::string& title, float scale, float fillOpacity, bool showBorder, Label** outLines,
      int lineCount, const char* const* glyphs
  ) {
    auto card = ui::column({
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0f,
        .configure = [scale, fillOpacity, showBorder](Flex& section) {
          applySectionCardStyle(section, scale, fillOpacity, showBorder);
        },
    });

    addTitle(*card, title, scale);

    for (int i = 0; i < lineCount; ++i) {
      card->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
              ui::glyph({
                  .glyph = glyphs[i],
                  .glyphSize = Style::fontSizeMini * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              }),
              ui::label({
                  .out = &outLines[i],
                  .fontSize = Style::fontSizeMini * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .flexGrow = 1.0f,
              })
          )
      );
    }

    auto* ptr = card.get();
    parent.addChild(std::move(card));
    return ptr;
  }

} // namespace

SystemTab::SystemTab(SystemMonitorService* monitor) : m_monitor(monitor) {
  if (m_monitor != nullptr) {
    m_monitor->retainCpuTemp();
    m_monitor->retainGpuTemp();
    m_monitor->retainGpuUsage();
    m_monitor->retainGpuVram();
  }
}

SystemTab::~SystemTab() {
  if (m_monitor != nullptr) {
    m_monitor->releaseCpuTemp();
    m_monitor->releaseGpuTemp();
    m_monitor->releaseGpuUsage();
    m_monitor->releaseGpuVram();
  }
}

std::unique_ptr<Flex> SystemTab::create() {
  const float sc = contentScale();

  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * sc,
  });

  // --- Graph grid ---
  // Row 1: CPU, Memory
  {
    auto row = ui::row({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * sc,
        .flexGrow = 1.0f,
    });

    // CPU card
    {
      auto card = ui::column({
          .out = &m_cpuCard,
          .flexGrow = 1.0f,
          .configure = [sc, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
            applySectionCardStyle(section, sc, opacity, borders);
          },
      });

      auto* header = makeHeaderRow(*card, i18n::tr("control-center.system.titles.cpu"), sc);
      auto* cpuPctGroup = makeIconLabel(*header, "cpu-usage", sc, &m_cpuPctIcon);
      m_cpuPctLabel = makeValueLabel(*cpuPctGroup, sc);
      auto* cpuTempGroup = makeIconLabel(*header, "cpu-temperature", sc, &m_cpuTempIcon);
      m_cpuTempLabel = makeValueLabel(*cpuTempGroup, sc);
      m_cpuGraph = addGraph(*card);

      row->addChild(std::move(card));
    }

    // Memory card
    {
      auto card = ui::column({
          .out = &m_ramCard,
          .flexGrow = 1.0f,
          .configure = [sc, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
            applySectionCardStyle(section, sc, opacity, borders);
          },
      });

      auto* header = makeHeaderRow(*card, i18n::tr("control-center.system.titles.memory"), sc);
      auto* ramGroup = makeIconLabel(*header, "memory", sc, &m_ramIcon);
      m_ramLabel = makeValueLabel(*ramGroup, sc);
      m_ramGraph = addGraph(*card);

      row->addChild(std::move(card));
    }

    tab->addChild(std::move(row));
  }

  // Row 2: GPU (optional), Network
  {
    auto row = ui::row({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * sc,
        .flexGrow = 1.0f,
    });

    // GPU card
    {
      auto card = ui::column({
          .out = &m_gpuCard,
          .flexGrow = 1.0f,
          .visible = false,
          .configure = [sc, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
            applySectionCardStyle(section, sc, opacity, borders);
          },
      });

      auto* header = makeHeaderRow(*card, i18n::tr("control-center.system.titles.gpu"), sc);
      m_gpuUsageGroup = makeIconLabel(*header, "gpu-usage", sc, &m_gpuUsageIcon);
      m_gpuUsageLabel = makeValueLabel(*m_gpuUsageGroup, sc);
      m_gpuUsageGroup->setVisible(false);
      m_gpuVramGroup = makeIconLabel(*header, "memory", sc, &m_gpuVramIcon);
      m_gpuVramLabel = makeValueLabel(*m_gpuVramGroup, sc);
      m_gpuVramGroup->setVisible(false);
      m_gpuTempGroup = makeIconLabel(*header, "temperature", sc, &m_gpuTempIcon);
      m_gpuTempLabel = makeValueLabel(*m_gpuTempGroup, sc);
      m_gpuTempGroup->setVisible(false);
      m_gpuGraph = addGraph(*card);

      row->addChild(std::move(card));
    }

    // Network card
    {
      auto card = ui::column({
          .out = &m_netCard,
          .flexGrow = 1.0f,
          .configure = [sc, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
            applySectionCardStyle(section, sc, opacity, borders);
          },
      });

      auto* header = makeHeaderRow(*card, i18n::tr("control-center.system.titles.network"), sc);
      auto* rxGroup = makeIconLabel(*header, "download-speed", sc, &m_rxIcon);
      m_rxLabel = makeValueLabel(*rxGroup, sc);
      auto* txGroup = makeIconLabel(*header, "upload-speed", sc, &m_txIcon);
      m_txLabel = makeValueLabel(*txGroup, sc);
      m_netGraph = addGraph(*card);

      row->addChild(std::move(card));
    }

    tab->addChild(std::move(row));
  }

  // --- Info row: System, Resources ---
  {
    auto row = ui::row({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * sc,
    });
    static constexpr const char* kSystemGlyphs[] = {"device-desktop", "layout-board", "cpu-usage",
                                                    "video",          "app-window",   "clock"};
    makeInfoCard(
        *row, i18n::tr("control-center.system.titles.system"), sc, panelCardOpacity(), panelBordersEnabled(),
        m_systemLines, kSystemLines, kSystemGlyphs
    )
        ->setFlexGrow(2.0f);

    static constexpr const char* kResourcesGlyphs[] = {"activity", "memory", "storage"};
    makeInfoCard(
        *row, i18n::tr("control-center.system.titles.resources"), sc, panelCardOpacity(), panelBordersEnabled(),
        m_resourcesLines, kResourcesLines, kResourcesGlyphs
    );

    tab->addChild(std::move(row));
  }

  return tab;
}

void SystemTab::setActive(bool active) { m_active = active; }

void SystemTab::onClose() {
  m_root = nullptr;
  m_cpuGraph = nullptr;
  m_ramGraph = nullptr;
  m_gpuGraph = nullptr;
  m_netGraph = nullptr;
  m_cpuCard = nullptr;
  m_ramCard = nullptr;
  m_gpuCard = nullptr;
  m_netCard = nullptr;
  m_cpuPctIcon = nullptr;
  m_cpuPctLabel = nullptr;
  m_cpuTempIcon = nullptr;
  m_cpuTempLabel = nullptr;
  m_gpuTempGroup = nullptr;
  m_gpuTempIcon = nullptr;
  m_gpuTempLabel = nullptr;
  m_gpuUsageGroup = nullptr;
  m_gpuUsageIcon = nullptr;
  m_gpuUsageLabel = nullptr;
  m_gpuVramGroup = nullptr;
  m_gpuVramIcon = nullptr;
  m_gpuVramLabel = nullptr;
  m_ramIcon = nullptr;
  m_ramLabel = nullptr;
  m_rxIcon = nullptr;
  m_rxLabel = nullptr;
  m_txIcon = nullptr;
  m_txLabel = nullptr;
  for (auto& l : m_systemLines)
    l = nullptr;
  for (auto& l : m_resourcesLines)
    l = nullptr;
  m_graphInitialized = false;
  m_gpuVisible = false;
  m_lastSampleAt = {};
  m_scrollProgress = 1.0f;
  m_cpuTempMin = 30.0;
  m_cpuTempMax = 80.0;
  m_gpuTempMin = 30.0;
  m_gpuTempMax = 80.0;
  m_netPeak = 0.0;
}

void SystemTab::onFrameTick(float deltaMs) {
  (void)deltaMs;

  if (!m_active || m_monitor == nullptr || !m_monitor->isRunning()) {
    return;
  }

  const auto latestSampleAt = m_monitor->latest().sampledAt;
  if (latestSampleAt != std::chrono::steady_clock::time_point{} && latestSampleAt != m_lastSampleAt) {
    PanelManager::instance().requestUpdateOnly();
  }

  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setScroll1(m_scrollProgress);
    m_cpuGraph->setScroll2(m_scrollProgress);
  }
  if (m_ramGraph != nullptr) {
    m_ramGraph->setScroll1(m_scrollProgress);
  }
  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setScroll1(m_scrollProgress);
    m_gpuGraph->setScroll2(m_scrollProgress);
    m_gpuGraph->setScroll3(m_scrollProgress);
  }
  if (m_netGraph != nullptr) {
    m_netGraph->setScroll1(m_scrollProgress);
    m_netGraph->setScroll2(m_scrollProgress);
  }

  PanelManager::instance().requestRedraw();
}

void SystemTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }

  const float sc = contentScale();

  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);

  const float cardPadH = Style::spaceMd * sc * 2.0f;

  auto sizeGraph = [&](GraphNode* g, Flex* card) {
    if (g == nullptr || card == nullptr || !card->visible()) {
      return;
    }
    const float graphW = std::max(0.0f, card->width() - cardPadH);
    const float usedAbove = g->y() - card->y();
    const float bottomPad = (Style::spaceSm + Style::spaceXs) * sc;
    const float graphH = std::max(0.0f, card->height() - usedAbove - bottomPad);
    g->setSize(graphW, graphH);
    g->setLineWidth(kGraphLineWidth * sc);
  };

  sizeGraph(m_cpuGraph, m_cpuCard);
  sizeGraph(m_ramGraph, m_ramCard);
  if (m_gpuVisible) {
    sizeGraph(m_gpuGraph, m_gpuCard);
  }
  sizeGraph(m_netGraph, m_netCard);

  m_root->layout(renderer);
}

void SystemTab::doUpdate(Renderer& renderer) {
  if (!m_active || m_monitor == nullptr) {
    return;
  }

  const bool monitorRunning = m_monitor->isRunning();

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setLineColor1(colorForRole(ColorRole::Primary));
    m_cpuGraph->setLineColor2(colorForRole(ColorRole::Error));
  }
  if (m_cpuPctIcon != nullptr) {
    m_cpuPctIcon->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_cpuPctLabel != nullptr) {
    m_cpuPctLabel->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_cpuTempIcon != nullptr) {
    m_cpuTempIcon->setColor(colorSpecFromRole(ColorRole::Error));
  }
  if (m_cpuTempLabel != nullptr) {
    m_cpuTempLabel->setColor(colorSpecFromRole(ColorRole::Error));
  }

  if (m_ramGraph != nullptr) {
    m_ramGraph->setLineColor1(colorForRole(ColorRole::Secondary));
  }
  if (m_ramIcon != nullptr) {
    m_ramIcon->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_ramLabel != nullptr) {
    m_ramLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
  }

  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setLineColor1(colorForRole(ColorRole::Primary));
    m_gpuGraph->setLineColor2(colorForRole(ColorRole::Secondary));
    m_gpuGraph->setLineColor3(colorForRole(ColorRole::Error));
  }
  if (m_gpuUsageIcon != nullptr) {
    m_gpuUsageIcon->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_gpuUsageLabel != nullptr) {
    m_gpuUsageLabel->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  if (m_gpuVramIcon != nullptr) {
    m_gpuVramIcon->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_gpuVramLabel != nullptr) {
    m_gpuVramLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_gpuTempIcon != nullptr) {
    m_gpuTempIcon->setColor(colorSpecFromRole(ColorRole::Error));
  }
  if (m_gpuTempLabel != nullptr) {
    m_gpuTempLabel->setColor(colorSpecFromRole(ColorRole::Error));
  }

  if (m_netGraph != nullptr) {
    m_netGraph->setLineColor1(colorForRole(ColorRole::Tertiary));
    m_netGraph->setLineColor2(colorForRole(ColorRole::Secondary));
  }
  if (m_rxIcon != nullptr) {
    m_rxIcon->setColor(colorSpecFromRole(ColorRole::Tertiary));
  }
  if (m_rxLabel != nullptr) {
    m_rxLabel->setColor(colorSpecFromRole(ColorRole::Tertiary));
  }
  if (m_txIcon != nullptr) {
    m_txIcon->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_txLabel != nullptr) {
    m_txLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
  }

  if (monitorRunning) {
    updateGraphs(renderer);
  } else {
    if (m_cpuGraph != nullptr) {
      m_cpuGraph->setCount1(0.0f);
      m_cpuGraph->setCount2(0.0f);
    }
    if (m_ramGraph != nullptr) {
      m_ramGraph->setCount1(0.0f);
    }
    if (m_gpuGraph != nullptr) {
      m_gpuGraph->setCount1(0.0f);
      m_gpuGraph->setCount2(0.0f);
      m_gpuGraph->setCount3(0.0f);
    }
    if (m_netGraph != nullptr) {
      m_netGraph->setCount1(0.0f);
      m_netGraph->setCount2(0.0f);
    }
    m_graphInitialized = false;
    m_lastSampleAt = {};
    m_scrollProgress = 1.0f;
  }
  syncLabels();
}

void SystemTab::updateGraphs(Renderer& renderer) {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
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
  const auto sz = n + 1U;
  const auto last = n;
  const auto prev = n - 1U;
  const auto prev2 = n - 2U;

  // CPU: usage (primary) + CPU temp (secondary)
  if (m_cpuGraph != nullptr) {
    std::vector<float> usage(sz);
    std::vector<float> cpuTemp(sz);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      usage[i] = static_cast<float>(std::clamp(s.cpuUsagePercent / 100.0, 0.0, 1.0));

      if (s.cpuTempC.has_value()) {
        const double t = *s.cpuTempC;
        m_cpuTempMin = std::min(m_cpuTempMin, t);
        m_cpuTempMax = std::max(m_cpuTempMax, t);
        const double range = m_cpuTempMax - m_cpuTempMin;
        cpuTemp[i] = range > 0.0 ? static_cast<float>(std::clamp((t - m_cpuTempMin) / range, 0.0, 1.0)) : 0.5f;
      }
    }
    usage[last] = std::clamp(usage[prev] + (usage[prev] - usage[prev2]) * 0.5f, 0.0f, 1.0f);
    cpuTemp[last] = std::clamp(cpuTemp[prev] + (cpuTemp[prev] - cpuTemp[prev2]) * 0.5f, 0.0f, 1.0f);
    m_cpuGraph->setData(renderer.textureManager(), usage.data(), texSize, cpuTemp.data(), texSize);
    m_cpuGraph->setCount1(static_cast<float>(n));
    m_cpuGraph->setCount2(static_cast<float>(n));
  }

  // Memory
  if (m_ramGraph != nullptr) {
    std::vector<float> ram(sz);
    for (std::size_t i = 0; i < n; ++i) {
      ram[i] = static_cast<float>(std::clamp(hist[i].ramUsagePercent / 100.0, 0.0, 1.0));
    }
    ram[last] = std::clamp(ram[prev] + (ram[prev] - ram[prev2]) * 0.5f, 0.0f, 1.0f);
    m_ramGraph->setData(renderer.textureManager(), ram.data(), texSize, nullptr, 0);
    m_ramGraph->setCount1(static_cast<float>(n));
  }

  // GPU: usage (primary) + VRAM (secondary) + temperature (tertiary)
  if (m_gpuGraph != nullptr) {
    bool hasGpuUsage = false;
    bool hasGpuTemp = false;
    bool hasGpuVram = false;
    std::vector<float> gpuUsage(sz);
    std::vector<float> gpuVram(sz);
    std::vector<float> gpuTemp(sz);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      if (s.gpuUsagePercent.has_value()) {
        hasGpuUsage = true;
        gpuUsage[i] = static_cast<float>(std::clamp(*s.gpuUsagePercent / 100.0, 0.0, 1.0));
      }
      if (s.gpuVramUsedBytes.has_value() && s.gpuVramTotalBytes.has_value() && *s.gpuVramTotalBytes > 0) {
        hasGpuVram = true;
        gpuVram[i] = static_cast<float>(
            std::clamp(static_cast<double>(*s.gpuVramUsedBytes) / static_cast<double>(*s.gpuVramTotalBytes), 0.0, 1.0)
        );
      }
      if (s.gpuTempC.has_value()) {
        hasGpuTemp = true;
        const double t = *s.gpuTempC;
        m_gpuTempMin = std::min(m_gpuTempMin, t);
        m_gpuTempMax = std::max(m_gpuTempMax, t);
        const double range = m_gpuTempMax - m_gpuTempMin;
        gpuTemp[i] = range > 0.0 ? static_cast<float>(std::clamp((t - m_gpuTempMin) / range, 0.0, 1.0)) : 0.5f;
      }
    }
    if (hasGpuUsage) {
      gpuUsage[last] = std::clamp(gpuUsage[prev] + (gpuUsage[prev] - gpuUsage[prev2]) * 0.5f, 0.0f, 1.0f);
    }
    if (hasGpuVram) {
      gpuVram[last] = std::clamp(gpuVram[prev] + (gpuVram[prev] - gpuVram[prev2]) * 0.5f, 0.0f, 1.0f);
    }
    if (hasGpuTemp) {
      gpuTemp[last] = std::clamp(gpuTemp[prev] + (gpuTemp[prev] - gpuTemp[prev2]) * 0.5f, 0.0f, 1.0f);
    }
    if (hasGpuUsage || hasGpuVram || hasGpuTemp) {
      m_gpuGraph->setData(
          renderer.textureManager(), hasGpuUsage ? gpuUsage.data() : nullptr, hasGpuUsage ? texSize : 0,
          hasGpuVram ? gpuVram.data() : nullptr, hasGpuVram ? texSize : 0, hasGpuTemp ? gpuTemp.data() : nullptr,
          hasGpuTemp ? texSize : 0
      );
      m_gpuGraph->setCount1(hasGpuUsage ? static_cast<float>(n) : 0.0f);
      m_gpuGraph->setCount2(hasGpuVram ? static_cast<float>(n) : 0.0f);
      m_gpuGraph->setCount3(hasGpuTemp ? static_cast<float>(n) : 0.0f);
    } else {
      m_gpuGraph->setCount1(0.0f);
      m_gpuGraph->setCount2(0.0f);
      m_gpuGraph->setCount3(0.0f);
    }
    const bool hasGpuData = hasGpuUsage || hasGpuVram || hasGpuTemp;
    if (hasGpuData != m_gpuVisible) {
      m_gpuVisible = hasGpuData;
      updateGpuVisibility();
    }
  }

  // Network
  if (m_netGraph != nullptr) {
    double maxVal = kNetMinScaleBps;
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      maxVal = std::max({maxVal, s.netRxBytesPerSec, s.netTxBytesPerSec});
    }
    m_netPeak = maxVal;

    std::vector<float> rx(sz);
    std::vector<float> tx(sz);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      rx[i] = static_cast<float>(std::clamp(s.netRxBytesPerSec / m_netPeak, 0.0, 1.0));
      tx[i] = static_cast<float>(std::clamp(s.netTxBytesPerSec / m_netPeak, 0.0, 1.0));
    }
    rx[last] = std::clamp(rx[prev] + (rx[prev] - rx[prev2]) * 0.5f, 0.0f, 1.0f);
    tx[last] = std::clamp(tx[prev] + (tx[prev] - tx[prev2]) * 0.5f, 0.0f, 1.0f);
    m_netGraph->setData(renderer.textureManager(), rx.data(), texSize, tx.data(), texSize);
    m_netGraph->setCount1(static_cast<float>(n));
    m_netGraph->setCount2(static_cast<float>(n));
  }

  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setScroll1(m_scrollProgress);
    m_cpuGraph->setScroll2(m_scrollProgress);
  }
  if (m_ramGraph != nullptr) {
    m_ramGraph->setScroll1(m_scrollProgress);
  }
  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setScroll1(m_scrollProgress);
    m_gpuGraph->setScroll2(m_scrollProgress);
    m_gpuGraph->setScroll3(m_scrollProgress);
  }
  if (m_netGraph != nullptr) {
    m_netGraph->setScroll1(m_scrollProgress);
    m_netGraph->setScroll2(m_scrollProgress);
  }

  PanelManager::instance().requestLayout();
  if (m_scrollProgress < 1.0f) {
    PanelManager::instance().requestRedraw();
  }
}

void SystemTab::updateGpuVisibility() {
  if (m_gpuCard != nullptr) {
    m_gpuCard->setVisible(m_gpuVisible);
  }
  PanelManager::instance().requestLayout();
}

void SystemTab::syncLabels() {
  if (m_monitor == nullptr || !m_monitor->isRunning()) {
    if (m_cpuPctLabel != nullptr) {
      m_cpuPctLabel->setText("--");
    }
    if (m_cpuTempLabel != nullptr) {
      m_cpuTempLabel->setText("--");
    }
    if (m_gpuTempLabel != nullptr) {
      m_gpuTempLabel->setText("--");
    }
    if (m_gpuUsageLabel != nullptr) {
      m_gpuUsageLabel->setText("--");
    }
    if (m_gpuVramLabel != nullptr) {
      m_gpuVramLabel->setText("--");
    }
    if (m_ramLabel != nullptr) {
      m_ramLabel->setText("--");
    }
    if (m_rxLabel != nullptr) {
      m_rxLabel->setText("--");
    }
    if (m_txLabel != nullptr) {
      m_txLabel->setText("--");
    }
    return;
  }

  const auto stats = m_monitor->latest();

  if (m_cpuPctLabel != nullptr) {
    m_cpuPctLabel->setText(std::format("{:.0f}%", stats.cpuUsagePercent));
  }
  if (m_cpuTempLabel != nullptr) {
    if (stats.cpuTempC.has_value()) {
      m_cpuTempLabel->setText(std::format("{:.0f}°C", *stats.cpuTempC));
    } else {
      m_cpuTempLabel->setText("--");
    }
  }
  if (m_gpuTempGroup != nullptr) {
    const bool hasTempData = stats.gpuTempC.has_value();
    m_gpuTempGroup->setVisible(hasTempData);
    if (hasTempData && m_gpuTempLabel != nullptr) {
      m_gpuTempLabel->setText(std::format("{:.0f}°C", *stats.gpuTempC));
    }
  }
  if (m_gpuUsageGroup != nullptr) {
    const bool hasUsageData = stats.gpuUsagePercent.has_value();
    m_gpuUsageGroup->setVisible(hasUsageData);
    if (hasUsageData && m_gpuUsageLabel != nullptr) {
      m_gpuUsageLabel->setText(std::format("{:.0f}%", *stats.gpuUsagePercent));
    }
  }
  if (m_gpuVramGroup != nullptr) {
    const bool hasVramData = stats.gpuVramUsedBytes.has_value();
    m_gpuVramGroup->setVisible(hasVramData);
    if (hasVramData && m_gpuVramLabel != nullptr) {
      m_gpuVramLabel->setText(formatGpuVramUsed(stats));
    }
  }
  if (m_ramLabel != nullptr) {
    m_ramLabel->setText(
        FormatUnits::formatBinaryMibAsGib(stats.ramUsedMb) + std::format(" · {:.0f}%", stats.ramUsagePercent)
    );
  }
  if (m_rxLabel != nullptr) {
    m_rxLabel->setText(FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec));
  }
  if (m_txLabel != nullptr) {
    m_txLabel->setText(FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec));
  }

  // System info
  if (m_systemLines[0] != nullptr) {
    m_systemLines[0]->setText(distroLabel() + " · " + kernelRelease());
  }
  if (m_systemLines[1] != nullptr) {
    m_systemLines[1]->setText(motherboardLabel());
  }
  if (m_systemLines[2] != nullptr) {
    m_systemLines[2]->setText(cpuModelName());
  }
  if (m_systemLines[3] != nullptr) {
    m_systemLines[3]->setText(gpuLabel());
  }
  if (m_systemLines[4] != nullptr) {
    m_systemLines[4]->setText(compositorLabel());
  }
  if (m_systemLines[5] != nullptr) {
    const auto uptime = systemUptime();
    const std::string uptimeText =
        uptime.has_value() ? formatDuration(*uptime) : i18n::tr("control-center.system.unknown");
    m_systemLines[5]->setText(
        i18n::tr("control-center.system.uptime-prefix", "uptime", uptimeText, "osAge", osAgeLabel())
    );
  }

  // Resources info
  if (m_resourcesLines[0] != nullptr) {
    m_resourcesLines[0]->setText(
        std::format("{:.2f} / {:.2f} / {:.2f}", stats.loadAvg1, stats.loadAvg5, stats.loadAvg15)
    );
  }
  if (m_resourcesLines[1] != nullptr) {
    m_resourcesLines[1]->setText(formatMemoryUsedTotal(stats));
  }
  if (m_resourcesLines[2] != nullptr) {
    m_resourcesLines[2]->setText(diskRootUsageLabel());
  }
}

float SystemTab::scrollProgressForSample(std::chrono::steady_clock::time_point sampledAt) const {
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
