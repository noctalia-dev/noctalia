#include "shell/bar/widget.h"

#include "render/animation/animation_manager.h"
#include "render/scene/node.h"
#include "ui/controls/box.h"
#include "ui/palette.h"

#include <algorithm>

namespace {

  constexpr float kCapsuleInkEpsilon = 0.5f;

} // namespace

Widget::~Widget() {
  if (m_animations != nullptr) {
    m_animations->cancelForOwner(this);
  }
}

ColorSpec Widget::widgetForegroundOr(const ColorSpec& fallback) const noexcept {
  // Per-widget `color` must win over bar/widget `capsule_foreground`, otherwise a bar-level
  // capsule_foreground (e.g. on_primary) overrides explicit `color = primary` after layout.
  if (m_widgetForeground.has_value()) {
    return *m_widgetForeground;
  }
  const auto& spec = m_barCapsuleSpec;
  if (spec.enabled && spec.foreground.has_value()) {
    return *spec.foreground;
  }
  return fallback;
}

bool Widget::shouldShowBarCapsule() const {
  if (!m_barCapsuleSpec.enabled) {
    return false;
  }
  const Node* r = root();
  if (r == nullptr || !r->visible()) {
    return false;
  }
  if (r->width() <= kCapsuleInkEpsilon || r->height() <= kCapsuleInkEpsilon) {
    return false;
  }
  // No scene children ⇒ nothing to frame (spacer bare node, empty tray flex, etc.).
  if (r->children().empty()) {
    return false;
  }
  return true;
}

float Widget::resolvedBarCapsuleRadius(float width, float height) const noexcept {
  const float maxRadius = std::max(0.0f, std::min(width, height) * 0.5f);
  if (!m_barCapsuleSpec.radius.has_value()) {
    return maxRadius;
  }
  return std::clamp(*m_barCapsuleSpec.radius * m_contentScale, 0.0f, maxRadius);
}

void Widget::setBarCapsuleScene(Node* shell, Box* box) noexcept {
  m_capsuleShell = shell;
  m_capsuleBox = box;
}

float Widget::width() const noexcept { return root() ? root()->width() : 0.0f; }

float Widget::height() const noexcept { return root() ? root()->height() : 0.0f; }

std::unique_ptr<Node> Widget::releaseRoot() {
  m_rootPtr = m_root.get();
  return std::move(m_root);
}

void Widget::setAnimationManager(AnimationManager* mgr) noexcept { m_animations = mgr; }

void Widget::setUpdateCallback(UpdateCallback callback) { m_updateCallback = std::move(callback); }

void Widget::setRedrawCallback(RedrawCallback callback) { m_redrawCallback = std::move(callback); }

void Widget::setFrameTickRequestCallback(FrameTickRequestCallback callback) {
  m_frameTickRequestCallback = std::move(callback);
}

void Widget::setPanelToggleCallback(PanelToggleCallback callback) { m_panelToggleCallback = std::move(callback); }

void Widget::requestUpdate() {
  if (m_updateCallback) {
    m_updateCallback();
  }
}

void Widget::requestRedraw() {
  if (m_redrawCallback) {
    m_redrawCallback();
  }
}

void Widget::requestFrameTick() {
  if (m_frameTickRequestCallback) {
    m_frameTickRequestCallback();
  }
}

void Widget::requestPanelToggle(
    std::string_view panelId, std::string_view context, std::optional<float> anchorSurfaceX,
    std::optional<float> anchorSurfaceY
) {
  if (m_panelToggleCallback) {
    m_panelToggleCallback(panelId, context, anchorSurfaceX, anchorSurfaceY);
  }
}
