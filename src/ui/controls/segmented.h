#pragma once

#include "ui/controls/flex.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/palette.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class Button;
class InputArea;
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

  void setPadding(float padding);
  void setPadding(float vertical, float horizontal);
  void setPadding(float top, float right, float bottom, float left);

  void setEnabled(bool enabled);
  void setSurfaceOpacity(float opacity);
  void setSurfaceRole(ColorRole role);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  // When true, each segment gets flexGrow 1 so the group fills the available width (e.g. full bar).
  void setEqualSegmentWidths(bool equalWidths);

  [[nodiscard]] InputArea* focusArea() const noexcept { return m_focusArea; }

  void doLayout(Renderer& renderer) override;

private:
  [[nodiscard]] std::unique_ptr<Separator> makeSegmentSeparator();
  [[nodiscard]] std::unique_ptr<Button>
  makeSegmentButton(std::string_view label, std::string_view glyph, std::size_t index);
  void applyButtonMetrics(Button& button) const;
  void refreshVariants();
  void applyOuterStyle();
  [[nodiscard]] float effectiveFontSize() const noexcept;

  RovingListNavController m_rovingNav;
  std::vector<Separator*> m_separators;
  std::vector<Button*> m_buttons;
  InputArea* m_focusArea = nullptr;
  std::size_t m_selected = 0;
  std::function<void(std::size_t)> m_onChange;
  float m_fontSize = 0.0f;
  float m_scale = 1.0f;
  bool m_equalSegmentWidths = false;
  bool m_compact = false;
  float m_outerPadding = 0.0f;
  float m_surfaceOpacity = 1.0f;
  ColorRole m_surfaceRole = ColorRole::SurfaceVariant;
  bool m_enabled = true;
};
