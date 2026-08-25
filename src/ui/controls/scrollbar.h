#pragma once

#include "render/scene/node.h"
#include "ui/signal.h"

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

  // Insets the track from both viewport ends (e.g. rounded-corner clearance in popup cards).
  // Scroll semantics keep using the full viewport height; only track geometry shrinks.
  void setTrackInset(float inset);

  void update(float viewportExtent, float contentExtent, float scrollOffset);

  [[nodiscard]] float thumbTravel() const noexcept { return m_thumbTravel; }
  [[nodiscard]] bool visible() const noexcept { return m_shown; }

  [[nodiscard]] ScrollOrientation orientation() const noexcept { return m_orientation; }

private:
  [[nodiscard]] float currentOffset() const noexcept;
  void applyPalette();
  void applyThumbPosition(float scrollOffset, float maxScroll);

  RectNode* m_track = nullptr;
  RectNode* m_thumb = nullptr;
  InputArea* m_trackArea = nullptr;
  InputArea* m_thumbArea = nullptr;

  Signal<>::ScopedConnection m_paletteConn;
  std::function<void(float)> m_onScrollChanged;

  float m_viewportExtent = 0.0F;
  float m_contentExtent = 0.0F;
  float m_trackInset = 0.0F;
  float m_maxScroll = 0.0F;
  float m_thumbTravel = 0.0F;
  float m_dragStartPosition = 0.0F;
  float m_dragStartOffset = 0.0F;
  bool m_shown = false;
  ScrollOrientation m_orientation = ScrollOrientation::Vertical;
};
