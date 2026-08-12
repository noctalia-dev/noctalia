#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <array>

struct RoundedRectStyle;
class RectNode;

// A single stop of a plugin-declared gradient: normalized position plus a
// palette token or fixed colour. ColourSpecs stay unresolved until paint, so
// a palette swap re-resolves role stops in place.
struct GradientColorStop {
  float position = 0.0F;
  ColorSpec color = clearColorSpec();
};

constexpr bool operator==(const GradientColorStop& lhs, const GradientColorStop& rhs) noexcept {
  return lhs.position == rhs.position && lhs.color == rhs.color;
}

// Retained paint-only gradient bar. Wraps one RectNode in the linear fill
// mode; angle is degrees (0 = left→right, 90 = top→bottom) and offset shifts
// the stop range along the gradient axis ([0..1] keeps stops fully in view).
class Gradient : public Node {
public:
  Gradient();

  [[nodiscard]] const RoundedRectStyle& style() const noexcept;
  void setGradient(float angleDeg, const std::array<GradientColorStop, 4>& stops);
  void clearGradient();
  void setOffset(float offset);
  void setRadius(float radius);
  void setSoftness(float softness);
  void setSize(float width, float height) override;

private:
  void applyPalette();

  RectNode* m_rect = nullptr;
  std::array<GradientColorStop, 4> m_stops{};
  float m_angleDeg = 0.0F;
  float m_offset = 0.0F;
  float m_radius = 0.0F;
  float m_softness = 1.0F;
  Signal<>::ScopedConnection m_paletteConn;
};
