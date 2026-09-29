#pragma once

#include "launcher/launcher_provider.h"

class PanelManager;
class ControlCenterPanel;
class ConfigService;

// Lists every panel PanelManager knows about (built-in or plugin-registered),
// plus Control Center's currently-visible tabs as their own rows, and toggles
// whichever one is selected. Names/glyphs come from panel_catalog; ids in
// shell.launcher.panels.ignored are left out.
class PanelProvider : public LauncherProvider {
public:
  PanelProvider(PanelManager* panelManager, ControlCenterPanel* controlCenterPanel, ConfigService* config);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "pan"; }
  [[nodiscard]] std::string_view id() const override { return "Panels"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "rectangle"; }
  [[nodiscard]] bool trackUsage() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  PanelManager* m_panelManager = nullptr;
  ControlCenterPanel* m_controlCenterPanel = nullptr;
  ConfigService* m_config = nullptr;
};
