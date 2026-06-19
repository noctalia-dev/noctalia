#pragma once

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "shell/panel/panel.h"
#include "ui/palette.h"

#include <cstdint>
#include <string>
#include <vector>

class ConfigService;
class Glyph;
class Node;

class WorkspaceTrayPanel : public Panel {
public:
  WorkspaceTrayPanel(CompositorPlatform* platform, ConfigService* config);
  ~WorkspaceTrayPanel() override = default;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] bool isContextActive(std::string_view /*context*/) const override { return true; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild(Renderer& renderer);

  void loadConfig() const;
  void parseOutput(std::string_view context) const;
  void ensureDataLoaded() const;
  [[nodiscard]] std::size_t visibleCount() const;

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;

  mutable wl_output* m_output = nullptr;
  mutable std::vector<Workspace> m_workspaces;
  mutable bool m_needsRebuild = true;

  mutable std::size_t m_maxLabelChars = 3;
  mutable bool m_displayModeName = false;
  mutable ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  mutable ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  mutable bool m_showNewWorkspace = false;
  mutable std::string m_newWorkspaceCommand;
  mutable std::string m_newWorkspaceGlyph = "plus";
  mutable bool m_hideWhenEmpty = false;

  Node* m_container = nullptr;
  float m_actualWidth = 0.0f;
};
