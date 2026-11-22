#pragma once

#include "render/scene/node.h"
#include "ui/controls/button.h"

#include <functional>
#include <optional>
#include <string>

class DesktopButton : public Node {
public:
  struct Options {
    std::string glyph;
    std::string label;
    ButtonVariant variant = ButtonVariant::Default;
    bool showBackground = true;
    std::optional<ColorSpec> labelColor;
    ColorSpec hoverBackground = colorSpecFromRole(ColorRole::Hover);
  };
  DesktopButton();
  void setOptions(const Options& options);
  void setOnClick(std::function<void()> callback);
  void setEditorPreview(bool enabled);
  void setTabStop(bool tabStop);
  void setContentScale(float scale);
  void setFontFamily(const std::string& family);
  void setFillSize(float width, float height);

private:
  void doLayout(Renderer& renderer) override;
  void syncButtonContent();
  void syncButtonStyle();
  Button* m_button = nullptr;
  std::string m_glyph;
  std::string m_label;
  ButtonVariant m_variant = ButtonVariant::Default;
  bool m_showBackground = true;
  std::optional<ColorSpec> m_labelColor;
  ColorSpec m_hoverBackground = colorSpecFromRole(ColorRole::Hover);
  float m_contentScale = 1.0F;
  std::string m_fontFamily;
  float m_fillWidth = 0.0F;
  float m_fillHeight = 0.0F;
};
