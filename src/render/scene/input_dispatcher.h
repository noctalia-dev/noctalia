#pragma once

#include <cstdint>
#include <functional>

class InputArea;
class Node;

class InputDispatcher {
public:
  using CursorShapeCallback = std::function<void(std::uint32_t serial, std::uint32_t shape)>;
  using HoverChangeCallback = std::function<void(InputArea* oldArea, InputArea* newArea)>;

  InputDispatcher() = default;

  void setSceneRoot(Node* root);
  void setCursorShapeCallback(CursorShapeCallback callback);
  void setHoverChangeCallback(HoverChangeCallback callback);

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

  // Dispatch keyboard events to the focused area
  void keyEvent(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit = false);

  // Focus management
  void setFocus(InputArea* area);
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

  Node* m_sceneRoot = nullptr;
  CursorShapeCallback m_cursorShapeCallback;
  HoverChangeCallback m_hoverChangeCallback;
  InputArea* m_hoveredArea = nullptr;
  InputArea* m_focusedArea = nullptr;
  InputArea* m_capturedArea = nullptr; // held while any button is pressed
  std::uint32_t m_lastSerial = 0;
  float m_lastPointerX = 0.0f;
  float m_lastPointerY = 0.0f;
  bool m_hasPointerPosition = false;
};
