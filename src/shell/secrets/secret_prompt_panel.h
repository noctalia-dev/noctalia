#pragma once

#include "shell/panel/panel.h"

#include <cstdint>
#include <functional>
#include <string_view>

class Button;
class Checkbox;
class Flex;
class Glyph;
class Input;
class InputArea;
class Label;
class Renderer;
class SecretPrompter;
class ConfigService;

// Renders the prompt requested over org.gnome.keyring.SystemPrompter: the keyring
// unlock dialog for niri sessions. Confirm prompts hide the password fields; a
// "new password" prompt shows a second field and refuses to submit a mismatch.
class SecretPromptPanel : public Panel {
public:
  SecretPromptPanel(ConfigService* config, std::function<SecretPrompter*()> prompterProvider);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  float preferredWidth() const override { return scaled(480.0F); }
  // Fits its content (password fields come and go with the prompt type); this is the maximum.
  float preferredHeight() const override { return scaled(640.0F); }
  bool fitsContentHeight() const noexcept override { return true; }
  float measureContentHeight(Renderer& renderer, float contentWidth) override;
  PanelPlacement panelPlacement() const noexcept override;
  LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  bool dismissOnOutsideClick() const override { return false; }
  InputArea* initialFocusArea() const override;
  bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit();
  void cancelPrompt();
  bool handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  SecretPrompter* prompter() const;

  ConfigService* m_config = nullptr;
  std::function<SecretPrompter*()> m_prompterProvider;
  Flex* m_rootLayout = nullptr;
  InputArea* m_focusArea = nullptr;
  Glyph* m_icon = nullptr;
  Label* m_titleLabel = nullptr;
  Label* m_messageLabel = nullptr;
  Label* m_descriptionLabel = nullptr;
  Label* m_warningLabel = nullptr;
  Input* m_input = nullptr;
  Input* m_confirmInput = nullptr;
  Flex* m_choiceRow = nullptr;
  Checkbox* m_choiceCheckbox = nullptr;
  Label* m_choiceLabel = nullptr;
  Button* m_continueButton = nullptr;
  Button* m_cancelButton = nullptr;
  // Set when the two "new password" fields disagree; cleared on the next edit.
  bool m_mismatch = false;
  bool m_lastNeedsPassword = false;
};
