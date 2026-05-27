#include "ui/controls/progress_bar.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

ProgressBar::ProgressBar() {
  auto track = std::make_unique<RectNode>();
  m_track = dynamic_cast<RectNode*>(addChild(std::move(track)));

  auto fill = std::make_unique<RectNode>();
  m_fill = dynamic_cast<RectNode*>(addChild(std::move(fill)));

  setTrack(colorSpecFromRole(ColorRole::SurfaceVariant));
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
  m_progress = std::clamp(progress, 0.0f, 1.0f);
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

void ProgressBar::applyPalette() {
  auto trackStyle = m_track->style();
  trackStyle.fill = resolveColorSpec(m_trackColor);
  trackStyle.fillMode = FillMode::Solid;
  m_track->setStyle(trackStyle);

  auto fillStyle = m_fill->style();
  fillStyle.fill = resolveColorSpec(m_fillColor);
  fillStyle.fillMode = FillMode::Solid;
  m_fill->setStyle(fillStyle);
}

void ProgressBar::updateGeometry() {
  m_track->setFrameSize(width(), height());
  if (m_orientation == ProgressBarOrientation::Vertical) {
    const float fillH = height() * m_progress;
    m_fill->setPosition(0.0f, height() - fillH);
    m_fill->setFrameSize(width(), fillH);
  } else {
    m_fill->setFrameSize(width() * m_progress, height());
  }
}
