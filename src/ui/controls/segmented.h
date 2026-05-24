#pragma once

#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

class Button;
class Separator;

class Segmented : public Flex {
public:
  Segmented();

  std::size_t addOption(std::string_view label);
  std::size_t addOption(std::string_view label, std::string_view glyph);

  void setSelectedIndex(std::size_t index);
  [[nodiscard]] std::size_t selectedIndex() const noexcept { return m_selected; }

  void setFontSize(float size);
  void setScale(float scale);

  void setOnChange(std::function<void(std::size_t)> callback);

  void setOptionTooltip(std::size_t index, std::string_view text);
  void setCompact(bool compact);
  void clearOptions();

  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  // When true, each segment gets flexGrow 1 so the group fills the available width (e.g. full bar).
  void setEqualSegmentWidths(bool equalWidths);

private:
  [[nodiscard]] std::unique_ptr<Separator> makeSegmentSeparator();
  [[nodiscard]] std::unique_ptr<Button>
  makeSegmentButton(std::string_view label, std::string_view glyph, std::size_t index);
  void applyButtonMetrics(Button& button) const;
  void refreshVariants();
  void applyOuterStyle();
  [[nodiscard]] float effectiveFontSize() const noexcept;

  std::vector<Separator*> m_separators;
  std::vector<Button*> m_buttons;
  std::size_t m_selected = 0;
  std::function<void(std::size_t)> m_onChange;
  float m_fontSize = 0.0f;
  float m_scale = 1.0f;
  bool m_equalSegmentWidths = false;
  bool m_compact = false;
  bool m_enabled = true;
};
