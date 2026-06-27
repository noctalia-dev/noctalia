#include "shell/control_center/system_tab.h"

#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "system/distro_info.h"
#include "system/format_units.h"
#include "system/hardware_info.h"
#include "system/system_monitor_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/graph.h"

#include <algorithm>
#include <format>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kGraphLineWidth = 0.75f;
  constexpr float kGraphFillOpacity = 0.15f;
  constexpr double kNetMinScaleBps = 10000.0;
  constexpr std::size_t kMaxDiskRows = 4;

  // Graph cards run tighter than the default section card so the plot fills more of the card,
  // but the title gets a touch more breathing room above it.
  constexpr float kGraphCardPadTop = Style::spaceSm;
  constexpr float kGraphCardPadV = Style::spaceXs;
  constexpr float kGraphCardPadH = Style::spaceMd;

  // Section card style with reduced padding, shared by the four graph cards.
  void applyGraphCardStyle(Flex& section, float scale, float opacity, bool borders) {
    control_center::applySectionCardStyle(section, scale, opacity, borders);
    section.setGap(Style::spaceXs * scale);
    section.setPadding(
        kGraphCardPadTop * scale, kGraphCardPadH * scale, kGraphCardPadV * scale, kGraphCardPadH * scale
    );
  }

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

  Flex* makeIconLabel(Flex& parent, const char* glyphName, float scale, Glyph** outIcon = nullptr) {
    Flex* ptr = nullptr;
    auto group = ui::row(
        {.out = &ptr, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::glyph({
            .out = outIcon,
            .glyph = glyphName,
            .glyphSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    parent.addChild(std::move(group));
    return ptr;
  }

  Label* makeValueLabel(Flex& parent, float scale) {
    Label* ptr = nullptr;
    parent.addChild(
        ui::label({
            .out = &ptr,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    return ptr;
  }

  // Legend row sits below the graph and centers its icon+value groups so wide
  // stats get the full card width instead of competing with the title.
  Flex* makeLegendRow(Flex& parent, float scale) {
    Flex* ptr = nullptr;
    auto row = ui::row({
        .out = &ptr,
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceMd * scale,
    });
    parent.addChild(std::move(row));
    return ptr;
  }

  Graph* addGraph(Flex& parent) {
    auto graph = std::make_unique<Graph>();
    graph->setFillOpacity(kGraphFillOpacity);
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

  // Resource row: glyph + name (grows, left) + value (natural width, right-aligned).
  // A non-empty tooltip is attached to the row's text (labels opt out of hit-testing by default).
  Label* addResourceRow(
      Flex& card, const char* glyphName, const std::string& name, float scale, Flex** outRow = nullptr,
      const std::string& tooltip = {}
  ) {
    Label* value = nullptr;
    Label* nameLabel = nullptr;
    Flex* row = nullptr;
    card.addChild(
        ui::row(
            {.out = &row, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
            ui::glyph({
                .glyph = glyphName,
                .glyphSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            }),
            ui::label({
                .out = &nameLabel,
                .text = name,
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
                .flexGrow = 1.0f,
            }),
            ui::label({
                .out = &value,
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
            })
        )
    );
    if (!tooltip.empty()) {
      for (Label* label : {nameLabel, value}) {
        if (label != nullptr) {
          label->setTooltip(tooltip);
        }
      }
    }
    if (outRow != nullptr) {
      *outRow = row;
    }
    return value;
  }

  Flex* makeInfoCard(
      Flex& parent, const std::string& title, float scale, float grow, float fillOpacity, bool showBorder,
      Label** outLines, int lineCount, const char* const* glyphs
  ) {
    auto card = ui::column({
        .flexGrow = grow,
        .configure = [scale, fillOpacity, showBorder](Flex& section) {
          applySectionCardStyle(section, scale, fillOpacity, showBorder);
          section.setGap(Style::spaceXs * scale);
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
            applyGraphCardStyle(section, sc, opacity, borders);
          },
      });

      makeHeaderRow(*card, i18n::tr("control-center.system.titles.cpu"), sc);
      m_cpuGraph = addGraph(*card);
      auto* legend = makeLegendRow(*card, sc);
      m_cpuLegend = legend;
      auto* cpuPctGroup = makeIconLabel(*legend, "cpu-usage", sc, &m_cpuPctIcon);
      m_cpuPctLabel = makeValueLabel(*cpuPctGroup, sc);
      auto* cpuTempGroup = makeIconLabel(*legend, "cpu-temperature", sc, &m_cpuTempIcon);
      m_cpuTempLabel = makeValueLabel(*cpuTempGroup, sc);

      row->addChild(std::move(card));
    }

    // Memory card
    {
      auto card = ui::column({
          .out = &m_ramCard,
          .flexGrow = 1.0f,
          .configure = [sc, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
            applyGraphCardStyle(section, sc, opacity, borders);
          },
      });

      makeHeaderRow(*card, i18n::tr("control-center.system.titles.memory"), sc);
      m_ramGraph = addGraph(*card);
      auto* legend = makeLegendRow(*card, sc);
      m_ramLegend = legend;
      auto* ramGroup = makeIconLabel(*legend, "memory", sc, &m_ramIcon);
      m_ramLabel = makeValueLabel(*ramGroup, sc);

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
            applyGraphCardStyle(section, sc, opacity, borders);
          },
      });

      makeHeaderRow(*card, i18n::tr("control-center.system.titles.gpu"), sc);
      m_gpuGraph = addGraph(*card);
      auto* legend = makeLegendRow(*card, sc);
      m_gpuLegend = legend;
      m_gpuUsageGroup = makeIconLabel(*legend, "gpu-usage", sc, &m_gpuUsageIcon);
      m_gpuUsageLabel = makeValueLabel(*m_gpuUsageGroup, sc);
      m_gpuUsageGroup->setVisible(false);
      m_gpuVramGroup = makeIconLabel(*legend, "memory", sc, &m_gpuVramIcon);
      m_gpuVramLabel = makeValueLabel(*m_gpuVramGroup, sc);
      m_gpuVramGroup->setVisible(false);
      m_gpuTempGroup = makeIconLabel(*legend, "temperature", sc, &m_gpuTempIcon);
      m_gpuTempLabel = makeValueLabel(*m_gpuTempGroup, sc);
      m_gpuTempGroup->setVisible(false);

      row->addChild(std::move(card));
    }

    // Network card
    {
      auto card = ui::column({
          .out = &m_netCard,
          .flexGrow = 1.0f,
          .configure = [sc, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& section) {
            applyGraphCardStyle(section, sc, opacity, borders);
          },
      });

      makeHeaderRow(*card, i18n::tr("control-center.system.titles.network"), sc);
      m_netGraph = addGraph(*card);
      auto* legend = makeLegendRow(*card, sc);
      m_netLegend = legend;
      auto* rxGroup = makeIconLabel(*legend, "download-speed", sc, &m_rxIcon);
      m_rxLabel = makeValueLabel(*rxGroup, sc);
      auto* txGroup = makeIconLabel(*legend, "upload-speed", sc, &m_txIcon);
      m_txLabel = makeValueLabel(*txGroup, sc);

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
    static constexpr const char* kSystemGlyphs[] = {"cpu-usage",        "video",      "device-desktop",
                                                    "layers-intersect", "app-window", "clock"};
    makeInfoCard(
        *row, i18n::tr("control-center.system.titles.system"), sc, 1.0f, panelCardOpacity(), panelBordersEnabled(),
        m_systemLines, kSystemLines, kSystemGlyphs
    );

    auto* resourcesCard = makeInfoCard(
        *row, i18n::tr("control-center.system.titles.resources"), sc, 1.0f, panelCardOpacity(), panelBordersEnabled(),
        nullptr, 0, nullptr
    );

    // Named rows with right-aligned values: load, RAM, then swap (hidden in doUpdate when absent).
    m_resourcesLines[0] = addResourceRow(
        *resourcesCard, "activity", i18n::tr("control-center.system.labels.load"), sc, nullptr,
        i18n::tr("control-center.system.tooltips.load")
    );
    m_resourcesLines[1] = addResourceRow(*resourcesCard, "memory", i18n::tr("control-center.system.labels.ram"), sc);
    m_swapLabel = addResourceRow(
        *resourcesCard, "arrows-exchange", i18n::tr("control-center.system.labels.swap"), sc, &m_swapRow
    );

    // Up to four physical disks, discovered automatically (root first). Usage refreshes in doUpdate.
    m_diskMountPoints = physicalDiskMountPoints();
    if (m_diskMountPoints.size() > kMaxDiskRows) {
      m_diskMountPoints.resize(kMaxDiskRows);
    }
    m_diskLabels.clear();
    m_diskLabels.reserve(m_diskMountPoints.size());
    // Mount path and usage are separate labels: the path grows and ellipsizes when long,
    // while the usage column keeps its natural width so it is never truncated. The full
    // mount path is recoverable via the path label's tooltip.
    for (const auto& mountPoint : m_diskMountPoints) {
      Label* usageLabel = nullptr;
      resourcesCard->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs * sc},
              ui::glyph({
                  .glyph = "storage",
                  .glyphSize = Style::fontSizeMini * sc,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              }),
              ui::label({
                  .text = mountPoint,
                  .fontSize = Style::fontSizeMini * sc,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  // Ellipsize from the start so the identifying tail stays visible ("…/long/mount/point").
                  .ellipsize = TextEllipsize::Start,
                  .flexGrow = 1.0f,
                  .configure = [mountPoint](Label& label) { label.setTooltip(mountPoint); },
              }),
              ui::label({
                  .out = &usageLabel,
                  .fontSize = Style::fontSizeMini * sc,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
              })
          )
      );
      m_diskLabels.push_back(usageLabel);
    }

    tab->addChild(std::move(row));
  }

  return tab;
}

void SystemTab::setActive(bool active) {
  m_active = active;
  if (!active) {
    m_redrawLimiter.reset();
  }
}

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
  m_cpuLegend = nullptr;
  m_ramLegend = nullptr;
  m_gpuLegend = nullptr;
  m_netLegend = nullptr;
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
  m_swapRow = nullptr;
  m_swapLabel = nullptr;
  m_diskLabels.clear();
  m_diskMountPoints.clear();
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
    m_redrawLimiter.reset();
    return;
  }

  if (!m_redrawLimiter.shouldStep([]() { PanelManager::instance().requestRedraw(); })) {
    return;
  }

  const auto latestSampleAt = m_monitor->latest().sampledAt;
  if (latestSampleAt != std::chrono::steady_clock::time_point{} && latestSampleAt != m_lastSampleAt) {
    PanelManager::instance().requestUpdateOnly();
  }

  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setScroll(m_scrollProgress);
  }
  if (m_ramGraph != nullptr) {
    m_ramGraph->setScroll(m_scrollProgress);
  }
  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setScroll(m_scrollProgress);
  }
  if (m_netGraph != nullptr) {
    m_netGraph->setScroll(m_scrollProgress);
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

  const float cardPadH = kGraphCardPadH * sc * 2.0f;

  auto sizeGraph = [&](Graph* g, Flex* card, Flex* legend) {
    if (g == nullptr || card == nullptr || !card->visible()) {
      return;
    }
    const float graphW = std::max(0.0f, card->width() - cardPadH);
    const float usedAbove = g->y() - card->y();
    const float bottomPad = kGraphCardPadV * sc;
    // Reserve room for the legend that now sits below the graph (plus the card gap above it).
    float usedBelow = bottomPad;
    if (legend != nullptr && legend->visible() && legend->height() > 0.0f) {
      usedBelow += legend->height() + Style::spaceXs * sc;
    }
    const float graphH = std::max(0.0f, card->height() - usedAbove - usedBelow);
    g->setSize(graphW, graphH);
    g->setLineWidth(kGraphLineWidth * sc);
  };

  sizeGraph(m_cpuGraph, m_cpuCard, m_cpuLegend);
  sizeGraph(m_ramGraph, m_ramCard, m_ramLegend);
  if (m_gpuVisible) {
    sizeGraph(m_gpuGraph, m_gpuCard, m_gpuLegend);
  }
  sizeGraph(m_netGraph, m_netCard, m_netLegend);

  m_root->layout(renderer);
}

void SystemTab::doUpdate(Renderer& renderer) {
  if (!m_active || m_monitor == nullptr) {
    return;
  }

  const bool monitorRunning = m_monitor->isRunning();

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setColor(colorSpecFromRole(ColorRole::Primary));
    m_cpuGraph->setColor2(colorSpecFromRole(ColorRole::Error));
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
    m_ramGraph->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_ramIcon != nullptr) {
    m_ramIcon->setColor(colorSpecFromRole(ColorRole::Secondary));
  }
  if (m_ramLabel != nullptr) {
    m_ramLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
  }

  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setColor(colorSpecFromRole(ColorRole::Primary));
    m_gpuGraph->setColor2(colorSpecFromRole(ColorRole::Secondary));
    m_gpuGraph->setColor3(colorSpecFromRole(ColorRole::Error));
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
    m_netGraph->setColor(colorSpecFromRole(ColorRole::Tertiary));
    m_netGraph->setColor2(colorSpecFromRole(ColorRole::Secondary));
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
    auto clearGraph = [&renderer](Graph* g) {
      if (g != nullptr) {
        g->setValues({});
        g->setValues2({});
        g->setValues3({});
        g->sync(renderer);
      }
    };
    clearGraph(m_cpuGraph);
    clearGraph(m_ramGraph);
    clearGraph(m_gpuGraph);
    clearGraph(m_netGraph);
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

  // CPU: usage (primary) + CPU temp (secondary)
  if (m_cpuGraph != nullptr) {
    std::vector<float> usage(n);
    std::vector<float> cpuTemp(n);
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
    m_cpuGraph->setValues(std::move(usage));
    m_cpuGraph->setValues2(std::move(cpuTemp));
    m_cpuGraph->sync(renderer);
  }

  // Memory
  if (m_ramGraph != nullptr) {
    std::vector<float> ram(n);
    for (std::size_t i = 0; i < n; ++i) {
      ram[i] = static_cast<float>(std::clamp(hist[i].ramUsagePercent / 100.0, 0.0, 1.0));
    }
    m_ramGraph->setValues(std::move(ram));
    m_ramGraph->sync(renderer);
  }

  // GPU: usage (primary) + VRAM (secondary) + temperature (tertiary)
  if (m_gpuGraph != nullptr) {
    bool hasGpuUsage = false;
    bool hasGpuTemp = false;
    bool hasGpuVram = false;
    std::vector<float> gpuUsage(n);
    std::vector<float> gpuVram(n);
    std::vector<float> gpuTemp(n);
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
    m_gpuGraph->setValues(hasGpuUsage ? std::move(gpuUsage) : std::vector<float>{});
    m_gpuGraph->setValues2(hasGpuVram ? std::move(gpuVram) : std::vector<float>{});
    m_gpuGraph->setValues3(hasGpuTemp ? std::move(gpuTemp) : std::vector<float>{});
    m_gpuGraph->sync(renderer);
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

    std::vector<float> rx(n);
    std::vector<float> tx(n);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = hist[i];
      rx[i] = static_cast<float>(std::clamp(s.netRxBytesPerSec / m_netPeak, 0.0, 1.0));
      tx[i] = static_cast<float>(std::clamp(s.netTxBytesPerSec / m_netPeak, 0.0, 1.0));
    }
    m_netGraph->setValues(std::move(rx));
    m_netGraph->setValues2(std::move(tx));
    m_netGraph->sync(renderer);
  }

  m_graphInitialized = true;
  m_lastSampleAt = latestSampleAt;
  m_scrollProgress = scrollProgressForSample(m_lastSampleAt);

  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setScroll(m_scrollProgress);
  }
  if (m_ramGraph != nullptr) {
    m_ramGraph->setScroll(m_scrollProgress);
  }
  if (m_gpuGraph != nullptr) {
    m_gpuGraph->setScroll(m_scrollProgress);
  }
  if (m_netGraph != nullptr) {
    m_netGraph->setScroll(m_scrollProgress);
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
    m_systemLines[0]->setText(cpuModelName());
  }
  if (m_systemLines[1] != nullptr) {
    m_systemLines[1]->setText(gpuLabel());
  }
  if (m_systemLines[2] != nullptr) {
    m_systemLines[2]->setText(distroLabel());
  }
  if (m_systemLines[3] != nullptr) {
    m_systemLines[3]->setText(kernelLabel());
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
  if (m_swapRow != nullptr) {
    const bool hasSwap = stats.swapTotalMb > 0;
    m_swapRow->setVisible(hasSwap);
    if (hasSwap && m_swapLabel != nullptr) {
      m_swapLabel->setText(FormatUnits::formatBinaryMibUsageAsGib(stats.swapUsedMb, stats.swapTotalMb));
    }
  }
  for (std::size_t i = 0; i < m_diskLabels.size(); ++i) {
    if (m_diskLabels[i] != nullptr) {
      m_diskLabels[i]->setText(diskUsageLabel(m_diskMountPoints[i]));
    }
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
