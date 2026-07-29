#pragma once

#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/controls/color_swatch_preview.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Renderer;

enum class ContextSubmenuDirection : std::uint8_t {
  Right,
  Left,
};

struct ContextMenuControlEntry {
  std::int32_t id = 0;
  std::string label;
  bool enabled = true;
  bool separator = false;
  // Non-interactive group heading; entries below it read as members of the group.
  bool header = false;
  bool hasSubmenu = false;
  bool checkmark = false;
  bool radio = false;
  std::int32_t toggleState = -1;
  // Optional leading visual (dropdown-style rows): a small color dot, or a palette swatch strip.
  std::optional<ColorSpec> indicatorColor;
  ColorSwatchPreview swatchPreview;
  TextEllipsize ellipsize = TextEllipsize::End;
};

class ContextMenuControl : public Node {
public:
  ContextMenuControl();

  void setEntries(std::vector<ContextMenuControlEntry> entries);
  void setMaxVisible(std::size_t maxVisible);
  void setMenuWidth(float width);
  void setContentScale(float scale);
  void setSubmenuDirection(ContextSubmenuDirection direction);
  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> onActivate);
  void setOnSubmenuOpen(std::function<void(const ContextMenuControlEntry&, float rowCenterY)> onSubmenuOpen);
  void setRedrawCallback(std::function<void()> redrawCallback);

  void setHighlightedIndex(std::size_t index);
  [[nodiscard]] bool moveHighlight(int delta);
  [[nodiscard]] bool activateHighlighted();
  [[nodiscard]] std::size_t highlightedIndex() const noexcept { return m_highlightedIndex; }
  [[nodiscard]] std::size_t entryCount() const noexcept { return m_entries.size(); }
  [[nodiscard]] float rowTop(std::size_t index) const noexcept;
  [[nodiscard]] float rowBottom(std::size_t index) const noexcept;

  [[nodiscard]] float preferredHeight() const;
  [[nodiscard]] static float
  preferredHeight(const std::vector<ContextMenuControlEntry>& entries, std::size_t maxVisible, float scale = 1.0f);
  // Width that fits the widest entry without elision (label + toggle/submenu slots + padding).
  [[nodiscard]] static float
  preferredWidth(Renderer& renderer, const std::vector<ContextMenuControlEntry>& entries, float scale = 1.0f);

private:
  struct RowVisual {
    std::function<void(bool)> apply;
    float y = 0.0f;
    float height = 0.0f;
    bool interactive = false;
  };

  void doLayout(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void rebuildRows(Renderer& renderer);
  void applyHighlightVisuals();
  [[nodiscard]] std::size_t firstInteractiveIndex() const noexcept;

  std::vector<ContextMenuControlEntry> m_entries;
  std::vector<RowVisual> m_rows;
  std::size_t m_maxVisible = 14;
  std::size_t m_highlightedIndex = 0;
  float m_menuWidth = 246.0f;
  float m_contentScale = 1.0f;
  ContextSubmenuDirection m_submenuDirection = ContextSubmenuDirection::Right;
  bool m_needsRebuild = true;
  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void(const ContextMenuControlEntry&, float rowCenterY)> m_onSubmenuOpen;
  std::function<void()> m_redrawCallback;
};
