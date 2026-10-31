#pragma once

#include "ui/controls/grid_tile.h"

#include <functional>
#include <string>
#include <string_view>

class Button;
class Checkbox;
class Label;
struct TooltipAnchorInsets;

namespace settings {

  class TemplateStoreTile : public GridTile {
  public:
    explicit TemplateStoreTile(float scale);

    void bind(
        std::string_view name, std::string_view category, bool enabled, bool selected, bool hovered, bool hasReadme,
        std::function<void(bool enabled)> onToggle
    );
    void setReadmeHovered(bool hovered);
    [[nodiscard]] static bool hitTestReadme(float cellWidth, float cellHeight, float scale, float localX, float localY);
    [[nodiscard]] static TooltipAnchorInsets readmeTooltipAnchorInsets(float cellWidth, float cellHeight, float scale);

  private:
    float m_scale;
    Checkbox* m_checkbox = nullptr;
    Label* m_nameLabel = nullptr;
    Label* m_categoryLabel = nullptr;
    Button* m_readmeButton = nullptr;
  };

} // namespace settings
