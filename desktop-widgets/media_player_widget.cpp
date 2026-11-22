#include "media_player_widget.h"

#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/style.h"
#include "visuals/audio_visualizer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
  constexpr float kArtSize = 120.0F;
  constexpr float kControlSize = 32.0F;
  constexpr float kPlayPauseSize = 40.0F;
  constexpr float kSpacing = 6.0F;
  constexpr float kBoxedHorizontalBreakpoint = 1.15F;
  constexpr float kVisualizerHeight = 26.0F;
  constexpr float kVisualizerAlpha = 0.5F;

} // namespace

namespace {
  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;

} // namespace

DesktopMediaPlayer::DesktopMediaPlayer() {
  setZIndex(2);

  auto artwork = ui::image({
      .out = &m_artwork,
      .fit = ImageFit::Cover,
      .styleRadius = Style::radiusMd,
      .styleRadiusScale = m_contentScale,
  });
  addChild(std::move(artwork));

  auto title = ui::label({
      .out = &m_title,
      .fontWeight = FontWeight::Bold,
      .color = m_color,
      .maxLines = 1,
  });
  addChild(std::move(title));

  auto artist = ui::label({
      .out = &m_artist,
      .color = m_color,
      .maxLines = 1,
  });
  addChild(std::move(artist));

  auto controls = ui::row(
      {
          .out = &m_controls,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
      },
      ui::button({.out = &m_prev, .glyph = "media-prev", .variant = ButtonVariant::Ghost}),
      ui::button({.out = &m_playPause, .glyph = "media-play", .variant = ButtonVariant::Primary}),
      ui::button({.out = &m_next, .glyph = "media-next", .variant = ButtonVariant::Ghost})
  );

  addChild(std::move(controls));
  applyShadow();
}

void DesktopMediaPlayer::setOptions(const Options& options) {
  if (m_options == options) {
    return;
  }
  m_options = options;
  m_vertical = options.vertical;
  m_color = options.color;
  m_shadow = options.shadow;
  m_albumArtBackground = options.albumArtBackground;
  m_audioVisualizerEnabled = options.audioVisualizer;
  m_hasSpectrum = options.hasSpectrum;
  m_title->setColor(m_color);
  m_artist->setColor(m_color);
  updateVisualizerColor();
  applyShadow();
  markLayoutDirty();
}

void DesktopMediaPlayer::setPlayback(
    const std::string& title, const std::string& artist, bool playing, bool previous, bool next
) {
  m_title->setText(title);
  m_artist->setText(artist);
  m_artist->setVisible(!artist.empty());
  m_playPause->setGlyph(playing ? "media-pause" : "media-play");
  m_prev->setVisible(previous);
  m_next->setVisible(next);
}

void DesktopMediaPlayer::setActions(
    std::function<void()> previous, std::function<void()> playPause, std::function<void()> next
) {
  m_prev->setOnClick(std::move(previous));
  m_playPause->setOnClick(std::move(playPause));
  m_next->setOnClick(std::move(next));
}

void DesktopMediaPlayer::setContentScale(float scale) {
  if (m_contentScale == scale && m_baseScale == scale) {
    return;
  }
  m_contentScale = scale;
  m_baseScale = scale;
  markLayoutDirty();
}

void DesktopMediaPlayer::setViewport(float width, float height) {
  if (m_viewWidth == width && m_viewHeight == height) {
    return;
  }
  m_viewWidth = width;
  m_viewHeight = height;
  markLayoutDirty();
}

void DesktopMediaPlayer::attachDecorations(Node& parent, std::function<void()> asyncReady) {
  auto backgroundArtwork = ui::image({
      .out = &m_backgroundArtwork,
      .fit = ImageFit::Cover,
      .visible = false,
      .participatesInLayout = false,
      .configure = [asyncReady = std::move(asyncReady)](Image& image) mutable {
        image.setHitTestVisible(false);
        image.setZIndex(0);
        // layoutDecorations shows the blurred backdrop once it has a texture.
        image.setAsyncReadyCallback(std::move(asyncReady));
      },
  });
  parent.addChild(std::move(backgroundArtwork));

  auto visualizer = std::make_unique<AudioVisualizer>();
  visualizer->setOrientation(AudioSpectrumOrientation::Horizontal);
  visualizer->setCentered(false);
  visualizer->setMirrored(false);
  visualizer->setVisible(false);
  visualizer->setParticipatesInLayout(false);
  visualizer->setHitTestVisible(false);
  visualizer->setZIndex(1);
  m_audioVisualizer = visualizer.get();
  parent.addChild(std::move(visualizer));

  updateVisualizerColor();
}

void DesktopMediaPlayer::setFontFamily(const std::string& family) {
  if (m_title != nullptr) {
    m_title->setFontFamily(family);
  }
  if (m_artist != nullptr) {
    m_artist->setFontFamily(family);
  }
}

void DesktopMediaPlayer::doLayout(Renderer& renderer) {
  if (m_artwork == nullptr || m_title == nullptr || m_artist == nullptr || m_controls == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float innerWidth = m_viewWidth;
  const float innerHeight = m_viewHeight;
  if (innerWidth > 0.0F && innerHeight > 0.0F) {
    layoutBoxed(renderer, innerWidth, innerHeight);
  } else if (m_vertical) {
    layoutVertical(renderer, scale);
  } else {
    layoutHorizontal(renderer, scale);
  }
  applyShadow();
}

void DesktopMediaPlayer::layoutHorizontal(Renderer& renderer, float scale) {
  const float artH = kArtSize * scale;
  const float spacing = kSpacing * scale;
  const float fontSize = Style::fontSizeBody * scale;
  const float textWidth = artH * 1.5F;

  m_artwork->setSize(artH, artH);
  m_artwork->setStyleRadius(Style::radiusMd, scale);
  m_artwork->setPosition(0.0F, 0.0F);

  const float textX = artH + spacing;
  const float totalWidth = textX + textWidth;

  m_title->setFontSize(fontSize);
  m_title->setMaxWidth(textWidth);
  m_title->measure(renderer);

  m_artist->setFontSize(fontSize * 0.9F);
  m_artist->setMaxWidth(textWidth);
  m_artist->measure(renderer);

  layoutButtons(renderer, scale, false);

  const float titleH = m_title->height();
  const float artistGap = m_artist->visible() ? spacing * 0.5F : 0.0F;
  const float artistH = m_artist->visible() ? m_artist->height() : 0.0F;
  const float controlsH = m_controls->height();
  const float textAreaH = std::max(0.0F, artH - controlsH - spacing);
  const float textBlockH = titleH + artistGap + artistH;
  const float textY = std::round(std::max(0.0F, (textAreaH - textBlockH) * 0.5F));

  m_title->setPosition(textX, textY);
  m_artist->setPosition(textX, textY + titleH + artistGap);

  const float controlsY = artH - controlsH;
  const float controlsX = totalWidth - m_controls->width();
  m_controls->setPosition(controlsX, controlsY);

  m_visualizerX = textX;
  m_visualizerHeight = std::min(kVisualizerHeight * scale, controlsH);
  m_visualizerY = controlsY + (controlsH - m_visualizerHeight) * 0.5F;
  m_visualizerWidth = std::max(0.0F, controlsX - spacing - m_visualizerX);

  setSize(totalWidth, artH);
}

void DesktopMediaPlayer::layoutVertical(Renderer& renderer, float scale) {
  const float artW = kArtSize * scale;
  const float spacing = kSpacing * scale;
  const float fontSize = Style::fontSizeBody * scale;

  m_artwork->setSize(artW, artW);
  m_artwork->setStyleRadius(Style::radiusMd, scale);
  m_artwork->setPosition(0.0F, 0.0F);

  m_title->setFontSize(fontSize);
  m_title->setMaxWidth(artW);
  m_title->measure(renderer);
  m_title->setPosition(0.0F, artW + spacing);

  m_artist->setFontSize(fontSize * 0.9F);
  m_artist->setMaxWidth(artW);
  m_artist->measure(renderer);
  const float artistY = m_title->y() + m_title->height() + spacing * 0.5F;
  m_artist->setPosition(0.0F, artistY);

  layoutButtons(renderer, scale, true);

  const float controlsY =
      (m_artist->visible() ? m_artist->y() + m_artist->height() : m_title->y() + m_title->height()) + spacing;
  m_visualizerX = 0.0F;
  m_visualizerY = controlsY;
  m_visualizerWidth = artW;
  m_visualizerHeight = m_audioVisualizerEnabled && m_hasSpectrum ? kVisualizerHeight * scale : 0.0F;
  const float visualizerSpacing = m_visualizerHeight > 0.0F ? spacing : 0.0F;
  const float placedControlsY = controlsY + m_visualizerHeight + visualizerSpacing;
  const float controlsX = std::round((artW - m_controls->width()) * 0.5F);
  m_controls->setPosition(controlsX, placedControlsY);

  setSize(artW, placedControlsY + m_controls->height());
}

void DesktopMediaPlayer::layoutBoxed(Renderer& renderer, float width, float height) {
  const bool vertical = width / std::max(1.0F, height) < kBoxedHorizontalBreakpoint;

  m_contentScale = m_baseScale;
  if (vertical) {
    layoutVertical(renderer, m_baseScale);
  } else {
    layoutHorizontal(renderer, m_baseScale);
  }

  const float naturalWidth = std::max(1.0F, this->width());
  const float naturalHeight = std::max(1.0F, this->height());
  const float fittedScale =
      std::clamp(m_baseScale * std::min(width / naturalWidth, height / naturalHeight), 0.05F, 20.0F);
  m_contentScale = fittedScale;
  if (vertical) {
    layoutVertical(renderer, fittedScale);
  } else {
    layoutHorizontal(renderer, fittedScale);
  }

  const float offsetX = std::round((width - this->width()) * 0.5F);
  const float offsetY = std::round((height - this->height()) * 0.5F);
  const auto offsetNode = [offsetX, offsetY](Node* node) {
    if (node != nullptr) {
      node->setPosition(node->x() + offsetX, node->y() + offsetY);
    }
  };
  offsetNode(m_artwork);
  offsetNode(m_title);
  offsetNode(m_artist);
  offsetNode(m_controls);
  m_visualizerX += offsetX;
  m_visualizerY += offsetY;
  setSize(width, height);
}

void DesktopMediaPlayer::layoutButtons(Renderer& renderer, float scale, bool vertical) {
  const float controlBtnSize = kControlSize * scale;
  const float playPauseBtnSize = kPlayPauseSize * scale;
  const float glyphSize = Style::fontSizeBody * scale;
  const float playPauseGlyphSize = Style::fontSizeBody * 1.2F * scale;

  m_controls->setGap(Style::spaceXs * scale);
  m_controls->setJustify(vertical ? FlexJustify::Center : FlexJustify::End);

  m_prev->setMinWidth(controlBtnSize);
  m_prev->setMinHeight(controlBtnSize);
  m_prev->setGlyphSize(glyphSize);
  m_prev->setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
  m_prev->setStyleRadius(Style::radiusMd, scale);

  m_playPause->setMinWidth(playPauseBtnSize);
  m_playPause->setMinHeight(playPauseBtnSize);
  m_playPause->setGlyphSize(playPauseGlyphSize);
  m_playPause->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
  m_playPause->setStyleRadius(Style::radiusLg, scale);

  m_next->setMinWidth(controlBtnSize);
  m_next->setMinHeight(controlBtnSize);
  m_next->setGlyphSize(glyphSize);
  m_next->setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
  m_next->setStyleRadius(Style::radiusMd, scale);

  m_controls->layout(renderer);
  m_prev->updateInputArea();
  m_playPause->updateInputArea();
  m_next->updateInputArea();
}

void DesktopMediaPlayer::applyShadow() {
  if (m_title == nullptr || m_artist == nullptr) {
    return;
  }
  if (m_shadow) {
    const float offset = kShadowOffset * contentScale();
    const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);
    m_title->setShadow(shadow, offset, offset);
    m_artist->setShadow(shadow, offset, offset);
  } else {
    m_title->clearShadow();
    m_artist->clearShadow();
  }
}

void DesktopMediaPlayer::updateVisualizerColor() {
  if (m_audioVisualizer == nullptr) {
    return;
  }
  const ColorSpec visualizerColor = scaleAlpha(m_color, kVisualizerAlpha);
  m_audioVisualizer->setGradient(visualizerColor, visualizerColor);
}

void DesktopMediaPlayer::layoutDecorations(Node& parent, float cornerRadius) {
  Node* card = &parent;
  if (card == nullptr) {
    return;
  }

  const float cardWidth = card->width();
  const float cardHeight = card->height();
  if (m_backgroundArtwork != nullptr) {
    const Color surface = withAlpha(colorForRole(ColorRole::Surface), 0.62F);
    m_backgroundArtwork->setPosition(0.0F, 0.0F);
    m_backgroundArtwork->setSize(cardWidth, cardHeight);
    if (cornerRadius > 0.0F) {
      m_backgroundArtwork->setRadius(cornerRadius);
    } else {
      m_backgroundArtwork->setStyleRadius(Style::radiusMd, contentScale());
    }
    m_backgroundArtwork->setScrim(
        ImageScrim{
            .direction = GradientDirection::Horizontal,
            .stops = {{{0.0F, surface}, {0.33F, surface}, {0.67F, surface}, {1.0F, surface}}},
            .enabled = true,
        }
    );
    m_backgroundArtwork->setVisible(m_albumArtBackground && m_backgroundArtwork->hasImage());
  }

  if (m_audioVisualizer == nullptr) {
    return;
  }
  m_audioVisualizer->setPosition(x() + m_visualizerX, y() + m_visualizerY);
  m_audioVisualizer->setSize(std::max(1.0F, m_visualizerWidth), std::max(1.0F, m_visualizerHeight));
  m_audioVisualizer->setVisible(
      m_audioVisualizerEnabled && m_hasSpectrum && m_visualizerWidth > 1.0F && m_visualizerHeight > 1.0F
  );
}
