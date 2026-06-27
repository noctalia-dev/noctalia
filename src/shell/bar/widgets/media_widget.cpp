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
#include <cmath>
#include <linux/input-event-codes.h>

using namespace mpris;

namespace {

  const Logger kLog{"media"};

} // namespace

MediaWidget::MediaWidget(
    MprisService* mpris, HttpClient* httpClient, wl_output* /*output*/, float maxWidth, float minWidth, float artSize,
    MediaTitleScrollMode titleScrollMode, bool hideWhenNoMedia, bool albumArtOnly
)
    : m_mpris(mpris), m_httpClient(httpClient), m_maxWidth(maxWidth), m_minWidth(minWidth), m_artSize(artSize),
      m_titleScrollMode(titleScrollMode), m_hideWhenNoMedia(hideWhenNoMedia), m_albumArtOnly(albumArtOnly) {}

void MediaWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  area->setOnEnter([this](const InputArea::PointerData&) {
    applyTitleScrollMode(m_label != nullptr && m_label->visible());
    this->requestUpdate();
  });
  area->setOnLeave([this]() {
    applyTitleScrollMode(m_label != nullptr && m_label->visible());
    this->requestUpdate();
  });
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      requestPanelToggle("control-center", "media");
      return;
    }
    if (data.button == BTN_RIGHT && m_mpris != nullptr) {
      m_mpris->playPauseActive();
    }
  });
  m_area = area.get();

  area->addChild(
      ui::image({
          .out = &m_art,
          .fit = ImageFit::Cover,
          .radius = (m_artSize * m_contentScale) * 0.5f,
          .width = m_artSize * m_contentScale,
          .height = m_artSize * m_contentScale,
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxWidth = m_maxWidth * m_contentScale,
          .maxLines = 1,
          .fontWeight = labelFontWeight(),
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
  if (rootNode == nullptr || m_art == nullptr || m_label == nullptr || m_emptyGlyph == nullptr) {
    return;
  }
  syncState(renderer);

  const bool isVertical = containerHeight > containerWidth;
  const bool artOnly = isVertical || m_albumArtOnly;
  const float maxLength = std::max(0.0f, m_maxWidth * m_contentScale);
  const float minLength = std::clamp(m_minWidth * m_contentScale, 0.0f, maxLength);

  m_label->setColor(
      m_lastPlaybackStatus == "Playing" ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                                        : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_emptyGlyph->setGlyph(m_lastPlaybackStatus.empty() ? "disc-filled" : "music-off");
  m_emptyGlyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_emptyGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_emptyGlyph->measure(renderer);

  const bool showArtSlot = m_art->hasImage();

  // Clamp art to the label's single-line height so oversized art_size cannot
  // distort the bar capsule. The bar uses a uniform cross-axis extent derived
  // from the same reference metrics.
  float artSize = 0.0f;
  if (showArtSlot) {
    const float requestedArtSize = m_artSize * m_contentScale;
    artSize = artOnly ? requestedArtSize : std::min(requestedArtSize, m_label->height());
    m_art->setVisible(true);
    m_art->setSize(artSize, artSize);
    m_art->setRadius(artSize * 0.5f);
  } else {
    m_art->setVisible(false);
    m_art->setSize(0.0f, 0.0f);
    m_art->setRadius(0.0f);
  }

  const bool showEmptyGlyph = !showArtSlot;
  m_label->setVisible(!artOnly && !m_label->text().empty());
  m_emptyGlyph->setVisible(showEmptyGlyph);
  const bool showLabel = m_label->visible();
  applyTitleScrollMode(showLabel);

  const float leadingWidth = showArtSlot ? artSize : (showEmptyGlyph ? m_emptyGlyph->width() : 0.0f);
  const float spacing = showLabel && leadingWidth > 0.0f ? Style::spaceXs : 0.0f;
  const float labelMaxWidth = showLabel ? std::max(0.0f, maxLength - leadingWidth - spacing) : 0.0f;
  m_label->setMaxWidth(labelMaxWidth);
  m_label->measure(renderer);

  float contentHeight = showLabel ? m_label->height() : 0.0f;
  if (showArtSlot) {
    contentHeight = std::max(contentHeight, artSize);
  }
  if (showEmptyGlyph) {
    contentHeight = std::max(contentHeight, m_emptyGlyph->height());
  }
  if (artOnly) {
    if (!showArtSlot) {
      m_art->setPosition(0.0f, 0.0f);
      m_emptyGlyph->setPosition(0.0f, 0.0f);
      rootNode->setSize(m_emptyGlyph->width(), m_emptyGlyph->height());
    } else {
      m_art->setPosition(0.0f, 0.0f);
      rootNode->setSize(artSize, artSize);
    }
  } else {
    if (showArtSlot) {
      m_art->setPosition(0.0f, std::round((contentHeight - artSize) * 0.5f));
      m_emptyGlyph->setPosition(0.0f, 0.0f);
      m_label->setPosition(artSize + spacing, std::round((contentHeight - m_label->height()) * 0.5f));
    } else {
      m_art->setPosition(0.0f, 0.0f);
      m_emptyGlyph->setPosition(0.0f, std::round((contentHeight - m_emptyGlyph->height()) * 0.5f));
      m_label->setPosition(m_emptyGlyph->width() + spacing, std::round((contentHeight - m_label->height()) * 0.5f));
    }
    const float contentWidth =
        showLabel ? m_label->x() + m_label->width() : (showArtSlot ? artSize : m_emptyGlyph->width());
    rootNode->setSize(std::clamp(contentWidth, minLength, maxLength), contentHeight);
  }
}

void MediaWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

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

void MediaWidget::syncState(Renderer& renderer) {
  if (m_art == nullptr || m_label == nullptr) {
    return;
  }

  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
  syncWidgetVisibility(active.has_value());
  if (m_hideWhenNoMedia && !active.has_value()) {
    return;
  }

  std::string playbackStatus;
  std::string displayText = i18n::tr("bar.widgets.media.nothing-playing");
  std::string artUrl;

  if (active.has_value()) {
    playbackStatus = active->playbackStatus;
    displayText = buildDisplayText(*active);
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

  const int artDecodePx = static_cast<int>(std::round(64.0f * m_contentScale));
  if (artChanged) {
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
  } else if (!m_lastArtUrl.empty() && !m_art->hasImage()) {
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

std::string MediaWidget::buildDisplayText(const MprisPlayerInfo& player) {
  const std::string artists = joinArtists(player.artists);
  if (!player.title.empty() && !artists.empty()) {
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
