#pragma once

#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

class DragDropController;
class InputArea;

class DragSource : public Flex {
public:
  explicit DragSource(DragDropController* controller);
  ~DragSource() override;

  DragSource(const DragSource&) = delete;
  DragSource& operator=(const DragSource&) = delete;

  void setDragType(std::string value);
  void setPayload(std::string value);
  void setEnabled(bool enabled);
  void setTooltip(std::string_view text);
  void setSourceOpacity(float opacity);
  void setPreviewAncestor(std::size_t levels);
  void setLiftFromLayout(bool enabled);
  // Hover cursor: the grab hand (a grip's affordance) or the plain pointer (a
  // whole item that is also clickable). Dragging always shows the grabbing hand.
  void setGrabCursor(bool enabled);
  // Tap handler: a press released before the drag threshold. Lets a source
  // that wraps a whole item stay clickable (its input area covers the item).
  void setOnClick(std::function<void()> handler);
  void fireClick();
  void setDragging(bool dragging);
  void setSize(float width, float height) override;

  std::string_view dragType() const noexcept { return m_dragType; }
  std::string_view payload() const noexcept { return m_payload; }
  bool enabled() const noexcept { return m_enabled; }
  bool dragging() const noexcept { return m_dragging; }
  bool liftFromLayout() const noexcept { return m_liftFromLayout; }
  InputArea* inputArea() const noexcept { return m_inputArea; }
  DragDropController* controller() const noexcept { return m_controller; }
  Node* previewTarget() noexcept;
  void detachController(DragDropController* controller) noexcept;

protected:
  void doLayout(Renderer& renderer) override;

private:
  void applyVisualState();
  void applyTooltip();
  void applyCursor();
  void updateInputArea();

  DragDropController* m_controller = nullptr;
  InputArea* m_inputArea = nullptr;
  std::string m_dragType;
  std::string m_payload;
  std::string m_tooltip;
  std::function<void()> m_onClick;
  float m_sourceOpacity = 1.0F;
  std::size_t m_previewAncestor = 0;
  bool m_enabled = true;
  bool m_dragging = false;
  bool m_grabCursor = true;
  bool m_liftFromLayout = false;
};
