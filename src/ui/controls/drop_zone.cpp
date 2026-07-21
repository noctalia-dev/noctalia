#include "ui/controls/drop_zone.h"

#include "render/animation/animation_manager.h"
#include "ui/drag_drop_controller.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

DropZone::DropZone(DragDropController* controller) : m_controller(controller) {
  if (m_controller != nullptr) {
    m_controller->registerDropZone(this);
  }
}

DropZone::~DropZone() {
  if (m_controller != nullptr) {
    m_controller->unregisterDropZone(this);
  }
}

void DropZone::detachController(DragDropController* controller) noexcept {
  if (m_controller == controller) {
    m_controller = nullptr;
  }
}

void DropZone::setAccepts(std::vector<std::string> values) {
  if (m_accepts == values) {
    return;
  }
  if (m_controller != nullptr) {
    m_controller->zoneChanged(this);
  }
  m_accepts = std::move(values);
}

void DropZone::setValue(std::string value) {
  if (m_value == value) {
    return;
  }
  if (m_controller != nullptr) {
    m_controller->zoneChanged(this);
  }
  m_value = std::move(value);
}

void DropZone::setOnDrop(std::string value) {
  if (m_onDrop == value) {
    return;
  }
  if (m_controller != nullptr) {
    m_controller->zoneChanged(this);
  }
  m_onDrop = std::move(value);
}

void DropZone::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  if (!enabled && m_controller != nullptr) {
    m_controller->zoneChanged(this);
  }
  m_enabled = enabled;
  if (!enabled) {
    setDragOver(false);
  }
}

void DropZone::setDragOver(bool dragOver, float draggedHeight) {
  if (m_dragOver == dragOver) {
    return;
  }
  m_dragOver = dragOver;
  if (m_expandOnDrag && m_collapsedHeight > 0.0f) {
    const float target = dragOver ? std::max(m_collapsedHeight, draggedHeight) : m_collapsedHeight;
    animateHeight(target);
  }
  applyVisualState();
}

void DropZone::setExpandOnDrag(bool enabled) {
  if (m_expandOnDrag == enabled) {
    return;
  }
  m_expandOnDrag = enabled;
  if (!enabled && m_collapsedHeight > 0.0f) {
    animateHeight(m_collapsedHeight);
  }
}

void DropZone::setCollapsedHeight(float height) {
  const float clamped = std::max(0.0f, height);
  if (m_collapsedHeight == clamped) {
    return;
  }
  m_collapsedHeight = clamped;
  if (!m_dragOver || !m_expandOnDrag) {
    applyAnimatedHeight(clamped);
  }
}

void DropZone::setHitSlop(float hitSlop) { m_hitSlop = std::clamp(hitSlop, 0.0f, 128.0f); }

void DropZone::setZoneRadius(float radius) {
  m_zoneRadius = std::max(0.0f, radius);
  m_idleRadius = m_zoneRadius;
  m_radiusApplied = true;
  Flex::setRadius(m_dragOver ? m_zoneRadius : m_idleRadius);
}

void DropZone::clearZoneRadius(float dragRadius) {
  m_zoneRadius = std::max(0.0f, dragRadius);
  m_idleRadius = 0.0f;
  if (m_radiusApplied || m_dragOver) {
    m_radiusApplied = true;
    Flex::setRadius(m_dragOver ? m_zoneRadius : m_idleRadius);
  }
}

void DropZone::animateHeight(float target) {
  target = std::max(0.0f, target);
  AnimationManager* animations = animationManager();
  if (animations == nullptr || m_animatedHeight <= 0.0f) {
    applyAnimatedHeight(target);
    return;
  }
  if (m_heightAnimation != 0) {
    animations->cancel(m_heightAnimation);
  }
  m_heightAnimation = animations->animate(
      m_animatedHeight, target, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [this](float value) { applyAnimatedHeight(value); }, [this] { m_heightAnimation = 0; }, this
  );
}

void DropZone::applyAnimatedHeight(float height) {
  m_animatedHeight = height;
  Flex::setMinHeight(height);
  Flex::setMaxHeight(height);
}

void DropZone::setZoneFill(const ColorSpec& color) {
  m_zoneFill = color;
  m_hasZoneFill = true;
  applyVisualState();
}

void DropZone::clearZoneFill() {
  m_zoneFill = clearColorSpec();
  m_hasZoneFill = false;
  applyVisualState();
}

void DropZone::setZoneBorder(const ColorSpec& color, float width) {
  m_zoneBorder = color;
  m_zoneBorderWidth = width;
  m_hasZoneBorder = true;
  applyVisualState();
}

void DropZone::clearZoneBorder() {
  m_zoneBorder = clearColorSpec();
  m_zoneBorderWidth = 0.0f;
  m_hasZoneBorder = false;
  applyVisualState();
}

bool DropZone::accepts(std::string_view dragType) const {
  return std::ranges::find(m_accepts, dragType) != m_accepts.end();
}

void DropZone::applyVisualState() {
  if (m_dragOver) {
    m_radiusApplied = true;
    Flex::setRadius(m_zoneRadius);
    Flex::setFill(colorSpecFromRole(ColorRole::Primary, 0.12f));
    Flex::setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
    return;
  }
  if (m_radiusApplied) {
    Flex::setRadius(m_idleRadius);
  }
  if (m_hasZoneFill) {
    Flex::setFill(m_zoneFill);
  } else {
    Flex::clearFill();
  }
  if (m_hasZoneBorder) {
    Flex::setBorder(m_zoneBorder, m_zoneBorderWidth);
  } else {
    Flex::clearBorder();
  }
}
