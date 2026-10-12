#include "ui/controls/scrollbar.h"

#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <memory>
#include <wayland-client-protocol.h>

namespace {

  RoundedRectStyle makeSolid(const Color& fill, float radius) {
    return RoundedRectStyle{
        .fill = fill,
        .border = fill,
        .fillMode = FillMode::Solid,
        .radius = radius,
        .softness = 1.0F,
        .borderWidth = 0.0F,
    };
  }

  float primaryPosition(const InputArea::PointerData& data, ScrollOrientation orientation) {
    return orientation == ScrollOrientation::Horizontal ? data.localX : data.localY;
  }

  bool acceptsScrollAxis(const InputArea::PointerData& data, ScrollOrientation orientation) {
    return data.axis == WL_POINTER_AXIS_VERTICAL_SCROLL
        || (orientation == ScrollOrientation::Horizontal && data.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL);
  }

} // namespace

Scrollbar::Scrollbar() {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });

  auto track = std::make_unique<RectNode>();
  m_track = static_cast<RectNode*>(addChild(std::move(track)));

  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto trackArea = std::make_unique<InputArea>();
  trackArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!acceptsScrollAxis(data, m_orientation) || !m_onScrollChanged) {
      return false;
    }
    m_onScrollChanged(std::clamp(currentOffset() + data.scrollDelta(Style::scrollWheelStep), 0.0F, m_maxScroll));
    return true;
  });
  trackArea->setOnEnter([this](const InputArea::PointerData&) { updateExpanded(); });
  trackArea->setOnLeave([this]() { updateExpanded(); });
  m_trackArea = static_cast<InputArea*>(addChild(std::move(trackArea)));

  auto thumbArea = std::make_unique<InputArea>();
  thumbArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  thumbArea->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (data.pressed) {
      const float localPosition = primaryPosition(data, m_orientation);
      const float thumbPosition = m_orientation == ScrollOrientation::Horizontal ? m_thumbArea->x() : m_thumbArea->y();
      m_dragStartPosition = localPosition + thumbPosition;
      m_dragStartOffset = currentOffset();
    }
    updateExpanded();
  });
  thumbArea->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_thumbTravel <= 0.0F || !m_onScrollChanged || m_thumbArea == nullptr || !m_thumbArea->pressed()) {
      return;
    }
    const float localPosition = primaryPosition(data, m_orientation);
    const float thumbPosition = m_orientation == ScrollOrientation::Horizontal ? m_thumbArea->x() : m_thumbArea->y();
    const float delta = localPosition + thumbPosition - m_dragStartPosition;
    const float offsetPerPx = m_maxScroll / m_thumbTravel;
    m_onScrollChanged(std::clamp(m_dragStartOffset + delta * offsetPerPx, 0.0F, m_maxScroll));
  });
  thumbArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!acceptsScrollAxis(data, m_orientation) || !m_onScrollChanged) {
      return false;
    }
    m_onScrollChanged(std::clamp(currentOffset() + data.scrollDelta(Style::scrollWheelStep), 0.0F, m_maxScroll));
    return true;
  });
  thumbArea->setOnEnter([this](const InputArea::PointerData&) { updateExpanded(); });
  thumbArea->setOnLeave([this]() { updateExpanded(); });
  m_thumbArea = static_cast<InputArea*>(addChild(std::move(thumbArea)));

  applyPalette();
}

void Scrollbar::setOrientation(ScrollOrientation orientation) {
  if (m_orientation == orientation) {
    return;
  }
  m_orientation = orientation;
  markLayoutDirty();
}

void Scrollbar::setOnScrollChanged(std::function<void(float)> callback) { m_onScrollChanged = std::move(callback); }

void Scrollbar::setTrackInset(float inset) { m_trackInset = std::max(0.0F, inset); }

void Scrollbar::setContentScale(float scale) {
  const float clamped = std::max(0.1F, scale);
  if (m_contentScale == clamped) {
    return;
  }
  m_contentScale = clamped;
  applyGeometry();
  markLayoutDirty();
}

float Scrollbar::currentOffset() const noexcept {
  const float thumbPosition = m_orientation == ScrollOrientation::Horizontal ? m_thumb->x() : m_thumb->y();
  return m_thumbTravel > 0.0F
      ? std::clamp(((thumbPosition - m_trackInset) / m_thumbTravel) * m_maxScroll, 0.0F, m_maxScroll)
      : 0.0F;
}

float Scrollbar::thickness() const noexcept {
  const float base = Style::scrollbarWidth * m_contentScale;
  const float expanded = Style::scrollbarHoverWidth * m_contentScale;
  return base + (expanded - base) * std::clamp(m_expansion, 0.0F, 1.0F);
}

void Scrollbar::update(float viewportExtent, float contentExtent, float scrollOffset) {
  m_viewportExtent = viewportExtent;
  m_contentExtent = contentExtent;
  m_lastScrollOffset = scrollOffset;
  m_maxScroll = std::max(0.0F, contentExtent - viewportExtent);

  m_shown = contentExtent > viewportExtent + 0.5F;
  m_track->setVisible(m_shown);
  m_thumb->setVisible(m_shown);
  m_trackArea->setVisible(m_shown);
  m_thumbArea->setVisible(m_shown);
  if (!m_shown) {
    m_thumbTravel = 0.0F;
    setExpanded(false);
    return;
  }

  applyGeometry();
}

void Scrollbar::applyGeometry() {
  if (!m_shown) {
    return;
  }

  // The reserved gutter is one base thickness wide; the expansion grows inward, over the
  // content, so the outer edge stays put and no relayout is needed while hovering.
  const float thick = thickness();
  const float overlay = thick - Style::scrollbarWidth * m_contentScale;
  m_crossOffset = Style::rtl() && m_orientation == ScrollOrientation::Vertical ? 0.0F : -overlay;
  const float slop = Style::scrollbarHitSlop * m_contentScale;
  const HitTestOutset outset = m_orientation == ScrollOrientation::Horizontal
      ? HitTestOutset{.top = slop}
      : (Style::rtl() ? HitTestOutset{.right = slop} : HitTestOutset{.left = slop});
  m_trackArea->setHitTestOutset(outset);
  m_thumbArea->setHitTestOutset(outset);

  const float trackExtent = std::max(0.0F, m_viewportExtent - m_trackInset * 2.0F);
  if (m_orientation == ScrollOrientation::Horizontal) {
    m_track->setPosition(m_trackInset, m_crossOffset);
    m_track->setFrameSize(trackExtent, thick);
    m_trackArea->setPosition(m_trackInset, m_crossOffset);
    m_trackArea->setFrameSize(trackExtent, thick);
  } else {
    m_track->setPosition(m_crossOffset, m_trackInset);
    m_track->setFrameSize(thick, trackExtent);
    m_trackArea->setPosition(m_crossOffset, m_trackInset);
    m_trackArea->setFrameSize(thick, trackExtent);
  }

  // A thumb shorter than the bar is thick reads as a blob, so the floor grows with the bar.
  const float minThumbExtent = std::max(Style::scrollbarMinThumbHeight * m_contentScale, thick);
  const float thumbExtent = std::min(
      trackExtent,
      std::max(minThumbExtent, (m_viewportExtent * m_viewportExtent) / std::max(m_viewportExtent, m_contentExtent))
  );
  m_thumbTravel = std::max(0.0F, trackExtent - thumbExtent);
  if (m_orientation == ScrollOrientation::Horizontal) {
    m_thumb->setFrameSize(thumbExtent, thick);
    m_thumbArea->setFrameSize(thumbExtent, thick);
  } else {
    m_thumb->setFrameSize(thick, thumbExtent);
    m_thumbArea->setFrameSize(thick, thumbExtent);
  }

  applyPalette();
  applyThumbPosition(m_lastScrollOffset, m_maxScroll);
}

void Scrollbar::updateExpanded() {
  const bool hovered =
      (m_trackArea != nullptr && m_trackArea->hovered()) || (m_thumbArea != nullptr && m_thumbArea->hovered());
  const bool dragging = m_thumbArea != nullptr && m_thumbArea->pressed();
  setExpanded(m_shown && (hovered || dragging));
}

void Scrollbar::setExpanded(bool expanded) {
  const float target = expanded ? 1.0F : 0.0F;
  if (m_expansion == target && m_expandAnim == 0) {
    return;
  }
  AnimationManager* animations = animationManager();
  if (animations == nullptr) {
    applyExpansion(target);
    return;
  }
  if (m_expandAnim != 0) {
    animations->cancel(m_expandAnim);
  }
  m_expandAnim = animations->animate(
      m_expansion, target, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [this](float value) { applyExpansion(value); }, [this] { m_expandAnim = 0; }, this
  );
  // Starting an animation mutates nothing yet, so the surface has no reason to schedule a frame
  // and would never tick it. Ask for the repaint that starts the frame loop.
  markPaintDirty();
}

void Scrollbar::applyExpansion(float expansion) {
  m_expansion = std::clamp(expansion, 0.0F, 1.0F);
  applyGeometry();
}

void Scrollbar::applyPalette() {
  const float radius = thickness() * 0.5F;
  if (m_track != nullptr) {
    m_track->setStyle(makeSolid(resolveColorSpec(scrollbarTrackColor()), radius));
  }
  if (m_thumb != nullptr) {
    m_thumb->setStyle(makeSolid(resolveColorSpec(scrollbarThumbColor()), radius));
  }
}

void Scrollbar::applyThumbPosition(float scrollOffset, float maxScroll) {
  const float t = maxScroll > 0.0F ? std::clamp(scrollOffset / maxScroll, 0.0F, 1.0F) : 0.0F;
  const float thumbPosition = m_trackInset + t * m_thumbTravel;
  if (m_orientation == ScrollOrientation::Horizontal) {
    m_thumb->setPosition(thumbPosition, m_crossOffset);
    m_thumbArea->setPosition(thumbPosition, m_crossOffset);
  } else {
    m_thumb->setPosition(m_crossOffset, thumbPosition);
    m_thumbArea->setPosition(m_crossOffset, thumbPosition);
  }
}
