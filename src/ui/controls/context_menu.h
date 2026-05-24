#pragma once

#include "render/scene/node.h"

#include <cstdint>
#include <functional>
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
  bool hasSubmenu = false;
  bool checkmark = false;
  bool radio = false;
  std::int32_t toggleState = -1;
};

class ContextMenuControl : public Node {
public:
  ContextMenuControl();

  void setEntries(std::vector<ContextMenuControlEntry> entries);
  void setMaxVisible(std::size_t maxVisible);
  void setMenuWidth(float width);
  void setSubmenuDirection(ContextSubmenuDirection direction);
  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> onActivate);
  void setOnSubmenuOpen(std::function<void(const ContextMenuControlEntry&, float rowCenterY)> onSubmenuOpen);
  void setRedrawCallback(std::function<void()> redrawCallback);

  [[nodiscard]] float preferredHeight() const;
  [[nodiscard]] static float
  preferredHeight(const std::vector<ContextMenuControlEntry>& entries, std::size_t maxVisible);

private:
  void doLayout(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void rebuildRows(Renderer& renderer);

  std::vector<ContextMenuControlEntry> m_entries;
  std::size_t m_maxVisible = 14;
  float m_menuWidth = 246.0f;
  ContextSubmenuDirection m_submenuDirection = ContextSubmenuDirection::Right;
  bool m_needsRebuild = true;
  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void(const ContextMenuControlEntry&, float rowCenterY)> m_onSubmenuOpen;
  std::function<void()> m_redrawCallback;
};
