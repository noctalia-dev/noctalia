#pragma once

#include "render/scene/node.h"

#include <functional>
#include <string>
#include <vector>

class InputArea;

namespace settings::display {

  class DisplayService;

  // Preview of the logical layout: one rounded box per enabled output, drag to
  // reposition with edge/center snapping, click to select. Reads the current
  // service state; the owning modal rebuilds it on every applied change.
  class MonitorLayoutCanvas : public Node {
  public:
    MonitorLayoutCanvas(
        DisplayService& display, std::string selectedOutput, std::function<void(std::string)> onSelect, float scale
    );

  private:
    struct OutputRect {
      std::string name;
      float x = 0.0F;
      float y = 0.0F;
      float width = 0.0F;
      float height = 0.0F;
    };

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;
    void computeSnap(
        const std::string& draggedName, float newX, float newY, float previewW, float previewH, float& snappedX,
        float& snappedY, float& guideX, float& guideY
    ) const;
    void commitDrag();

    DisplayService& m_display;
    std::string m_selectedOutput;
    std::function<void(std::string)> m_onSelect;
    float m_scale = 1.0F;

    std::vector<OutputRect> m_rects;
    std::vector<OutputRect> m_previewRects;
    std::vector<std::pair<std::string, Node*>> m_boxes;
    float m_canvasWidth = 0.0F;
    float m_canvasHeight = 0.0F;
    Node* m_guideX = nullptr;
    Node* m_guideY = nullptr;
    InputArea* m_input = nullptr;

    bool m_dragging = false;
    std::string m_dragName;
    float m_dragStartLocalX = 0.0F;
    float m_dragStartLocalY = 0.0F;
    float m_dragStartRectX = 0.0F;
    float m_dragStartRectY = 0.0F;
    float m_dragStartPressX = 0.0F;
    float m_dragStartPressY = 0.0F;
  };

} // namespace settings::display
