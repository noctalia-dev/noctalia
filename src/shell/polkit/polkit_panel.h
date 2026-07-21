#pragma once

#include "shell/panel/panel.h"
#include "system/icon_resolver.h"

#include <functional>

class Button;
class Flex;
class Glyph;
class Image;
class Input;
class InputArea;
class Label;
class Node;
class PolkitAgent;
class PolkitRequest;
class Renderer;
class ConfigService;

class PolkitPanel : public Panel {
public:
  PolkitPanel(ConfigService* config, std::function<PolkitAgent*()> agentProvider);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(480.0f); }
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }
  [[nodiscard]] InputArea* initialFocusArea() const override;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit();
  bool handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void resolveIcon(Renderer& renderer, const PolkitRequest& request);

  ConfigService* m_config = nullptr;
  std::function<PolkitAgent*()> m_agentProvider;
  Flex* m_rootLayout = nullptr;
  InputArea* m_focusArea = nullptr;
  Label* m_titleLabel = nullptr;
  Label* m_messageLabel = nullptr;
  Label* m_promptLabel = nullptr;
  Label* m_supplementaryLabel = nullptr;
  Input* m_input = nullptr;
  Button* m_submitButton = nullptr;
  Button* m_cancelButton = nullptr;
  Node* m_iconContainer = nullptr;
  Image* m_icon = nullptr;
  Glyph* m_fallbackIcon = nullptr;
  IconResolver m_iconResolver;
  std::string m_lastIconName;
  bool m_iconResolved = false;
  bool m_lastResponseRequired = false;
};
