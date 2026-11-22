#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <cstdint>
#include <string>
#include <string_view>

class Label;

// The host formats time and schedules updates; this control lays out literal text.
class DigitalClockWidget : public Node {
public:
  DigitalClockWidget();

  bool setText(std::string_view text);
  void setFontFamily(std::string family);
  void setColor(const ColorSpec& color);
  void setShadow(bool enabled);
  void setCenterText(bool center);
  void setContentScale(float scale);

  const std::string& text() const noexcept;
  const std::string& fontFamily() const noexcept;
  bool shadow() const noexcept { return m_shadow; }
  bool centerText() const noexcept { return m_centerText; }
  float contentScale() const noexcept { return m_contentScale; }

private:
  void doLayout(Renderer& renderer) override;
  void updateStableWidth(Renderer& renderer, float fontSize);
  void applyShadow();

  Label* m_label = nullptr;
  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  float m_contentScale = 1.0F;
  bool m_shadow = true;
  bool m_centerText = true;
  bool m_metricsDirty = true;
  float m_metricsRenderScale = -1.0F;
  std::uint64_t m_metricsGeneration = 0;
  char m_widestDigit = '0';
  std::string m_sample;
  float m_stableWidth = 0.0F;
};
