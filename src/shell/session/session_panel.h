#pragma once

#include "config/config_types.h"
#include "shell/panel/panel.h"
#include "ui/style.h"

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Button;
class Flex;
class GridView;
class InputArea;
class Renderer;
class ConfigService;

namespace compositors::niri {
  class NiriRuntime;
}

struct SessionActionHooks {
  std::function<bool()> onLogout;
  std::function<bool()> onReboot;
  std::function<bool()> onShutdown;
};

class SessionPanel : public Panel {
public:
  explicit SessionPanel(
      ConfigService* config, SessionActionHooks actionHooks = {}, compositors::niri::NiriRuntime* niriRuntime = nullptr
  )
      : m_config(config), m_actionHooks(std::move(actionHooks)), m_niriRuntime(niriRuntime) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool hasDecoration() const override { return true; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  static constexpr float kActionButtonMinHeight = 112.0f;
  static constexpr float kButtonMinWidth = 152.0f;
  static constexpr float kPanelMinWidth = 680.0f;
  static constexpr std::size_t kMaxColumns = 5;

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void activateSelected();
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void updateSelectionVisuals();
  void activateMouse();
  void invokeEntry(const SessionPanelActionConfig& cfg);
  [[nodiscard]] std::vector<SessionPanelActionConfig> effectiveActions() const;
  [[nodiscard]] std::function<bool()> hookFor(const std::string& action) const;
  [[nodiscard]] Button* createActionButton(const SessionPanelActionConfig& cfg, float scale);
  [[nodiscard]] std::size_t entryCountForLayout() const;
  [[nodiscard]] std::size_t visibleColumnCount() const;
  [[nodiscard]] std::size_t visibleRowCount() const;

  GridView* m_rootLayout = nullptr;
  InputArea* m_focusArea = nullptr;
  std::vector<SessionPanelActionConfig> m_visibleEntries;
  std::vector<Button*> m_visibleButtons;
  std::optional<std::size_t> m_selectedIndex;
  bool m_mouseActive = false;
  ConfigService* m_config = nullptr;
  SessionActionHooks m_actionHooks;
  compositors::niri::NiriRuntime* m_niriRuntime = nullptr;
};
