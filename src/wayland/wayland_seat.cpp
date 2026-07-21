#include "wayland/wayland_seat.h"

#include "core/input/shortcut_keysym.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"

#include <algorithm>
#include <clocale>
#include <cstring>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  const wl_keyboard_listener kKeyboardListener = {
      .keymap = &WaylandSeat::handleKeyboardKeymap,
      .enter = &WaylandSeat::handleKeyboardEnter,
      .leave = &WaylandSeat::handleKeyboardLeave,
      .key = &WaylandSeat::handleKeyboardKey,
      .modifiers = &WaylandSeat::handleKeyboardModifiers,
      .repeat_info = &WaylandSeat::handleKeyboardRepeatInfo,
  };

  const wl_seat_listener kSeatListener = {
      .capabilities = &WaylandSeat::handleSeatCapabilities,
      .name = &WaylandSeat::handleSeatName,
  };

  const wl_pointer_listener kPointerListener = {
      .enter = &WaylandSeat::handlePointerEnter,
      .leave = &WaylandSeat::handlePointerLeave,
      .motion = &WaylandSeat::handlePointerMotion,
      .button = &WaylandSeat::handlePointerButton,
      .axis = &WaylandSeat::handlePointerAxis,
      .frame = &WaylandSeat::handlePointerFrame,
      .axis_source = &WaylandSeat::handlePointerAxisSource,
      .axis_stop = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
      .axis_discrete = &WaylandSeat::handlePointerAxisDiscrete,
      .axis_value120 = &WaylandSeat::handlePointerAxisValue120,
      .axis_relative_direction = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {},
  };

  const wl_touch_listener kTouchListener = {
      .down = &WaylandSeat::handleTouchDown,
      .up = &WaylandSeat::handleTouchUp,
      .motion = &WaylandSeat::handleTouchMotion,
      .frame = &WaylandSeat::handleTouchFrame,
      .cancel = &WaylandSeat::handleTouchCancel,
      .shape = [](void*, wl_touch*, std::int32_t, std::int32_t, std::int32_t) {},
      .orientation = [](void*, wl_touch*, std::int32_t, std::int32_t) {},
  };

  constexpr Logger kLog("seat");
  constexpr float kAxisValue120PerStep = 120.0f;
  // libinput reports one wheel detent as 15 degrees of rotation.
  constexpr float kLegacyWheelAxisUnitsPerStep = 15.0f;

} // namespace

void WaylandSeat::bind(wl_seat* seat) {
  m_seat = seat;
  m_lastUserActivitySteady = SteadyClock::now();
  wl_seat_add_listener(seat, &kSeatListener, this);
}

void WaylandSeat::bumpUserActivity() noexcept { m_lastUserActivitySteady = SteadyClock::now(); }

double WaylandSeat::userIdleSeconds() const noexcept {
  const auto now = SteadyClock::now();
  const auto dur = std::chrono::duration<double>(now - m_lastUserActivitySteady);
  return std::max(0.0, dur.count());
}

void WaylandSeat::setCursorShapeManager(wp_cursor_shape_manager_v1* manager) { m_cursorShapeManager = manager; }

void WaylandSeat::setPointerEventCallback(PointerEventCallback callback) {
  m_pointerEventCallback = std::move(callback);
}

void WaylandSeat::setKeyboardEventCallback(KeyboardEventCallback callback) {
  m_keyboardEventCallback = std::move(callback);
}

void WaylandSeat::setKeyboardFocusCallback(KeyboardFocusCallback callback) {
  m_keyboardFocusCallback = std::move(callback);
}

void WaylandSeat::setLockKeysChangeCallback(LockKeysChangeCallback callback) {
  m_lockKeysChangeCallback = std::move(callback);
}

void WaylandSeat::setCursorShape(std::uint32_t serial, std::uint32_t shape) {
  if (m_cursorShapeDevice == nullptr) {
    return;
  }
  // Use the wl_pointer.enter serial for the surface hover lifetime.
  const std::uint32_t effectiveSerial = m_pointerEnterSerial != 0 ? m_pointerEnterSerial : serial;
  if (effectiveSerial == 0) {
    return;
  }
  wp_cursor_shape_device_v1_set_shape(m_cursorShapeDevice, effectiveSerial, shape);
}

void WaylandSeat::forgetSurface(wl_surface* surface) noexcept {
  if (surface == nullptr) {
    return;
  }
  if (m_lastPointerSurface == surface) {
    if (m_cursorShapeDevice != nullptr && m_pointerEnterSerial != 0) {
      wp_cursor_shape_device_v1_set_shape(
          m_cursorShapeDevice, m_pointerEnterSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
      );
    }
    m_lastPointerSurface = nullptr;
    m_pointerEnterSerial = 0;
    m_hasPointerPosition = false;
  }
  if (m_lastKeyboardSurface == surface) {
    m_lastKeyboardSurface = nullptr;
    m_repeatActive = false;
  }
  std::erase_if(m_pendingPointerEvents, [surface](const PointerEvent& event) { return event.surface == surface; });
  if (m_touchSurface == surface) {
    m_touchSurface = nullptr;
    m_activeTouchId = -1;
  }
  std::erase_if(m_pendingTouchEvents, [surface](const PointerEvent& event) { return event.surface == surface; });
}

void WaylandSeat::cleanup() {
  if (m_cursorShapeDevice != nullptr) {
    wp_cursor_shape_device_v1_destroy(m_cursorShapeDevice);
    m_cursorShapeDevice = nullptr;
  }
  if (m_pointer != nullptr) {
    wl_pointer_destroy(m_pointer);
    m_pointer = nullptr;
  }
  m_cursorShapeManager = nullptr;

  if (m_touch != nullptr) {
    wl_touch_destroy(m_touch);
    m_touch = nullptr;
  }
  m_activeTouchId = -1;
  m_touchSurface = nullptr;
  m_pendingTouchEvents.clear();

  if (m_composeState != nullptr) {
    xkb_compose_state_unref(m_composeState);
    m_composeState = nullptr;
  }
  if (m_composeTable != nullptr) {
    xkb_compose_table_unref(m_composeTable);
    m_composeTable = nullptr;
  }
  if (m_xkbState != nullptr) {
    xkb_state_unref(m_xkbState);
    m_xkbState = nullptr;
  }
  if (m_xkbKeymap != nullptr) {
    xkb_keymap_unref(m_xkbKeymap);
    m_xkbKeymap = nullptr;
  }
  if (m_xkbContext != nullptr) {
    xkb_context_unref(m_xkbContext);
    m_xkbContext = nullptr;
  }
  if (m_keyboard != nullptr) {
    wl_keyboard_destroy(m_keyboard);
    m_keyboard = nullptr;
  }
  m_repeatActive = false;
}

void WaylandSeat::handleSeatCapabilities(void* data, wl_seat* seat, std::uint32_t caps) {
  auto* self = static_cast<WaylandSeat*>(data);

  const bool hasKeyboard = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

  if (hasKeyboard && self->m_keyboard == nullptr) {
    if (self->m_xkbContext == nullptr) {
      self->m_xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }
    self->m_keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(self->m_keyboard, &kKeyboardListener, self);
    kLog.info("keyboard: bound");
  } else if (!hasKeyboard && self->m_keyboard != nullptr) {
    wl_keyboard_destroy(self->m_keyboard);
    self->m_keyboard = nullptr;
    kLog.info("keyboard: released");
  }

  const bool hasPointer = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;

  if (hasPointer && self->m_pointer == nullptr) {
    self->m_pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(self->m_pointer, &kPointerListener, self);
    kLog.info("pointer: bound");

    if (self->m_cursorShapeManager != nullptr) {
      self->m_cursorShapeDevice = wp_cursor_shape_manager_v1_get_pointer(self->m_cursorShapeManager, self->m_pointer);
      kLog.info("pointer: cursor-shape-v1 available");
    }
  } else if (!hasPointer && self->m_pointer != nullptr) {
    if (self->m_cursorShapeDevice != nullptr) {
      wp_cursor_shape_device_v1_destroy(self->m_cursorShapeDevice);
      self->m_cursorShapeDevice = nullptr;
    }
    wl_pointer_destroy(self->m_pointer);
    self->m_pointer = nullptr;
    kLog.info("pointer: released");
  }

  const bool hasTouch = (caps & WL_SEAT_CAPABILITY_TOUCH) != 0;

  if (hasTouch && self->m_touch == nullptr) {
    self->m_touch = wl_seat_get_touch(seat);
    wl_touch_add_listener(self->m_touch, &kTouchListener, self);
    kLog.info("touch: bound");
  } else if (!hasTouch && self->m_touch != nullptr) {
    wl_touch_destroy(self->m_touch);
    self->m_touch = nullptr;
    self->m_activeTouchId = -1;
    self->m_touchSurface = nullptr;
    self->m_pendingTouchEvents.clear();
    kLog.info("touch: released");
  }
}

void WaylandSeat::handleSeatName(void* /*data*/, wl_seat* /*seat*/, const char* /*name*/) {}

void WaylandSeat::handlePointerEnter(
    void* data, wl_pointer* /*pointer*/, std::uint32_t serial, wl_surface* surface, std::int32_t sx, std::int32_t sy
) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_lastSerial = serial;
  self->m_lastInputSource = InputSource::Pointer;
  self->m_lastPointerSurface = surface;
  self->m_pointerEnterSerial = serial;
  self->m_lastPointerX = wl_fixed_to_double(sx);
  self->m_lastPointerY = wl_fixed_to_double(sy);
  self->m_hasPointerPosition = true;
  self->m_pendingPointerEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Enter,
          .serial = serial,
          .surface = surface,
          .sx = self->m_lastPointerX,
          .sy = self->m_lastPointerY,
      }
  );
}

void WaylandSeat::handlePointerLeave(void* data, wl_pointer* /*pointer*/, std::uint32_t serial, wl_surface* surface) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (self->m_cursorShapeDevice != nullptr
      && self->m_pointerEnterSerial != 0
      && self->m_lastPointerSurface == surface) {
    wp_cursor_shape_device_v1_set_shape(
        self->m_cursorShapeDevice, self->m_pointerEnterSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
    );
  }
  self->m_lastSerial = serial;
  self->m_lastInputSource = InputSource::Pointer;
  self->m_lastPointerSurface = surface;
  self->m_pointerEnterSerial = 0;
  self->m_hasPointerPosition = false;
  self->m_pendingPointerEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Leave,
          .serial = serial,
          .surface = surface,
      }
  );
}

void WaylandSeat::handlePointerMotion(
    void* data, wl_pointer* /*pointer*/, std::uint32_t time, std::int32_t sx, std::int32_t sy
) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_lastPointerX = wl_fixed_to_double(sx);
  self->m_lastPointerY = wl_fixed_to_double(sy);
  self->m_hasPointerPosition = true;
  self->m_pendingPointerEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Motion,
          .surface = self->m_lastPointerSurface,
          .sx = self->m_lastPointerX,
          .sy = self->m_lastPointerY,
          .time = time,
      }
  );
}

void WaylandSeat::handlePointerButton(
    void* data, wl_pointer* /*pointer*/, std::uint32_t serial, std::uint32_t time, std::uint32_t button,
    std::uint32_t state
) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_lastSerial = serial;
  self->m_lastInputSource = InputSource::Pointer;
  self->m_pendingPointerEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Button,
          .serial = serial,
          .surface = self->m_lastPointerSurface,
          .sx = self->m_hasPointerPosition ? self->m_lastPointerX : 0.0,
          .sy = self->m_hasPointerPosition ? self->m_lastPointerY : 0.0,
          .time = time,
          .button = button,
          .pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED),
      }
  );
}

void WaylandSeat::handlePointerAxis(
    void* data, wl_pointer* /*pointer*/, std::uint32_t time, std::uint32_t axis, std::int32_t value
) {
  auto* self = static_cast<WaylandSeat*>(data);
  PointerEvent event{
      .type = PointerEvent::Type::Axis,
      .surface = self->m_lastPointerSurface,
      .sx = self->m_hasPointerPosition ? self->m_lastPointerX : 0.0,
      .sy = self->m_hasPointerPosition ? self->m_lastPointerY : 0.0,
      .time = time,
      .axis = axis,
      .axisSource = self->m_pendingAxisSource,
      .axisValue = wl_fixed_to_double(value),
  };

  // axis_discrete/axis_value120 arrive before the axis event they describe, and
  // the protocol couples each of them to exactly one axis event of the same
  // axis within the frame. Claim whatever was stashed for this axis.
  if (axis < self->m_pendingAxisDetents.size()) {
    AxisDetent& detent = self->m_pendingAxisDetents[axis];
    if (detent.valid) {
      event.axisDiscrete = detent.discrete;
      event.axisValue120 = detent.value120;
      event.axisLines = detent.lines;
      detent = {};
    }
  }

  self->m_pendingPointerEvents.push_back(event);
}

void WaylandSeat::handlePointerAxisSource(void* data, wl_pointer* /*pointer*/, std::uint32_t axisSource) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_pendingAxisSource = axisSource;
}

void WaylandSeat::handlePointerAxisDiscrete(
    void* data, wl_pointer* /*pointer*/, std::uint32_t axis, std::int32_t discrete
) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (axis >= self->m_pendingAxisDetents.size()) {
    return;
  }
  AxisDetent& detent = self->m_pendingAxisDetents[axis];
  detent.valid = true;
  detent.discrete = discrete;
  if (detent.lines == 0.0f) {
    detent.lines = static_cast<float>(discrete);
  }
}

void WaylandSeat::handlePointerAxisValue120(
    void* data, wl_pointer* /*pointer*/, std::uint32_t axis, std::int32_t value120
) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (axis >= self->m_pendingAxisDetents.size()) {
    return;
  }
  AxisDetent& detent = self->m_pendingAxisDetents[axis];
  detent.valid = true;
  detent.value120 = value120;
  detent.lines = static_cast<float>(value120) / kAxisValue120PerStep;
}

void WaylandSeat::handlePointerFrame(void* data, wl_pointer* /*pointer*/) {
  auto* self = static_cast<WaylandSeat*>(data);
  std::vector<PointerEvent> events = std::move(self->m_pendingPointerEvents);
  self->m_pendingPointerEvents.clear();
  self->m_pendingAxisSource = 0;
  // Detents never carry across a frame; drop any the compositor left uncoupled.
  self->m_pendingAxisDetents.fill({});

  if (!self->m_pointerEventCallback) {
    return;
  }

  for (auto& event : events) {
    if (event.type == PointerEvent::Type::Axis
        && event.axisLines == 0.0f
        && (event.axisSource == WL_POINTER_AXIS_SOURCE_WHEEL || event.axisSource == WL_POINTER_AXIS_SOURCE_WHEEL_TILT)
        && event.axisValue != 0.0) {
      // Some compositors send wheel-source axis events without discrete/value120.
      // Normalize those legacy wheel deltas into logical wheel steps centrally.
      event.axisLines = static_cast<float>(event.axisValue / kLegacyWheelAxisUnitsPerStep);
    }

    switch (event.type) {
    case PointerEvent::Type::Enter:
    case PointerEvent::Type::Motion:
    case PointerEvent::Type::Button:
    case PointerEvent::Type::Axis:
      self->bumpUserActivity();
      break;
    case PointerEvent::Type::Leave:
      break;
    }

    self->m_pointerEventCallback(event);
  }
}

void WaylandSeat::handleTouchDown(
    void* data, wl_touch* /*touch*/, std::uint32_t serial, std::uint32_t time, wl_surface* surface, std::int32_t id,
    std::int32_t x, std::int32_t y
) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (self->m_activeTouchId != -1) {
    return;
  }
  self->m_activeTouchId = id;
  self->m_touchSurface = surface;
  self->m_lastPointerSurface = surface;
  self->m_lastPointerX = wl_fixed_to_double(x);
  self->m_lastPointerY = wl_fixed_to_double(y);
  self->m_hasPointerPosition = true;
  self->m_lastSerial = serial;
  self->m_lastInputSource = InputSource::Touch;
  self->m_pendingTouchEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Enter,
          .serial = serial,
          .surface = surface,
          .sx = self->m_lastPointerX,
          .sy = self->m_lastPointerY,
          .time = time,
      }
  );
  self->m_pendingTouchEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Button,
          .serial = serial,
          .surface = surface,
          .sx = self->m_lastPointerX,
          .sy = self->m_lastPointerY,
          .time = time,
          .button = BTN_LEFT,
          .pressed = true,
      }
  );
}

void WaylandSeat::handleTouchUp(
    void* data, wl_touch* /*touch*/, std::uint32_t serial, std::uint32_t time, std::int32_t id
) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (id != self->m_activeTouchId) {
    return;
  }
  self->m_lastSerial = serial;
  auto* surface = self->m_touchSurface;
  self->m_pendingTouchEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Button,
          .serial = serial,
          .surface = surface,
          .sx = self->m_lastPointerX,
          .sy = self->m_lastPointerY,
          .time = time,
          .button = BTN_LEFT,
          .pressed = false,
      }
  );
  self->m_pendingTouchEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Leave,
          .serial = serial,
          .surface = surface,
      }
  );
  self->m_activeTouchId = -1;
  self->m_touchSurface = nullptr;
  self->m_lastPointerSurface = nullptr;
  self->m_hasPointerPosition = false;
}

void WaylandSeat::handleTouchMotion(
    void* data, wl_touch* /*touch*/, std::uint32_t time, std::int32_t id, std::int32_t x, std::int32_t y
) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (id != self->m_activeTouchId) {
    return;
  }
  self->m_lastPointerX = wl_fixed_to_double(x);
  self->m_lastPointerY = wl_fixed_to_double(y);
  self->m_pendingTouchEvents.push_back(
      PointerEvent{
          .type = PointerEvent::Type::Motion,
          .surface = self->m_touchSurface,
          .sx = self->m_lastPointerX,
          .sy = self->m_lastPointerY,
          .time = time,
      }
  );
}

void WaylandSeat::handleTouchFrame(void* data, wl_touch* /*touch*/) {
  auto* self = static_cast<WaylandSeat*>(data);
  std::vector<PointerEvent> events = std::move(self->m_pendingTouchEvents);
  self->m_pendingTouchEvents.clear();

  if (!self->m_pointerEventCallback) {
    return;
  }

  for (const auto& event : events) {
    switch (event.type) {
    case PointerEvent::Type::Enter:
    case PointerEvent::Type::Motion:
    case PointerEvent::Type::Button:
    case PointerEvent::Type::Axis:
      self->bumpUserActivity();
      break;
    case PointerEvent::Type::Leave:
      break;
    }

    self->m_pointerEventCallback(event);
  }
}

void WaylandSeat::handleTouchCancel(void* data, wl_touch* /*touch*/) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (self->m_activeTouchId == -1) {
    return;
  }
  auto* surface = self->m_touchSurface;
  self->m_activeTouchId = -1;
  self->m_touchSurface = nullptr;
  self->m_lastPointerSurface = nullptr;
  self->m_hasPointerPosition = false;
  self->m_pendingTouchEvents.clear();

  if (self->m_pointerEventCallback) {
    self->bumpUserActivity();
    self->m_pointerEventCallback(
        PointerEvent{
            .type = PointerEvent::Type::Button,
            .surface = surface,
            .sx = self->m_lastPointerX,
            .sy = self->m_lastPointerY,
            .button = BTN_LEFT,
            .pressed = false,
        }
    );
    self->m_pointerEventCallback(
        PointerEvent{
            .type = PointerEvent::Type::Leave,
            .surface = surface,
        }
    );
  }
}

void WaylandSeat::handleKeyboardKeymap(
    void* data, wl_keyboard* /*keyboard*/, std::uint32_t format, int fd, std::uint32_t size
) {
  auto* self = static_cast<WaylandSeat*>(data);

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }

  void* buf = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (buf == MAP_FAILED) {
    kLog.warn("keyboard: failed to mmap keymap");
    return;
  }

  auto* keymap = xkb_keymap_new_from_string(
      self->m_xkbContext, static_cast<const char*>(buf), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
  );
  munmap(buf, size);

  if (keymap == nullptr) {
    kLog.warn("keyboard: failed to compile keymap");
    return;
  }

  auto* state = xkb_state_new(keymap);
  if (state == nullptr) {
    xkb_keymap_unref(keymap);
    kLog.warn("keyboard: failed to create xkb state");
    return;
  }

  if (self->m_xkbState != nullptr) {
    xkb_state_unref(self->m_xkbState);
  }
  if (self->m_xkbKeymap != nullptr) {
    xkb_keymap_unref(self->m_xkbKeymap);
  }
  self->m_xkbKeymap = keymap;
  self->m_xkbState = state;

  // (Re)create compose state for dead key / intl layout support
  if (self->m_composeState != nullptr) {
    xkb_compose_state_unref(self->m_composeState);
    self->m_composeState = nullptr;
  }
  if (self->m_composeTable != nullptr) {
    xkb_compose_table_unref(self->m_composeTable);
    self->m_composeTable = nullptr;
  }
  self->m_composeTable =
      xkb_compose_table_new_from_locale(self->m_xkbContext, setlocale(LC_CTYPE, nullptr), XKB_COMPOSE_COMPILE_NO_FLAGS);
  if (self->m_composeTable != nullptr) {
    self->m_composeState = xkb_compose_state_new(self->m_composeTable, XKB_COMPOSE_STATE_NO_FLAGS);
  }

  kLog.info("keyboard: keymap loaded");
}

void WaylandSeat::handleKeyboardEnter(
    void* data, wl_keyboard* /*keyboard*/, std::uint32_t /*serial*/, wl_surface* surface, wl_array* /*keys*/
) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_repeatActive = false;
  self->m_lastKeyboardSurface = surface;
  if (self->m_keyboardFocusCallback) {
    self->m_keyboardFocusCallback(surface, true);
  }
}

void WaylandSeat::handleKeyboardLeave(
    void* data, wl_keyboard* /*keyboard*/, std::uint32_t /*serial*/, wl_surface* surface
) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_repeatActive = false;
  if (self->m_lastKeyboardSurface == surface) {
    self->m_lastKeyboardSurface = nullptr;
  }
  if (self->m_keyboardFocusCallback) {
    self->m_keyboardFocusCallback(surface, false);
  }
}

void WaylandSeat::handleKeyboardKey(
    void* data, wl_keyboard* /*keyboard*/, std::uint32_t serial, std::uint32_t /*time*/, std::uint32_t key,
    std::uint32_t state
) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_lastSerial = serial;
  self->m_lastInputSource = InputSource::Keyboard;
  if (self->m_xkbState == nullptr || !self->m_keyboardEventCallback) {
    return;
  }

  const std::uint32_t xkbKeycode = key + 8; // evdev → XKB
  auto sym = static_cast<std::uint32_t>(xkb_state_key_get_one_sym(self->m_xkbState, xkbKeycode));
  auto utf32 = static_cast<std::uint32_t>(xkb_state_key_get_utf32(self->m_xkbState, xkbKeycode));

  std::uint32_t mods = 0;
  if (xkb_state_mod_name_is_active(self->m_xkbState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
    mods |= KeyMod::Shift;
  if (xkb_state_mod_name_is_active(self->m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
    mods |= KeyMod::Ctrl;
  if (xkb_state_mod_name_is_active(self->m_xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
    mods |= KeyMod::Alt;
  if (xkb_state_mod_name_is_active(self->m_xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
    mods |= KeyMod::Super;

  const bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

  // Feed through compose state for dead key / intl layout support
  if (pressed && self->m_composeState != nullptr) {
    xkb_compose_state_feed(self->m_composeState, static_cast<xkb_keysym_t>(sym));
    const auto status = xkb_compose_state_get_status(self->m_composeState);
    if (status == XKB_COMPOSE_COMPOSED) {
      sym = static_cast<std::uint32_t>(xkb_compose_state_get_one_sym(self->m_composeState));
      utf32 = static_cast<std::uint32_t>(xkb_keysym_to_utf32(static_cast<xkb_keysym_t>(sym)));
      xkb_compose_state_reset(self->m_composeState);
    } else if (status == XKB_COMPOSE_COMPOSING) {
      // Send dead key visual as preedit preview.
      // xkb_keysym_to_utf32 returns 0 for dead key syms, so get the keysym name,
      // strip the "dead_" prefix, and look up the base (spacing) keysym instead.
      utf32 = static_cast<std::uint32_t>(xkb_keysym_to_utf32(static_cast<xkb_keysym_t>(sym)));
      if (utf32 == 0) {
        char symName[64];
        if (xkb_keysym_get_name(static_cast<xkb_keysym_t>(sym), symName, sizeof(symName)) > 0) {
          constexpr const char kDeadPrefix[] = "dead_";
          if (std::strncmp(symName, kDeadPrefix, 5) == 0) {
            const xkb_keysym_t baseSym = xkb_keysym_from_name(symName + 5, XKB_KEYSYM_NO_FLAGS);
            if (baseSym != XKB_KEY_NoSymbol) {
              utf32 = static_cast<std::uint32_t>(xkb_keysym_to_utf32(baseSym));
            }
          }
        }
      }
      if (utf32 != 0) {
        self->bumpUserActivity();
        self->m_keyboardEventCallback(
            KeyboardEvent{
                .sym = sym,
                .utf32 = utf32,
                .key = key,
                .modifiers = mods,
                .pressed = true,
                .preedit = true,
            }
        );
      }
      return;
    } else if (status == XKB_COMPOSE_CANCELLED) {
      xkb_compose_state_reset(self->m_composeState);
    }
    // XKB_COMPOSE_NOTHING → pass through normally
  }

  // Prefer Latin letter for Ctrl/Alt/Super shortcuts (active layout may be non-Latin).
  if ((mods & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0) {
    if (const auto latin = input::latinShortcutKeysym(self->m_xkbKeymap, xkbKeycode); latin.has_value()) {
      sym = *latin;
    }
  }

  // Set up repeat state BEFORE dispatching, so the callback can authoritatively
  // cancel it via stopKeyRepeat() if the key press causes a state transition
  // (e.g. lockscreen unlock) that makes the held key irrelevant.
  if (pressed && self->m_repeatRate > 0) {
    self->m_repeatKey = KeyboardEvent{.sym = sym, .utf32 = utf32, .key = key, .modifiers = mods, .pressed = true};
    self->m_repeatActive = true;
    self->m_repeatInDelay = true;
    self->m_repeatNextFire = SteadyClock::now() + std::chrono::milliseconds(self->m_repeatDelayMs);
  } else if (!pressed && self->m_repeatKey.key == key) {
    self->m_repeatActive = false;
  }

  self->bumpUserActivity();
  self->m_keyboardEventCallback(
      KeyboardEvent{
          .sym = sym,
          .utf32 = utf32,
          .key = key,
          .modifiers = mods,
          .pressed = pressed,
      }
  );
}

void WaylandSeat::handleKeyboardModifiers(
    void* data, wl_keyboard* /*keyboard*/, std::uint32_t /*serial*/, std::uint32_t modsDepressed,
    std::uint32_t modsLatched, std::uint32_t modsLocked, std::uint32_t group
) {
  auto* self = static_cast<WaylandSeat*>(data);
  if (self->m_xkbState != nullptr) {
    xkb_state_update_mask(self->m_xkbState, modsDepressed, modsLatched, modsLocked, 0, 0, group);
  }
  if (self->m_lockKeysChangeCallback) {
    const LockKeysState current = self->lockKeysState();
    if (current != self->m_lastLockKeysState) {
      self->m_lastLockKeysState = current;
      self->m_lockKeysChangeCallback();
    }
  }
}

void WaylandSeat::handleKeyboardRepeatInfo(
    void* data, wl_keyboard* /*keyboard*/, std::int32_t rate, std::int32_t delay
) {
  auto* self = static_cast<WaylandSeat*>(data);
  self->m_repeatRate = rate;
  self->m_repeatDelayMs = delay;
}

int WaylandSeat::repeatPollTimeoutMs() const {
  if (!m_repeatActive || m_repeatRate <= 0) {
    return -1;
  }
  const auto now = SteadyClock::now();
  if (now >= m_repeatNextFire) {
    return 0;
  }
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(m_repeatNextFire - now).count();
  return static_cast<int>(std::min<long long>(ms, std::numeric_limits<int>::max()));
}

void WaylandSeat::stopKeyRepeat() { m_repeatActive = false; }

void WaylandSeat::repeatTick() {
  if (!m_repeatActive || m_repeatRate <= 0 || !m_keyboardEventCallback) {
    return;
  }
  const auto now = SteadyClock::now();
  if (now < m_repeatNextFire) {
    return;
  }
  bumpUserActivity();
  m_keyboardEventCallback(m_repeatKey);
  m_repeatInDelay = false;
  const auto intervalMs = std::chrono::milliseconds(1000 / m_repeatRate);
  m_repeatNextFire = now + intervalMs;
}

std::string WaylandSeat::currentLayoutName() const {
  if (m_xkbState == nullptr || m_xkbKeymap == nullptr) {
    return {};
  }

  const xkb_layout_index_t layout = xkb_state_serialize_layout(m_xkbState, XKB_STATE_LAYOUT_EFFECTIVE);
  if (layout == XKB_LAYOUT_INVALID) {
    return {};
  }

  const xkb_layout_index_t layoutCount = xkb_keymap_num_layouts(m_xkbKeymap);
  if (layout >= layoutCount) {
    return {};
  }

  const char* name = xkb_keymap_layout_get_name(m_xkbKeymap, layout);
  if (name == nullptr) {
    return {};
  }

  return name;
}

std::vector<std::string> WaylandSeat::layoutNames() const {
  std::vector<std::string> layouts;
  if (m_xkbKeymap == nullptr) {
    return layouts;
  }

  const xkb_layout_index_t layoutCount = xkb_keymap_num_layouts(m_xkbKeymap);
  layouts.reserve(layoutCount);
  for (xkb_layout_index_t i = 0; i < layoutCount; ++i) {
    const char* name = xkb_keymap_layout_get_name(m_xkbKeymap, i);
    if (name != nullptr && name[0] != '\0') {
      layouts.emplace_back(name);
    }
  }

  return layouts;
}

WaylandSeat::LockKeysState WaylandSeat::lockKeysState() const {
  LockKeysState state{};
  if (m_xkbState == nullptr) {
    return state;
  }

  state.capsLock = xkb_state_led_name_is_active(m_xkbState, XKB_LED_NAME_CAPS) != 0;
  state.numLock = xkb_state_led_name_is_active(m_xkbState, XKB_LED_NAME_NUM) != 0;
  state.scrollLock = xkb_state_led_name_is_active(m_xkbState, XKB_LED_NAME_SCROLL) != 0;
  return state;
}
