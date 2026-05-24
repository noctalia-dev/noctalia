#include "shell/control_center/media_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/audio_spectrum.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace control_center;
using namespace mpris;

namespace {

  const Logger kLog{"media_tab"};

  // Layout-grid unit for the media tab. Calibrated visual size, decoupled from
  // Style::controlHeightLg which is a control-row height — bumping that token to
  // a roomier value would otherwise inflate the artwork, card, and menu widths
  // and overflow the panel content area.
  constexpr float kMediaUnit = 36.0f;

  constexpr float kArtworkSize = kMediaUnit * 6;
  constexpr float kMediaNowCardMinHeight = kMediaUnit * 11 + Style::spaceSm * 2;
  constexpr float kMediaControlsHeight = kMediaUnit + Style::spaceXs;
  constexpr float kMediaPlayPauseHeight = kMediaUnit + Style::spaceSm;
  constexpr float kMediaArtworkMinHeight = kMediaUnit * 4;
  constexpr auto kNoActivePlayerGrace = std::chrono::milliseconds(2000);
  constexpr auto kTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr std::int64_t kSeekArrivedToleranceUs = 1'500'000;
  constexpr std::int64_t kSeekNearZeroUs = 2'000'000;
  constexpr auto kProgressSettleHold = std::chrono::milliseconds(2500);
  constexpr auto kPendingSeekTimeout = std::chrono::milliseconds(5000);

  std::string playPauseGlyph(const std::string& playbackStatus) {
    return playbackStatus == "Playing" ? "media-pause" : "media-play";
  }

  std::string repeatGlyph(const std::string& loopStatus) { return loopStatus == "Track" ? "repeat-once" : "repeat"; }

  ButtonVariant toggleVariant(bool active) { return active ? ButtonVariant::Primary : ButtonVariant::Ghost; }
  constexpr int kVisualizerBandCount = 32;

} // namespace

MediaTab::MediaTab(
    MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, ConfigService* config,
    WaylandConnection* wayland, RenderContext* renderContext
)
    : m_mpris(mpris), m_httpClient(httpClient), m_spectrum(spectrum), m_config(config), m_wayland(wayland),
      m_renderContext(renderContext) {}

MediaTab::~MediaTab() { m_aliveGuard.reset(); }

void MediaTab::openPlayerMenu() {
  if (m_playerMenuPopup == nullptr || m_mpris == nullptr || m_playerMenuButton == nullptr) {
    return;
  }

  const auto pinnedBusName = m_mpris->pinnedPlayerPreference();
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(m_playerBusNames.size() + 1);
  entries.push_back(
      {.id = 0,
       .label = i18n::tr("control-center.media.active-player"),
       .enabled = true,
       .separator = false,
       .hasSubmenu = false}
  );
  for (std::size_t i = 0; i < m_playerBusNames.size(); ++i) {
    const auto& busName = m_playerBusNames[i];
    const bool selected = pinnedBusName.has_value() && busName == *pinnedBusName;
    std::string identity;
    if (auto it = m_mpris->players().find(busName); it != m_mpris->players().end()) {
      identity = it->second.identity;
    }
    const std::string label = (selected ? "• " : "") + (identity.empty() ? busName : identity);
    entries.push_back(
        {.id = static_cast<std::int32_t>(i + 1),
         .label = label,
         .enabled = true,
         .separator = false,
         .hasSubmenu = false}
    );
  }

  Flex* anchor = m_playerMenuButton->parent() != nullptr ? static_cast<Flex*>(m_playerMenuButton->parent())
                                                         : static_cast<Flex*>(m_nowCard);
  if (anchor == nullptr) {
    return;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return;
  }

  float anchorAbsX = 0.0f;
  float anchorAbsY = 0.0f;
  Node::absolutePosition(anchor, anchorAbsX, anchorAbsY);

  const float scale = contentScale();
  const float menuWidth = std::clamp(
      kMediaUnit * 6.0f * scale, kMediaUnit * 4.2f * scale,
      m_nowCard != nullptr ? std::max(1.0f, m_nowCard->width()) : 240.0f * scale
  );

  if (m_config != nullptr) {
    m_playerMenuPopup->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_playerMenuPopup.get());

  m_playerMenuPopup->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  m_playerMenuPopup->open(
      std::move(entries), menuWidth, 10, static_cast<std::int32_t>(anchorAbsX), static_cast<std::int32_t>(anchorAbsY),
      static_cast<std::int32_t>(anchor->width()), static_cast<std::int32_t>(anchor->height()), parentCtx->layerSurface,
      parentCtx->output
  );

  m_playerMenuOpen = true;
}

std::unique_ptr<Flex> MediaTab::create() {
  const float scale = contentScale();

  auto tab = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto mediaColumn = ui::column({
      .out = &m_mediaColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 3.0f,
  });

  auto nowCard = ui::column({
      .out = &m_nowCard,
      .gap = Style::spaceMd * scale,
      .minHeight = kMediaNowCardMinHeight * scale,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });

  auto nowHeader = ui::row(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceSm * scale,
       .minHeight = Style::controlHeightSm * scale},
      ui::label({
          .text = i18n::tr("control-center.media.now-playing"),
          .fontSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .fontWeight = FontWeight::Bold,
          .flexGrow = 1.0f,
      }),
      ui::button({
          .out = &m_playerMenuButton,
          .glyph = "headphones",
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = [this]() {
            if (m_playerBusNames.empty()) {
              return;
            }
            if (m_playerMenuPopup != nullptr && m_playerMenuPopup->isOpen()) {
              m_playerMenuPopup->close();
              PanelManager::instance().clearActivePopup();
            } else {
              openPlayerMenu();
            }
          },
      })
  );
  nowCard->addChild(std::move(nowHeader));

  auto mediaStack = ui::column({
      .out = &m_mediaStack,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0f,
  });

  auto artworkRow = ui::row(
      {.out = &m_artworkRow, .align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 0.0f, .flexGrow = 1.0f},
      ui::image({
          .out = &m_artwork,
          .fit = ImageFit::Contain,
          .radius = Style::scaledRadiusXl(scale),
          .width = kArtworkSize * scale,
          .height = kArtworkSize * scale,
      })
  );
  mediaStack->addChild(std::move(artworkRow));

  mediaStack->addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
          ui::label({
              .out = &m_trackTitle,
              .text = i18n::tr("control-center.media.nothing-playing"),
              .fontSize = (Style::fontSizeTitle + Style::spaceXs) * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .fontWeight = FontWeight::Bold,
          }),
          ui::label({
              .out = &m_trackArtist,
              .text = i18n::tr("control-center.media.start-playback"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &m_trackAlbum,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Secondary),
              .visible = false,
              .configure = [](Label& label) { label.setCaptionStyle(); },
          })
      )
  );

  mediaStack->addChild(
      ui::slider({
          .out = &m_progressSlider,
          .minValue = 0.0f,
          .maxValue = 100.0f,
          .step = 1.0f,
          .trackHeight = 7.0f * scale,
          .thumbSize = 16.0f * scale,
          .controlHeight = (Style::controlHeight + Style::spaceXs) * scale,
          .onValueChanged =
              [this](float value) {
                if (m_syncingProgress || m_mpris == nullptr) {
                  return;
                }
                const auto active = m_mpris->activePlayer();
                const std::int64_t targetUs = static_cast<std::int64_t>(std::llround(value * 1000000.0f));
                const auto now = std::chrono::steady_clock::now();
                m_positionUs = targetUs;
                m_positionSampleAt = now;
                m_pendingSeekBusName = active.has_value()
                                           ? active->busName
                                           : (!m_positionBusName.empty() ? m_positionBusName : std::string{});
                m_pendingSeekUs = targetUs;
                m_pendingSeekUntil = now + kPendingSeekTimeout;
                m_progressSettleUntil = now + kProgressSettleHold;
              },
          .onDragEnd =
              [this]() {
                if (m_syncingProgress || m_mpris == nullptr || m_progressSlider == nullptr) {
                  return;
                }
                commitPendingSeek(m_progressSlider->value());
              },
      })
  );

  auto controls = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
  });

  controls->addChild(
      ui::button({
          .out = &m_repeatButton,
          .glyph = "repeat",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              const auto current = m_mpris->loopStatusActive().value_or("None");
              const std::string next = current == "None" ? "Playlist" : (current == "Playlist" ? "Track" : "None");
              (void)m_mpris->setLoopStatusActive(next);
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_prevButton,
          .glyph = "media-prev",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->previousActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_playPauseButton,
          .glyph = "media-play",
          .variant = ButtonVariant::Primary,
          .minWidth = kMediaPlayPauseHeight * scale,
          .minHeight = kMediaPlayPauseHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->playPauseActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_nextButton,
          .glyph = "media-next",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->nextActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_shuffleButton,
          .glyph = "shuffle",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              const bool enabled = m_mpris->shuffleActive().value_or(false);
              (void)m_mpris->setShuffleActive(!enabled);
              PanelManager::instance().refresh();
            });
          },
      })
  );

  auto controlsRow = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = 0.0f,
      .fillWidth = true,
  });
  controlsRow->addChild(std::move(controls));
  mediaStack->addChild(std::move(controlsRow));

  nowCard->addChild(std::move(mediaStack));
  mediaColumn->addChild(std::move(nowCard));

  auto visualizerColumn = ui::column({
      .out = &m_visualizerColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .clipChildren = true,
      .flexGrow = 2.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& column) {
        applySectionCardStyle(column, scale, opacity, borders);
      },
  });

  auto visualizerBody = ui::row({
      .out = &m_visualizerBody,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .fillWidth = true,
      .flexGrow = 1.0f,
  });

  auto visualizerSpectrum = std::make_unique<AudioSpectrum>();
  visualizerSpectrum->setGradient(colorForRole(ColorRole::Secondary), colorForRole(ColorRole::Tertiary));
  visualizerSpectrum->setOrientation(AudioSpectrumOrientation::Vertical);
  visualizerSpectrum->setMirrored(true);
  visualizerSpectrum->setCentered(true);
  visualizerSpectrum->setValues(std::vector<float>(kVisualizerBandCount, 0.0f));
  visualizerSpectrum->tick(0.0f);
  visualizerSpectrum->setFlexGrow(1.0f);
  m_visualizerSpectrum = visualizerSpectrum.get();
  visualizerBody->addChild(std::move(visualizerSpectrum));
  visualizerColumn->addChild(std::move(visualizerBody));
  tab->addChild(std::move(mediaColumn));
  tab->addChild(std::move(visualizerColumn));

  if (m_wayland != nullptr && m_renderContext != nullptr) {
    m_playerMenuPopup = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
    m_playerMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) {
      const std::weak_ptr<void> aliveGuard = m_aliveGuard;
      DeferredCall::callLater([this, aliveGuard, entry]() {
        if (aliveGuard.expired() || m_mpris == nullptr) {
          return;
        }
        if (entry.id == 0) {
          m_mpris->clearPinnedPlayerPreference();
        } else {
          const std::size_t idx = static_cast<std::size_t>(entry.id - 1);
          if (idx < m_playerBusNames.size()) {
            m_mpris->setPinnedPlayerPreference(m_playerBusNames[idx]);
          }
        }
        PanelManager::instance().refresh();
      });
    });
  }

  return tab;
}

void MediaTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr || m_nowCard == nullptr || m_mediaStack == nullptr) {
    return;
  }

  const float scale = contentScale();
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  const float cardInnerWidth =
      std::max(0.0f, m_nowCard->width() - (m_nowCard->paddingLeft() + m_nowCard->paddingRight()));
  const float mediaWidth = std::clamp(cardInnerWidth, 1.0f, kMediaUnit * 11.0f * scale);
  const float mediaStackHeight = m_mediaStack->height();
  m_mediaStack->setSize(mediaWidth, mediaStackHeight);

  if (m_artworkRow != nullptr) {
    // Horizontal Flex with justify Center under-reports its width when the child is narrower than
    // the stretched cross-axis; min width keeps the row full-bleed so art centers.
    m_artworkRow->setMinWidth(mediaWidth);
  }

  if (m_artwork != nullptr) {
    const float sideButtonSize = kMediaControlsHeight * scale;
    const float playPauseButtonSize = kMediaPlayPauseHeight * scale;
    const float sideGlyphSize = Style::fontSizeTitle * scale;
    const float playPauseGlyphSize = (Style::fontSizeTitle + Style::spaceXs) * scale;

    for (auto* button : {m_repeatButton, m_prevButton, m_nextButton, m_shuffleButton}) {
      if (button != nullptr) {
        button->setMinWidth(sideButtonSize);
        button->setMinHeight(sideButtonSize);
        button->setGlyphSize(sideGlyphSize);
        button->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
        button->setRadius(Style::scaledRadiusLg(scale));
      }
    }
    if (m_playPauseButton != nullptr) {
      m_playPauseButton->setMinWidth(playPauseButtonSize);
      m_playPauseButton->setMinHeight(playPauseButtonSize);
      m_playPauseButton->setGlyphSize(playPauseGlyphSize);
      m_playPauseButton->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
      m_playPauseButton->setRadius(Style::scaledRadiusLg(scale));
    }
  }

  if (m_trackTitle != nullptr) {
    m_trackTitle->setMaxWidth(mediaWidth);
  }
  if (m_trackArtist != nullptr) {
    m_trackArtist->setMaxWidth(mediaWidth);
  }
  if (m_trackAlbum != nullptr) {
    m_trackAlbum->setMaxWidth(mediaWidth);
  }
  if (m_progressSlider != nullptr) {
    m_progressSlider->setSize(mediaWidth, 0.0f);
  }

  m_mediaStack->layout(renderer);

  if (m_artwork != nullptr && m_artworkRow != nullptr) {
    const float artWidth =
        std::max(1.0f, m_artworkRow->width() - (m_artworkRow->paddingLeft() + m_artworkRow->paddingRight()));
    const float artHeight = std::max(
        kMediaArtworkMinHeight * scale,
        m_artworkRow->height() - (m_artworkRow->paddingTop() + m_artworkRow->paddingBottom())
    );
    float targetWidth = artWidth;
    float targetHeight = artHeight;
    if (m_artwork->hasImage()) {
      const float imageAspect = std::max(0.01f, m_artwork->aspectRatio());
      const float boundsAspect = artWidth / std::max(1.0f, artHeight);
      if (imageAspect > boundsAspect) {
        targetHeight = artWidth / imageAspect;
      } else {
        targetWidth = artHeight * imageAspect;
      }
    }
    m_artwork->setSize(targetWidth, targetHeight);
    m_artwork->setRadius(Style::scaledRadiusXl(scale));
    m_mediaStack->layout(renderer);
  }

  if (m_visualizerBody != nullptr && m_visualizerSpectrum != nullptr) {
    const float bodyWidth = std::max(
        0.0f, m_visualizerBody->width() - (m_visualizerBody->paddingLeft() + m_visualizerBody->paddingRight())
    );
    const float bodyHeightAvail = std::max(
        0.0f, m_visualizerBody->height() - (m_visualizerBody->paddingTop() + m_visualizerBody->paddingBottom())
    );
    const float spectrumWidth = std::max(1.0f, bodyWidth);
    const float spectrumHeight = std::max(1.0f, bodyHeightAvail);
    m_visualizerSpectrum->setSize(spectrumWidth, spectrumHeight);
    m_visualizerBody->layout(renderer);
  }
}

void MediaTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }
  if (m_visualizerSpectrum != nullptr && m_spectrum != nullptr && m_spectrumListenerId != 0) {
    if (!m_spectrum->idle() || !m_visualizerSpectrum->converged()) {
      m_visualizerSpectrum->setValues(m_spectrum->values(m_spectrumListenerId));
    }
  }

  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
  const auto now = std::chrono::steady_clock::now();
  const bool hasPendingSeek = m_pendingSeekUs >= 0 && now < m_pendingSeekUntil;
  const bool withinProgressSettle =
      m_progressSettleUntil != std::chrono::steady_clock::time_point{} && now < m_progressSettleUntil;
  const bool playing = active.has_value() && active->playbackStatus == "Playing";
  if (playing || hasPendingSeek || withinProgressSettle) {
    if (!m_progressTimer.active()) {
      m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        if (!m_active) {
          return;
        }
        PanelManager::instance().requestUpdateOnly();
        PanelManager::instance().requestRedraw();
      });
    }
  } else {
    m_progressTimer.stop();
  }

  refresh(renderer);
}

void MediaTab::onFrameTick(float deltaMs) {
  if (!m_active) {
    return;
  }

  if (m_visualizerSpectrum != nullptr) {
    if (m_spectrum != nullptr && m_spectrumListenerId != 0) {
      if (!m_spectrum->idle() || !m_visualizerSpectrum->converged()) {
        m_visualizerSpectrum->setValues(m_spectrum->values(m_spectrumListenerId));
      }
    }
    m_visualizerSpectrum->tick(deltaMs);
  }
}

void MediaTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (m_spectrum != nullptr) {
    if (active && m_spectrumListenerId == 0) {
      m_spectrumListenerId = m_spectrum->addChangeListener(kVisualizerBandCount, [this]() {
        if (!m_active || m_spectrum->idle()) {
          return;
        }
        PanelManager::instance().requestFrameTick();
      });
    } else if (!active && m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
  }
  if (!active) {
    m_progressTimer.stop();
    m_positionSampleAt = {};
    m_positionTrackSignature.clear();
    m_progressSettleUntil = {};
    m_nextRealtimeUpdateAt = {};
    m_lastRealtimeMprisPollAt = {};
  }
  if (becameActive && m_mpris != nullptr) {
    m_positionSampleAt = {};
  }
}

void MediaTab::onClose() {
  m_progressTimer.stop();
  if (m_spectrum != nullptr) {
    if (m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
  }
  m_active = false;
  m_rootLayout = nullptr;
  m_mediaColumn = nullptr;
  m_visualizerColumn = nullptr;
  m_visualizerBody = nullptr;
  m_visualizerSpectrum = nullptr;
  m_artwork = nullptr;
  m_artworkRow = nullptr;
  m_nowCard = nullptr;
  m_mediaStack = nullptr;
  m_playerMenuButton = nullptr;
  if (m_playerMenuPopup != nullptr) {
    PanelManager::instance().clearActivePopup();
    m_playerMenuPopup->close();
  }
  m_playerMenuOpen = false;
  m_trackTitle = nullptr;
  m_trackArtist = nullptr;
  m_trackAlbum = nullptr;
  m_progressSlider = nullptr;
  m_prevButton = nullptr;
  m_playPauseButton = nullptr;
  m_nextButton = nullptr;
  m_repeatButton = nullptr;
  m_shuffleButton = nullptr;
  m_lastArtPath.clear();
  m_lastBusName.clear();
  m_lastPlaybackStatus.clear();
  m_lastLoopStatus.clear();
  m_playerBusNames.clear();
  m_lastActiveSnapshot.reset();
  m_pendingSeekBusName.clear();
  m_pendingSeekUs = -1;
  m_progressSettleUntil = {};
  m_positionTrackSignature.clear();
  m_nextRealtimeUpdateAt = {};
  m_lastRealtimeMprisPollAt = {};
}

void MediaTab::clearArt(Renderer& renderer) {
  if (m_artwork != nullptr) {
    m_artwork->clear(renderer);
  }
}

void MediaTab::commitPendingSeek(float valueSeconds) {
  if (m_mpris == nullptr) {
    return;
  }

  const std::int64_t targetUs = static_cast<std::int64_t>(std::llround(valueSeconds * 1000000.0f));
  const auto now = std::chrono::steady_clock::now();
  m_positionUs = targetUs;
  m_positionSampleAt = now;
  const auto active = m_mpris->activePlayer();
  const std::string seekBusName =
      active.has_value() ? active->busName : (!m_positionBusName.empty() ? m_positionBusName : std::string{});
  m_pendingSeekBusName = seekBusName;
  m_pendingSeekUs = targetUs;
  m_pendingSeekUntil = now + kPendingSeekTimeout;
  m_progressSettleUntil = now + kProgressSettleHold;

  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard, seekBusName, targetUs]() {
    if (aliveGuard.expired() || m_mpris == nullptr) {
      return;
    }
    if (!seekBusName.empty()) {
      (void)m_mpris->setPosition(seekBusName, targetUs);
    } else {
      (void)m_mpris->setPositionActive(targetUs);
    }
    PanelManager::instance().refresh();
  });
}

void MediaTab::refresh(Renderer& renderer) {
  std::vector<MprisPlayerInfo> players;
  std::optional<MprisPlayerInfo> active;
  const auto now = std::chrono::steady_clock::now();
  if (m_mpris != nullptr) {
    players = m_mpris->listPlayers();
    active = m_mpris->activePlayer();
    kLog.debug(
        "media tab refresh initial players={} active={} active_bus=\"{}\"", players.size(), active.has_value(),
        active.has_value() ? active->busName : std::string{}
    );
  }

  if (!active.has_value() && m_lastActiveSnapshot.has_value() && now - m_lastActiveSeenAt <= kNoActivePlayerGrace) {
    // Keep last player briefly to hide transient MPRIS discovery gaps.
    active = m_lastActiveSnapshot;
  }

  if (m_playerMenuButton != nullptr) {
    const auto pinnedBusName = m_mpris != nullptr ? m_mpris->pinnedPlayerPreference() : std::nullopt;
    std::vector<std::string> playerBusNames;
    playerBusNames.reserve(players.size());
    std::vector<ContextMenuControlEntry> entries;
    entries.reserve(players.size() + 1);
    entries.push_back(
        {.id = 0,
         .label = i18n::tr("control-center.media.active-player"),
         .enabled = true,
         .separator = false,
         .hasSubmenu = false}
    );

    for (std::size_t i = 0; i < players.size(); ++i) {
      const auto& player = players[i];
      playerBusNames.push_back(player.busName);
      const bool selected = pinnedBusName.has_value() && player.busName == *pinnedBusName;
      const std::string label = (selected ? "• " : "") + (player.identity.empty() ? player.busName : player.identity);
      entries.push_back(
          {.id = static_cast<std::int32_t>(i + 1),
           .label = label,
           .enabled = true,
           .separator = false,
           .hasSubmenu = false}
      );
    }

    m_playerBusNames = std::move(playerBusNames);
    m_playerMenuButton->setEnabled(!m_playerBusNames.empty());
    m_playerMenuButton->setVariant(!m_playerBusNames.empty() ? ButtonVariant::Ghost : ButtonVariant::Default);
    if (m_playerBusNames.empty() && m_playerMenuPopup != nullptr && m_playerMenuPopup->isOpen()) {
      m_playerMenuPopup->close();
      PanelManager::instance().clearActivePopup();
    }
  }

  if (m_trackTitle == nullptr || m_trackArtist == nullptr || m_progressSlider == nullptr ||
      m_playPauseButton == nullptr || m_repeatButton == nullptr || m_shuffleButton == nullptr) {
    return;
  }

  if (active.has_value()) {
    const auto& player = *active;
    m_lastActiveSnapshot = player;
    m_lastActiveSeenAt = now;
    const std::string trackSignature = std::format(
        "{}\n{}\n{}\n{}\n{}", player.trackId, player.title, joinArtists(player.artists), player.album, player.sourceUrl
    );
    std::int64_t livePositionUs = player.positionUs;
    if (player.lengthUs > 0) {
      livePositionUs = std::clamp<std::int64_t>(livePositionUs, 0, player.lengthUs);
    } else {
      livePositionUs = std::max<std::int64_t>(0, livePositionUs);
    }

    const bool pendingMatchesPlayer = m_pendingSeekBusName.empty() || m_pendingSeekBusName == player.busName;
    const bool seekArrived = pendingMatchesPlayer && m_pendingSeekUs >= 0 &&
                             std::llabs(livePositionUs - m_pendingSeekUs) <= kSeekArrivedToleranceUs &&
                             (m_pendingSeekUs <= kSeekNearZeroUs || livePositionUs > kSeekNearZeroUs ||
                              livePositionUs >= m_pendingSeekUs - kSeekArrivedToleranceUs);
    const bool seekPending = pendingMatchesPlayer && m_pendingSeekUs >= 0 && !seekArrived && now < m_pendingSeekUntil;
    const bool withinProgressSettle =
        m_progressSettleUntil != std::chrono::steady_clock::time_point{} && now < m_progressSettleUntil;
    const bool sameDisplayedTrack = m_positionBusName == player.busName && m_positionTrackSignature == trackSignature;
    const bool withinTransientRegressionWindow = m_positionSampleAt != std::chrono::steady_clock::time_point{} &&
                                                 now - m_positionSampleAt <= kTransientPositionRegressionWindow;
    const bool preserveDisplayedPosition =
        !seekPending && sameDisplayedTrack && m_lastPlaybackStatus == "Playing" && player.playbackStatus == "Playing" &&
        m_positionUs >= kTransientPositionRegressionFloorUs &&
        livePositionUs <= kTransientPositionRegressionCeilingUs &&
        livePositionUs + kTransientPositionRegressionDeltaUs < m_positionUs && withinTransientRegressionWindow;
    if (preserveDisplayedPosition) {
      livePositionUs = m_positionUs;
    }

    std::int64_t displayPositionUs = livePositionUs;
    if (seekPending) {
      displayPositionUs = m_pendingSeekUs;
    } else if (withinProgressSettle && livePositionUs + kTransientPositionRegressionDeltaUs < m_positionUs) {
      displayPositionUs = m_positionUs;
    } else if (preserveDisplayedPosition) {
      displayPositionUs = m_positionUs;
    }

    const bool samePlayerAsDisplayed = m_positionBusName == player.busName || m_pendingSeekBusName == player.busName;

    m_positionBusName = player.busName;
    m_positionTrackId = player.trackId;
    m_positionTrackSignature = trackSignature;
    m_positionUs = displayPositionUs;
    m_positionSampleAt = now;

    if (seekArrived) {
      m_pendingSeekBusName.clear();
      m_pendingSeekUs = -1;
    }

    m_trackTitle->setText(player.title.empty() ? player.identity : player.title);
    m_trackArtist->setText(joinArtists(player.artists).empty() ? player.identity : joinArtists(player.artists));
    if (m_trackAlbum != nullptr) {
      m_trackAlbum->setText(player.album);
      m_trackAlbum->setVisible(!player.album.empty());
    }

    const std::string resolvedArtUrl = effectiveArtUrl(player);
    std::string artPath = normalizeArtPath(resolvedArtUrl);
    if (artPath.empty() && isRemoteArtUrl(resolvedArtUrl)) {
      const auto cached = artCachePath(resolvedArtUrl);
      std::error_code ec;
      if (std::filesystem::exists(cached, ec) && std::filesystem::file_size(cached, ec) > 0) {
        artPath = cached.string();
      } else if (m_httpClient != nullptr && m_pendingArtDownloads.find(resolvedArtUrl) == m_pendingArtDownloads.end()) {
        std::filesystem::create_directories(cached.parent_path(), ec);
        m_pendingArtDownloads.insert(resolvedArtUrl);
        m_httpClient->download(resolvedArtUrl, cached, [this, url = resolvedArtUrl](bool success) {
          m_pendingArtDownloads.erase(url);
          if (success) {
            m_lastArtPath.clear();
            PanelManager::instance().refresh();
          }
        });
      }
    }

    if (m_artwork != nullptr &&
        (!resolvedArtUrl.empty() && (resolvedArtUrl != m_lastArtPath || !m_artwork->hasImage()))) {
      bool loaded = false;
      if (artPath.empty()) {
        kLog.debug("artwork unresolved url=\"{}\"", resolvedArtUrl);
        clearArt(renderer);
      } else if (!m_artwork->setSourceFile(renderer, artPath, static_cast<int>(kArtworkSize))) {
        kLog.warn("artwork load failed url=\"{}\" path=\"{}\"", resolvedArtUrl, artPath);
        clearArt(renderer);
      } else {
        kLog.debug("artwork loaded url=\"{}\" path=\"{}\"", resolvedArtUrl, artPath);
        loaded = true;
      }

      // Only lock this URL once we actually have an image.
      // Otherwise keep retrying while metadata/download catches up.
      m_lastArtPath = loaded ? resolvedArtUrl : std::string{};
    } else if (m_artwork != nullptr && resolvedArtUrl.empty()) {
      clearArt(renderer);
      m_lastArtPath.clear();
    }

    std::int64_t trackLengthUs = player.lengthUs;
    if (trackLengthUs > 0) {
      m_lastTrackLengthUs = trackLengthUs;
    } else if (m_lastTrackLengthUs > 0 && samePlayerAsDisplayed) {
      trackLengthUs = m_lastTrackLengthUs;
    }
    const bool progressInteracting = m_progressSlider->dragging() || seekPending || withinProgressSettle;
    const bool progressEnabled = player.canSeek && (trackLengthUs > 0 || progressInteracting);

    m_syncingProgress = true;
    m_progressSlider->setEnabled(progressEnabled);
    if (trackLengthUs > 0) {
      m_progressSlider->setRange(0.0f, static_cast<float>(trackLengthUs) / 1000000.0f);
    }
    if (!m_progressSlider->dragging()) {
      const float sliderMax = m_progressSlider->maxValue();
      const float nextValue =
          sliderMax > 0.0f ? std::clamp(static_cast<float>(displayPositionUs) / 1000000.0f, 0.0f, sliderMax) : 0.0f;
      m_progressSlider->setValue(nextValue);
    }
    m_syncingProgress = false;

    m_playPauseButton->setGlyph(playPauseGlyph(player.playbackStatus));
    m_playPauseButton->setVariant(ButtonVariant::Primary);
    if (m_prevButton != nullptr) {
      m_prevButton->setEnabled(player.canGoPrevious);
    }
    if (m_nextButton != nullptr) {
      m_nextButton->setEnabled(player.canGoNext);
    }
    m_repeatButton->setGlyph(repeatGlyph(player.loopStatus));
    m_repeatButton->setVariant(toggleVariant(player.loopStatus != "None"));
    m_shuffleButton->setVariant(toggleVariant(player.shuffle));

    m_lastBusName = player.busName;
    m_lastPlaybackStatus = player.playbackStatus;
    m_lastLoopStatus = player.loopStatus;
    m_lastShuffle = player.shuffle;
    return;
  }

  m_pendingSeekBusName.clear();
  m_pendingSeekUs = -1;
  m_progressSettleUntil = {};
  m_lastActiveSnapshot.reset();
  m_positionBusName.clear();
  m_positionTrackId.clear();
  m_positionTrackSignature.clear();
  m_positionUs = 0;
  m_lastTrackLengthUs = 0;
  m_positionSampleAt = {};
  m_trackTitle->setText(i18n::tr("control-center.media.nothing-playing"));
  m_trackArtist->setText(i18n::tr("control-center.media.start-playback"));
  if (m_trackAlbum != nullptr) {
    m_trackAlbum->setText("");
    m_trackAlbum->setVisible(false);
  }
  clearArt(renderer);
  m_lastArtPath.clear();
  m_syncingProgress = true;
  m_progressSlider->setEnabled(false);
  m_progressSlider->setRange(0.0f, 100.0f);
  m_progressSlider->setValue(0.0f);
  m_syncingProgress = false;
  m_playPauseButton->setGlyph("media-play");
  if (m_prevButton != nullptr) {
    m_prevButton->setEnabled(false);
  }
  if (m_nextButton != nullptr) {
    m_nextButton->setEnabled(false);
  }
  m_repeatButton->setGlyph("repeat");
  m_repeatButton->setVariant(ButtonVariant::Ghost);
  m_shuffleButton->setVariant(ButtonVariant::Ghost);
  m_lastBusName.clear();
  m_lastPlaybackStatus.clear();
  m_lastLoopStatus.clear();
  m_lastShuffle = false;
}
