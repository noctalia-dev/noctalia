#pragma once

#include "render/scene/node.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <cstdint>
#include <functional>

class InputArea;
class RectNode;

enum class ScrollOrientation {
  Vertical,
  Horizontal,
};

class Scrollbar : public Node {
public:
  Scrollbar();

  void setOrientation(ScrollOrientation orientation);

  void setOnScrollChanged(std::function<void(float)> callback);

  // Layout scale of the surface hosting this bar (1.0 = base logical px).
  void setContentScale(float scale);

  // Insets the track from both viewport ends (e.g. rounded-corner clearance in popup cards).
  // Scroll semantics keep using the full viewport height; only track geometry shrinks.
  void setTrackInset(float inset);

  void update(float viewportExtent, float contentExtent, float scrollOffset);

  [[nodiscard]] float thumbTravel() const noexcept { return m_thumbTravel; }
  [[nodiscard]] bool visible() const noexcept { return m_shown; }

  [[nodiscard]] ScrollOrientation orientation() const noexcept { return m_orientation; }

  // Thickness the host reserves for the bar. The hover expansion overlays the content instead
  // of widening this, so the reserved gutter never changes while the pointer moves.
  [[nodiscard]] float reservedThickness() const noexcept { return Style::scrollbarWidth * m_contentScale; }

private:
  [[nodiscard]] float currentOffset() const noexcept;
  [[nodiscard]] float thickness() const noexcept;
  void applyPalette();
  void applyThumbPosition(float scrollOffset, float maxScroll);
  void applyGeometry();
  void updateExpanded();
  void setExpanded(bool expanded);
  void applyExpansion(float expansion);

  RectNode* m_track = nullptr;
  RectNode* m_thumb = nullptr;
  InputArea* m_trackArea = nullptr;
  InputArea* m_thumbArea = nullptr;

  Signal<>::ScopedConnection m_paletteConn;
  std::function<void(float)> m_onScrollChanged;

  float m_contentScale = 1.0F;
  float m_expansion = 0.0F;
  std::uint32_t m_expandAnim = 0;
  // Cross-axis offset of the bar's parts: negative while the hover expansion overlays content.
  float m_crossOffset = 0.0F;
  float m_viewportExtent = 0.0F;
  float m_contentExtent = 0.0F;
  float m_trackInset = 0.0F;
  float m_maxScroll = 0.0F;
  float m_thumbTravel = 0.0F;
  float m_dragStartPosition = 0.0F;
  float m_dragStartOffset = 0.0F;
  float m_lastScrollOffset = 0.0F;
  bool m_shown = false;
  ScrollOrientation m_orientation = ScrollOrientation::Vertical;
};
