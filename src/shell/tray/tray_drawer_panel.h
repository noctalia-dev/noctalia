#pragma once

#include "shell/panel/panel.h"

#include <memory>
#include <string>
#include <vector>

class ConfigService;
class TrayService;
class TrayWidget;

class TrayDrawerPanel : public Panel {
public:
  TrayDrawerPanel(TrayService* tray, ConfigService* config, std::size_t drawerColumns = 3);
  ~TrayDrawerPanel() override;

  void create() override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  [[nodiscard]] std::size_t currentDrawerColumns() const;
  [[nodiscard]] std::vector<std::string> currentHiddenItems() const;
  [[nodiscard]] std::vector<std::string> currentPinnedItems() const;
  [[nodiscard]] std::size_t visibleItemCount() const;

  TrayService* m_tray = nullptr;
  ConfigService* m_config = nullptr;
  std::size_t m_drawerColumns = 3;
  std::unique_ptr<TrayWidget> m_drawerWidget;
};
