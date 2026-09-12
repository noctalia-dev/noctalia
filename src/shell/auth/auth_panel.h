#pragma once

#include "shell/auth/auth_source.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

class Button;
class ConfigService;
class Flex;
class Glyph;
class Image;
class Input;
class InputArea;
class Label;
class Node;
class Renderer;

class AuthPanel : public Panel {
public:
  using ActiveSourceProvider = std::function<AuthSource*()>;

  AuthPanel(ConfigService* config, ActiveSourceProvider activeProvider);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  [[nodiscard]] AuthSource* activeOrNull();
  [[nodiscard]] const AuthSource* activeOrNull() const;
  [[nodiscard]] const AuthView& currentView() const;
  [[nodiscard]] const std::string freshSessionId() const;
  [[nodiscard]] bool refreshAuthView() const;
  [[nodiscard]] std::string authCancelLabel() const;
  [[nodiscard]] std::string authFallbackGlyph() const;
  [[nodiscard]] std::string authIconKey(AuthIconKind iconKind, const std::string& iconValue) const;
  void onAuthSubmit(std::string_view response);
  void onAuthCancel();
  void refreshAuthIcon(Renderer& renderer);
  void showGlyphIcon(Renderer& renderer, const std::string_view glyph);
  void showImageIcon(Renderer& renderer, const std::string& path, float iconSize);
  [[nodiscard]] float authIconSize() const noexcept { return scaled(48.0F); }

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit(std::string_view response = {});
  bool handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers);

  ConfigService* m_config = nullptr;
  ActiveSourceProvider m_activeProvider;
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
  Glyph* m_glyphIcon = nullptr;
  IconResolver m_iconResolver;
  std::string m_lastIconKey;
  bool m_iconResolved = false;
  bool m_lastResponseRequired = false;
  bool m_lastPasswordMode = true;
  std::string m_trackedSessionId;
  bool m_hasTrackedSession = false;

  mutable AuthView m_cachedView;
  mutable bool m_isCachedViewValid = false;
};
