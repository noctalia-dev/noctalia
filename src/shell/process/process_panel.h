#pragma once

#include "core/timer_manager.h"
#include "shell/panel/panel.h"
#include "system/process_service.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class Button;
class Flex;
class Input;
class InputArea;
class Label;
class Segmented;
class VirtualGridView;
class ProcessListAdapter;

class ProcessPanel : public Panel {
public:
  explicit ProcessPanel(ProcessService* service);
  ~ProcessPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(720.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(560.0F); }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void refreshProcesses();
  void applyFilterAndSort();
  void onFilterChanged(const std::string& text);
  void onSortChanged(std::size_t index);
  void handleKill(pid_t pid);
  void updateStatus();

  ProcessService* m_service = nullptr;

  Flex* m_rootColumn = nullptr;
  Flex* m_headerRow = nullptr;
  Label* m_titleLabel = nullptr;
  Input* m_filterInput = nullptr;
  Segmented* m_sortToggle = nullptr;
  Button* m_refreshButton = nullptr;
  VirtualGridView* m_listGrid = nullptr;
  Label* m_emptyLabel = nullptr;
  Label* m_statusLabel = nullptr;
  InputArea* m_focusArea = nullptr;

  std::unique_ptr<ProcessListAdapter> m_adapter;
  std::vector<ProcessInfo> m_processes;
  std::vector<std::size_t> m_filteredIndices;
  ProcessSort m_sort = ProcessSort::Memory;
  std::string m_filter;
  std::string m_pendingFilter;
  pid_t m_pendingKillPid = -1;

  Timer m_refreshTimer;
  Timer m_filterDebounceTimer;
  Timer m_killConfirmTimer;

  float m_lastWidth = 0.0F;
  float m_lastHeight = 0.0F;
};
