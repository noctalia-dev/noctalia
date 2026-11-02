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
  TrayDrawerPanel(TrayService* tray, ConfigService* config);
  ~TrayDrawerPanel() override;

  void create() override;
  void onClose() override;

  float preferredWidth() const override;
  float preferredHeight() const override;
  PanelPlacement panelPlacement() const noexcept override;
  LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }
  void setAnimationManager(AnimationManager* mgr) noexcept override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  std::size_t currentDrawerColumns() const;
  std::optional<float> currentDrawerItemSize() const;
  float resolvedItemGap() const;
  std::size_t visibleItemCount() const;

  TrayService* m_tray = nullptr;
  ConfigService* m_config = nullptr;
  std::unique_ptr<TrayWidget> m_drawerWidget;
};
