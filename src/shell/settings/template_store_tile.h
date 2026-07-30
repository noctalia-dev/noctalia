#pragma once

#include "ui/controls/grid_tile.h"

#include <functional>
#include <string>
#include <string_view>

class Checkbox;
class Label;

namespace settings {

  class TemplateStoreTile : public GridTile {
  public:
    explicit TemplateStoreTile(float scale);

    void bind(
        std::string_view name, std::string_view category, bool enabled, bool selected, bool hovered,
        std::function<void(bool enabled)> onToggle
    );

  private:
    float m_scale;
    Checkbox* m_checkbox = nullptr;
    Label* m_nameLabel = nullptr;
    Label* m_categoryLabel = nullptr;
  };

} // namespace settings
