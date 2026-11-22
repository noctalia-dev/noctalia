#include "shell/desktop/widgets/desktop_media_player_widget.h"

#include "core/ui_phase.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "media_player_widget.h"
#include "net/http_client.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/core/renderer.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "visuals/audio_visualizer.h"

#include <algorithm>
#include <cmath>

using namespace mpris;

namespace {
  constexpr int kMaxBackgroundBlurRadius = 96;
  constexpr int kMinBackgroundArtDecodeSize = 384;
  constexpr int kMaxArtworkDecodeSize = 1536;
  constexpr int kMaxBackgroundArtDecodeSize = 768;
  constexpr int kVisualizerBands = 16;

  int quantizeDecodeSize(float size, int minimum, int maximum) {
    constexpr int kStep = 64;
    const int rounded = static_cast<int>(std::ceil(std::max(1.0F, size) / static_cast<float>(kStep))) * kStep;
    return std::clamp(rounded, minimum, maximum);
  }

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
  auto view = std::make_unique<DesktopMediaPlayer>();
  m_view = view.get();
  view->setActions(
      [this] {
        if (m_mpris != nullptr) {
          m_mpris->previousActive();
          requestRedraw();
        }
      },
      [this] {
        if (m_mpris != nullptr) {
          m_mpris->playPauseActive();
          requestRedraw();
        }
      },
      [this] {
        if (m_mpris != nullptr) {
          m_mpris->nextActive();
          requestRedraw();
        }
      }
  );
  setRoot(std::move(view));
  m_view->attachDecorations(*presentationRoot(), [this] { requestLayout(); });
  m_artwork = &m_view->artwork();
  m_backgroundArtwork = &m_view->backgroundArtwork();
  m_audioVisualizer = &m_view->visualizer();
  syncStyle();
  updateSpectrumSubscription();
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
      syncStyle();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      syncStyle();
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

void DesktopMediaPlayerWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  if (m_view != nullptr) {
    m_view->setFontFamily(family);
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
  if (m_view == nullptr) {
    return;
  }
  applyVisibility();
  sync(renderer);
  syncStyle();
  m_view->setContentScale(contentScale());
  m_view->setViewport(boxInnerWidth(), boxInnerHeight());
  m_view->layout(renderer);
  m_contentScale = m_view->contentScale();
}

void DesktopMediaPlayerWidget::layoutDecorations() {
  if (m_view != nullptr && presentationRoot() != nullptr) {
    syncStyle();
    m_view->layoutDecorations(*presentationRoot(), backgroundRadius());
  }
}

void DesktopMediaPlayerWidget::refreshArtworkTextures(Renderer& renderer) {
  if (m_artPath.empty() || m_artwork == nullptr) {
    return;
  }

  const int foregroundTarget =
      quantizeDecodeSize(std::max(m_artwork->width(), m_artwork->height()), 64, kMaxArtworkDecodeSize);
  m_artwork->setSourceFile(
      renderer, m_artPath, {.targetSize = foregroundTarget, .mipmap = true, .centerSquareCrop = true}
  );

  Node* card = presentationRoot();
  if (!m_albumArtBackground || m_backgroundArtwork == nullptr || card == nullptr) {
    return;
  }

  const float cardWidth = card->width();
  const float cardHeight = card->height();
  const int backgroundTarget =
      quantizeDecodeSize(std::max(cardWidth, cardHeight), kMinBackgroundArtDecodeSize, kMaxBackgroundArtDecodeSize);
  m_backgroundArtwork->setSourceFile(
      renderer, m_artPath,
      {.targetSize = backgroundTarget, .mipmap = true, .blurRadius = static_cast<float>(m_albumArtBlur)}
  );
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
  if (m_audioVisualizer->advance(deltaMs, m_spectrum != nullptr && !m_spectrum->idle())) {
    requestRedraw();
  }
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
    m_spectrumListenerId = m_spectrum->addChangeListener(kVisualizerBands, [this] {
      m_pendingSpectrumUpdate = true;
      requestFrameTick();
    });
    m_pendingSpectrumUpdate = true;
  }
}

void DesktopMediaPlayerWidget::sync(Renderer& renderer) {
  if (m_view == nullptr) {
    return;
  }

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
  // A remote art URL without a local path yet keeps syncing until its download lands in the artwork cache.
  const bool artworkPending = m_artPath.empty() && !artUrl.empty();
  if (!titleChanged
      && !artistChanged
      && !artChanged
      && !statusChanged
      && !canGoPreviousChanged
      && !canGoNextChanged
      && !artworkPending) {
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

  m_view->setPlayback(
      m_lastTitle.empty() ? i18n::tr("desktop-widgets.media.nothing-playing") : m_lastTitle, m_lastArtist,
      m_lastPlaybackStatus == "Playing", canGoPrevious, canGoNext
  );

  if (artChanged && m_artPath.empty()) {
    if (m_artwork != nullptr) {
      m_artwork->clear(renderer);
    }
    if (m_backgroundArtwork != nullptr) {
      m_backgroundArtwork->clear(renderer);
    }
  }

  if (titleChanged || artistChanged || canGoPreviousChanged || canGoNextChanged || artPathChanged) {
    requestLayout();
  } else {
    requestRedraw();
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

void DesktopMediaPlayerWidget::syncStyle() {
  if (m_view != nullptr) {
    m_view->setOptions(
        {.vertical = m_vertical,
         .color = m_color,
         .shadow = m_shadow,
         .albumArtBackground = m_albumArtBackground,
         .audioVisualizer = m_audioVisualizerEnabled,
         .hasSpectrum = m_spectrum != nullptr}
    );
  }
}
