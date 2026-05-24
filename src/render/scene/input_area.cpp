#include "render/scene/input_area.h"

#include "cursor-shape-v1-client-protocol.h"

namespace {

  constexpr std::uint32_t kMouseButtonBase = BTN_MOUSE;
  constexpr std::uint32_t kMaxTrackedMouseButtons = 32;

} // namespace

InputArea::InputArea() : Node(NodeType::Base) {}

InputArea::~InputArea() {
  if (m_destroyCallback) {
    m_destroyCallback(this);
  }
}

void InputArea::setDestroyCallback(DestroyCallback callback) { m_destroyCallback = std::move(callback); }

std::uint32_t InputArea::buttonMask(std::uint32_t button) noexcept {
  if (button < kMouseButtonBase) {
    return 0;
  }
  const std::uint32_t index = button - kMouseButtonBase;
  if (index >= kMaxTrackedMouseButtons) {
    return 0;
  }
  return 1u << index;
}

std::uint32_t InputArea::buttonMask(std::initializer_list<std::uint32_t> buttons) noexcept {
  std::uint32_t mask = 0;
  for (const auto button : buttons) {
    mask |= buttonMask(button);
  }
  return mask;
}

void InputArea::setOnEnter(PointerCallback callback) { m_onEnter = std::move(callback); }
void InputArea::setOnLeave(VoidCallback callback) { m_onLeave = std::move(callback); }
void InputArea::setOnMotion(PointerCallback callback) { m_onMotion = std::move(callback); }
void InputArea::setOnPress(PointerCallback callback) { m_onPress = std::move(callback); }
void InputArea::setOnAxis(PointerCallback callback) {
  m_onAxis = [callback = std::move(callback)](const PointerData& data) {
    callback(data);
    return true;
  };
}
void InputArea::setOnAxisHandler(AxisCallback callback) { m_onAxis = std::move(callback); }
void InputArea::setOnClick(PointerCallback callback) {
  m_onClick = std::move(callback);
  if (m_onClick && m_cursorShape == 0) {
    m_cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
  }
}

void InputArea::setCursorShape(std::uint32_t shape) { m_cursorShape = shape; }
void InputArea::setAcceptedButtons(std::uint32_t mask) { m_acceptedButtons = mask; }
bool InputArea::acceptsButton(std::uint32_t button) const noexcept {
  return (m_acceptedButtons & buttonMask(button)) != 0;
}
void InputArea::setPropagateEvents(bool propagate) { m_propagateEvents = propagate; }
void InputArea::setEnabled(bool enabled) { m_enabled = enabled; }

void InputArea::setTooltip(std::string text) { m_tooltipContent = std::move(text); }
void InputArea::setTooltip(std::vector<TooltipRow> rows) { m_tooltipContent = std::move(rows); }
void InputArea::clearTooltip() { m_tooltipContent = std::monostate{}; }
bool InputArea::hasTooltip() const noexcept { return !std::holds_alternative<std::monostate>(m_tooltipContent); }
void InputArea::setFocusable(bool focusable) { m_focusable = focusable; }
void InputArea::setOnKeyDown(KeyCallback callback) { m_onKeyDown = std::move(callback); }
void InputArea::setOnKeyUp(KeyCallback callback) { m_onKeyUp = std::move(callback); }
void InputArea::setOnFocusGain(VoidCallback callback) { m_onFocusGain = std::move(callback); }
void InputArea::setOnFocusLoss(VoidCallback callback) { m_onFocusLoss = std::move(callback); }

void InputArea::dispatchEnter(float localX, float localY) {
  m_hovered = true;
  if (m_onEnter) {
    m_onEnter({.localX = localX, .localY = localY});
  }
}

void InputArea::dispatchLeave() {
  m_hovered = false;
  m_pressed = false;
  m_pressedButton = 0;
  if (m_onLeave) {
    m_onLeave();
  }
}

void InputArea::dispatchMotion(float localX, float localY) {
  if (m_onMotion) {
    m_onMotion({.localX = localX, .localY = localY});
  }
}

void InputArea::dispatchPress(float localX, float localY, std::uint32_t button, bool isPressed) {
  if (isPressed) {
    m_pressed = true;
    m_pressedButton = button;
    if (m_onPress) {
      m_onPress({.localX = localX, .localY = localY, .button = button, .pressed = true});
    }
  } else {
    const bool shouldClick = m_pressed && m_pressedButton == button && m_onClick;
    m_pressed = false;
    m_pressedButton = 0;

    if (m_onPress) {
      m_onPress({.localX = localX, .localY = localY, .button = button, .pressed = false});
    }

    // Click: release at the same InputArea that received the press
    if (shouldClick) {
      m_onClick({.localX = localX, .localY = localY, .button = button, .pressed = false});
    }
  }
}

bool InputArea::dispatchAxis(
    float localX, float localY, std::uint32_t axis, std::uint32_t axisSource, double axisValue,
    std::int32_t axisDiscrete, std::int32_t axisValue120, float axisLines
) {
  if (m_onAxis) {
    return m_onAxis(
        {.localX = localX,
         .localY = localY,
         .axis = axis,
         .axisSource = axisSource,
         .pressed = false,
         .axisValue = axisValue,
         .axisDiscrete = axisDiscrete,
         .axisValue120 = axisValue120,
         .axisLines = axisLines}
    );
  }
  return false;
}

void InputArea::dispatchKey(
    std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit
) {
  const KeyData data{.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = pressed, .preedit = preedit};
  if (pressed) {
    if (m_onKeyDown) {
      m_onKeyDown(data);
    }
  } else {
    if (m_onKeyUp) {
      m_onKeyUp(data);
    }
  }
}

void InputArea::dispatchFocusGain() {
  m_focused = true;
  if (m_onFocusGain) {
    m_onFocusGain();
  }
}

void InputArea::dispatchFocusLoss() {
  m_focused = false;
  if (m_onFocusLoss) {
    m_onFocusLoss();
  }
}
