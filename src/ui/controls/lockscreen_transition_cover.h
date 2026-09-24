#pragma once

#include "render/core/lockscreen_transition_types.h"
#include "render/scene/node.h"

class LockscreenTransitionNode;

class LockscreenTransitionCover final : public Node {
public:
  LockscreenTransitionCover();

  void setTexture(TextureId texture);
  void setTransition(LockscreenTransitionKind transition, float progress, const LockscreenTransitionParams& params);
  void setSize(float width, float height) override;
  void setFrameSize(float width, float height);

private:
  LockscreenTransitionNode* m_transitionNode = nullptr;
};
