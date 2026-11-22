#pragma once

#include "shell/desktop/desktop_widget.h"

#include <array>
#include <string>

class Box;
class Label;
class Glyph;
class Image;
class Button;

// Inert greeter chrome; input areas and actions belong to the editor, never authentication.
class GreeterPreviewWidget final : public DesktopWidget {
public:
  GreeterPreviewWidget(
      std::string component, float width, bool minimal, std::string userName, std::string displayName,
      std::string keyboardLayout
  );
  void create() override;
  void layout(Renderer& renderer) override;

protected:
  void doLayout(Renderer& renderer) override;
  bool stabilizeNaturalSize() const noexcept override { return false; }

private:
  std::string m_component;
  float m_width;
  bool m_minimal;
  std::string m_userName;
  std::string m_displayName;
  std::string m_keyboardLayout;
  Box* m_panel = nullptr;
  Box* m_avatar = nullptr;
  Box* m_identity = nullptr;
  Box* m_password = nullptr;
  Box* m_session = nullptr;
  Button* m_submit = nullptr;
  Image* m_logo = nullptr;
  Glyph* m_symbol = nullptr;
  Glyph* m_userChevron = nullptr;
  Glyph* m_sessionChevron = nullptr;
  std::array<Label*, 9> m_labels{};
};
