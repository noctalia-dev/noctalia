#include "shell/bar/widgets/media_widget.h"

#include "core/log.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <wayland-client-protocol.h>

using namespace mpris;

namespace {

  const Logger kLog{"media"};

} // namespace

MediaWidget::MediaWidget(MprisService* mpris, HttpClient* httpClient, wl_output* /*output*/, Options options)
    : m_mpris(mpris), m_httpClient(httpClient), m_maxWidth(static_cast<float>(options.maxWidth)),
      m_minWidth(static_cast<float>(options.minWidth)), m_artSize(static_cast<float>(options.artSize)),
      m_titleScrollMode(options.titleScrollMode), m_hideWhenNoMedia(options.hideWhenNoMedia),
      m_albumArtOnly(options.albumArtOnly), m_hideAlbumArt(options.hideAlbumArt), m_hideArtist(options.hideArtist),
      m_artistFirst(options.artistFirst), m_showProgress(options.showProgress) {}

void MediaWidget::create() {
  auto area = ui::inputArea({});
  area->setOnEnter([this](const InputArea::PointerData&) {
    applyTitleScrollMode(m_label != nullptr && m_label->visible());
    this->requestUpdate();
  });
  area->setOnLeave([this]() {
    applyTitleScrollMode(m_label != nullptr && m_label->visible());
    this->requestUpdate();
  });
  m_area = area.get();

  area->addChild(
      ui::progressBar(
          {.out = &m_progressBar,
           .fill = scaleAlpha(widgetForegroundOr(colorSpecFromRole(ColorRole::Primary)), 0.25F),
           .track = clearColorSpec(),
           .visible = false,
           .participatesInLayout = false,
           .configure = [](ProgressBar& bar) {
             bar.setZIndex(-1);
             bar.setHitTestVisible(false);
             bar.setProgress(0.0F);
           }}
      )
  );
  area->addChild(
      ui::image({
          .out = &m_art,
          .fit = ImageFit::Cover,
          .radius = (m_artSize * m_contentScale) * 0.5F,
          .width = m_artSize * m_contentScale,
          .height = m_artSize * m_contentScale,
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxWidth = m_maxWidth * m_contentScale,
          .maxLines = 1,
          .autoScroll = false,
      })
  );

  area->addChild(
      ui::glyph({
          .out = &m_emptyGlyph,
          .glyph = "music-off",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );

  setRoot(std::move(area));
}

void MediaWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr
      || m_art == nullptr
      || m_label == nullptr
      || m_emptyGlyph == nullptr
      || m_progressBar == nullptr) {
    return;
  }
  const bool wasVertical = m_isVertical;
  m_isVertical = containerHeight > containerWidth;
  const auto active = activePlayer();
  syncState(renderer, active);

  const bool artOnly = m_isVertical || m_albumArtOnly;
  const float maxLength = std::max(0.0F, m_maxWidth * m_contentScale);
  const float minLength = std::clamp(m_minWidth * m_contentScale, 0.0F, maxLength);
  const bool showProgressFill = progressFillEligible(active);

  m_label->setColor(
      m_lastPlaybackStatus == "Playing" ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                                        : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_emptyGlyph->setGlyph(m_lastPlaybackStatus.empty() ? "disc-filled" : "music-off");
  m_emptyGlyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_emptyGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_emptyGlyph->measure(renderer);

  const bool hideAlbumArt = m_hideAlbumArt && !m_isVertical;
  const bool showArtSlot = !hideAlbumArt && m_art->hasImage();

  // Clamp art to the label's single-line height so oversized art_size cannot
  // distort the bar capsule. The bar uses a uniform cross-axis extent derived
  // from the same reference metrics.
  float artSize = 0.0F;
  if (showArtSlot) {
    const float requestedArtSize = m_artSize * m_contentScale;
    artSize = artOnly ? requestedArtSize : std::min(requestedArtSize, m_label->height());
    m_art->setVisible(true);
    m_art->setSize(artSize, artSize);
    m_art->setRadius(artSize * 0.5F);
  } else {
    m_art->setVisible(false);
    m_art->setSize(0.0F, 0.0F);
    m_art->setRadius(0.0F);
  }

  const bool showEmptyGlyph = !showArtSlot && !hideAlbumArt;
  m_label->setVisible(!artOnly && !m_label->text().empty());
  m_emptyGlyph->setVisible(showEmptyGlyph);
  const bool showLabel = m_label->visible();
  applyTitleScrollMode(showLabel);

  const float leadingWidth = showArtSlot ? artSize : (showEmptyGlyph ? m_emptyGlyph->width() : 0.0F);
  const float spacing = showLabel && leadingWidth > 0.0F ? Style::spaceXs : 0.0F;
  const float labelMaxWidth = showLabel ? std::max(0.0F, maxLength - leadingWidth - spacing) : 0.0F;
  m_label->setMaxWidth(labelMaxWidth);
  m_label->measure(renderer);

  float contentHeight = showLabel ? m_label->height() : 0.0F;
  if (showArtSlot) {
    contentHeight = std::max(contentHeight, artSize);
  }
  if (showEmptyGlyph) {
    contentHeight = std::max(contentHeight, m_emptyGlyph->height());
  }
  if (artOnly) {
    if (showArtSlot) {
      m_art->setPosition(0.0F, 0.0F);
      rootNode->setSize(artSize, artSize);
    } else if (showEmptyGlyph) {
      m_art->setPosition(0.0F, 0.0F);
      m_emptyGlyph->setPosition(0.0F, 0.0F);
      rootNode->setSize(m_emptyGlyph->width(), m_emptyGlyph->height());
    } else {
      m_art->setPosition(0.0F, 0.0F);
      m_emptyGlyph->setPosition(0.0F, 0.0F);
      rootNode->setSize(0.0F, 0.0F);
    }
  } else {
    if (showArtSlot) {
      m_art->setPosition(0.0F, std::round((contentHeight - artSize) * 0.5F));
      m_emptyGlyph->setPosition(0.0F, 0.0F);
      m_label->setPosition(artSize + spacing, std::round((contentHeight - m_label->height()) * 0.5F));
    } else if (showEmptyGlyph) {
      m_art->setPosition(0.0F, 0.0F);
      m_emptyGlyph->setPosition(0.0F, std::round((contentHeight - m_emptyGlyph->height()) * 0.5F));
      m_label->setPosition(m_emptyGlyph->width() + spacing, std::round((contentHeight - m_label->height()) * 0.5F));
    } else {
      m_art->setPosition(0.0F, 0.0F);
      m_emptyGlyph->setPosition(0.0F, 0.0F);
      m_label->setPosition(0.0F, std::round((contentHeight - m_label->height()) * 0.5F));
    }
    const float contentWidth = showLabel ? m_label->x() + m_label->width()
                                         : (showArtSlot ? artSize : (showEmptyGlyph ? m_emptyGlyph->width() : 0.0F));
    rootNode->setSize(std::clamp(contentWidth, minLength, maxLength), contentHeight);
  }
  m_progressBar->setVisible(showProgressFill);
  if (showProgressFill) {
    const float fillWidth = rootNode->width();
    const float fillHeight = rootNode->height();
    m_progressBar->setPosition(0.0F, 0.0F);
    m_progressBar->setSize(fillWidth, fillHeight);
    m_progressBar->setRadius(resolvedBarCapsuleRadius(fillWidth, fillHeight));
  }
  // The update phase owns the fill value and the timer, and it runs before layout. Re-enter it when
  // a layout-only pass changes an input it cannot observe, so the fill never shows a stale value and
  // the timer cannot stay unarmed while the fill is on screen.
  if (m_isVertical != wasVertical || showProgressFill != m_progressFillVisible) {
    m_progressFillVisible = showProgressFill;
    requestUpdate();
  }
}

void MediaWidget::doUpdate(Renderer& renderer) {
  const auto active = activePlayer();
  syncState(renderer, active);
  syncProgress(active);
}

void MediaWidget::applyTitleScrollMode(bool titleVisible) {
  if (m_label == nullptr) {
    return;
  }

  const bool shouldScroll = titleVisible
      && (m_titleScrollMode == MediaTitleScrollMode::Always
          || (m_titleScrollMode == MediaTitleScrollMode::OnHover && m_area != nullptr && m_area->hovered()));
  m_label->setAutoScroll(shouldScroll);
  m_label->setAutoScrollOnlyWhenHovered(false);
}

void MediaWidget::syncWidgetVisibility(bool hasMedia) {
  const bool showWidget = !m_hideWhenNoMedia || hasMedia;
  if (Node* rootNode = root(); rootNode != nullptr) {
    if (rootNode->visible() != showWidget || rootNode->participatesInLayout() != showWidget) {
      rootNode->setVisible(showWidget);
      rootNode->setParticipatesInLayout(showWidget);
      requestUpdate();
    }
  }
}

std::optional<MprisPlayerInfo> MediaWidget::activePlayer() const {
  return m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
}

bool MediaWidget::progressFillEligible(const std::optional<MprisPlayerInfo>& active) const noexcept {
  return m_showProgress && !m_isVertical && !m_albumArtOnly && active.has_value() && active->lengthUs > 0;
}

void MediaWidget::syncProgress(const std::optional<MprisPlayerInfo>& active) {
  if (m_progressBar == nullptr) {
    return;
  }
  // Reset while ineligible so a later reveal cannot flash the previous track's position.
  if (!progressFillEligible(active)) {
    m_progressTimer.stop();
    if (m_progressBar->progress() != 0.0F) {
      m_progressBar->setProgress(0.0F);
      requestRedraw();
    }
    return;
  }

  const float progress = static_cast<float>(active->positionUs) / static_cast<float>(active->lengthUs);
  if (std::abs(progress - m_progressBar->progress()) > 0.0005F) {
    m_progressBar->setProgress(progress);
    requestRedraw();
  }
  if (active->playbackStatus != "Playing") {
    m_progressTimer.stop();
    return;
  }
  // One wakeup per pixel of travel: a 3-minute track in a 200 px widget moves a pixel every ~900 ms.
  const float fillWidth = std::max(1.0F, m_progressBar->width());
  const auto intervalMs =
      static_cast<std::int64_t>(std::clamp(static_cast<float>(active->lengthUs / 1000) / fillWidth, 250.0F, 1000.0F));
  m_progressTimer.start(std::chrono::milliseconds(intervalMs), [this]() { requestUpdate(); });
}

void MediaWidget::syncState(Renderer& renderer, const std::optional<MprisPlayerInfo>& active) {
  if (m_art == nullptr || m_label == nullptr) {
    return;
  }

  syncWidgetVisibility(active.has_value());
  if (m_hideWhenNoMedia && !active.has_value()) {
    applyTitleScrollMode(false);
    return;
  }

  std::string playbackStatus;
  std::string displayText = i18n::tr("bar.widgets.media.nothing-playing");
  std::string artUrl;

  if (active.has_value()) {
    playbackStatus = active->playbackStatus;
    displayText = buildDisplayText(*active, m_hideArtist, m_artistFirst);
    artUrl = effectiveArtUrl(*active);
  }

  const bool textChanged = displayText != m_lastText;
  const bool artChanged = artUrl != m_lastArtUrl;
  const bool playbackChanged = playbackStatus != m_lastPlaybackStatus;
  const bool artAwaitingDecode = !artUrl.empty() && !m_art->hasImage();
  if (!textChanged && !artChanged && !playbackChanged && !artAwaitingDecode) {
    return;
  }

  if (playbackChanged && !textChanged && !artChanged && !artAwaitingDecode) {
    m_lastPlaybackStatus = playbackStatus;
    m_label->setColor(
        m_lastPlaybackStatus == "Playing" ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                                          : colorSpecFromRole(ColorRole::OnSurfaceVariant)
    );
    requestRedraw();
    return;
  }

  m_lastText = displayText;
  m_lastArtUrl = artUrl;
  m_lastPlaybackStatus = playbackStatus;

  if (textChanged) {
    m_label->setText(m_lastText);
  }
  m_label->setColor(
      m_lastPlaybackStatus == "Playing" ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                                        : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );

  const int artDecodePx = static_cast<int>(std::round(64.0F * m_contentScale));
  if (artChanged) {
    if (m_hideAlbumArt) {
      m_art->clear(renderer);
    } else {
      const std::string artPath = resolveArtworkSource(
          m_httpClient, m_pendingArtDownloads, m_lastArtUrl, [this] { requestUpdate(); }, m_aliveGuard
      );
      if (!artPath.empty()) {
        if (!m_art->setSourceFile(renderer, artPath, artDecodePx, true, true)) {
          kLog.warn(R"(artwork load failed url="{}" path="{}")", m_lastArtUrl, artPath);
          m_art->clear(renderer);
        } else {
          kLog.debug(R"(artwork loaded url="{}" path="{}")", m_lastArtUrl, artPath);
        }
      } else {
        if (!m_lastArtUrl.empty()) {
          kLog.debug("artwork unresolved url=\"{}\"", m_lastArtUrl);
        }
        m_art->clear(renderer);
      }
    }
  } else if (!m_lastArtUrl.empty() && !m_art->hasImage() && !m_hideAlbumArt) {
    const std::string artPath = cachedArtworkPath(m_lastArtUrl);
    if (!artPath.empty()) {
      if (m_art->setSourceFile(renderer, artPath, artDecodePx, false, true)) {
        requestRedraw();
      }
    }
  }

  if (textChanged || artChanged) {
    requestUpdate();
  } else {
    requestRedraw();
  }
}

std::string MediaWidget::buildDisplayText(const MprisPlayerInfo& player, bool hideArtist, bool artistFirst) {
  const std::string artists = hideArtist ? std::string() : joinArtists(player.artists);
  if (!player.title.empty() && !artists.empty()) {
    if (artistFirst) {
      return artists + " - " + player.title;
    }
    return player.title + " - " + artists;
  }
  if (!player.title.empty()) {
    return player.title;
  }
  if (!artists.empty()) {
    return artists;
  }
  if (!player.identity.empty()) {
    return player.identity;
  }
  if (!player.busName.empty()) {
    return player.busName;
  }
  if (player.playbackStatus == "Playing") {
    return i18n::tr("bar.widgets.media.playing");
  }
  return i18n::tr("bar.widgets.media.nothing-playing");
}
