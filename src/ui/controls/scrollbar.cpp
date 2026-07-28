#include "ui/controls/scrollbar.h"

#include "cursor-shape-v1-client-protocol.h"
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

  RoundedRectStyle makeSolid(const Color& fill) {
    return RoundedRectStyle{
        .fill = fill,
        .border = fill,
        .fillMode = FillMode::Solid,
        .radius = Style::scrollbarWidth * 0.5f,
        .softness = 1.0f,
        .borderWidth = 0.0f,
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
    m_onScrollChanged(std::clamp(currentOffset() + data.scrollDelta(Style::scrollWheelStep), 0.0f, m_maxScroll));
    return true;
  });
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
    }
  });
  thumbArea->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_thumbTravel <= 0.0f || !m_onScrollChanged || m_thumbArea == nullptr || !m_thumbArea->pressed()) {
      return;
    }
    const float localPosition = primaryPosition(data, m_orientation);
    const float thumbPosition = m_orientation == ScrollOrientation::Horizontal ? m_thumbArea->x() : m_thumbArea->y();
    const float delta = localPosition + thumbPosition - m_dragStartPosition;
    const float offsetPerPx = m_maxScroll / m_thumbTravel;
    m_onScrollChanged(std::clamp(m_dragStartOffset + delta * offsetPerPx, 0.0f, m_maxScroll));
  });
  thumbArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!acceptsScrollAxis(data, m_orientation) || !m_onScrollChanged) {
      return false;
    }
    m_onScrollChanged(std::clamp(currentOffset() + data.scrollDelta(Style::scrollWheelStep), 0.0f, m_maxScroll));
    return true;
  });
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

void Scrollbar::setTrackInset(float inset) { m_trackInset = std::max(0.0f, inset); }

float Scrollbar::currentOffset() const noexcept {
  const float thumbPosition = m_orientation == ScrollOrientation::Horizontal ? m_thumb->x() : m_thumb->y();
  return m_thumbTravel > 0.0f
      ? std::clamp(((thumbPosition - m_trackInset) / m_thumbTravel) * m_maxScroll, 0.0f, m_maxScroll)
      : 0.0f;
}

void Scrollbar::update(float viewportExtent, float contentExtent, float scrollOffset) {
  m_viewportExtent = viewportExtent;
  m_contentExtent = contentExtent;
  m_maxScroll = std::max(0.0f, contentExtent - viewportExtent);

  m_shown = contentExtent > viewportExtent + 0.5f;
  m_track->setVisible(m_shown);
  m_thumb->setVisible(m_shown);
  m_trackArea->setVisible(m_shown);
  m_thumbArea->setVisible(m_shown);
  if (!m_shown) {
    m_thumbTravel = 0.0f;
    return;
  }

  const float trackExtent = std::max(0.0f, viewportExtent - m_trackInset * 2.0f);
  const float thickness = Style::scrollbarWidth;
  if (m_orientation == ScrollOrientation::Horizontal) {
    m_track->setPosition(m_trackInset, 0.0f);
    m_track->setFrameSize(trackExtent, thickness);
    m_trackArea->setPosition(m_trackInset, 0.0f);
    m_trackArea->setFrameSize(trackExtent, thickness);
  } else {
    m_track->setPosition(0.0f, m_trackInset);
    m_track->setFrameSize(thickness, trackExtent);
    m_trackArea->setPosition(0.0f, m_trackInset);
    m_trackArea->setFrameSize(thickness, trackExtent);
  }

  const float thumbExtent = std::min(
      trackExtent,
      std::max(
          Style::scrollbarMinThumbHeight, (viewportExtent * viewportExtent) / std::max(viewportExtent, contentExtent)
      )
  );
  m_thumbTravel = std::max(0.0f, trackExtent - thumbExtent);
  if (m_orientation == ScrollOrientation::Horizontal) {
    m_thumb->setFrameSize(thumbExtent, thickness);
    m_thumbArea->setFrameSize(thumbExtent, thickness);
  } else {
    m_thumb->setFrameSize(thickness, thumbExtent);
    m_thumbArea->setFrameSize(thickness, thumbExtent);
  }

  applyThumbPosition(scrollOffset, m_maxScroll);
}

void Scrollbar::applyPalette() {
  if (m_track != nullptr) {
    m_track->setStyle(makeSolid(resolveColorSpec(scrollbarTrackColor())));
  }
  if (m_thumb != nullptr) {
    m_thumb->setStyle(makeSolid(resolveColorSpec(scrollbarThumbColor())));
  }
}

void Scrollbar::applyThumbPosition(float scrollOffset, float maxScroll) {
  const float t = maxScroll > 0.0f ? std::clamp(scrollOffset / maxScroll, 0.0f, 1.0f) : 0.0f;
  const float thumbPosition = m_trackInset + t * m_thumbTravel;
  if (m_orientation == ScrollOrientation::Horizontal) {
    m_thumb->setPosition(thumbPosition, 0.0f);
    m_thumbArea->setPosition(thumbPosition, 0.0f);
  } else {
    m_thumb->setPosition(0.0f, thumbPosition);
    m_thumbArea->setPosition(0.0f, thumbPosition);
  }
}
