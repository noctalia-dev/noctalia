#include "render/scene/input_area.h"

#include "cursor-shape-v1-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace {

  constexpr std::uint32_t kMouseButtonBase = BTN_MOUSE;
  constexpr std::uint32_t kMaxTrackedMouseButtons = 32;
  // Continuous-source axis units that trigger one wheel-detent step. libinput's
  // detent convention is 15 units; we require a bit more so touchpad swipes
  // step deliberately rather than racing the finger.
  constexpr float kScrollUnitsPerStep = 20.0F;
  // A pause longer than this ends a scroll gesture: the next axis event starts
  // fresh so a partial detent left over from a free-spin flick can't bank into
  // the following one and tip it into an extra step.
  constexpr auto kScrollGestureGap = std::chrono::milliseconds(100);
  // Shortest interval between steps taken from a detent-less stream. Those
  // sources emit frames as fast as the finger moves, so the threshold alone
  // would fire dozens of times a second on a flick.
  constexpr auto kContinuousStepInterval = std::chrono::milliseconds(80);

  bool isWheelSource(std::uint32_t axisSource) noexcept {
    return axisSource == WL_POINTER_AXIS_SOURCE_WHEEL || axisSource == WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
  }

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
  return 1U << index;
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
void InputArea::setOnCancel(VoidCallback callback) { m_onCancel = std::move(callback); }
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
void InputArea::setHitShape(HitShape shape) { m_hitShape = shape; }

bool InputArea::containsLocalPoint(float localX, float localY, bool includeHitOutset) const {
  if (m_hitShape == HitShape::Rect) {
    return Node::containsLocalPoint(localX, localY, includeHitOutset);
  }

  const HitTestOutset outset = includeHitOutset ? hitTestOutset() : HitTestOutset{};
  const float centerX = width() * 0.5F;
  const float centerY = height() * 0.5F;
  const float baseRadius = std::min(width(), height()) * 0.5F;
  const float radius = baseRadius + std::max({outset.left, outset.top, outset.right, outset.bottom});
  const float dx = localX - centerX;
  const float dy = localY - centerY;
  return dx * dx + dy * dy <= radius * radius;
}

void InputArea::setTooltip(std::string text) {
  m_tooltipProvider = {};
  m_tooltipRefreshInterval = {};
  m_tooltipContent = std::move(text);
  notifyTooltipChanged();
}

void InputArea::setTooltip(std::vector<TooltipRow> rows) {
  m_tooltipProvider = {};
  m_tooltipRefreshInterval = {};
  m_tooltipContent = std::move(rows);
  notifyTooltipChanged();
}

void InputArea::setTooltipProvider(TooltipProvider provider, std::chrono::milliseconds refreshInterval) {
  m_tooltipContent = std::monostate{};
  m_tooltipProvider = std::move(provider);
  m_tooltipRefreshInterval = refreshInterval;
  notifyTooltipChanged();
}

void InputArea::clearTooltip() {
  m_tooltipContent = std::monostate{};
  m_tooltipProvider = {};
  m_tooltipRefreshInterval = {};
  notifyTooltipChanged();
}

void InputArea::requestTooltipRefresh() { notifyTooltipChanged(); }

void InputArea::setTooltipChangedCallback(TooltipChangedCallback callback) {
  m_tooltipChangedCallback = std::move(callback);
}

void InputArea::setTooltipPlacement(TooltipPlacement placement) { m_tooltipPlacement = placement; }
void InputArea::setTooltipAnchorInsets(TooltipAnchorInsets insets) {
  m_tooltipAnchorInsets = insets;
  m_hasTooltipAnchorInsets = true;
}
void InputArea::clearTooltipAnchorInsets() { m_hasTooltipAnchorInsets = false; }
bool InputArea::hasTooltip() const noexcept {
  return m_tooltipProvider != nullptr || !std::holds_alternative<std::monostate>(m_tooltipContent);
}

TooltipContent InputArea::tooltipContent() const {
  if (m_tooltipProvider) {
    return m_tooltipProvider();
  }
  return m_tooltipContent;
}

void InputArea::notifyTooltipChanged() {
  onTooltipChanged();
  if (m_hovered && m_tooltipChangedCallback) {
    m_tooltipChangedCallback(this);
  }
}
void InputArea::setFocusable(bool focusable) { m_focusable = focusable; }

void InputArea::setTabStop(bool tabStop) { m_tabStop = tabStop; }
void InputArea::setTabFocusKey(std::string key) { m_tabFocusKey = std::move(key); }
void InputArea::setOnKeyDown(KeyCallback callback) { m_onKeyDown = std::move(callback); }
void InputArea::setOnKeyUp(KeyCallback callback) { m_onKeyUp = std::move(callback); }
void InputArea::setOnFocusGain(VoidCallback callback) { m_onFocusGain = std::move(callback); }
void InputArea::setOnFocusLoss(VoidCallback callback) { m_onFocusLoss = std::move(callback); }
void InputArea::setTextInputClient(TextInputClient* client) { m_textInputClient = client; }
void InputArea::setRetainsFocusOnPointerRelease(bool retain) { m_retainsFocusOnPointerRelease = retain; }

void InputArea::dispatchEnter(float localX, float localY) {
  m_hovered = true;
  resetScrollAccumulators();
  if (m_onEnter) {
    m_onEnter({.localX = localX, .localY = localY});
  }
}

void InputArea::resetScrollAccumulators() noexcept {
  m_scrollStepAccum.fill(0.0F);
  m_lastScrollStepSign.fill(0.0F);
  m_lastScrollStepTime.fill({});
}

void InputArea::dispatchLeave() {
  m_hovered = false;
  m_pressed = false;
  m_pressedButton = 0;
  resetScrollAccumulators();
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
    const bool releasedInside = containsLocalPoint(localX, localY, true);
    const bool shouldClick = m_pressed && m_pressedButton == button && releasedInside && m_onClick;
    m_pressed = false;
    m_pressedButton = 0;

    if (m_onPress) {
      m_onPress({.localX = localX, .localY = localY, .button = button, .pressed = false});
    }

    // Click: release inside the same InputArea that received the press.
    if (shouldClick) {
      m_onClick({.localX = localX, .localY = localY, .button = button, .pressed = false});
    }
  }
}

void InputArea::dispatchCancel() {
  m_pressed = false;
  m_pressedButton = 0;
  if (m_onCancel) {
    m_onCancel();
  }
}

bool InputArea::dispatchAxis(
    float localX, float localY, std::uint32_t axis, std::uint32_t axisSource, double axisValue,
    std::int32_t axisDiscrete, std::int32_t axisValue120, float axisLines, std::uint32_t axisGestureSerial
) {
  if (!m_onAxis) {
    return false;
  }

  // Reject unaccepted directions before they reach the accumulator, so a direction this area has
  // given up cannot bank fractional detents here on its way to an ancestor.
  if (m_acceptedScrollDirections != allScrollDirections()) {
    const float delta = axisLines != 0.0F ? axisLines : static_cast<float>(axisValue);
    if (delta != 0.0F) {
      // Wayland reports up/left as a negative delta.
      std::optional<ScrollDirection> direction;
      if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        direction = delta < 0.0F ? ScrollDirection::Up : ScrollDirection::Down;
      } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
        direction = delta < 0.0F ? ScrollDirection::Left : ScrollDirection::Right;
      }
      if (direction.has_value() && (m_acceptedScrollDirections & scrollDirectionMask(*direction)) == 0) {
        return false;
      }
    }
  }

  // Quantize scroll into whole detent steps, at most one per frame. A wheel
  // notch is a hardware detent: the user feels one click, so it is one step
  // even when the compositor scales the delta (niri's scroll-factor), and a
  // free-spinning hi-res wheel accrues sub-detent frames until a full detent
  // has turned. Continuous sources (touchpads) accrue axisValue the same way.
  // Scrolling content stays on scrollDelta() and keeps the scaling.
  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastAxisTime > kScrollGestureGap) {
    resetScrollAccumulators();
  }
  m_lastAxisTime = now;

  float axisSteps = 0.0F;
  bool startsGesture = false;
  if (axis < m_scrollStepAccum.size()) {
    if (m_axisGestureSerial[axis] != axisGestureSerial) {
      m_scrollStepAccum[axis] = 0.0F;
      m_lastScrollStepSign[axis] = 0.0F;
      m_lastScrollStepTime[axis] = {};
      m_axisGestureSerial[axis] = axisGestureSerial;
    }
    float& accum = m_scrollStepAccum[axis];
    const float detentDelta = axisLines != 0.0F ? axisLines : static_cast<float>(axisValue) / kScrollUnitsPerStep;
    if ((detentDelta > 0.0F && accum < 0.0F) || (detentDelta < 0.0F && accum > 0.0F)) {
      accum = 0.0F;
    }
    accum += detentDelta;
    axisSteps = std::trunc(accum);
    accum -= axisSteps;

    if (axisSteps != 0.0F) {
      const float sign = std::copysign(1.0F, axisSteps);
      const float previousSign = m_lastScrollStepSign[axis];
      startsGesture = previousSign == 0.0F || previousSign != sign;
      // value120/axis_discrete means the compositor counted the notches for us, so every notch
      // steps: spinning faster stays proportional. Without them the stream is continuous
      // (touchpads, and wheels on compositors that send neither) and crosses the threshold as
      // fast as the finger moves, so it is rate-capped instead.
      const bool detentCounted = isWheelSource(axisSource) && (axisValue120 != 0 || axisDiscrete != 0);
      if (!detentCounted && !startsGesture && now - m_lastScrollStepTime[axis] < kContinuousStepInterval) {
        // Drop the surplus rather than banking it, so a capped step cannot fire late.
        axisSteps = 0.0F;
        accum = 0.0F;
      } else {
        axisSteps = sign;
        m_lastScrollStepSign[axis] = sign;
        m_lastScrollStepTime[axis] = now;
      }
    }
  }

  return m_onAxis(
      {.localX = localX,
       .localY = localY,
       .axis = axis,
       .axisSource = axisSource,
       .pressed = false,
       .axisValue = axisValue,
       .axisDiscrete = axisDiscrete,
       .axisValue120 = axisValue120,
       .axisLines = axisLines,
       .axisSteps = axisSteps,
       .axisStepStartsGesture = startsGesture}
  );
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
