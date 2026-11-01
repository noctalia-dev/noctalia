#include "ui/controls/lockscreen_transition_cover.h"

#include "render/scene/lockscreen_transition_node.h"

#include <memory>

LockscreenTransitionCover::LockscreenTransitionCover() {
  setHitTestVisible(false);
  setParticipatesInLayout(false);
  auto transitionNode = std::make_unique<LockscreenTransitionNode>();
  transitionNode->setHitTestVisible(false);
  transitionNode->setParticipatesInLayout(false);
  m_transitionNode = static_cast<LockscreenTransitionNode*>(addChild(std::move(transitionNode)));
}

void LockscreenTransitionCover::setTexture(TextureId texture) { m_transitionNode->setTexture(texture); }

void LockscreenTransitionCover::setTransition(
    LockscreenTransitionKind transition, float progress, const LockscreenTransitionParams& params
) {
  m_transitionNode->setTransition(transition, progress, params);
}

void LockscreenTransitionCover::setSize(float width, float height) {
  Node::setSize(width, height);
  m_transitionNode->setFrameSize(width, height);
}

void LockscreenTransitionCover::setFrameSize(float width, float height) {
  Node::setFrameSize(width, height);
  m_transitionNode->setFrameSize(width, height);
}
