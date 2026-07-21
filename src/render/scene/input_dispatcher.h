#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class InputArea;
class Node;
class TextInputService;
struct wl_surface;

class InputDispatcher {
public:
  using CursorShapeCallback = std::function<void(std::uint32_t serial, std::uint32_t shape)>;
  using HoverChangeCallback = std::function<void(InputArea* oldArea, InputArea* newArea)>;
  using FocusChangeCallback = std::function<void(InputArea* oldArea, InputArea* newArea)>;

  InputDispatcher() = default;

  void setSceneRoot(Node* root);
  void setCursorShapeCallback(CursorShapeCallback callback);
  void setHoverChangeCallback(HoverChangeCallback callback);
  void setFocusChangeCallback(FocusChangeCallback callback);
  void setTextInputContext(
      wl_surface* surface, TextInputService* service, bool keyboardFocusActivation = false,
      wl_surface* keyboardFocusParentSurface = nullptr
  );

  // Dispatch Wayland pointer events into the scene graph
  void pointerEnter(float x, float y, std::uint32_t serial);
  void pointerLeave();
  void pointerMotion(float x, float y, std::uint32_t serial);
  void syncPointerHover();
  // Returns true if the event was consumed by a scene widget
  bool pointerButton(float x, float y, std::uint32_t button, bool pressed);
  bool pointerAxis(
      float x, float y, std::uint32_t axis, std::uint32_t axisSource, double value, std::int32_t discrete,
      std::int32_t value120, float lines
  );
  void cancelPointerCapture();

  // Dispatch keyboard events to the focused area
  void keyEvent(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit = false);

  // Focus management
  void setFocus(InputArea* area);
  void stashTabFocus();
  void restoreStashedTabFocus();
  [[nodiscard]] bool cycleTabFocus(bool reverse);
  [[nodiscard]] bool cycleTabFocusInSubtree(Node* subtree, bool reverse);
  [[nodiscard]] InputArea* inputAreaAt(float x, float y);
  [[nodiscard]] InputArea* firstTabFocusUnder(Node* subtree) const;
  [[nodiscard]] InputArea* lastTabFocusUnder(Node* subtree) const;
  [[nodiscard]] InputArea* focusedArea() const noexcept { return m_focusedArea; }
  [[nodiscard]] InputArea* hoveredArea() const noexcept { return m_hoveredArea; }
  [[nodiscard]] bool pointerCaptured() const noexcept { return m_capturedArea != nullptr; }

private:
  InputArea* findInputAreaAt(float x, float y);
  [[nodiscard]] bool isAttachedToScene(const InputArea* area) const;
  void pruneDetachedAreas();
  void updateHover(float x, float y, std::uint32_t serial);
  void updateCursor(std::uint32_t serial);
  void trackArea(InputArea* area);
  void clearTextInputFocus(InputArea* area);
  void syncTextInputFocus();

  Node* m_sceneRoot = nullptr;
  CursorShapeCallback m_cursorShapeCallback;
  HoverChangeCallback m_hoverChangeCallback;
  FocusChangeCallback m_focusChangeCallback;
  wl_surface* m_textInputSurface = nullptr;
  TextInputService* m_textInputService = nullptr;
  bool m_textInputKeyboardFocusActivation = false;
  wl_surface* m_textInputKeyboardFocusParentSurface = nullptr;
  InputArea* m_hoveredArea = nullptr;
  InputArea* m_focusedArea = nullptr;
  InputArea* m_capturedArea = nullptr; // held while any button is pressed
  std::optional<std::size_t> m_stashedTabFocusIndex;
  std::optional<std::string> m_stashedTabFocusKey;
  std::uint32_t m_lastSerial = 0;
  float m_lastPointerX = 0.0f;
  float m_lastPointerY = 0.0f;
  bool m_hasPointerPosition = false;
  bool m_cancelingPointerCapture = false;
};
