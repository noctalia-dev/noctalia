#include "render/scene/input_dispatcher.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"

namespace {

  bool nodeAttachedToRoot(const Node* node, const Node* root) {
    if (node == nullptr || root == nullptr) {
      return false;
    }
    for (auto* current = node; current != nullptr; current = current->parent()) {
      if (current == root) {
        return true;
      }
    }
    return false;
  }

  InputArea* inputAreaAcceptingButton(InputArea* area, std::uint32_t button) {
    for (Node* node = area; node != nullptr; node = node->parent()) {
      auto* candidate = dynamic_cast<InputArea*>(node);
      if (candidate != nullptr && candidate->enabled() && candidate->acceptsButton(button)) {
        return candidate;
      }
    }
    return nullptr;
  }

} // namespace

void InputDispatcher::setSceneRoot(Node* root) {
  if (root != m_sceneRoot) {
    m_capturedArea = nullptr;
    if (m_hoveredArea != nullptr) {
      auto* old = m_hoveredArea;
      old->dispatchLeave();
      m_hoveredArea = nullptr;
      if (m_hoverChangeCallback) {
        m_hoverChangeCallback(old, nullptr);
      }
    }
  }
  if (root == nullptr) {
    if (m_focusedArea != nullptr) {
      m_focusedArea->dispatchFocusLoss();
      m_focusedArea = nullptr;
    }
  }
  m_sceneRoot = root;
  if (m_sceneRoot != nullptr && m_hasPointerPosition) {
    updateHover(m_lastPointerX, m_lastPointerY, m_lastSerial);
  }
}

void InputDispatcher::setHoverChangeCallback(HoverChangeCallback callback) {
  m_hoverChangeCallback = std::move(callback);
}

void InputDispatcher::setCursorShapeCallback(CursorShapeCallback callback) {
  m_cursorShapeCallback = std::move(callback);
}

void InputDispatcher::pointerEnter(float x, float y, std::uint32_t serial) {
  m_lastSerial = serial;
  m_lastPointerX = x;
  m_lastPointerY = y;
  m_hasPointerPosition = true;
  updateHover(x, y, serial);
}

void InputDispatcher::pointerLeave() {
  m_capturedArea = nullptr;
  m_hasPointerPosition = false;
  if (m_hoveredArea != nullptr) {
    auto* old = m_hoveredArea;
    old->dispatchLeave();
    m_hoveredArea = nullptr;
    if (m_hoverChangeCallback) {
      m_hoverChangeCallback(old, nullptr);
    }
  }
}

void InputDispatcher::pointerMotion(float x, float y, std::uint32_t serial) {
  if (serial != 0) {
    m_lastSerial = serial;
  }
  m_lastPointerX = x;
  m_lastPointerY = y;
  m_hasPointerPosition = true;
  updateHover(x, y, m_lastSerial);
}

bool InputDispatcher::pointerButton(float x, float y, std::uint32_t button, bool pressed) {
  m_lastPointerX = x;
  m_lastPointerY = y;
  m_hasPointerPosition = true;

  pruneDetachedAreas();

  InputArea* target = m_capturedArea != nullptr ? m_capturedArea : inputAreaAcceptingButton(m_hoveredArea, button);

  // Press with no hover target: subtree may have been rebuilt (same global coords, new InputArea*).
  if (target == nullptr && m_capturedArea == nullptr && pressed && m_hasPointerPosition) {
    updateHover(x, y, m_lastSerial);
    target = inputAreaAcceptingButton(m_hoveredArea, button);
  }

  if (target != nullptr) {
    if (pressed && m_capturedArea == nullptr) {
      if (target != m_focusedArea && m_focusedArea != nullptr) {
        setFocus(nullptr);
        pruneDetachedAreas();
        updateHover(x, y, m_lastSerial);
        target = inputAreaAcceptingButton(m_hoveredArea, button);
        if (target == nullptr) {
          return false;
        }
      }
      if (target->focusable()) {
        setFocus(target);
      }
    }

    float localX = 0.0f;
    float localY = 0.0f;
    (void)Node::mapFromScene(target, x, y, localX, localY);
    target->dispatchPress(localX, localY, button, pressed);
    if (pressed) {
      m_capturedArea = target;
      trackArea(target);
    } else {
      m_capturedArea = nullptr;
      updateHover(x, y, m_lastSerial);
    }
    return true;
  }

  // Release lost its capture (e.g. onClick rebuilt the scene); restore hover without mis-delivering the release.
  if (!pressed && m_capturedArea == nullptr && m_hasPointerPosition) {
    updateHover(x, y, m_lastSerial);
  }

  if (pressed) {
    setFocus(nullptr);
  }
  return false;
}

void InputDispatcher::syncPointerHover() {
  if (!m_hasPointerPosition || m_capturedArea != nullptr) {
    return;
  }
  updateHover(m_lastPointerX, m_lastPointerY, m_lastSerial);
}

bool InputDispatcher::pointerAxis(
    float x, float y, std::uint32_t axis, std::uint32_t axisSource, double value, std::int32_t discrete,
    std::int32_t value120, float lines
) {
  pruneDetachedAreas();
  InputArea* target = m_capturedArea != nullptr ? m_capturedArea : findInputAreaAt(x, y);
  if (target == nullptr) {
    return false;
  }

  bool consumedAny = false;
  for (Node* node = target; node != nullptr; node = node->parent()) {
    auto* area = dynamic_cast<InputArea*>(node);
    if (area == nullptr || !area->enabled()) {
      continue;
    }

    float localX = 0.0f;
    float localY = 0.0f;
    (void)Node::mapFromScene(area, x, y, localX, localY);
    const bool consumed = area->dispatchAxis(localX, localY, axis, axisSource, value, discrete, value120, lines);

    if (!consumed) {
      continue;
    }

    consumedAny = true;
    if (!area->propagateEvents()) {
      break;
    }
  }
  return consumedAny;
}

void InputDispatcher::keyEvent(
    std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit
) {
  pruneDetachedAreas();
  if (m_focusedArea != nullptr) {
    m_focusedArea->dispatchKey(sym, utf32, modifiers, pressed, preedit);
  }
}

void InputDispatcher::setFocus(InputArea* area) {
  if (area == m_focusedArea) {
    return;
  }
  if (m_focusedArea != nullptr) {
    m_focusedArea->dispatchFocusLoss();
  }
  m_focusedArea = area;
  if (m_focusedArea != nullptr) {
    trackArea(m_focusedArea);
    m_focusedArea->dispatchFocusGain();
  }
}

InputArea* InputDispatcher::findInputAreaAt(float x, float y) {
  if (m_sceneRoot == nullptr) {
    return nullptr;
  }

  auto* hitNode = Node::hitTest(m_sceneRoot, x, y);
  if (hitNode == nullptr) {
    return nullptr;
  }

  for (auto* node = hitNode; node != nullptr; node = node->parent()) {
    auto* area = dynamic_cast<InputArea*>(node);
    if (area != nullptr && area->enabled()) {
      return area;
    }
  }

  return nullptr;
}

void InputDispatcher::updateHover(float x, float y, std::uint32_t serial) {
  pruneDetachedAreas();

  // While a button is held, all motion goes to the captured area; hover stays frozen.
  if (m_capturedArea != nullptr) {
    float localX = 0.0f;
    float localY = 0.0f;
    (void)Node::mapFromScene(m_capturedArea, x, y, localX, localY);
    m_capturedArea->dispatchMotion(localX, localY);
    updateCursor(serial);
    return;
  }

  auto* area = findInputAreaAt(x, y);

  if (area != m_hoveredArea) {
    auto* old = m_hoveredArea;
    if (old != nullptr) {
      old->dispatchLeave();
    }
    m_hoveredArea = area;
    if (m_hoverChangeCallback) {
      m_hoverChangeCallback(old, m_hoveredArea);
    }
    if (m_hoveredArea != nullptr) {
      trackArea(m_hoveredArea);
      float localX = 0.0f;
      float localY = 0.0f;
      (void)Node::mapFromScene(m_hoveredArea, x, y, localX, localY);
      m_hoveredArea->dispatchEnter(localX, localY);
    }
  } else if (m_hoveredArea != nullptr) {
    float localX = 0.0f;
    float localY = 0.0f;
    (void)Node::mapFromScene(m_hoveredArea, x, y, localX, localY);
    m_hoveredArea->dispatchMotion(localX, localY);
  }

  updateCursor(serial);
}

bool InputDispatcher::isAttachedToScene(const InputArea* area) const { return nodeAttachedToRoot(area, m_sceneRoot); }

void InputDispatcher::pruneDetachedAreas() {
  if (!isAttachedToScene(m_hoveredArea)) {
    if (m_hoveredArea != nullptr) {
      auto* old = m_hoveredArea;
      old->dispatchLeave();
      m_hoveredArea = nullptr;
      if (m_hoverChangeCallback) {
        m_hoverChangeCallback(old, nullptr);
      }
    }
  }
  if (!isAttachedToScene(m_capturedArea)) {
    m_capturedArea = nullptr;
  }
  if (!isAttachedToScene(m_focusedArea)) {
    if (m_focusedArea != nullptr) {
      m_focusedArea->dispatchFocusLoss();
    }
    m_focusedArea = nullptr;
  }
}

void InputDispatcher::trackArea(InputArea* area) {
  area->setDestroyCallback([this](InputArea* a) {
    if (m_hoveredArea == a) {
      m_hoveredArea = nullptr;
    }
    if (m_focusedArea == a) {
      m_focusedArea = nullptr;
    }
    if (m_capturedArea == a) {
      m_capturedArea = nullptr;
    }
  });
}

void InputDispatcher::updateCursor(std::uint32_t serial) {
  if (!m_cursorShapeCallback) {
    return;
  }

  std::uint32_t shape = 1; // Default arrow

  if (m_hoveredArea != nullptr) {
    auto areaShape = m_hoveredArea->cursorShape();
    if (areaShape != 0) {
      shape = areaShape;
    }
  }

  m_cursorShapeCallback(serial, shape);
}
