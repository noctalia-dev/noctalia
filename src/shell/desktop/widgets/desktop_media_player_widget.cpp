#include "shell/desktop/widgets/desktop_media_player_widget.h"

#include "core/ui_phase.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/visuals/audio_visualizer.h"

#include <algorithm>
#include <cmath>

using namespace mpris;

namespace {

  constexpr float kArtSize = 120.0F;
  constexpr float kControlSize = 32.0F;
  constexpr float kPlayPauseSize = 40.0F;
  constexpr float kSpacing = 6.0F;
  constexpr float kBoxedHorizontalBreakpoint = 1.15F;
  constexpr int kMaxBackgroundBlurRadius = 96;
  constexpr int kMinBackgroundArtDecodeSize = 384;
  constexpr int kMaxArtworkDecodeSize = 1536;
  constexpr int kMaxBackgroundArtDecodeSize = 768;
  constexpr int kVisualizerBands = 16;
  constexpr float kVisualizerHeight = 26.0F;
  constexpr float kVisualizerAlpha = 0.5F;

  int quantizeDecodeSize(float size, int minimum, int maximum) {
    constexpr int kStep = 64;
    const int rounded = static_cast<int>(std::ceil(std::max(1.0F, size) / static_cast<float>(kStep))) * kStep;
    return std::clamp(rounded, minimum, maximum);
  }

} // namespace

namespace {

  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;

} // namespace

DesktopMediaPlayerWidget::DesktopMediaPlayerWidget(
    MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, Options options
)
    : m_mpris(mpris), m_httpClient(httpClient), m_spectrum(spectrum), m_vertical(options.vertical),
      m_color(options.color), m_shadow(options.shadow), m_hideWhenNoMedia(options.hideWhenNoMedia),
      m_albumArtBackground(options.albumArtBackground),
      m_albumArtBlur(std::clamp(options.albumArtBlur, 0, kMaxBackgroundBlurRadius)),
      m_audioVisualizerEnabled(options.audioVisualizer) {}

DesktopMediaPlayerWidget::~DesktopMediaPlayerWidget() {
  if (m_spectrum != nullptr && m_spectrumListenerId != 0) {
    m_spectrum->removeChangeListener(m_spectrumListenerId);
  }
  m_aliveGuard.reset();
}

void DesktopMediaPlayerWidget::create() {
  auto rootNode = ui::node({});
  rootNode->setZIndex(2);

  auto artwork = ui::image({
      .out = &m_artwork,
      .fit = ImageFit::Cover,
      .radius = Style::scaledRadiusMd(contentScale()),
  });
  rootNode->addChild(std::move(artwork));

  auto title = ui::label({
      .out = &m_title,
      .fontWeight = FontWeight::Bold,
      .color = m_color,
      .maxLines = 1,
  });
  rootNode->addChild(std::move(title));

  auto artist = ui::label({
      .out = &m_artist,
      .color = m_color,
      .maxLines = 1,
  });
  rootNode->addChild(std::move(artist));

  auto controls = ui::row(
      {
          .out = &m_controls,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
      },
      ui::button({
          .out = &m_prev,
          .glyph = "media-prev",
          .variant = ButtonVariant::Ghost,
          .onClick =
              [this]() {
                if (m_mpris != nullptr) {
                  m_mpris->previousActive();
                  requestRedraw();
                }
              },
      }),
      ui::button({
          .out = &m_playPause,
          .glyph = "media-play",
          .variant = ButtonVariant::Primary,
          .onClick =
              [this]() {
                if (m_mpris != nullptr) {
                  m_mpris->playPauseActive();
                  requestRedraw();
                }
              },
      }),
      ui::button({
          .out = &m_next,
          .glyph = "media-next",
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              m_mpris->nextActive();
              requestRedraw();
            }
          },
      })
  );

  rootNode->addChild(std::move(controls));
  setRoot(std::move(rootNode));

  auto backgroundArtwork = ui::image({
      .out = &m_backgroundArtwork,
      .fit = ImageFit::Cover,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Image& image) {
        image.setHitTestVisible(false);
        image.setZIndex(0);
      },
  });
  presentationRoot()->addChild(std::move(backgroundArtwork));

  auto visualizer = std::make_unique<AudioVisualizer>();
  visualizer->setOrientation(AudioSpectrumOrientation::Horizontal);
  visualizer->setCentered(false);
  visualizer->setMirrored(false);
  visualizer->setVisible(false);
  visualizer->setParticipatesInLayout(false);
  visualizer->setHitTestVisible(false);
  visualizer->setZIndex(1);
  m_audioVisualizer = visualizer.get();
  presentationRoot()->addChild(std::move(visualizer));

  updateVisualizerColor();
  updateSpectrumSubscription();
  applyShadow();
}

void DesktopMediaPlayerWidget::layout(Renderer& renderer) {
  if (boxInnerWidth() <= 0.0F || boxInnerHeight() <= 0.0F) {
    DesktopWidget::layout(renderer);
    refreshArtworkTextures(renderer);
    layoutDecorations();
    return;
  }

  UiPhaseScope layoutPhase(UiPhase::Layout);
  m_inLayout = true;
  struct InLayoutReset {
    bool& flag;
    ~InLayoutReset() { flag = false; }
  } inLayoutReset{m_inLayout};

  onFontFamilyChanged(m_fontFamily, renderer);
  m_contentScale = m_baseScale;
  doLayout(renderer);
  applyBackground();
  refreshArtworkTextures(renderer);
  layoutDecorations();
}

bool DesktopMediaPlayerWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      if (m_title != nullptr)
        m_title->setColor(m_color);
      if (m_artist != nullptr)
        m_artist->setColor(m_color);
      updateVisualizerColor();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      return true;
    }
    return false;
  }
  if (key == "hide_when_no_media") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_hideWhenNoMedia = *v;
      if (applyVisibility()) {
        requestLayout();
      }
      return true;
    }
    return false;
  }
  if (key == "album_art_background") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_albumArtBackground = *v;
      if (!m_albumArtBackground && m_backgroundArtwork != nullptr) {
        m_backgroundArtwork->clear(renderer);
        m_backgroundArtwork->setVisible(false);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "album_art_blur") {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_albumArtBlur = std::clamp(static_cast<int>(*v), 0, kMaxBackgroundBlurRadius);
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "audio_visualizer") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_audioVisualizerEnabled = *v;
      updateSpectrumSubscription();
      if (m_audioVisualizer != nullptr) {
        m_audioVisualizer->setVisible(m_audioVisualizerEnabled && m_spectrum != nullptr);
      }
      if (m_audioVisualizerEnabled) {
        requestFrameTick();
      }
      requestLayout();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopMediaPlayerWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_title != nullptr) {
    m_title->setFontFamily(family);
  }
  if (m_artist != nullptr) {
    m_artist->setFontFamily(family);
  }
}

void DesktopMediaPlayerWidget::setEditorPreview(bool enabled) noexcept {
  if (m_editorPreview == enabled) {
    return;
  }
  m_editorPreview = enabled;
  if (root() == nullptr) {
    return;
  }
  if (applyVisibility()) {
    requestLayout();
  } else if (enabled && m_visible) {
    requestRedraw();
  }
}

void DesktopMediaPlayerWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_artwork == nullptr || m_title == nullptr || m_artist == nullptr || m_controls == nullptr)
    return;

  applyVisibility();
  sync(renderer);

  const float scale = contentScale();
  const float innerWidth = boxInnerWidth();
  const float innerHeight = boxInnerHeight();
  if (innerWidth > 0.0F && innerHeight > 0.0F) {
    layoutBoxed(renderer, innerWidth, innerHeight);
  } else if (m_vertical) {
    layoutVertical(renderer, scale);
  } else {
    layoutHorizontal(renderer, scale);
  }
  applyShadow();
}

void DesktopMediaPlayerWidget::layoutDecorations() {
  Node* card = presentationRoot();
  if (card == nullptr || root() == nullptr) {
    return;
  }

  const float cardWidth = card->width();
  const float cardHeight = card->height();
  const float radius = backgroundRadius() > 0.0F ? backgroundRadius() : Style::scaledRadiusMd(contentScale());
  if (m_backgroundArtwork != nullptr) {
    const Color surface = withAlpha(colorForRole(ColorRole::Surface), 0.62F);
    m_backgroundArtwork->setPosition(0.0F, 0.0F);
    m_backgroundArtwork->setSize(cardWidth, cardHeight);
    m_backgroundArtwork->setRadius(radius);
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
  m_audioVisualizer->setPosition(root()->x() + m_visualizerX, root()->y() + m_visualizerY);
  m_audioVisualizer->setSize(std::max(1.0F, m_visualizerWidth), std::max(1.0F, m_visualizerHeight));
  m_audioVisualizer->setVisible(
      m_audioVisualizerEnabled && m_spectrum != nullptr && m_visualizerWidth > 1.0F && m_visualizerHeight > 1.0F
  );
}

void DesktopMediaPlayerWidget::refreshArtworkTextures(Renderer& renderer) {
  if (m_artPath.empty() || m_artwork == nullptr) {
    return;
  }

  const int foregroundTarget =
      quantizeDecodeSize(std::max(m_artwork->width(), m_artwork->height()), 64, kMaxArtworkDecodeSize);
  if (!m_artwork->setSourceFile(renderer, m_artPath, foregroundTarget, true, true)) {
    m_artwork->clear(renderer);
  }

  Node* card = presentationRoot();
  if (!m_albumArtBackground || m_backgroundArtwork == nullptr || card == nullptr) {
    return;
  }

  const float cardWidth = card->width();
  const float cardHeight = card->height();
  const int backgroundTarget =
      quantizeDecodeSize(std::max(cardWidth, cardHeight), kMinBackgroundArtDecodeSize, kMaxBackgroundArtDecodeSize);
  if (!m_backgroundArtwork->setSourceFileBlurred(renderer, m_artPath, backgroundTarget, m_albumArtBlur, true)) {
    m_backgroundArtwork->clear(renderer);
  }
}

void DesktopMediaPlayerWidget::layoutVertical(Renderer& renderer, float scale) {
  const float artW = kArtSize * scale;
  const float spacing = kSpacing * scale;
  const float fontSize = Style::fontSizeBody * scale;

  m_artwork->setSize(artW, artW);
  m_artwork->setRadius(Style::scaledRadiusMd(scale));
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
  m_visualizerHeight = m_audioVisualizerEnabled && m_spectrum != nullptr ? kVisualizerHeight * scale : 0.0F;
  const float visualizerSpacing = m_visualizerHeight > 0.0F ? spacing : 0.0F;
  const float placedControlsY = controlsY + m_visualizerHeight + visualizerSpacing;
  const float controlsX = std::round((artW - m_controls->width()) * 0.5F);
  m_controls->setPosition(controlsX, placedControlsY);

  root()->setSize(artW, placedControlsY + m_controls->height());
}

void DesktopMediaPlayerWidget::layoutHorizontal(Renderer& renderer, float scale) {
  const float artH = kArtSize * scale;
  const float spacing = kSpacing * scale;
  const float fontSize = Style::fontSizeBody * scale;
  const float textWidth = artH * 1.5F;

  m_artwork->setSize(artH, artH);
  m_artwork->setRadius(Style::scaledRadiusMd(scale));
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

  root()->setSize(totalWidth, artH);
}

void DesktopMediaPlayerWidget::layoutBoxed(Renderer& renderer, float width, float height) {
  const bool vertical = width / std::max(1.0F, height) < kBoxedHorizontalBreakpoint;

  m_contentScale = m_baseScale;
  if (vertical) {
    layoutVertical(renderer, m_baseScale);
  } else {
    layoutHorizontal(renderer, m_baseScale);
  }

  const float naturalWidth = std::max(1.0F, root()->width());
  const float naturalHeight = std::max(1.0F, root()->height());
  const float fittedScale =
      std::clamp(m_baseScale * std::min(width / naturalWidth, height / naturalHeight), 0.05F, 20.0F);
  m_contentScale = fittedScale;
  if (vertical) {
    layoutVertical(renderer, fittedScale);
  } else {
    layoutHorizontal(renderer, fittedScale);
  }

  const float offsetX = std::round((width - root()->width()) * 0.5F);
  const float offsetY = std::round((height - root()->height()) * 0.5F);
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
  root()->setSize(width, height);
}

void DesktopMediaPlayerWidget::layoutButtons(Renderer& renderer, float scale, bool vertical) {
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
  m_prev->setRadius(Style::scaledRadiusMd(scale));

  m_playPause->setMinWidth(playPauseBtnSize);
  m_playPause->setMinHeight(playPauseBtnSize);
  m_playPause->setGlyphSize(playPauseGlyphSize);
  m_playPause->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
  m_playPause->setRadius(Style::scaledRadiusLg(scale));

  m_next->setMinWidth(controlBtnSize);
  m_next->setMinHeight(controlBtnSize);
  m_next->setGlyphSize(glyphSize);
  m_next->setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
  m_next->setRadius(Style::scaledRadiusMd(scale));

  m_controls->layout(renderer);
  m_prev->updateInputArea();
  m_playPause->updateInputArea();
  m_next->updateInputArea();
}

void DesktopMediaPlayerWidget::doUpdate(Renderer& renderer) {
  if (applyVisibility()) {
    requestLayout();
  }
  sync(renderer);
}

bool DesktopMediaPlayerWidget::needsFrameTick() const {
  return m_audioVisualizerEnabled
      && m_audioVisualizer != nullptr
      && m_spectrum != nullptr
      && (m_pendingSpectrumUpdate || !m_audioVisualizer->converged() || !m_spectrum->idle());
}

void DesktopMediaPlayerWidget::onFrameTick(float deltaMs, Renderer& /*renderer*/) {
  if (!m_audioVisualizerEnabled || m_audioVisualizer == nullptr) {
    return;
  }
  syncSpectrum();
  m_audioVisualizer->tick(deltaMs);
  requestRedraw();
}

void DesktopMediaPlayerWidget::syncSpectrum() {
  if (m_audioVisualizer == nullptr || m_spectrum == nullptr || m_spectrumListenerId == 0) {
    return;
  }
  const bool shouldPull = m_pendingSpectrumUpdate || !m_spectrum->idle();
  if (!shouldPull) {
    return;
  }
  m_audioVisualizer->setValues(m_spectrum->values(m_spectrumListenerId));
  m_pendingSpectrumUpdate = false;
}

void DesktopMediaPlayerWidget::updateSpectrumSubscription() {
  if (m_spectrum == nullptr) {
    return;
  }
  if (!m_audioVisualizerEnabled) {
    if (m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
    m_pendingSpectrumUpdate = false;
    if (m_audioVisualizer != nullptr) {
      m_audioVisualizer->setValues({});
    }
    return;
  }
  if (m_spectrumListenerId == 0) {
    m_spectrumListenerId = m_spectrum->addChangeListener(kVisualizerBands, [this]() {
      m_pendingSpectrumUpdate = true;
      requestFrameTick();
    });
    m_pendingSpectrumUpdate = true;
  }
}

void DesktopMediaPlayerWidget::updateVisualizerColor() {
  if (m_audioVisualizer == nullptr) {
    return;
  }
  const ColorSpec visualizerColor = scaleAlpha(m_color, kVisualizerAlpha);
  m_audioVisualizer->setGradient(visualizerColor, visualizerColor);
}

void DesktopMediaPlayerWidget::sync(Renderer& renderer) {
  if (m_title == nullptr || m_artist == nullptr || m_playPause == nullptr)
    return;

  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;

  std::string title;
  std::string artist;
  std::string artUrl;
  std::string playbackStatus;
  bool canGoPrevious = false;
  bool canGoNext = false;

  if (active.has_value()) {
    title = active->title;
    artist = joinArtists(active->artists);
    artUrl = effectiveArtUrl(*active);
    playbackStatus = active->playbackStatus;
    canGoPrevious = active->canGoPrevious;
    canGoNext = active->canGoNext;
  }

  const bool titleChanged = title != m_lastTitle;
  const bool artistChanged = artist != m_lastArtist;
  const bool artChanged = artUrl != m_lastArtUrl;
  const bool statusChanged = playbackStatus != m_lastPlaybackStatus;
  const bool canGoPreviousChanged = canGoPrevious != m_lastCanGoPrevious;
  const bool canGoNextChanged = canGoNext != m_lastCanGoNext;
  const bool foregroundArtAwaitingDecode = m_artwork != nullptr && !artUrl.empty() && !m_artwork->hasImage();
  const bool backgroundArtAwaitingDecode =
      m_albumArtBackground && m_backgroundArtwork != nullptr && !artUrl.empty() && !m_backgroundArtwork->hasImage();
  if (!titleChanged
      && !artistChanged
      && !artChanged
      && !statusChanged
      && !canGoPreviousChanged
      && !canGoNextChanged
      && !foregroundArtAwaitingDecode
      && !backgroundArtAwaitingDecode) {
    return;
  }

  m_lastTitle = title;
  m_lastArtist = artist;
  m_lastArtUrl = artUrl;
  m_lastPlaybackStatus = playbackStatus;
  m_lastCanGoPrevious = canGoPrevious;
  m_lastCanGoNext = canGoNext;

  bool artPathChanged = false;
  if (artChanged) {
    const std::string artPath = resolveArtworkSource(
        m_httpClient, m_pendingArtDownloads, m_lastArtUrl, [this] { requestUpdate(); }, m_aliveGuard
    );
    artPathChanged = artPath != m_artPath;
    m_artPath = artPath;
  } else if (m_artPath.empty() && !m_lastArtUrl.empty()) {
    const std::string artPath = cachedArtworkPath(m_lastArtUrl);
    if (!artPath.empty()) {
      m_artPath = artPath;
      artPathChanged = true;
    }
  }

  m_title->setText(m_lastTitle.empty() ? i18n::tr("desktop-widgets.media.nothing-playing") : m_lastTitle);
  m_artist->setText(m_lastArtist);
  m_artist->setVisible(!m_lastArtist.empty());

  m_playPause->setGlyph(m_lastPlaybackStatus == "Playing" ? "media-pause" : "media-play");
  if (m_prev != nullptr) {
    m_prev->setVisible(canGoPrevious);
  }
  if (m_next != nullptr) {
    m_next->setVisible(canGoNext);
  }

  if (artChanged && m_artPath.empty()) {
    if (m_artwork != nullptr) {
      m_artwork->clear(renderer);
    }
    if (m_backgroundArtwork != nullptr) {
      m_backgroundArtwork->clear(renderer);
    }
  }

  const bool artworkNeedsLayout = !m_artPath.empty() && (foregroundArtAwaitingDecode || backgroundArtAwaitingDecode);
  if (titleChanged
      || artistChanged
      || canGoPreviousChanged
      || canGoNextChanged
      || artPathChanged
      || artworkNeedsLayout) {
    requestLayout();
  } else {
    requestRedraw();
  }
}

void DesktopMediaPlayerWidget::applyShadow() {
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

bool DesktopMediaPlayerWidget::hasActiveMedia() const {
  return m_mpris != nullptr && m_mpris->activePlayer().has_value();
}

bool DesktopMediaPlayerWidget::shouldBeVisible() const {
  return m_editorPreview || !m_hideWhenNoMedia || hasActiveMedia();
}

bool DesktopMediaPlayerWidget::applyVisibility() {
  if (presentationRoot() == nullptr) {
    return false;
  }
  const bool nextVisible = shouldBeVisible();
  if (!m_visibilityInitialized) {
    m_visibilityInitialized = true;
    m_visible = nextVisible;
    presentationRoot()->setOpacity(m_visible ? 1.0F : 0.0F);
    setVisibilityCollapsed(!m_visible);
    return !m_visible;
  }

  if (m_visible == nextVisible) {
    return false;
  }

  m_visible = nextVisible;
  presentationRoot()->setOpacity(m_visible ? 1.0F : 0.0F);
  setVisibilityCollapsed(!m_visible);
  return true;
}

void DesktopMediaPlayerWidget::setVisibilityCollapsed(bool collapsed) {
  if (Node* node = presentationRoot(); node != nullptr) {
    node->setVisible(!collapsed);
  }
}
