#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <string>
#include <string_view>

class Label;

class LabelWidget : public Node {
public:
  LabelWidget();

  void setTitle(std::string_view title);
  void setDescription(std::string_view description);
  void setFontFamily(std::string family);
  void setColor(const ColorSpec& color);
  void setShadow(bool enabled);
  void setContentScale(float scale);

  const std::string& title() const noexcept;
  const std::string& description() const noexcept;
  const std::string& fontFamily() const noexcept;
  float contentScale() const noexcept { return m_contentScale; }
  bool shadow() const noexcept { return m_shadow; }

private:
  void doLayout(Renderer& renderer) override;
  void applyShadow();

  Label* m_title = nullptr;
  Label* m_description = nullptr;
  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  float m_contentScale = 1.0F;
  bool m_shadow = true;
};
