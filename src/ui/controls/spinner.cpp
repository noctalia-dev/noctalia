#include "ui/controls/spinner.h"

#include "render/animation/animation_manager.h"
#include "render/scene/spinner_node.h"
#include "ui/palette.h"

#include <memory>
#include <numbers>

namespace {

  constexpr float kDefaultSize = 20.0f;
  constexpr float kDefaultThickness = 2.0f;
  constexpr float kRevolutionMs = 1200.0f;
  constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

} // namespace

Spinner::Spinner() {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
  auto node = std::make_unique<SpinnerNode>();
  node->setThickness(kDefaultThickness);
  m_spinnerNode = static_cast<SpinnerNode*>(addChild(std::move(node)));
  m_spinnerSize = kDefaultSize;
  applyPalette();
  updateGeometry();
}

void Spinner::setColor(const ColorSpec& color) {
  m_color = color;
  applyPalette();
}

void Spinner::setColor(const Color& color) { setColor(fixedColorSpec(color)); }

void Spinner::setSpinnerSize(float size) {
  m_spinnerSize = size;
  updateGeometry();
}

void Spinner::setThickness(float thickness) { m_spinnerNode->setThickness(thickness); }

void Spinner::start() {
  if (m_spinning) {
    return;
  }
  m_spinning = true;
  startLoop();
}

void Spinner::setAnimationManager(AnimationManager* mgr) {
  Node::setAnimationManager(mgr);
  if (mgr == nullptr) {
    m_animId = 0; // the old manager no longer drives us; allow a restart on re-attach
    return;
  }
  // start() called before attach finds no manager and bails; pick the loop back up
  // now that one is available so a spinner built with spinning=true animates.
  if (m_spinning && m_animId == 0) {
    startLoop();
  }
}

void Spinner::stop() {
  m_spinning = false;
  if (animationManager() != nullptr && m_animId != 0) {
    animationManager()->cancel(m_animId);
    m_animId = 0;
  }
}

void Spinner::applyPalette() {
  if (m_spinnerNode != nullptr) {
    m_spinnerNode->setColor(resolveColorSpec(m_color));
  }
}

void Spinner::startLoop() {
  if (animationManager() == nullptr || !m_spinning) {
    return;
  }

  m_animId = animationManager()->animate(
      0.0f, kTwoPi, kRevolutionMs, Easing::Linear,
      [this](float angle) {
        m_spinnerNode->setRotation(angle);
        markPaintDirty();
      },
      [this]() {
        m_animId = 0;
        if (m_spinning) {
          startLoop();
        }
      },
      this
  );
  markPaintDirty();
}

void Spinner::updateGeometry() {
  setSize(m_spinnerSize, m_spinnerSize);
  m_spinnerNode->setFrameSize(m_spinnerSize, m_spinnerSize);
  m_spinnerNode->setPosition(0.0f, 0.0f);
}
