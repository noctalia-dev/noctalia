#include "shell/process/process_panel.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/segmented.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace {
  constexpr Logger kLog("process-panel");
  constexpr auto kFilterDebounce = std::chrono::milliseconds(150);
  constexpr auto kRefreshInterval = std::chrono::milliseconds(2000);
  constexpr auto kKillConfirmTimeout = std::chrono::milliseconds(3000);
  constexpr float kRowHeight = 42.0F;

  std::string formatMem(std::uint64_t kb) {
    if (kb >= 1024 * 1024) return std::format("{:.1f} GiB", kb / 1024.0 / 1024.0);
    if (kb >= 1024) return std::format("{:.0f} MiB", kb / 1024.0);
    return std::format("{} KiB", kb);
  }

  class ProcessRow final : public InputArea {
  public:
    ProcessRow(float scale, std::function<void(pid_t)> onKill) : m_scale(scale), m_onKill(std::move(onKill)) {
      setVisible(false);
      addChild(ui::box({.out = &m_bg, .radius = Style::scaledRadiusMd(scale)}));
      auto row = ui::row({.out = &m_row, .align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingV = Style::spaceXs * scale, .paddingH = Style::spaceSm * scale});
      addChild(std::move(row));
      m_row->addChild(ui::label({.out = &m_pid, .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .minWidth = 50.0F * scale}));
      m_row->addChild(ui::label({.out = &m_name, .fontSize = Style::fontSizeBody * scale, .fontWeight = FontWeight::Medium, .maxLines = 1, .flexGrow = 1.0F}));
      m_row->addChild(ui::label({.out = &m_cpu, .fontSize = Style::fontSizeCaption * scale, .minWidth = 55.0F * scale}));
      m_row->addChild(ui::label({.out = &m_mem, .fontSize = Style::fontSizeCaption * scale, .minWidth = 70.0F * scale}));
      m_row->addChild(ui::button({.out = &m_kill, .glyph = "trash", .glyphSize = Style::fontSizeBody * scale, .variant = ButtonVariant::Destructive, .minWidth = Style::controlHeightSm * scale, .minHeight = Style::controlHeightSm * scale, .padding = Style::spaceXs * scale, .radius = Style::scaledRadiusSm(scale), .onClick = [this]() { if (m_pidValue>0 && m_onKill) m_onKill(m_pidValue); }}));
    }

    void bind(Renderer& renderer, const ProcessInfo& info, float w, float h, bool selected, bool hovered, bool pendingKill) {
      (void)renderer; (void)selected;
      m_pidValue = info.pid;
      setVisible(true);
      setSize(w, h);
      m_pid->setText(std::to_string(info.pid));
      m_name->setText(info.name);
      m_cpu->setText(std::format("{:.1f}%", info.cpuPercent));
      m_mem->setText(formatMem(info.rssKb));
      // color mem/cpu faint
      m_cpu->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_mem->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      if (m_kill) {
        if (pendingKill) {
          m_kill->setGlyph("warning");
          m_kill->setVariant(ButtonVariant::Destructive);
        } else {
          m_kill->setGlyph("trash");
          m_kill->setVariant(ButtonVariant::Destructive);
        }
        // Disable kill for non-user? We'll keep enabled and let service reject
      }
      if (m_bg) {
        if (hovered) m_bg->setFill(colorSpecFromRole(ColorRole::Hover));
        else if (pendingKill) m_bg->setFill(colorSpecFromRole(ColorRole::Error, 0.12F));
        else m_bg->setFill(clearColorSpec());
      }
      layout(renderer);
    }

    void doLayout(Renderer& renderer) override {
      if (m_bg) { m_bg->setPosition(0,0); m_bg->setSize(width(), height()); }
      if (m_row) { m_row->setPosition(0,0); m_row->setSize(width(), height()); }
      InputArea::doLayout(renderer);
    }

  private:
    float m_scale = 1.0F;
    std::function<void(pid_t)> m_onKill;
    pid_t m_pidValue = -1;
    Box* m_bg = nullptr;
    Flex* m_row = nullptr;
    Label* m_pid = nullptr;
    Label* m_name = nullptr;
    Label* m_cpu = nullptr;
    Label* m_mem = nullptr;
    Button* m_kill = nullptr;
  };

} // namespace

class ProcessListAdapter final : public VirtualGridAdapter {
public:
  ProcessListAdapter(float scale, std::function<void(pid_t)> onKill) : m_scale(scale), m_onKill(std::move(onKill)) {}
  void setProcesses(const std::vector<ProcessInfo>* procs, const std::vector<std::size_t>* indices, pid_t pendingKill) {
    m_processes = procs; m_indices = indices; m_pendingKill = pendingKill;
  }
  void setRenderer(Renderer* r) { m_renderer = r; }
  [[nodiscard]] std::size_t itemCount() const override { return m_indices ? m_indices->size() : 0; }
  [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<ProcessRow>(m_scale, m_onKill); }
  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (!m_renderer || !m_processes || !m_indices || index >= m_indices->size()) return;
    auto* row = static_cast<ProcessRow*>(&tile);
    const std::size_t procIdx = (*m_indices)[index];
    if (procIdx >= m_processes->size()) return;
    const auto& info = (*m_processes)[procIdx];
    row->bind(*m_renderer, info, row->width(), row->height(), selected, hovered, info.pid == m_pendingKill);
  }
private:
  float m_scale = 1.0F;
  std::function<void(pid_t)> m_onKill;
  const std::vector<ProcessInfo>* m_processes = nullptr;
  const std::vector<std::size_t>* m_indices = nullptr;
  pid_t m_pendingKill = -1;
  Renderer* m_renderer = nullptr;
};

ProcessPanel::ProcessPanel(ProcessService* service) : m_service(service) {}
ProcessPanel::~ProcessPanel() = default;

void ProcessPanel::create() {
  const float scale = contentScale();
  auto col = ui::column({.out = &m_rootColumn, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .padding = Style::spaceSm * scale});
  // focus area for key handling
  auto focus = ui::inputArea({});
  focus->setFocusable(true); focus->setTabStop(false); focus->setVisible(false);
  focus->setOnKeyDown([this](const InputArea::KeyData& k){ if(k.pressed) return handleGlobalKey(k.sym,k.modifiers,true,false); return false; });
  m_focusArea = static_cast<InputArea*>(col->addChild(std::move(focus)));

  auto header = ui::row({.out = &m_headerRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  header->addChild(ui::label({.out = &m_titleLabel, .text = "Processes", .fontSize = Style::fontSizeTitle * scale, .fontWeight = FontWeight::Bold, .color = colorSpecFromRole(ColorRole::Primary)}));
  header->addChild(ui::label({.text = "", .flexGrow = 1.0F}));
  // Segmented sort toggle
  auto seg = ui::segmented({.out = &m_sortToggle, .scale = scale});
  seg->addOption("Memory");
  seg->addOption("CPU");
  seg->setSelectedIndex(m_sort == ProcessSort::Memory ? 0 : 1);
  seg->setOnChange([this](std::size_t idx){ onSortChanged(idx); });
  header->addChild(std::move(seg));

  header->addChild(ui::button({.out = &m_refreshButton, .glyph = "refresh", .glyphSize = Style::fontSizeBody * scale, .variant = ButtonVariant::Default, .minWidth = Style::controlHeightSm * scale, .minHeight = Style::controlHeightSm * scale, .padding = Style::spaceXs * scale, .radius = Style::scaledRadiusMd(scale), .onClick = [this](){ refreshProcesses(); }}));
  col->addChild(std::move(header));

  col->addChild(ui::input({.out = &m_filterInput, .placeholder = "Filter by name...", .fontSize = Style::fontSizeBody * scale, .controlHeight = Style::controlHeight * scale, .horizontalPadding = Style::spaceMd * scale, .clearButtonEnabled = true, .surfaceOpacity = panelCardOpacity(), .onChange = [this](const std::string& t){ onFilterChanged(t); }}));

  m_adapter = std::make_unique<ProcessListAdapter>(scale, [this](pid_t pid){ handleKill(pid); });
  m_adapter->setProcesses(&m_processes, &m_filteredIndices, m_pendingKillPid);
  col->addChild(ui::virtualGridView({.out = &m_listGrid, .columns = 1, .cellHeight = kRowHeight * scale, .squareCells = false, .columnGap = 0, .rowGap = Style::spaceXs * scale, .overscanRows = 3, .scrollbarVisible = true, .adapter = m_adapter.get(), .flexGrow = 1.0F}));

  col->addChild(ui::label({.out = &m_emptyLabel, .text = "No processes", .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .visible = false, .participatesInLayout = false}));
  col->addChild(ui::label({.out = &m_statusLabel, .text = "", .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}));

  setRoot(std::move(col));
  if (m_animations) root()->setAnimationManager(m_animations);
}

void ProcessPanel::onOpen(std::string_view) {
  m_filter.clear(); m_pendingFilter.clear();
  if (m_filterInput) m_filterInput->setValue("");
  m_pendingKillPid = -1;
  m_killConfirmTimer.stop();
  m_filterDebounceTimer.stop();
  refreshProcesses();
  // Auto-refresh every 2s
  m_refreshTimer.startRepeating(kRefreshInterval, [this](){ refreshProcesses(); PanelManager::instance().refresh(); });
}

void ProcessPanel::onClose() {
  m_refreshTimer.stop();
  m_filterDebounceTimer.stop();
  m_killConfirmTimer.stop();
  m_rootColumn = nullptr; m_headerRow = nullptr; m_titleLabel = nullptr; m_filterInput = nullptr; m_sortToggle = nullptr; m_refreshButton = nullptr; m_listGrid = nullptr; m_emptyLabel = nullptr; m_statusLabel = nullptr; m_focusArea = nullptr;
  m_adapter.reset();
  m_filteredIndices.clear();
  m_processes.clear();
  clearReleasedRoot();
}

InputArea* ProcessPanel::initialFocusArea() const { return m_filterInput ? m_filterInput->inputArea() : m_focusArea; }

bool ProcessPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t mods, bool pressed, bool preedit) {
  (void)mods; (void)preedit;
  if (!pressed) return false;
  if (sym == 65307) { // Escape
    if (m_pendingKillPid != -1) { m_pendingKillPid = -1; m_killConfirmTimer.stop(); if(m_adapter) m_adapter->setProcesses(&m_processes,&m_filteredIndices,m_pendingKillPid); if(m_listGrid) m_listGrid->notifyDataChanged(); PanelManager::instance().refresh(); return true; }
    PanelManager::instance().close(); return true;
  }
  return false;
}

void ProcessPanel::doLayout(Renderer& renderer, float w, float h) {
  if (!m_rootColumn || !m_listGrid) return;
  m_lastWidth = w; m_lastHeight = h;
  if (m_adapter) m_adapter->setRenderer(&renderer);
  m_rootColumn->setSize(w,h);
  m_rootColumn->layout(renderer);
}

void ProcessPanel::doUpdate(Renderer& renderer) { (void)renderer; updateStatus(); }

void ProcessPanel::onPanelCardOpacityChanged(float o) {
  if (m_filterInput) m_filterInput->setSurfaceOpacity(o);
}

void ProcessPanel::refreshProcesses() {
  if (!m_service) return;
  m_processes = m_service->fetchProcesses();
  applyFilterAndSort();
  if (m_adapter) m_adapter->setProcesses(&m_processes, &m_filteredIndices, m_pendingKillPid);
  if (m_listGrid) m_listGrid->notifyDataChanged();
  updateStatus();
}

void ProcessPanel::applyFilterAndSort() {
  m_filteredIndices.clear();
  std::string needle = m_filter;
  for (char& c: needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (std::size_t i=0;i<m_processes.size();++i) {
    if (!needle.empty()) {
      std::string hay = m_processes[i].name;
      for (char& c: hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (!hay.contains(needle)) continue;
    }
    m_filteredIndices.push_back(i);
  }
  if (m_sort == ProcessSort::Memory) {
    std::ranges::sort(m_filteredIndices, [this](std::size_t a, std::size_t b){ return m_processes[a].rssKb > m_processes[b].rssKb; });
  } else {
    std::ranges::sort(m_filteredIndices, [this](std::size_t a, std::size_t b){ return m_processes[a].cpuPercent > m_processes[b].cpuPercent; });
  }
}

void ProcessPanel::onFilterChanged(const std::string& text) {
  m_pendingFilter = text;
  m_filterDebounceTimer.start(kFilterDebounce, [this](){
    if (m_pendingFilter == m_filter) return;
    m_filter = m_pendingFilter;
    applyFilterAndSort();
    if (m_adapter) m_adapter->setProcesses(&m_processes,&m_filteredIndices,m_pendingKillPid);
    if (m_listGrid) { m_listGrid->notifyDataChanged(); m_listGrid->scrollView().setScrollOffset(0); }
    updateStatus();
    PanelManager::instance().refresh();
  });
}

void ProcessPanel::onSortChanged(std::size_t idx) {
  m_sort = (idx == 1 ? ProcessSort::Cpu : ProcessSort::Memory);
  applyFilterAndSort();
  if (m_adapter) m_adapter->setProcesses(&m_processes,&m_filteredIndices,m_pendingKillPid);
  if (m_listGrid) m_listGrid->notifyDataChanged();
  updateStatus();
  PanelManager::instance().refresh();
}

void ProcessPanel::handleKill(pid_t pid) {
  if (m_pendingKillPid != pid) {
    m_pendingKillPid = pid;
    m_killConfirmTimer.start(kKillConfirmTimeout, [this](){ m_pendingKillPid = -1; if(m_adapter) m_adapter->setProcesses(&m_processes,&m_filteredIndices,m_pendingKillPid); if(m_listGrid) m_listGrid->notifyDataChanged(); PanelManager::instance().refresh(); });
    if (m_adapter) m_adapter->setProcesses(&m_processes,&m_filteredIndices,m_pendingKillPid);
    if (m_listGrid) m_listGrid->notifyDataChanged();
    PanelManager::instance().refresh();
    return;
  }
  // confirmed
  m_killConfirmTimer.stop();
  m_pendingKillPid = -1;
  std::string err;
  bool ok = m_service ? m_service->killProcess(pid, err) : false;
  if (!ok) {
    kLog.warn("kill {} failed: {}", pid, err);
  }
  // refresh soon
  refreshProcesses();
  if (m_adapter) m_adapter->setProcesses(&m_processes,&m_filteredIndices,m_pendingKillPid);
  if (m_listGrid) m_listGrid->notifyDataChanged();
  PanelManager::instance().refresh();
}

void ProcessPanel::updateStatus() {
  if (!m_statusLabel || !m_emptyLabel || !m_listGrid) return;
  const bool empty = m_filteredIndices.empty();
  m_emptyLabel->setVisible(empty);
  m_emptyLabel->setParticipatesInLayout(empty);
  m_listGrid->setVisible(!empty);
  m_listGrid->setParticipatesInLayout(!empty);
  if (m_processes.empty()) {
    m_statusLabel->setText("");
  } else {
    std::string sortName = (m_sort == ProcessSort::Memory ? "Memory" : "CPU");
    m_statusLabel->setText(std::format("{} processes • Sorted by {} • Tap trash to kill (user only)", m_filteredIndices.size(), sortName));
  }
}
