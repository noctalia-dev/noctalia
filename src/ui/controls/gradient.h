#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <array>
#include <cstdint>

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

// How the gradient offset travels over time. Loop repeats the same
// from→to trip; PingPong alternates direction on every completed trip.
enum class GradientMotion : std::uint8_t { None, Loop, PingPong };

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

  // durationMs is one trip (half a ping-pong cycle). Equal endpoints, a
  // non-positive or non-finite duration, or non-finite endpoints all mean a
  // static gradient. Re-applying the identical configuration keeps the
  // running trip and its phase.
  void setMotion(GradientMotion motion, float durationMs, float from, float to);
  // Explicit visibility handoff for motion: Node::setVisible is non-virtual,
  // so the reconciler calls this after applying the common `visible` prop.
  void setMotionVisible(bool visible);
  void setAnimationManager(AnimationManager* manager) override;

  [[nodiscard]] float offset() const noexcept { return m_offset; }

private:
  void applyPalette();
  void startTrip();
  void stopTrip();

  RectNode* m_rect = nullptr;
  std::array<GradientColorStop, 4> m_stops{};
  float m_angleDeg = 0.0F;
  float m_offset = 0.0F;
  float m_radius = 0.0F;
  float m_softness = 1.0F;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<bool>::ScopedConnection m_motionEnabledConn;

  GradientMotion m_motion = GradientMotion::None;
  float m_motionDurationMs = 0.0F;
  float m_motionFrom = 0.0F;
  float m_motionTo = 0.0F;
  bool m_outbound = true;
  bool m_motionVisible = true;
  AnimationManager::Id m_motionId = 0;
};
