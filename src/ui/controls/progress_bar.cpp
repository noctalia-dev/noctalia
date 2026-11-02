#include "ui/controls/progress_bar.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

ProgressBar::ProgressBar() {
  auto track = std::make_unique<RectNode>();
  m_track = static_cast<RectNode*>(addChild(std::move(track)));

  // The fill is a full-size copy of the track revealed through a clip node, so
  // its rounded (closed) end always matches the track exactly instead of
  // squaring off at small progress values.
  auto fillClip = std::make_unique<Node>();
  fillClip->setClipChildren(true);
  m_fillClip = addChild(std::move(fillClip));

  auto fill = std::make_unique<RectNode>();
  m_fill = static_cast<RectNode*>(m_fillClip->addChild(std::move(fill)));

  // Added last so it draws above the track and fill.
  auto marker = std::make_unique<RectNode>();
  marker->setVisible(false);
  m_marker = static_cast<RectNode*>(addChild(std::move(marker)));

  setTrack(colorSpecFromRole(ColorRole::SurfaceContainer));
  setFill(colorSpecFromRole(ColorRole::Primary));
  setRadius(Style::scaledRadiusSm());
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

void ProgressBar::setFill(const ColorSpec& color) { setFillColor(color); }

void ProgressBar::setFillColor(const ColorSpec& color) {
  m_fillColor = color;
  applyPalette();
}

void ProgressBar::setFill(const Color& color) { setFillColor(color); }

void ProgressBar::setFillColor(const Color& color) { setFillColor(fixedColorSpec(color)); }

void ProgressBar::setTrack(const ColorSpec& color) { setTrackColor(color); }

void ProgressBar::setTrackColor(const ColorSpec& color) {
  m_trackColor = color;
  applyPalette();
}

void ProgressBar::setTrack(const Color& color) { setTrackColor(color); }

void ProgressBar::setTrackColor(const Color& color) { setTrackColor(fixedColorSpec(color)); }

void ProgressBar::setRadius(float radius) {
  auto style = m_track->style();
  style.radius = radius;
  m_track->setStyle(style);
  auto fillStyle = m_fill->style();
  fillStyle.radius = radius;
  m_fill->setStyle(fillStyle);
}

void ProgressBar::setSoftness(float softness) {
  auto style = m_track->style();
  style.softness = softness;
  m_track->setStyle(style);
  auto fillStyle = m_fill->style();
  fillStyle.softness = softness;
  m_fill->setStyle(fillStyle);
}

void ProgressBar::setProgress(float progress) {
  m_progress = std::clamp(progress, 0.0F, 1.0F);
  updateGeometry();
}

void ProgressBar::setSize(float w, float h) {
  Node::setSize(w, h);
  updateGeometry();
}

void ProgressBar::setOrientation(ProgressBarOrientation orientation) {
  m_orientation = orientation;
  updateGeometry();
}

void ProgressBar::setMarker(float marker) {
  // Negative or NaN positions hide the marker; valid positions clamp to 0-1.
  m_markerPos = marker >= 0.0F ? std::optional<float>{std::clamp(marker, 0.0F, 1.0F)} : std::nullopt;
  updateGeometry();
}

void ProgressBar::clearMarker() {
  m_markerPos = std::nullopt;
  updateGeometry();
}

void ProgressBar::setMarkerColor(const ColorSpec& color) {
  m_markerColor = color;
  applyPalette();
}

void ProgressBar::setMarkerColor(const Color& color) { setMarkerColor(fixedColorSpec(color)); }

void ProgressBar::setMarkerWidth(float width) {
  m_markerWidth = std::max(width, 1.0F);
  updateGeometry();
}

void ProgressBar::applyPalette() {
  auto trackStyle = m_track->style();
  trackStyle.fill = resolveColorSpec(m_trackColor);
  trackStyle.fillMode = FillMode::Solid;
  m_track->setStyle(trackStyle);

  auto fillStyle = m_fill->style();
  fillStyle.fill = resolveColorSpec(m_fillColor);
  fillStyle.fillMode = FillMode::Solid;
  m_fill->setStyle(fillStyle);

  auto markerStyle = m_marker->style();
  markerStyle.fill = resolveColorSpec(m_markerColor);
  markerStyle.fillMode = FillMode::Solid;
  m_marker->setStyle(markerStyle);
}

void ProgressBar::updateGeometry() {
  const float w = width();
  const float h = height();
  m_track->setFrameSize(w, h);

  // The marker crosses the track with a 3/4-thickness overhang on each side; 0 is the left (or bottom) edge.
  if (m_markerPos.has_value()) {
    const float overscan = m_markerWidth * 0.75F;
    if (m_orientation == ProgressBarOrientation::Vertical) {
      const float y = h * (1.0F - *m_markerPos) - m_markerWidth * 0.5F;
      m_marker->setPosition(-overscan, y);
      m_marker->setFrameSize(w + overscan * 2.0F, m_markerWidth);
    } else {
      const float x = w * *m_markerPos - m_markerWidth * 0.5F;
      m_marker->setPosition(x, -overscan);
      m_marker->setFrameSize(m_markerWidth, h + overscan * 2.0F);
    }
  }
  m_marker->setVisible(m_markerPos.has_value());

  if (m_orientation == ProgressBarOrientation::HorizontalCentered) {
    // Both ends are leading edges, so keep the fill's own rounded shape (a
    // shrinking pill) rather than clipping it to flat slice edges.
    const float fillW = w * m_progress;
    m_fillClip->setPosition(0.0F, 0.0F);
    m_fillClip->setFrameSize(w, h);
    m_fill->setFrameSize(fillW, h);
    m_fill->setPosition((w - fillW) * 0.5F, 0.0F);
    return;
  }

  // Anchored fill: reveal a full-size copy of the track through the clip so the
  // rounded closed end always matches the track instead of squaring off.
  m_fill->setFrameSize(w, h);
  float clipX = 0.0F;
  float clipY = 0.0F;
  float clipW = w;
  float clipH = h;
  if (m_orientation == ProgressBarOrientation::Vertical) {
    clipH = h * m_progress;
    clipY = h - clipH;
  } else {
    clipW = w * m_progress;
  }

  m_fillClip->setPosition(clipX, clipY);
  m_fillClip->setFrameSize(clipW, clipH);
  m_fill->setPosition(-clipX, -clipY);
}
