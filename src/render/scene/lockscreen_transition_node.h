#pragma once

#include "render/core/lockscreen_transition_types.h"
#include "render/scene/node.h"

#include <algorithm>

class LockscreenTransitionNode final : public Node {
public:
  LockscreenTransitionNode() : Node(NodeType::LockscreenTransition) { setHitTestVisible(false); }

  [[nodiscard]] TextureId texture() const noexcept { return m_texture; }
  [[nodiscard]] LockscreenTransitionKind transition() const noexcept { return m_transition; }
  [[nodiscard]] float progress() const noexcept { return m_progress; }
  [[nodiscard]] const LockscreenTransitionParams& transitionParams() const noexcept { return m_params; }

  void setTexture(TextureId texture) {
    if (m_texture == texture) {
      return;
    }
    m_texture = texture;
    markPaintDirty();
  }

  void setTransition(LockscreenTransitionKind transition, float progress, const LockscreenTransitionParams& params) {
    const float clampedProgress = std::clamp(progress, 0.0F, 1.0F);
    if (m_transition == transition
        && m_progress == clampedProgress
        && m_params.direction == params.direction
        && m_params.centerX == params.centerX
        && m_params.centerY == params.centerY
        && m_params.stripeCount == params.stripeCount
        && m_params.angle == params.angle
        && m_params.cellSize == params.cellSize
        && m_params.smoothness == params.smoothness
        && m_params.aspectRatio == params.aspectRatio) {
      return;
    }
    m_transition = transition;
    m_progress = clampedProgress;
    m_params = params;
    markPaintDirty();
  }

private:
  TextureId m_texture;
  LockscreenTransitionKind m_transition = LockscreenTransitionKind::Fade;
  float m_progress = 0.0F;
  LockscreenTransitionParams m_params;
};
