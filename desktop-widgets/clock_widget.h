#pragma once

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

class Box;
class Label;

// The host supplies formatted text/time-of-day and schedules updates.
class DesktopClock : public Node {
public:
  enum class Style : std::uint8_t { Digital, Analog };

  DesktopClock();

  void setStyle(Style style);
  bool setTime(std::chrono::seconds time);
  void setShowCircle(bool show);
  void setShowSeconds(bool show);

  bool setText(std::string_view text);
  void setFontFamily(std::string family);
  void setFontSize(float size);
  void setFontWeight(FontWeight weight);
  void setTightText(bool enabled);
  void setColor(const ColorSpec& color);
  void setShadow(bool enabled);
  void setCenterText(bool center);
  void setContentScale(float scale);

  Style style() const noexcept { return m_style; }
  std::chrono::seconds time() const noexcept { return m_time; }
  bool showCircle() const noexcept { return m_showCircle; }
  bool showSeconds() const noexcept { return m_showSeconds; }
  const std::string& text() const noexcept;
  const std::string& fontFamily() const noexcept;
  bool shadow() const noexcept { return m_shadow; }
  bool centerText() const noexcept { return m_centerText; }
  float contentScale() const noexcept { return m_contentScale; }

private:
  void doLayout(Renderer& renderer) override;
  void layoutDigital(Renderer& renderer);
  void layoutAnalog();
  void updateHands();
  void updateStableWidth(Renderer& renderer, float fontSize);
  void applyShadow();

  Style m_style = Style::Digital;
  std::chrono::seconds m_time{0};
  bool m_showCircle = true;
  bool m_showSeconds = true;
  Label* m_label = nullptr;
  Node* m_analogRoot = nullptr;
  Box* m_face = nullptr;
  std::array<Node*, 60> m_ticks{};
  Node* m_hour = nullptr;
  Node* m_minute = nullptr;
  Node* m_second = nullptr;
  Box* m_hub = nullptr;
  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  float m_contentScale = 1.0F;
  float m_fontSize = 0.0F;
  bool m_shadow = true;
  bool m_centerText = true;
  bool m_metricsDirty = true;
  float m_metricsRenderScale = -1.0F;
  std::uint64_t m_metricsGeneration = 0;
  char m_widestDigit = '0';
  std::string m_sample;
  float m_stableWidth = 0.0F;
};
