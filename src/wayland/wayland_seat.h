#pragma once

#include "core/input/key_modifiers.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

struct wl_array;
struct wl_keyboard;
struct wl_pointer;
struct wl_seat;
struct wl_surface;
struct wl_touch;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
struct xkb_compose_state;
struct xkb_compose_table;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

struct PointerEvent {
  enum class Type : std::uint8_t { Enter, Leave, Motion, Button, Axis };
  Type type;
  std::uint32_t serial = 0;
  wl_surface* surface = nullptr;
  double sx = 0.0;
  double sy = 0.0;
  std::uint32_t time = 0;
  std::uint32_t button = 0;
  bool pressed = false;
  std::uint32_t axis = 0;
  std::uint32_t axisSource = 0;
  double axisValue = 0.0;
  std::int32_t axisDiscrete = 0;
  std::int32_t axisValue120 = 0;
  float axisLines = 0.0f;
};

struct KeyboardEvent {
  std::uint32_t sym = 0;       // XKB keysym
  std::uint32_t utf32 = 0;     // Unicode codepoint (0 for non-printable keys)
  std::uint32_t key = 0;       // raw Linux keycode
  std::uint32_t modifiers = 0; // KeyMod bitmask
  bool pressed = false;
  bool preedit = false; // dead key preview (composing in progress)
};

class WaylandSeat {
public:
  enum class InputSource : std::uint8_t {
    None,
    Pointer,
    Keyboard,
    Touch,
  };

  struct LockKeysState {
    bool capsLock = false;
    bool numLock = false;
    bool scrollLock = false;

    bool operator==(const LockKeysState&) const = default;
  };

  using PointerEventCallback = std::function<void(const PointerEvent&)>;
  using KeyboardEventCallback = std::function<void(const KeyboardEvent&)>;
  using KeyboardFocusCallback = std::function<void(wl_surface* surface, bool entered)>;
  using LockKeysChangeCallback = std::function<void()>;

  void bind(wl_seat* seat);
  void setCursorShapeManager(wp_cursor_shape_manager_v1* manager);
  void setPointerEventCallback(PointerEventCallback callback);
  void setKeyboardEventCallback(KeyboardEventCallback callback);
  void setKeyboardFocusCallback(KeyboardFocusCallback callback);
  void setLockKeysChangeCallback(LockKeysChangeCallback callback);
  void setCursorShape(std::uint32_t serial, std::uint32_t shape);
  void forgetSurface(wl_surface* surface) noexcept;
  void cleanup();

  [[nodiscard]] std::uint32_t lastSerial() const noexcept { return m_lastSerial; }
  [[nodiscard]] wl_seat* seat() const noexcept { return m_seat; }

  // Key repeat — driven by KeyRepeatPollSource
  [[nodiscard]] int repeatPollTimeoutMs() const;
  void repeatTick();
  void stopKeyRepeat();

  // Pointer listener entrypoints
  static void handleSeatCapabilities(void* data, wl_seat* seat, std::uint32_t caps);
  static void handleSeatName(void* data, wl_seat* seat, const char* name);
  static void handlePointerEnter(
      void* data, wl_pointer* pointer, std::uint32_t serial, wl_surface* surface, std::int32_t sx, std::int32_t sy
  );
  static void handlePointerLeave(void* data, wl_pointer* pointer, std::uint32_t serial, wl_surface* surface);
  static void
  handlePointerMotion(void* data, wl_pointer* pointer, std::uint32_t time, std::int32_t sx, std::int32_t sy);
  static void handlePointerButton(
      void* data, wl_pointer* pointer, std::uint32_t serial, std::uint32_t time, std::uint32_t button,
      std::uint32_t state
  );
  static void
  handlePointerAxis(void* data, wl_pointer* pointer, std::uint32_t time, std::uint32_t axis, std::int32_t value);
  static void handlePointerAxisSource(void* data, wl_pointer* pointer, std::uint32_t axisSource);
  static void handlePointerAxisDiscrete(void* data, wl_pointer* pointer, std::uint32_t axis, std::int32_t discrete);
  static void handlePointerAxisValue120(void* data, wl_pointer* pointer, std::uint32_t axis, std::int32_t value120);
  static void handlePointerFrame(void* data, wl_pointer* pointer);

  // Touch listener entrypoints
  static void handleTouchDown(
      void* data, wl_touch* touch, std::uint32_t serial, std::uint32_t time, wl_surface* surface, std::int32_t id,
      std::int32_t x, std::int32_t y
  );
  static void handleTouchUp(void* data, wl_touch* touch, std::uint32_t serial, std::uint32_t time, std::int32_t id);
  static void
  handleTouchMotion(void* data, wl_touch* touch, std::uint32_t time, std::int32_t id, std::int32_t x, std::int32_t y);
  static void handleTouchFrame(void* data, wl_touch* touch);
  static void handleTouchCancel(void* data, wl_touch* touch);

  // Keyboard listener entrypoints
  static void handleKeyboardKeymap(void* data, wl_keyboard* keyboard, std::uint32_t format, int fd, std::uint32_t size);
  static void
  handleKeyboardEnter(void* data, wl_keyboard* keyboard, std::uint32_t serial, wl_surface* surface, wl_array* keys);
  static void handleKeyboardLeave(void* data, wl_keyboard* keyboard, std::uint32_t serial, wl_surface* surface);
  static void handleKeyboardKey(
      void* data, wl_keyboard* keyboard, std::uint32_t serial, std::uint32_t time, std::uint32_t key,
      std::uint32_t state
  );
  static void handleKeyboardModifiers(
      void* data, wl_keyboard* keyboard, std::uint32_t serial, std::uint32_t modsDepressed, std::uint32_t modsLatched,
      std::uint32_t modsLocked, std::uint32_t group
  );
  static void handleKeyboardRepeatInfo(void* data, wl_keyboard* keyboard, std::int32_t rate, std::int32_t delay);

  [[nodiscard]] wl_surface* lastPointerSurface() const noexcept { return m_lastPointerSurface; }
  [[nodiscard]] wl_surface* lastKeyboardSurface() const noexcept { return m_lastKeyboardSurface; }
  [[nodiscard]] bool hasPointerPosition() const noexcept { return m_hasPointerPosition; }
  [[nodiscard]] double lastPointerX() const noexcept { return m_lastPointerX; }
  [[nodiscard]] double lastPointerY() const noexcept { return m_lastPointerY; }
  [[nodiscard]] std::string currentLayoutName() const;
  [[nodiscard]] std::vector<std::string> layoutNames() const;
  [[nodiscard]] LockKeysState lockKeysState() const;
  [[nodiscard]] InputSource lastInputSource() const noexcept { return m_lastInputSource; }

  [[nodiscard]] double userIdleSeconds() const noexcept;

private:
  void bumpUserActivity() noexcept;

  using SteadyClock = std::chrono::steady_clock;

  // Pointer
  wl_pointer* m_pointer = nullptr;
  wp_cursor_shape_manager_v1* m_cursorShapeManager = nullptr;
  wp_cursor_shape_device_v1* m_cursorShapeDevice = nullptr;
  PointerEventCallback m_pointerEventCallback;
  std::vector<PointerEvent> m_pendingPointerEvents;
  std::uint32_t m_pendingAxisSource = 0;
  // Detent info for an axis, received ahead of the axis event it belongs to.
  struct AxisDetent {
    bool valid = false;
    std::int32_t discrete = 0;
    std::int32_t value120 = 0;
    float lines = 0.0f;
  };
  // Indexed by wl_pointer axis (vertical, horizontal).
  std::array<AxisDetent, 2> m_pendingAxisDetents{};
  wl_surface* m_lastPointerSurface = nullptr;
  std::uint32_t m_pointerEnterSerial = 0;
  double m_lastPointerX = 0.0;
  double m_lastPointerY = 0.0;
  bool m_hasPointerPosition = false;
  wl_surface* m_lastKeyboardSurface = nullptr;

  // Touch
  wl_touch* m_touch = nullptr;
  std::int32_t m_activeTouchId = -1;
  wl_surface* m_touchSurface = nullptr;
  std::vector<PointerEvent> m_pendingTouchEvents;

  wl_seat* m_seat = nullptr;
  std::uint32_t m_lastSerial = 0;
  InputSource m_lastInputSource = InputSource::None;

  // Keyboard
  wl_keyboard* m_keyboard = nullptr;
  xkb_context* m_xkbContext = nullptr;
  xkb_keymap* m_xkbKeymap = nullptr;
  xkb_state* m_xkbState = nullptr;
  xkb_compose_table* m_composeTable = nullptr;
  xkb_compose_state* m_composeState = nullptr;
  KeyboardEventCallback m_keyboardEventCallback;
  KeyboardFocusCallback m_keyboardFocusCallback;
  LockKeysChangeCallback m_lockKeysChangeCallback;
  LockKeysState m_lastLockKeysState;

  // Key repeat
  SteadyClock::time_point m_lastUserActivitySteady;
  std::int32_t m_repeatRate = 0;    // chars/sec; 0 = no repeat
  std::int32_t m_repeatDelayMs = 0; // initial delay in ms
  KeyboardEvent m_repeatKey;
  bool m_repeatActive = false;
  bool m_repeatInDelay = false;
  SteadyClock::time_point m_repeatNextFire;
};
