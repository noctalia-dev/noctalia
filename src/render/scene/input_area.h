#pragma once

#include "render/scene/node.h"
#include "shell/tooltip/tooltip_content.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <linux/input-event-codes.h>

class InputArea : public Node {
public:
  struct PointerData {
    float localX = 0.0f;
    float localY = 0.0f;
    std::uint32_t button = 0;
    std::uint32_t axis = 0;
    std::uint32_t axisSource = 0;
    bool pressed = false;
    double axisValue = 0.0;
    std::int32_t axisDiscrete = 0;
    std::int32_t axisValue120 = 0;
    float axisLines = 0.0f;

    [[nodiscard]] float scrollDelta(float wheelStep) const noexcept {
      if (axisLines != 0.0f) {
        return axisLines * wheelStep;
      }
      return static_cast<float>(axisValue);
    }
  };

  struct KeyData {
    std::uint32_t sym = 0;       // XKB keysym
    std::uint32_t utf32 = 0;     // Unicode codepoint (0 for non-printable keys)
    std::uint32_t modifiers = 0; // KeyMod bitmask
    bool pressed = false;
    bool preedit = false; // dead key preview (composing in progress)
  };

  using PointerCallback = std::function<void(const PointerData&)>;
  using AxisCallback = std::function<bool(const PointerData&)>;
  using KeyCallback = std::function<void(const KeyData&)>;
  using VoidCallback = std::function<void()>;
  using DestroyCallback = std::function<void(InputArea*)>;

  InputArea();
  ~InputArea() override;

  [[nodiscard]] static std::uint32_t buttonMask(std::uint32_t button) noexcept;
  [[nodiscard]] static std::uint32_t buttonMask(std::initializer_list<std::uint32_t> buttons) noexcept;

  // InputArea is a transparent hit-test wrapper with no layout semantics of its
  // own; its internal layout hook forwards to visible children so callers can
  // use it as a clickable container without manually re-laying children.

  // Pointer callback setters
  void setOnEnter(PointerCallback callback);
  void setOnLeave(VoidCallback callback);
  void setOnMotion(PointerCallback callback);
  void setOnPress(PointerCallback callback);
  void setOnClick(PointerCallback callback);
  void setOnAxis(PointerCallback callback);
  void setOnAxisHandler(AxisCallback callback);

  // Keyboard / focus
  void setFocusable(bool focusable);
  [[nodiscard]] bool focusable() const noexcept { return m_focusable; }
  [[nodiscard]] bool focused() const noexcept { return m_focused; }
  void setOnKeyDown(KeyCallback callback);
  void setOnKeyUp(KeyCallback callback);
  void setOnFocusGain(VoidCallback callback);
  void setOnFocusLoss(VoidCallback callback);

  // Configuration
  void setCursorShape(std::uint32_t shape);
  [[nodiscard]] std::uint32_t cursorShape() const noexcept { return m_cursorShape; }

  void setAcceptedButtons(std::uint32_t mask);
  [[nodiscard]] std::uint32_t acceptedButtons() const noexcept { return m_acceptedButtons; }
  [[nodiscard]] bool acceptsButton(std::uint32_t button) const noexcept;

  void setPropagateEvents(bool propagate);
  [[nodiscard]] bool propagateEvents() const noexcept { return m_propagateEvents; }

  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  // Tooltip
  void setTooltip(std::string text);
  void setTooltip(std::vector<TooltipRow> rows);
  void clearTooltip();
  [[nodiscard]] bool hasTooltip() const noexcept;
  [[nodiscard]] const TooltipContent& tooltipContent() const noexcept { return m_tooltipContent; }

  // Auto-tracked state (read-only)
  [[nodiscard]] bool hovered() const noexcept { return m_hovered; }
  [[nodiscard]] bool pressed() const noexcept { return m_pressed; }

  // Called by InputDispatcher to get notified when this area is destroyed
  void setDestroyCallback(DestroyCallback callback);

  // Dispatch methods (called by InputDispatcher)
  void dispatchEnter(float localX, float localY);
  void dispatchLeave();
  void dispatchMotion(float localX, float localY);
  void dispatchPress(float localX, float localY, std::uint32_t button, bool isPressed);
  [[nodiscard]] bool dispatchAxis(
      float localX, float localY, std::uint32_t axis, std::uint32_t axisSource, double axisValue,
      std::int32_t axisDiscrete, std::int32_t axisValue120, float axisLines
  );
  void dispatchKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit = false);
  void dispatchFocusGain();
  void dispatchFocusLoss();

protected:
private:
  DestroyCallback m_destroyCallback;
  PointerCallback m_onEnter;
  VoidCallback m_onLeave;
  PointerCallback m_onMotion;
  PointerCallback m_onPress;
  PointerCallback m_onClick;
  AxisCallback m_onAxis;
  KeyCallback m_onKeyDown;
  KeyCallback m_onKeyUp;
  VoidCallback m_onFocusGain;
  VoidCallback m_onFocusLoss;

  std::uint32_t m_cursorShape = 0;
  std::uint32_t m_acceptedButtons = buttonMask(BTN_LEFT);
  bool m_propagateEvents = false;
  bool m_enabled = true;
  bool m_hovered = false;
  bool m_pressed = false;
  std::uint32_t m_pressedButton = 0;
  bool m_focusable = false;
  bool m_focused = false;

  TooltipContent m_tooltipContent;
};
