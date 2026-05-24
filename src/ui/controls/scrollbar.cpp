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

} // namespace

Scrollbar::Scrollbar() {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });

  auto track = std::make_unique<RectNode>();
  m_track = static_cast<RectNode*>(addChild(std::move(track)));

  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto trackArea = std::make_unique<InputArea>();
  trackArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !m_onScrollChanged) {
      return false;
    }
    const float current = m_thumbTravel > 0.0f ? (m_thumb->y() / m_thumbTravel) * m_maxScroll : 0.0f;
    m_onScrollChanged(std::clamp(current + data.scrollDelta(Style::scrollWheelStep), 0.0f, m_maxScroll));
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
      m_dragStartY = data.localY + m_thumbArea->y();
      m_dragStartOffset =
          std::clamp(m_maxScroll > 0.0f ? (m_thumb->y() / m_thumbTravel) * m_maxScroll : 0.0f, 0.0f, m_maxScroll);
    }
  });
  thumbArea->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_thumbTravel <= 0.0f || !m_onScrollChanged || m_thumbArea == nullptr || !m_thumbArea->pressed()) {
      return;
    }
    const float pointerY = data.localY + m_thumbArea->y();
    const float deltaY = pointerY - m_dragStartY;
    const float offsetPerPx = m_maxScroll / m_thumbTravel;
    m_onScrollChanged(std::clamp(m_dragStartOffset + deltaY * offsetPerPx, 0.0f, m_maxScroll));
  });
  thumbArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !m_onScrollChanged) {
      return false;
    }
    const float current = m_thumbTravel > 0.0f ? (m_thumb->y() / m_thumbTravel) * m_maxScroll : 0.0f;
    m_onScrollChanged(std::clamp(current + data.scrollDelta(Style::scrollWheelStep), 0.0f, m_maxScroll));
    return true;
  });
  m_thumbArea = static_cast<InputArea*>(addChild(std::move(thumbArea)));

  applyPalette();
}

void Scrollbar::setOnScrollChanged(std::function<void(float)> callback) { m_onScrollChanged = std::move(callback); }

void Scrollbar::update(float viewportHeight, float contentHeight, float scrollOffset) {
  m_viewportHeight = viewportHeight;
  m_contentHeight = contentHeight;
  m_maxScroll = std::max(0.0f, contentHeight - viewportHeight);

  m_shown = contentHeight > viewportHeight + 0.5f;
  m_track->setVisible(m_shown);
  m_thumb->setVisible(m_shown);
  m_trackArea->setVisible(m_shown);
  m_thumbArea->setVisible(m_shown);
  if (!m_shown) {
    m_thumbTravel = 0.0f;
    return;
  }

  const float trackH = viewportHeight;
  m_track->setPosition(0.0f, 0.0f);
  m_track->setFrameSize(Style::scrollbarWidth, trackH);
  m_trackArea->setPosition(0.0f, 0.0f);
  m_trackArea->setFrameSize(Style::scrollbarWidth, trackH);

  const float thumbH = std::clamp(
      (viewportHeight * viewportHeight) / std::max(viewportHeight, contentHeight), Style::scrollbarMinThumbHeight,
      trackH
  );
  m_thumbTravel = std::max(0.0f, trackH - thumbH);
  m_thumb->setFrameSize(Style::scrollbarWidth, thumbH);
  m_thumbArea->setFrameSize(Style::scrollbarWidth, thumbH);

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
  const float thumbY = t * m_thumbTravel;
  m_thumb->setPosition(0.0f, thumbY);
  m_thumbArea->setPosition(0.0f, thumbY);
}
