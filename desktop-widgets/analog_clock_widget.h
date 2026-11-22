#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <array>
#include <chrono>

class Box;

// The host supplies local time-of-day and schedules updates.
class AnalogClockWidget : public Node {
public:
  AnalogClockWidget();

  // Seconds since midnight in [0, 24h). Returns whether the visible hands changed.
  bool setTime(std::chrono::seconds time);
  void setColor(const ColorSpec& color);
  void setShowCircle(bool show);
  void setShowSeconds(bool show);
  void setContentScale(float scale);

  std::chrono::seconds time() const noexcept { return m_time; }
  bool showCircle() const noexcept { return m_showCircle; }
  bool showSeconds() const noexcept { return m_showSeconds; }
  float contentScale() const noexcept { return m_contentScale; }

private:
  void doLayout(Renderer& renderer) override;
  void updateHands();

  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  std::chrono::seconds m_time{0};
  float m_contentScale = 1.0F;
  bool m_showCircle = true;
  bool m_showSeconds = true;
  Box* m_face = nullptr;
  std::array<Node*, 60> m_ticks{};
  Node* m_hour = nullptr;
  Node* m_minute = nullptr;
  Node* m_second = nullptr;
  Box* m_hub = nullptr;
};
