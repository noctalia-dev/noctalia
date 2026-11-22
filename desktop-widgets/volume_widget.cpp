#include "volume_widget.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <utility>

namespace {
  constexpr float kBaseWidth = 220.0F;
  constexpr float kBaseHeight = 120.0F;
  constexpr float kTextGap = 12.0F;
  constexpr float kShadowAlpha = 0.55F;
  constexpr float kShadowOffset = 1.5F;
  constexpr float kTrackAlphaScale = 0.45F;
  constexpr float kMinTextWidth = 72.0F;

  float percentFontSize(float layoutScale) { return Style::fontSizeTitle * 1.35F * layoutScale; }
  float statusFontSize(float layoutScale) { return Style::fontSizeBody * layoutScale; }
  float deviceFontSize(float layoutScale) { return Style::fontSizeCaption * layoutScale; }

} // namespace

DesktopVolume::DesktopVolume() {
  auto area = ui::inputArea({});
  // Whole card: scroll only. Mute is on the glyph hit target below.
  area->setAcceptedButtons(0);
  area->addChild(
      ui::label({
          .out = &m_percentLabel,
          .fontSize = percentFontSize(m_contentScale),
          .fontWeight = FontWeight::Bold,
          .color = m_fillColor,
          .textAlign = TextAlign::Start,
      })
  );
  area->addChild(
      ui::label({
          .out = &m_statusLabel,
          .fontSize = statusFontSize(m_contentScale),
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .textAlign = TextAlign::Start,
      })
  );
  area->addChild(
      ui::label({
          .out = &m_deviceLabel,
          .fontSize = deviceFontSize(m_contentScale),
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
          .textAlign = TextAlign::Start,
          .ellipsize = TextEllipsize::End,
          .visible = m_showDevice,
      })
  );

  auto glyphHit = ui::inputArea({});
  glyphHit->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
  glyphHit->addChild(
      ui::glyph({
          .out = &m_trackGlyph,
          .glyph = "volume-high",
          .color = m_trackColor,
      })
  );
  auto fillClip = ui::node({});
  fillClip->setClipChildren(true);
  m_fillClip = glyphHit->addChild(std::move(fillClip));
  m_fillClip->addChild(
      ui::glyph({
          .out = &m_fillGlyph,
          .glyph = "volume-high",
          .color = m_fillColor,
      })
  );
  m_glyphHit = static_cast<InputArea*>(area->addChild(std::move(glyphHit)));

  m_area = area.get();
  addChild(std::move(area));
  applyColors();
  applyShadow();
}

void DesktopVolume::setData(const Data& data) {
  m_percentLabel->setText(data.percent);
  m_statusLabel->setText(data.status);
  m_deviceLabel->setText(data.device);
  m_trackGlyph->setGlyph(data.glyph);
  m_fillGlyph->setGlyph(data.glyph);
  m_lastDeviceText = data.device;
  m_fillProgress = data.progress;
  updateFillClip();
}

void DesktopVolume::setAppearance(const ColorSpec& fill, const ColorSpec& track, bool showDevice, bool shadow) {
  m_fillColor = fill;
  m_trackColor = track;
  m_showDevice = showDevice;
  m_shadow = shadow;
  m_deviceLabel->setVisible(showDevice);
  applyColors();
  applyShadow();
}

void DesktopVolume::setOnScroll(std::function<void(float)> callback) {
  m_area->setOnAxis([callback = std::move(callback)](const InputArea::PointerData& data) {
    if (callback) {
      callback(data.scrollSteps());
    }
  });
}

void DesktopVolume::setOnToggleMute(std::function<void()> callback) {
  m_glyphHit->setOnClick([callback = std::move(callback)](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT && callback) {
      callback();
    }
  });
}

void DesktopVolume::setContentScale(float scale) {
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

void DesktopVolume::setViewport(float width, float height, float radius) {
  if (m_viewWidth == width && m_viewHeight == height && m_cornerRadius == radius) {
    return;
  }
  m_viewWidth = width;
  m_viewHeight = height;
  m_cornerRadius = radius;
  markLayoutDirty();
}

void DesktopVolume::setFontFamily(const std::string& family) {
  if (m_percentLabel != nullptr) {
    m_percentLabel->setFontFamily(family);
  }
  if (m_statusLabel != nullptr) {
    m_statusLabel->setFontFamily(family);
  }
  if (m_deviceLabel != nullptr) {
    m_deviceLabel->setFontFamily(family);
  }
}

void DesktopVolume::doLayout(Renderer& renderer) {
  if (m_percentLabel == nullptr
      || m_statusLabel == nullptr
      || m_deviceLabel == nullptr
      || m_glyphHit == nullptr
      || m_trackGlyph == nullptr
      || m_fillClip == nullptr
      || m_fillGlyph == nullptr) {
    return;
  }

  const float innerW = m_viewWidth;
  const float innerH = m_viewHeight;
  const bool boxed = innerW > 0.0F && innerH > 0.0F;
  const float width = boxed ? innerW : kBaseWidth * m_contentScale;
  const float height = boxed ? innerH : kBaseHeight * m_contentScale;
  // Keep content clear of the outer rounded clip (padding alone is not enough near corners).
  const float edgeInset = boxed ? std::round(std::max({4.0F, height * 0.06F, m_cornerRadius * 0.35F})) : 0.0F;
  const float layoutW = std::max(1.0F, width - 2.0F * edgeInset);
  const float layoutH = std::max(1.0F, height - 2.0F * edgeInset);
  const float layoutScale = std::max(0.05F, layoutH / kBaseHeight);
  const float gap = kTextGap * layoutScale;

  float glyphBox = layoutH * 0.85F;
  const float minTextW = kMinTextWidth * layoutScale;
  glyphBox = std::min(glyphBox, std::max(layoutH * 0.35F, layoutW - minTextW - gap));
  glyphBox = std::round(glyphBox);
  m_glyphBox = glyphBox;
  const float textMaxW = std::max(1.0F, layoutW - glyphBox - gap);

  m_percentLabel->setFontSize(percentFontSize(layoutScale));
  m_percentLabel->setMaxWidth(textMaxW);
  m_percentLabel->measure(renderer);

  m_statusLabel->setFontSize(statusFontSize(layoutScale));
  m_statusLabel->setMaxWidth(textMaxW);
  m_statusLabel->measure(renderer);

  const bool showDevice = m_showDevice && !m_lastDeviceText.empty();
  m_deviceLabel->setVisible(showDevice);
  float deviceH = 0.0F;
  if (showDevice) {
    m_deviceLabel->setFontSize(deviceFontSize(layoutScale));
    m_deviceLabel->setMaxWidth(textMaxW);
    m_deviceLabel->setMaxLines(2);
    m_deviceLabel->measure(renderer);
    deviceH = m_deviceLabel->height();
  }

  const float textBlockH =
      m_percentLabel->height() + m_statusLabel->height() + (showDevice ? Style::spaceXs * layoutScale + deviceH : 0.0F);
  const float textY = edgeInset + std::max(0.0F, (layoutH - textBlockH) * 0.5F);

  m_percentLabel->setPosition(edgeInset, textY);
  m_statusLabel->setPosition(edgeInset, textY + m_percentLabel->height());
  if (showDevice) {
    m_deviceLabel->setPosition(
        edgeInset, textY + m_percentLabel->height() + m_statusLabel->height() + Style::spaceXs * layoutScale
    );
  }

  // Same square box + glyph size for track and fill so ink centers stay identical.
  m_trackGlyph->setGlyphSize(glyphBox);
  m_trackGlyph->setSize(glyphBox, glyphBox);
  m_trackGlyph->measure(renderer);
  m_fillGlyph->setGlyphSize(glyphBox);
  m_fillGlyph->setSize(glyphBox, glyphBox);
  m_fillGlyph->measure(renderer);

  const float glyphW = glyphBox;
  const float glyphH = glyphBox;
  const float glyphX = edgeInset + layoutW - glyphW;
  const float glyphY = edgeInset + (layoutH - glyphH) * 0.5F;

  m_glyphHit->setPosition(glyphX, glyphY);
  m_glyphHit->setFrameSize(glyphW, glyphH);
  m_trackGlyph->setPosition(0.0F, 0.0F);
  updateFillClip();

  setSize(width, height);
  if (m_area != nullptr) {
    m_area->setFrameSize(width, height);
  }

  applyColors();
  applyShadow();
}

void DesktopVolume::updateFillClip() {
  if (m_fillClip == nullptr || m_fillGlyph == nullptr || m_glyphBox <= 0.0F) {
    return;
  }
  // Full-size fill glyph, revealed through a bottom-anchored clip (same as ProgressBar).
  const float progress = std::clamp(m_fillProgress, 0.0F, 1.0F);
  const float clipH = m_glyphBox * progress;
  const float clipY = m_glyphBox - clipH;
  m_fillClip->setPosition(0.0F, clipY);
  m_fillClip->setFrameSize(m_glyphBox, clipH);
  m_fillGlyph->setPosition(0.0F, -clipY);
}

void DesktopVolume::applyColors() {
  if (m_percentLabel != nullptr) {
    m_percentLabel->setColor(m_fillColor);
  }
  if (m_statusLabel != nullptr) {
    m_statusLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  }
  if (m_deviceLabel != nullptr) {
    m_deviceLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
  }
  if (m_trackGlyph != nullptr) {
    ColorSpec track = m_trackColor;
    track.alpha *= kTrackAlphaScale;
    m_trackGlyph->setColor(track);
  }
  if (m_fillGlyph != nullptr) {
    m_fillGlyph->setColor(m_fillColor);
  }
}

void DesktopVolume::applyShadow() {
  const ColorSpec shadowColor = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);
  const auto applyLabel = [&](Label* label) {
    if (label == nullptr) {
      return;
    }
    if (m_shadow) {
      label->setShadow(shadowColor, kShadowOffset, kShadowOffset);
    } else {
      label->clearShadow();
    }
  };
  applyLabel(m_percentLabel);
  applyLabel(m_statusLabel);
  applyLabel(m_deviceLabel);

  if (m_trackGlyph != nullptr) {
    if (m_shadow) {
      m_trackGlyph->setShadow(shadowColor, kShadowOffset, kShadowOffset);
    } else {
      m_trackGlyph->clearShadow();
    }
  }
  if (m_fillGlyph != nullptr) {
    m_fillGlyph->clearShadow();
  }
}
