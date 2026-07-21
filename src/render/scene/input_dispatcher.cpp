#include "render/scene/input_dispatcher.h"

#include "core/input/keybind_matcher.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "wayland/text_input_service.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {

  bool nodeVisibleInScene(const Node* node) {
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (!current->visible()) {
        return false;
      }
    }
    return true;
  }

  bool isTabFocusTarget(const InputArea* area) {
    return area != nullptr && area->enabled() && area->focusable() && area->tabStop() && nodeVisibleInScene(area);
  }

  void collectTabFocusTargets(Node* node, std::vector<InputArea*>& out) {
    if (node == nullptr || !node->visible() || node->excludeSubtreeFromTabOrder()) {
      return;
    }
    if (auto* area = dynamic_cast<InputArea*>(node); isTabFocusTarget(area)) {
      out.push_back(area);
    }
    for (const auto& child : node->children()) {
      collectTabFocusTargets(child.get(), out);
    }
  }

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
    cancelPointerCapture();
    if (m_hoveredArea != nullptr) {
      auto* old = m_hoveredArea;
      old->dispatchLeave();
      m_hoveredArea = nullptr;
      if (m_hoverChangeCallback) {
        m_hoverChangeCallback(old, nullptr);
      }
      updateCursor(m_lastSerial);
    }
  }
  if (root == nullptr) {
    if (m_focusedArea != nullptr) {
      clearTextInputFocus(m_focusedArea);
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

void InputDispatcher::setFocusChangeCallback(FocusChangeCallback callback) {
  m_focusChangeCallback = std::move(callback);
}

void InputDispatcher::setCursorShapeCallback(CursorShapeCallback callback) {
  m_cursorShapeCallback = std::move(callback);
}

void InputDispatcher::setTextInputContext(
    wl_surface* surface, TextInputService* service, bool keyboardFocusActivation, wl_surface* keyboardFocusParentSurface
) {
  if ((m_textInputSurface != surface || m_textInputService != service) && m_focusedArea != nullptr) {
    clearTextInputFocus(m_focusedArea);
  }
  m_textInputSurface = surface;
  m_textInputService = service;
  m_textInputKeyboardFocusActivation = keyboardFocusActivation;
  m_textInputKeyboardFocusParentSurface = keyboardFocusParentSurface;
  syncTextInputFocus();
}

void InputDispatcher::pointerEnter(float x, float y, std::uint32_t serial) {
  m_lastSerial = serial;
  m_lastPointerX = x;
  m_lastPointerY = y;
  m_hasPointerPosition = true;
  updateHover(x, y, serial);
}

void InputDispatcher::pointerLeave() {
  cancelPointerCapture();
  m_hasPointerPosition = false;
  if (m_hoveredArea != nullptr) {
    auto* old = m_hoveredArea;
    old->dispatchLeave();
    m_hoveredArea = nullptr;
    if (m_hoverChangeCallback) {
      m_hoverChangeCallback(old, nullptr);
    }
  }
  // Restore the default cursor while the enter serial is still valid.
  // Some compositors such as Hyprland keep the last wp_cursor_shape until the client clears it.
  updateCursor(m_lastSerial);
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

  InputArea* target = m_capturedArea;
  if (target == nullptr) {
    if (pressed) {
      target = inputAreaAcceptingButton(findInputAreaAt(x, y), button);
    }
    if (target == nullptr) {
      target = inputAreaAcceptingButton(m_hoveredArea, button);
    }
  }

  // Press with no target: subtree may have been rebuilt (same global coords, new InputArea*).
  if (target == nullptr && m_capturedArea == nullptr && pressed && m_hasPointerPosition) {
    updateHover(x, y, m_lastSerial);
    target = inputAreaAcceptingButton(findInputAreaAt(x, y), button);
    if (target == nullptr) {
      target = inputAreaAcceptingButton(m_hoveredArea, button);
    }
  }

  if (target != nullptr) {
    if (pressed && m_capturedArea == nullptr) {
      if (target != m_focusedArea && m_focusedArea != nullptr && target->focusable()) {
        setFocus(nullptr);
        pruneDetachedAreas();
        updateHover(x, y, m_lastSerial);
        target = inputAreaAcceptingButton(findInputAreaAt(x, y), button);
        if (target == nullptr) {
          target = inputAreaAcceptingButton(m_hoveredArea, button);
        }
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
    // Register capture before dispatchPress.
    if (pressed) {
      m_capturedArea = target;
      trackArea(target);
    }
    target->dispatchPress(localX, localY, button, pressed);
    if (pressed) {
      if (m_capturedArea == nullptr) {
        updateHover(x, y, m_lastSerial);
      }
    } else {
      m_capturedArea = nullptr;
      updateHover(x, y, m_lastSerial);
      // Pointer clicks on switches and buttons should not leave keyboard focus behind.
      // Settings rebuilds restore stashed focus and can land on the wrong control.
      if (m_focusedArea == target && target->textInputClient() == nullptr && !target->retainsFocusOnPointerRelease()) {
        setFocus(nullptr);
      }
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

void InputDispatcher::cancelPointerCapture() {
  if (m_capturedArea == nullptr || m_cancelingPointerCapture) {
    return;
  }
  m_cancelingPointerCapture = true;
  m_capturedArea->dispatchCancel();
  m_capturedArea = nullptr;
  m_cancelingPointerCapture = false;
  updateCursor(m_lastSerial);
}

InputArea* InputDispatcher::firstTabFocusUnder(Node* subtree) const {
  if (subtree == nullptr) {
    return nullptr;
  }
  std::vector<InputArea*> order;
  order.reserve(32);
  collectTabFocusTargets(subtree, order);
  return order.empty() ? nullptr : order.front();
}

InputArea* InputDispatcher::lastTabFocusUnder(Node* subtree) const {
  if (subtree == nullptr) {
    return nullptr;
  }
  std::vector<InputArea*> order;
  order.reserve(32);
  collectTabFocusTargets(subtree, order);
  return order.empty() ? nullptr : order.back();
}

bool InputDispatcher::cycleTabFocusInSubtree(Node* subtree, bool reverse) {
  if (subtree == nullptr) {
    return false;
  }
  std::vector<InputArea*> order;
  order.reserve(32);
  collectTabFocusTargets(subtree, order);
  if (order.empty()) {
    return false;
  }

  auto it = std::ranges::find(order, m_focusedArea);
  if (it == order.end()) {
    setFocus(reverse ? order.back() : order.front());
    return true;
  }

  if (reverse) {
    if (it == order.begin()) {
      setFocus(order.back());
    } else {
      setFocus(*std::prev(it));
    }
  } else if (std::next(it) == order.end()) {
    setFocus(order.front());
  } else {
    setFocus(*std::next(it));
  }
  return true;
}

void InputDispatcher::keyEvent(
    std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit
) {
  pruneDetachedAreas();
  if (pressed && !preedit) {
    if (KeybindMatcher::matches(KeybindAction::TabPrevious, sym, modifiers)) {
      if (cycleTabFocus(true)) {
        return;
      }
    }
    if (KeybindMatcher::matches(KeybindAction::TabNext, sym, modifiers)) {
      if (cycleTabFocus(false)) {
        return;
      }
    }
  }
  if (m_focusedArea != nullptr) {
    m_focusedArea->dispatchKey(sym, utf32, modifiers, pressed, preedit);
  }
}

bool InputDispatcher::cycleTabFocus(bool reverse) {
  if (m_sceneRoot == nullptr) {
    return false;
  }

  std::vector<InputArea*> order;
  order.reserve(32);
  collectTabFocusTargets(m_sceneRoot, order);
  if (order.empty()) {
    return false;
  }

  auto it = std::ranges::find(order, m_focusedArea);
  if (it == order.end()) {
    setFocus(reverse ? order.back() : order.front());
    return true;
  }

  if (reverse) {
    if (it == order.begin()) {
      setFocus(order.back());
    } else {
      setFocus(*std::prev(it));
    }
  } else if (std::next(it) == order.end()) {
    setFocus(order.front());
  } else {
    setFocus(*std::next(it));
  }
  return true;
}

void InputDispatcher::stashTabFocus() {
  m_stashedTabFocusIndex.reset();
  m_stashedTabFocusKey.reset();
  if (m_sceneRoot == nullptr || m_focusedArea == nullptr) {
    return;
  }

  if (!m_focusedArea->tabFocusKey().empty()) {
    m_stashedTabFocusKey = std::string(m_focusedArea->tabFocusKey());
    return;
  }

  std::vector<InputArea*> order;
  order.reserve(32);
  collectTabFocusTargets(m_sceneRoot, order);
  if (order.empty()) {
    return;
  }

  const auto it = std::ranges::find(order, m_focusedArea);
  if (it == order.end()) {
    return;
  }
  m_stashedTabFocusIndex = static_cast<std::size_t>(std::distance(order.begin(), it));
}

void InputDispatcher::restoreStashedTabFocus() {
  if (m_sceneRoot == nullptr) {
    m_stashedTabFocusIndex.reset();
    m_stashedTabFocusKey.reset();
    return;
  }

  std::vector<InputArea*> order;
  order.reserve(32);
  collectTabFocusTargets(m_sceneRoot, order);
  if (order.empty()) {
    m_stashedTabFocusIndex.reset();
    m_stashedTabFocusKey.reset();
    return;
  }

  if (m_stashedTabFocusKey.has_value()) {
    const auto it = std::ranges::find_if(order, [key = *m_stashedTabFocusKey](const InputArea* area) {
      return area != nullptr && area->tabFocusKey() == key;
    });
    if (it != order.end()) {
      setFocus(*it);
    }
    m_stashedTabFocusKey.reset();
    m_stashedTabFocusIndex.reset();
    return;
  }

  if (!m_stashedTabFocusIndex.has_value()) {
    return;
  }

  const std::size_t index = std::min(*m_stashedTabFocusIndex, order.size() - 1);
  setFocus(order[index]);
  m_stashedTabFocusIndex.reset();
}

void InputDispatcher::setFocus(InputArea* area) {
  if (area == m_focusedArea) {
    return;
  }
  InputArea* const old = m_focusedArea;
  if (m_focusedArea != nullptr) {
    clearTextInputFocus(m_focusedArea);
    m_focusedArea->dispatchFocusLoss();
  }
  m_focusedArea = area;
  if (m_focusedArea != nullptr) {
    trackArea(m_focusedArea);
    m_focusedArea->dispatchFocusGain();
    syncTextInputFocus();
  }
  if (m_focusChangeCallback) {
    m_focusChangeCallback(old, m_focusedArea);
  }
}

InputArea* InputDispatcher::inputAreaAt(float x, float y) { return findInputAreaAt(x, y); }

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
  if (!isAttachedToScene(m_capturedArea)) {
    cancelPointerCapture();
  }
  if (!isAttachedToScene(m_hoveredArea)) {
    if (m_hoveredArea != nullptr) {
      auto* old = m_hoveredArea;
      old->dispatchLeave();
      m_hoveredArea = nullptr;
      if (m_hoverChangeCallback) {
        m_hoverChangeCallback(old, nullptr);
      }
      updateCursor(m_lastSerial);
    }
  }
  if (!isAttachedToScene(m_focusedArea)) {
    if (m_focusedArea != nullptr) {
      clearTextInputFocus(m_focusedArea);
      m_focusedArea->dispatchFocusLoss();
    }
    m_focusedArea = nullptr;
  }
}

void InputDispatcher::trackArea(InputArea* area) {
  area->setDestroyCallback([this](InputArea* a) {
    const bool ownedCursor = m_hoveredArea == a || m_capturedArea == a;
    if (m_capturedArea == a) {
      m_capturedArea = nullptr;
    }
    if (m_hoveredArea == a) {
      m_hoveredArea = nullptr;
      if (m_hoverChangeCallback) {
        m_hoverChangeCallback(a, nullptr);
      }
    }
    if (m_focusedArea == a) {
      clearTextInputFocus(a);
      m_focusedArea = nullptr;
    }
    if (ownedCursor) {
      updateCursor(m_lastSerial);
    }
  });
}

void InputDispatcher::clearTextInputFocus(InputArea* area) {
  if (m_textInputService == nullptr || area == nullptr) {
    return;
  }
  if (auto* client = area->textInputClient(); client != nullptr) {
    m_textInputService->clearFocusedClient(client);
  }
}

void InputDispatcher::syncTextInputFocus() {
  if (m_textInputService == nullptr || m_textInputSurface == nullptr || m_focusedArea == nullptr) {
    return;
  }
  if (auto* client = m_focusedArea->textInputClient(); client != nullptr) {
    m_textInputService->setFocusedClient(
        m_textInputSurface, client, m_textInputKeyboardFocusActivation, m_textInputKeyboardFocusParentSurface
    );
  }
}

void InputDispatcher::updateCursor(std::uint32_t serial) {
  if (!m_cursorShapeCallback) {
    return;
  }

  std::uint32_t shape = 1; // Default arrow

  InputArea* cursorAuthority = m_capturedArea != nullptr ? m_capturedArea : m_hoveredArea;
  if (cursorAuthority != nullptr) {
    auto areaShape = cursorAuthority->cursorShape();
    if (areaShape != 0) {
      shape = areaShape;
    }
  }

  m_cursorShapeCallback(serial, shape);
}
