#pragma once

#include "ui/controls/flex.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class DragDropController;

class DropZone : public Flex {
public:
  explicit DropZone(DragDropController* controller);
  ~DropZone() override;

  DropZone(const DropZone&) = delete;
  DropZone& operator=(const DropZone&) = delete;

  void setAccepts(std::vector<std::string> values);
  void setValue(std::string value);
  void setOnDrop(std::string value);
  void setEnabled(bool enabled);
  // While targeted, an expandOnDrag zone grows along its parent flex's main
  // axis: width inside a row, height inside a column.
  void setDragOver(bool dragOver, float draggedWidth = 0.0F, float draggedHeight = 0.0F);
  void setExpandOnDrag(bool enabled);
  void setCollapsedWidth(float width);
  void setCollapsedHeight(float height);
  void setHitSlop(float hitSlop);
  void setZoneRadius(float radius);
  void clearZoneRadius(float dragRadius);

  void setZoneFill(const ColorSpec& color);
  void clearZoneFill();
  void setZoneBorder(const ColorSpec& color, float width);
  void clearZoneBorder();

  [[nodiscard]] bool accepts(std::string_view dragType) const;
  [[nodiscard]] std::string_view value() const noexcept { return m_value; }
  [[nodiscard]] std::string_view onDrop() const noexcept { return m_onDrop; }
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] bool dragOver() const noexcept { return m_dragOver; }
  [[nodiscard]] float hitSlop() const noexcept { return m_hitSlop; }
  [[nodiscard]] DragDropController* controller() const noexcept { return m_controller; }
  void detachController(DragDropController* controller) noexcept;

private:
  void applyVisualState();
  [[nodiscard]] bool expandsAlongWidth() const;
  void animateWidth(float target);
  void applyAnimatedWidth(float width);
  void animateHeight(float target);
  void applyAnimatedHeight(float height);

  DragDropController* m_controller = nullptr;
  std::vector<std::string> m_accepts;
  std::string m_value;
  std::string m_onDrop;
  ColorSpec m_zoneFill = clearColorSpec();
  ColorSpec m_zoneBorder = clearColorSpec();
  float m_zoneBorderWidth = 0.0F;
  float m_collapsedWidth = 0.0F;
  float m_animatedWidth = 0.0F;
  float m_collapsedHeight = 0.0F;
  float m_animatedHeight = 0.0F;
  float m_hitSlop = 0.0F;
  float m_zoneRadius = 0.0F;
  float m_idleRadius = 0.0F;
  std::uint32_t m_widthAnimation = 0;
  std::uint32_t m_heightAnimation = 0;
  bool m_hasZoneFill = false;
  bool m_hasZoneBorder = false;
  bool m_enabled = true;
  bool m_dragOver = false;
  bool m_expandOnDrag = false;
  bool m_radiusApplied = false;
};
