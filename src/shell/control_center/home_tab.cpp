#include "shell/control_center/home_tab.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/wallpaper/wallpaper.h"
#include "system/dependency_service.h"
#include "system/distro_info.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/grid_view.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

using namespace control_center;

namespace {

  constexpr float kHomeAvatarScale = 2.6f;
  // Bottom row: 1 : 1 — equal split so media/clock and shortcuts feel balanced (tweak either value slightly if needed).
  constexpr float kHomeMainColumnFlexGrow = 1.66f;
  constexpr float kHomeShortcutsFlexGrow = 1.0f;
  constexpr auto kHomeTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kHomeTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kHomeTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kHomeTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr int kHomeMediaArtLayoutPassLimit = 8;

  float homeAvatarSize(float scale) { return Style::controlHeightLg * kHomeAvatarScale * scale; }

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  std::string formatShellTime(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.timeFormat.c_str() : "{:%H:%M}";
    return formatLocalTime(format);
  }

  std::string formatShellDate(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.dateFormat.c_str() : "%A, %x";
    return formatLocalTime(format);
  }

  void applyHomeCardStyle(Flex& card, float scale, float fillOpacity, bool showBorder) {
    applySectionCardStyle(card, scale, fillOpacity, showBorder);
    card.setGap(Style::spaceSm * scale);
  }

  Button::ButtonPalette inactiveShortcutPalette(float fillOpacity) {
    constexpr float kDisabledAlpha = 0.55f;
    const float opacity = std::clamp(fillOpacity, 0.0f, 1.0f);
    return Button::ButtonPalette{
        .borderWidth = Style::borderWidth,
        .normal =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::SurfaceVariant, opacity),
                .border = colorSpecFromRole(ColorRole::Outline, 0.5f),
                .label = colorSpecFromRole(ColorRole::OnSurface),
            },
        .hover =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::Hover, std::max(opacity, 0.78f)),
                .border = clearColorSpec(),
                .label = colorSpecFromRole(ColorRole::OnHover),
            },
        .pressed =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::Primary),
                .border = colorSpecFromRole(ColorRole::Primary),
                .label = colorSpecFromRole(ColorRole::OnPrimary),
            },
        .disabled = Button::ButtonStateColors{
            .bg = colorSpecFromRole(ColorRole::SurfaceVariant, opacity * kDisabledAlpha),
            .border = colorSpecFromRole(ColorRole::Outline, 0.5f * kDisabledAlpha),
            .label = colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha),
        },
    };
  }

  void applyShortcutButtonStyle(Button& button, bool enabled, bool active, float fillOpacity) {
    if (enabled && active) {
      button.setVariant(ButtonVariant::Primary);
    } else {
      button.setVariant(ButtonVariant::Outline);
      button.setCustomPalette(inactiveShortcutPalette(fillOpacity));
    }
    button.setEnabled(enabled);
  }

} // namespace

HomeTab::HomeTab(
    MprisService* mpris, HttpClient* httpClient, WeatherService* weather, PipeWireService* audio,
    PowerProfilesService* powerProfiles, ConfigService* config, INetworkService* network, BluetoothService* bluetooth,
    GammaService* nightLight, noctalia::theme::ThemeService* theme, NotificationManager* notifications,
    IdleInhibitor* idleInhibitor, DependencyService* dependencies, CompositorPlatform* platform, Wallpaper* wallpaper
)
    : m_mpris(mpris), m_httpClient(httpClient), m_weather(weather), m_config(config), m_wallpaper(wallpaper),
      m_services{
          .network = network,
          .bluetooth = bluetooth,
          .nightLight = nightLight,
          .theme = theme,
          .notifications = notifications,
          .idleInhibitor = idleInhibitor,
          .audio = audio,
          .powerProfiles = powerProfiles,
          .mpris = mpris,
          .weather = weather,
          .config = config,
          .dependencies = dependencies,
          .platform = platform,
      } {}

HomeTab::~HomeTab() = default;

std::unique_ptr<Flex> HomeTab::create() {
  const float scale = contentScale();
  const std::string displayName = sessionDisplayName();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  // --- User card ---
  auto userCard = ui::column({
      .out = &m_userCard,
      .justify = FlexJustify::Center,
      .fillHeight = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });

  {
    userCard->addChild(
        ui::image({
            .out = &m_wallpaperBg,
            .fit = ImageFit::Cover,
            .radius = std::max(0.0f, Style::scaledRadiusXl(scale) - Style::borderWidth),
            .participatesInLayout = false,
            .configure = [](Image& image) { image.setZIndex(-1); },
        })
    );

    userCard->addChild(
        ui::box({
            .out = &m_wallpaperGradient,
            .participatesInLayout = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );
  }

  const float avatarSize = homeAvatarSize(scale);
  auto userRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
      ui::image({
          .out = &m_userAvatar,
          .fit = ImageFit::Cover,
          .radius = avatarSize * 0.5f,
          .padding = 1.0f * scale,
          .width = avatarSize,
          .height = avatarSize,
          .configure =
              [](Image& image) { image.setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * 3.0f); },
      }),
      ui::column(
          {.out = &m_userMain,
           .align = FlexAlign::Stretch,
           .justify = FlexJustify::Center,
           .gap = Style::spaceXs * 0.5f * scale,
           .minHeight = avatarSize,
           .width = 0.0f,
           .height = avatarSize,
           .flexGrow = 1.0f},
          ui::label({
              .text = displayName,
              .fontSize = Style::fontSizeTitle * 1.12f * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .fontWeight = FontWeight::Bold,
              .configure =
                  [scale](Label& label) { label.setShadow(Color{0.0f, 0.0f, 0.0f, 0.42f}, 0.0f, 1.0f * scale); },
          }),
          ui::label({
              .out = &m_userFacts,
              .text = "…",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = [scale](Label& label) {
                label.setShadow(Color{0.0f, 0.0f, 0.0f, 0.36f}, 0.0f, 1.0f * scale);
              },
          })
      )
  );
  userCard->addChild(std::move(userRow));

  userCard->addChild(
      ui::button({
          .out = &m_wallpaperButton,
          .glyph = "wallpaper-selector",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .participatesInLayout = false,
          .onClick = []() { PanelManager::instance().togglePanel("wallpaper"); },
          .configure = [](Button& button) { button.setZIndex(2); },
      })
  );

  tab->addChild(std::move(userCard));

  auto bottomRow = ui::row({
      .out = &m_bottomRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .fillWidth = true,
  });

  auto leftColumn = ui::column({
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .flexGrow = kHomeMainColumnFlexGrow,
  });

  // --- Media (top of left column) ---
  auto mediaCard = ui::column({
      .out = &m_mediaCard,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.4f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });

  const float artSize = Style::controlHeightLg * 1.22f * scale;
  auto mediaContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::column(
          {.out = &m_mediaArtSlot,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .width = artSize,
           .height = artSize},
          ui::glyph({
              .out = &m_mediaArtFallback,
              .glyph = "disc-filled",
              .glyphSize = artSize * 0.55f,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::image({
              .out = &m_mediaArt,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusLg(scale),
              .width = artSize,
              .height = artSize,
              .participatesInLayout = false,
              .configure = [](Image& image) { image.setZIndex(1); },
          })
      ),
      ui::column(
          {.out = &m_mediaText, .align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5f * scale, .flexGrow = 1.0f},
          ui::label({
              .out = &m_mediaTrack,
              .text = "...",
              .fontSize = Style::fontSizeBody * 0.95f * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::label({
              .out = &m_mediaArtist,
              .text = i18n::tr("control-center.home.media.no-active-player"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &m_mediaStatus,
              .text = i18n::tr("control-center.home.media.idle"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &m_mediaProgress,
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Secondary),
              .visible = false,
          })
      )
  );
  mediaCard->addChild(std::move(mediaContent));

  mediaCard->addChild(
      ui::button({
          .out = &m_mediaButton,
          .glyph = "disc-filled",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .participatesInLayout = false,
          .onClick = []() { openControlCenterTab("media"); },
          .configure = [](Button& button) { button.setZIndex(2); },
      })
  );

  // --- Date/Time + Weather (below media) ---
  auto dateTimeCard = ui::row(
      {.out = &m_dateTimeCard,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceLg * scale,
       .fillWidth = true,
       .fillHeight = true,
       .flexGrow = 1.0f,
       .configure =
           [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
             applyHomeCardStyle(card, scale, opacity, borders);
             card.setDirection(FlexDirection::Horizontal);
             card.setAlign(FlexAlign::Center);
             card.setJustify(FlexJustify::Center);
             card.setGap(Style::spaceLg * scale);
           }},
      ui::label({
          .out = &m_timeLabel,
          .text = formatShellTime(m_config),
          .fontSize = Style::fontSizeTitle * 1.7f * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
          .fontWeight = FontWeight::Bold,
      }),
      ui::column(
          {.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = Style::spaceXs * 0.5f * scale},
          ui::label({
              .out = &m_dateLabel,
              .text = formatShellDate(m_config),
              .fontSize = Style::fontSizeBody * 0.9f * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
              ui::glyph({
                  .out = &m_weatherGlyph,
                  .glyph = "weather-cloud-sun",
                  .glyphSize = Style::fontSizeCaption * 1.12f * scale,
                  .color = colorSpecFromRole(ColorRole::Primary),
              }),
              ui::label({
                  .out = &m_weatherLine,
                  .text = "—",
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              })
          )
      )
  );

  dateTimeCard->addChild(
      ui::button({
          .out = &m_weatherButton,
          .glyph = "weather-cloud-sun",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .participatesInLayout = false,
          .onClick = []() { openControlCenterTab("weather"); },
          .configure = [](Button& button) { button.setZIndex(2); },
      })
  );

  leftColumn->addChild(std::move(mediaCard));
  leftColumn->addChild(std::move(dateTimeCard));
  bottomRow->addChild(std::move(leftColumn));

  // --- Shortcuts (right of media + clock) ---
  const auto& shortcuts =
      m_config != nullptr ? m_config->config().controlCenter.shortcuts : std::vector<ShortcutConfig>{};
  const std::size_t count = std::min(shortcuts.size(), std::size_t{6});

  auto grid = std::make_unique<GridView>();
  grid->setColumns(2);
  grid->setColumnGap(Style::spaceSm * scale);
  grid->setRowGap(Style::spaceSm * scale);
  grid->setPadding(0.0f);
  grid->setUniformCellSize(true);
  grid->setStretchItems(true);
  grid->setSquareCells(false);
  grid->setMinCellHeight(0.0f);
  grid->setFlexGrow(kHomeShortcutsFlexGrow);
  m_shortcutsGrid = grid.get();
  m_shortcutPads.clear();

  for (std::size_t i = 0; i < count; ++i) {
    const auto& sc = shortcuts[i];
    auto shortcut = ShortcutRegistry::create(sc.type, m_services);
    if (shortcut == nullptr) {
      continue;
    }

    const std::string label = shortcut->displayLabel();
    const bool enabled = shortcut->enabled();
    const bool isActive = shortcut->isToggle() && shortcut->active();

    const std::size_t padIdx = m_shortcutPads.size();
    auto btn = ui::button({
        .text = label,
        .glyph = shortcut->displayIcon(),
        .glyphSize = Style::fontSizeTitle * 1.35f * scale,
        .minHeight = 0.0f,
        .padding = Style::spaceSm * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusLg(scale),
        .onClick =
            [this, padIdx]() {
              if (padIdx < m_shortcutPads.size()) {
                m_shortcutPads[padIdx].shortcut->onClick();
              }
            },
        .onRightClick =
            [this, padIdx]() {
              if (padIdx < m_shortcutPads.size()) {
                m_shortcutPads[padIdx].shortcut->onRightClick();
              }
            },
        .configure =
            [enabled, isActive, fillOpacity = panelCardOpacity(), scale](Button& button) {
              // Match media card column: Stretch so label width follows the cell; Center uses intrinsic text width and
              // fights setMaxWidth.
              button.setAlign(FlexAlign::Stretch);
              // Label font only: Button::setFontSize also resizes the glyph. Mini + uiScale keeps tiles closer to
              // other CC rows that use raw fontSizeCaption, while still scaling with shell.uiScale for consistency.
              button.label()->setFontSize(Style::fontSizeMini * scale);
              button.label()->setBaselineMode(LabelBaselineMode::InkCentered);
              button.label()->setMaxLines(1);
              button.label()->setTextAlign(TextAlign::Center);
              button.setDirection(FlexDirection::Vertical);
              applyShortcutButtonStyle(button, enabled, isActive, fillOpacity);
            },
    });

    Button* btnPtr = btn.get();
    ShortcutPad pad;
    pad.shortcut = std::move(shortcut);
    pad.button = btnPtr;
    pad.glyph = btnPtr->glyph();
    pad.label = btnPtr->label();
    m_shortcutPads.push_back(std::move(pad));
    grid->addChild(std::move(btn));
  }

  if (!m_shortcutPads.empty()) {
    bottomRow->addChild(std::move(grid));
  } else {
    m_shortcutsGrid = nullptr;
  }
  tab->addChild(std::move(bottomRow));

  return tab;
}

std::unique_ptr<Flex> HomeTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_settingsButton,
          .glyph = "settings",
          .onClick = []() { PanelManager::instance().openSettingsWindow(); },
          .configure = [scale, opacity = panelCardOpacity()](
                           Button& button
                       ) { panel_button_style::configureHeaderIconButton(button, scale, opacity); },
      }),
      ui::button({
          .out = &m_sessionButton,
          .glyph = "shutdown",
          .onClick = []() { PanelManager::instance().togglePanel("session"); },
          .configure = [scale, opacity = panelCardOpacity()](
                           Button& button
                       ) { panel_button_style::configureHeaderIconButton(button, scale, opacity); },
      })
  );
}

void HomeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  (void)bodyHeight;
  if (m_rootLayout == nullptr) {
    return;
  }

  if (m_dateTimeCard != nullptr) {
    m_dateTimeCard->setMinHeight(0.0f);
  }
  if (m_mediaCard != nullptr) {
    m_mediaCard->setMinHeight(0.0f);
  }
  if (m_userAvatar != nullptr && m_userMain != nullptr) {
    const float userMainHeight = std::max(1.0f, m_userAvatar->height());
    m_userMain->setMinHeight(userMainHeight);
    m_userMain->setSize(m_userMain->width(), userMainHeight);
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);

  // Cap shortcut labels to the button's content width after cells are sized (avoids elide from grid math mismatch).
  if (!m_shortcutPads.empty() && m_shortcutsGrid != nullptr) {
    const float scale = contentScale();
    for (auto& pad : m_shortcutPads) {
      if (pad.label == nullptr) {
        continue;
      }
      float inner = 1.0f;
      if (pad.button != nullptr && pad.button->width() > 1.0f) {
        inner = std::max(1.0f, pad.button->width() - pad.button->paddingLeft() - pad.button->paddingRight());
      } else {
        const float gridW = m_shortcutsGrid->width();
        const float innerGrid =
            std::max(1.0f, gridW - m_shortcutsGrid->paddingLeft() - m_shortcutsGrid->paddingRight());
        const std::size_t cols = std::max<std::size_t>(1, std::min(m_shortcutsGrid->columns(), m_shortcutPads.size()));
        const float cellWidth =
            (innerGrid - static_cast<float>(cols - 1) * m_shortcutsGrid->columnGap()) / static_cast<float>(cols);
        inner = std::max(1.0f, cellWidth - 2.0f * Style::spaceSm * scale);
      }
      pad.label->setMaxWidth(inner);
    }
  }

  const auto innerWidth = [](Flex* card) {
    if (card == nullptr) {
      return 1.0f;
    }
    return std::max(1.0f, card->width() - (card->paddingLeft() + card->paddingRight()));
  };
  const float dateTimeWrap = innerWidth(m_dateTimeCard);
  if (m_timeLabel != nullptr) {
    m_timeLabel->setMaxWidth(dateTimeWrap);
    m_timeLabel->setMaxLines(1);
  }

  float dateTimeRightWrap = dateTimeWrap;
  if (m_timeLabel != nullptr && m_dateTimeCard != nullptr) {
    dateTimeRightWrap = std::max(1.0f, dateTimeWrap - m_timeLabel->width() - m_dateTimeCard->gap());
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setMaxWidth(dateTimeRightWrap);
    m_dateLabel->setMaxLines(1);
  }
  if (m_weatherLine != nullptr) {
    const float weatherTextWrap = std::max(
        1.0f, dateTimeRightWrap - (m_weatherGlyph != nullptr ? m_weatherGlyph->width() : 0.0f) -
                  Style::spaceXs * contentScale()
    );
    m_weatherLine->setMaxWidth(weatherTextWrap);
    m_weatherLine->setMaxLines(2);
  }
  // Grow the album art square to fill the media card height so the row feels balanced
  // when the card flex-grows. A later bottom-row min-height pass can change the card
  // height, so this runs again after that final layout pass below.
  resizeMediaArtToCard();

  // Labels auto-wrap to mediaText's assigned width via Flex stretch propagation.
  for (Label* label : {m_mediaArtist, m_mediaStatus, m_mediaProgress}) {
    if (label != nullptr) {
      label->setMaxLines(1);
    }
  }
  if (m_mediaTrack != nullptr) {
    m_mediaTrack->setMaxLines(2);
  }

  if (m_userCard != nullptr && m_userFacts != nullptr) {
    const float userWrap = innerWidth(m_userCard);
    m_userFacts->setMaxWidth(userWrap);
    m_userFacts->setMaxLines(1);
  }

  if (m_userAvatar != nullptr && m_userMain != nullptr) {
    const float scale = contentScale();
    const float minAvatar = homeAvatarSize(scale);
    const float desiredAvatar = std::max(minAvatar, m_userMain->height());
    if (std::abs(m_userAvatar->width() - desiredAvatar) > 0.5f) {
      m_userAvatar->setSize(desiredAvatar, desiredAvatar);
      m_userAvatar->setRadius(desiredAvatar * 0.5f);
      m_userAvatar->setPadding(1.0f * scale);
    }
    m_userMain->setMinHeight(desiredAvatar);
    m_userMain->setSize(m_userMain->width(), desiredAvatar);
  }

  // Lock the shortcuts grid height to its square-cell natural size so it does not vary
  // when the media or clock cards change. The leftColumn stretches to match this height.
  if (m_shortcutsGrid != nullptr && !m_shortcutPads.empty()) {
    const float gridW = m_shortcutsGrid->width();
    const float innerGridW = std::max(1.0f, gridW - m_shortcutsGrid->paddingLeft() - m_shortcutsGrid->paddingRight());
    const std::size_t cols = std::max<std::size_t>(1, std::min(m_shortcutsGrid->columns(), m_shortcutPads.size()));
    const std::size_t rows = (m_shortcutPads.size() + cols - 1) / cols;
    const float cellWidth = std::max(
        1.0f, (innerGridW - static_cast<float>(cols - 1) * m_shortcutsGrid->columnGap()) / static_cast<float>(cols)
    );
    // Cells aim for square but trimmed slightly so the grid stays compact and the bottom row
    // doesn't tower over the user card area.
    const float cellSide = cellWidth * 0.82f;
    const float gridH = static_cast<float>(rows) * cellSide +
                        static_cast<float>(rows > 0 ? rows - 1 : 0) * m_shortcutsGrid->rowGap() +
                        m_shortcutsGrid->paddingTop() + m_shortcutsGrid->paddingBottom();
    if (m_bottomRow != nullptr) {
      m_bottomRow->setMinHeight(gridH);
    }
  }

  bool artSizeChanged = false;
  for (int pass = 0; pass < kHomeMediaArtLayoutPassLimit; ++pass) {
    m_rootLayout->layout(renderer);
    artSizeChanged = resizeMediaArtToCard();
    if (!artSizeChanged) {
      break;
    }
  }
  if (artSizeChanged) {
    // Keep the final tree consistent even if an unusual layout combination hits the pass cap.
    m_rootLayout->layout(renderer);
  }
  layoutWallpaperBackground(renderer);
  layoutCardButton(renderer, m_userCard, m_wallpaperButton);
  layoutCardButton(renderer, m_mediaCard, m_mediaButton);
  layoutCardButton(renderer, m_dateTimeCard, m_weatherButton);
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->measure(renderer);
  }
}

bool HomeTab::resizeMediaArtToCard() {
  if (m_mediaCard == nullptr || m_mediaArt == nullptr || m_mediaArtSlot == nullptr) {
    return false;
  }

  const float scale = contentScale();
  const float minArt = Style::controlHeightLg * 1.22f * scale;
  const float maxArt = Style::controlHeightLg * 2.6f * scale;
  const float available =
      std::max(0.0f, m_mediaCard->height() - m_mediaCard->paddingTop() - m_mediaCard->paddingBottom());
  const float desired = std::clamp(available, minArt, maxArt);
  if (std::abs(m_mediaArtSlot->width() - desired) <= 0.5f) {
    return false;
  }

  m_mediaArtSlot->setSize(desired, desired);
  m_mediaArt->setSize(desired, desired);
  m_mediaArt->setRadius(Style::scaledRadiusLg(scale));
  if (m_mediaArtFallback != nullptr) {
    m_mediaArtFallback->setGlyphSize(desired * 0.55f);
  }
  return true;
}

void HomeTab::layoutWallpaperBackground(Renderer& renderer) {
  if (m_userCard == nullptr || m_wallpaperBg == nullptr) {
    return;
  }

  const float bw = Style::borderWidth;
  const float cw = std::max(0.0f, m_userCard->width() - bw * 2.0f);
  const float ch = std::max(0.0f, m_userCard->height() - bw * 2.0f);
  m_wallpaperBg->setPosition(bw, bw);
  m_wallpaperBg->setSize(cw, ch);

  if (m_wallpaperGradient != nullptr) {
    const float radius = std::max(0.0f, Style::scaledRadiusXl(contentScale()) - bw);
    m_wallpaperGradient->setPosition(bw, bw);
    m_wallpaperGradient->setFrameSize(cw, ch);
    const Color surface = colorForRole(ColorRole::Surface);
    const Color translucentSurface = rgba(surface.r, surface.g, surface.b, surface.a * 0.9f);
    const Color transparentSurface = rgba(surface.r, surface.g, surface.b, 0.0f);
    m_wallpaperGradient->setStyle(
        RoundedRectStyle{
            .fill = surface,
            .fillMode = FillMode::LinearGradient,
            .gradientDirection = GradientDirection::Horizontal,
            .gradientStops =
                {GradientStop{0.0f, translucentSurface}, GradientStop{0.25f, translucentSurface},
                 GradientStop{0.9f, transparentSurface}, GradientStop{1.0f, transparentSurface}},
            .radius = radius,
        }
    );
  }

  syncWallpaperBackground(renderer);
}

void HomeTab::syncWallpaperBackground(Renderer& renderer) {
  if (m_wallpaperBg == nullptr) {
    return;
  }

  const TextureHandle source = m_wallpaper != nullptr ? m_wallpaper->currentTexture() : TextureHandle{};
  if (!source.valid()) {
    m_wallpaperBg->clear(renderer);
    m_wallpaperBg->setVisible(false);
    return;
  }

  if (m_wallpaperBg->width() <= 0.0f || m_wallpaperBg->height() <= 0.0f) {
    m_wallpaperBg->setVisible(false);
    return;
  }

  m_wallpaperBg->setExternalTexture(renderer, source);
  m_wallpaperBg->setVisible(true);
}

void HomeTab::layoutCardButton(Renderer& renderer, Flex* card, Button* button) {
  if (card == nullptr || button == nullptr) {
    return;
  }

  const float scale = contentScale();
  button->setGlyphSize(Style::fontSizeBody * scale);
  button->layout(renderer);

  const float x = std::max(0.0f, card->width() - card->paddingRight() - button->width());
  const float y = std::max(0.0f, card->height() - card->paddingBottom() - button->height());
  button->setPosition(x, y);
}

void HomeTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }

  const bool playing =
      m_mpris != nullptr && m_mpris->activePlayer().has_value() && m_mpris->activePlayer()->playbackStatus == "Playing";
  if (playing) {
    if (!m_progressTimer.active()) {
      m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        if (!m_active) {
          return;
        }
        // refresh() schedules update+layout (Surface::requestUpdate); update-only ticks skipped
        // HomeTab::doLayout so album art never picked up the final media card height.
        PanelManager::instance().refresh();
        PanelManager::instance().requestRedraw();
      });
    }
  } else {
    m_progressTimer.stop();
  }
  sync(renderer);
}

void HomeTab::onFrameTick(float /*deltaMs*/) {}

void HomeTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (!active) {
    m_progressTimer.stop();
    m_nextRealtimeUpdateAt = {};
    m_lastRealtimeMprisPollAt = {};
    m_mediaPositionBusName.clear();
    m_mediaPositionTrackId.clear();
    m_mediaPositionTrackSignature.clear();
    m_mediaLastPlaybackStatus.clear();
    m_mediaPositionUs = 0;
    m_mediaPositionSampleAt = {};
    return;
  }

  if (becameActive) {
    // Other tabs were laid out while this body was hidden; flex sizes for the media row can be stale.
    // Defer so the tab container receives its configure size before HomeTab::doLayout runs.
    DeferredCall::callLater([]() {
      PanelManager::instance().requestLayout();
      PanelManager::instance().requestUpdateOnly();
    });
  }
}

void HomeTab::onClose() {
  m_progressTimer.stop();
  m_rootLayout = nullptr;
  m_bottomRow = nullptr;
  m_dateTimeCard = nullptr;
  m_mediaCard = nullptr;
  m_mediaText = nullptr;
  m_userCard = nullptr;
  m_userMain = nullptr;
  m_userAvatar = nullptr;
  m_timeLabel = nullptr;
  m_dateLabel = nullptr;
  m_weatherGlyph = nullptr;
  m_weatherLine = nullptr;
  m_userFacts = nullptr;
  m_settingsButton = nullptr;
  m_sessionButton = nullptr;
  m_wallpaperButton = nullptr;
  m_mediaButton = nullptr;
  m_weatherButton = nullptr;
  m_loadedAvatarPath.clear();
  m_wallpaperBg = nullptr;
  m_wallpaperGradient = nullptr;
  m_mediaTrack = nullptr;
  m_mediaArtist = nullptr;
  m_mediaStatus = nullptr;
  m_mediaProgress = nullptr;
  m_mediaArt = nullptr;
  m_mediaArtSlot = nullptr;
  m_mediaArtFallback = nullptr;
  m_loadedMediaArtUrl.clear();
  m_mediaPositionBusName.clear();
  m_mediaPositionTrackId.clear();
  m_mediaPositionTrackSignature.clear();
  m_mediaLastPlaybackStatus.clear();
  m_mediaPositionUs = 0;
  m_mediaPositionSampleAt = {};
  m_nextRealtimeUpdateAt = {};
  m_lastRealtimeMprisPollAt = {};
  m_shortcutsGrid = nullptr;
  m_shortcutPads.clear();
}

void HomeTab::onPanelCardOpacityChanged(float opacity) {
  if (m_settingsButton != nullptr) {
    panel_button_style::applyHeaderButtonStyle(*m_settingsButton, opacity);
  }
  if (m_sessionButton != nullptr) {
    panel_button_style::applyHeaderButtonStyle(*m_sessionButton, opacity);
  }
  syncShortcuts();
}

void HomeTab::syncScaledFonts() {
  const float s = contentScale();
  if (m_timeLabel != nullptr) {
    m_timeLabel->setFontSize(Style::fontSizeTitle * 1.7f * s);
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setFontSize(Style::fontSizeBody * 0.9f * s);
  }
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->setGlyphSize(Style::fontSizeCaption * 1.12f * s);
  }
  if (m_weatherLine != nullptr) {
    m_weatherLine->setFontSize(Style::fontSizeCaption * s);
  }
  if (m_userFacts != nullptr) {
    m_userFacts->setFontSize(Style::fontSizeCaption * s);
  }
  if (m_wallpaperButton != nullptr) {
    m_wallpaperButton->setGlyphSize(Style::fontSizeBody * s);
  }
  if (m_mediaButton != nullptr) {
    m_mediaButton->setGlyphSize(Style::fontSizeBody * s);
  }
  if (m_weatherButton != nullptr) {
    m_weatherButton->setGlyphSize(Style::fontSizeBody * s);
  }
  if (m_mediaTrack != nullptr) {
    m_mediaTrack->setFontSize(Style::fontSizeBody * 0.95f * s);
  }
  if (m_mediaArtist != nullptr) {
    m_mediaArtist->setFontSize(Style::fontSizeCaption * s);
  }
  if (m_mediaStatus != nullptr) {
    m_mediaStatus->setFontSize(Style::fontSizeCaption * s);
  }
  if (m_mediaProgress != nullptr) {
    m_mediaProgress->setFontSize(Style::fontSizeCaption * s);
  }
  for (auto& pad : m_shortcutPads) {
    if (pad.label != nullptr) {
      pad.label->setFontSize(Style::fontSizeMini * s);
    }
    if (pad.glyph != nullptr) {
      pad.glyph->setGlyphSize(Style::fontSizeTitle * 1.35f * s);
    }
  }
}

void HomeTab::sync(Renderer& renderer) {
  syncScaledFonts();
  syncShortcuts();

  if (m_timeLabel != nullptr) {
    m_timeLabel->setText(formatShellTime(m_config));
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setText(formatShellDate(m_config));
  }

  syncWallpaperBackground(renderer);

  if (m_userAvatar != nullptr && m_config != nullptr) {
    const std::string avatarPath = m_config->config().shell.avatarPath;
    if (avatarPath != m_loadedAvatarPath) {
      if (avatarPath.empty()) {
        m_userAvatar->clear(renderer);
      } else {
        m_userAvatar->setSourceFile(renderer, avatarPath, static_cast<int>(std::round(m_userAvatar->width())), true);
      }
      m_loadedAvatarPath = avatarPath;
    }
  }

  if (m_userFacts != nullptr) {
    const auto uptime = systemUptime();
    const std::string uptimeText =
        uptime.has_value() ? formatDuration(*uptime) : i18n::tr("control-center.home.unknown");
    m_userFacts->setText(
        i18n::tr(
            "control-center.home.user-facts", "user", sessionDisplayName(), "host", hostName(), "uptime", uptimeText,
            "version", noctalia::build_info::displayVersion()
        )
    );
  }

  if (m_weatherGlyph != nullptr && m_weatherLine != nullptr) {
    if (m_weather == nullptr || !m_weather->enabled()) {
      m_weatherGlyph->setGlyph("weather-cloud-off");
      m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_weatherLine->setText(i18n::tr("control-center.home.weather.disabled"));
    } else if (!m_weather->locationConfigured()) {
      m_weatherGlyph->setGlyph("weather-cloud");
      m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_weatherLine->setText(i18n::tr("control-center.home.weather.configure-location"));
    } else {
      const auto& snapshot = m_weather->snapshot();
      if (!snapshot.valid) {
        m_weatherGlyph->setGlyph("weather-cloud");
        m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        m_weatherLine->setText(
            m_weather->loading() ? i18n::tr("control-center.home.weather.fetching")
                                 : i18n::tr("control-center.home.weather.data-unavailable")
        );
      } else {
        m_weatherGlyph->setGlyph(WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay));
        m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::Primary));
        const int t = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
        m_weatherLine->setText(
            std::format(
                "{}{} · {}", t, m_weather->displayTemperatureUnit(),
                WeatherService::descriptionForCode(snapshot.current.weatherCode)
            )
        );
      }
    }
  }

  if (m_mediaTrack != nullptr && m_mediaArtist != nullptr && m_mediaStatus != nullptr && m_mediaProgress != nullptr) {
    if (m_mpris == nullptr) {
      m_mediaTrack->setText(i18n::tr("control-center.home.media.playback-unavailable"));
      m_mediaArtist->setText("");
      m_mediaArtist->setVisible(false);
      m_mediaStatus->setText(i18n::tr("control-center.home.media.unavailable"));
      m_mediaProgress->setText(" ");
      m_mediaProgress->setVisible(false);
      m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      if (m_mediaArt != nullptr) {
        m_mediaArt->clear(renderer);
        m_mediaArt->setVisible(false);
      }
      m_loadedMediaArtUrl.clear();
      PanelManager::instance().requestLayout();
    } else {
      const auto active = m_mpris->activePlayer();
      if (!active.has_value()) {
        m_mediaPositionBusName.clear();
        m_mediaPositionTrackId.clear();
        m_mediaPositionTrackSignature.clear();
        m_mediaLastPlaybackStatus.clear();
        m_mediaPositionUs = 0;
        m_mediaPositionSampleAt = {};
        m_mediaTrack->setText(i18n::tr("control-center.home.media.nothing-playing"));
        m_mediaArtist->setText("");
        m_mediaArtist->setVisible(false);
        m_mediaStatus->setText(i18n::tr("control-center.home.media.idle"));
        m_mediaProgress->setText(" ");
        m_mediaProgress->setVisible(false);
        m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        if (m_mediaArt != nullptr) {
          m_mediaArt->clear(renderer);
          m_mediaArt->setVisible(false);
        }
        m_loadedMediaArtUrl.clear();
        PanelManager::instance().requestLayout();
      } else {
        const std::string trackText =
            active->title.empty() ? i18n::tr("control-center.home.media.unknown-track") : active->title;
        const std::string artists = mpris::joinArtists(active->artists);
        const std::string artistText = artists.empty() ? i18n::tr("control-center.home.media.unknown-artist") : artists;
        if (m_mediaTrack->text() != trackText || m_mediaArtist->text() != artistText) {
          m_mediaTrack->setText(trackText);
          m_mediaArtist->setText(artistText);
          PanelManager::instance().requestLayout();
        }
        m_mediaArtist->setVisible(true);
        const std::string trackSignature = std::format(
            "{}\n{}\n{}\n{}\n{}", active->trackId, active->title, artists, active->album, active->sourceUrl
        );
        std::string progressText;
        if (active->lengthUs > 0) {
          const auto now = std::chrono::steady_clock::now();
          std::int64_t livePositionUs = std::max<std::int64_t>(0, active->positionUs);
          livePositionUs = std::clamp<std::int64_t>(livePositionUs, 0, active->lengthUs);
          const bool sameDisplayedTrack =
              m_mediaPositionBusName == active->busName && m_mediaPositionTrackSignature == trackSignature;
          const bool withinTransientRegressionWindow =
              m_mediaPositionSampleAt != std::chrono::steady_clock::time_point{} &&
              now - m_mediaPositionSampleAt <= kHomeTransientPositionRegressionWindow;
          const bool preserveDisplayedPosition =
              sameDisplayedTrack && m_mediaLastPlaybackStatus == "Playing" && active->playbackStatus == "Playing" &&
              m_mediaPositionUs >= kHomeTransientPositionRegressionFloorUs &&
              livePositionUs <= kHomeTransientPositionRegressionCeilingUs &&
              livePositionUs + kHomeTransientPositionRegressionDeltaUs < m_mediaPositionUs &&
              withinTransientRegressionWindow;
          if (preserveDisplayedPosition) {
            livePositionUs = m_mediaPositionUs;
          }

          m_mediaPositionBusName = active->busName;
          m_mediaPositionTrackId = active->trackId;
          m_mediaPositionTrackSignature = trackSignature;
          m_mediaLastPlaybackStatus = active->playbackStatus;
          if (!preserveDisplayedPosition) {
            m_mediaPositionUs = livePositionUs;
            m_mediaPositionSampleAt = now;
          }

          const std::int64_t positionSec = std::max<std::int64_t>(0, livePositionUs / 1000000);
          const std::int64_t lengthSec = std::max<std::int64_t>(1, active->lengthUs / 1000000);
          progressText = std::format("{} / {}", formatClockTime(positionSec), formatClockTime(lengthSec));
        } else {
          m_mediaPositionBusName.clear();
          m_mediaPositionTrackId.clear();
          m_mediaPositionTrackSignature.clear();
          m_mediaLastPlaybackStatus.clear();
          m_mediaPositionUs = 0;
          m_mediaPositionSampleAt = {};
        }
        m_mediaProgress->setText(" ");
        m_mediaProgress->setVisible(false);
        if (m_mediaArt != nullptr) {
          const std::string artUrl = mpris::effectiveArtUrl(*active);
          const bool artRetry = !artUrl.empty() && !m_mediaArt->hasImage();
          if (artUrl != m_loadedMediaArtUrl || artRetry) {
            std::string artPath = mpris::normalizeArtPath(artUrl);
            if (artPath.empty() && mpris::isRemoteArtUrl(artUrl)) {
              const auto cached = mpris::artCachePath(artUrl);
              std::error_code ec;
              if (std::filesystem::exists(cached, ec) && std::filesystem::file_size(cached, ec) > 0) {
                artPath = cached.string();
              } else if (m_httpClient != nullptr && m_pendingArtDownloads.find(artUrl) == m_pendingArtDownloads.end()) {
                std::filesystem::create_directories(cached.parent_path(), ec);
                m_pendingArtDownloads.insert(artUrl);
                m_httpClient->download(artUrl, cached, [this, url = artUrl](bool success) {
                  m_pendingArtDownloads.erase(url);
                  if (success) {
                    m_loadedMediaArtUrl.clear();
                    PanelManager::instance().refresh();
                  }
                });
              }
            }
            bool loaded = false;
            if (!artPath.empty()) {
              const int decodeSize = static_cast<int>(std::round(Style::controlHeightLg * 2.6f * contentScale()));
              loaded = m_mediaArt->setSourceFile(renderer, artPath, decodeSize, true);
              if (!loaded) {
                m_mediaArt->clear(renderer);
              }
            } else {
              m_mediaArt->clear(renderer);
            }
            m_mediaArt->setVisible(loaded);
            m_mediaArtFallback->setVisible(!loaded);
            m_loadedMediaArtUrl = loaded ? artUrl : std::string{};
            PanelManager::instance().requestLayout();
          }
        }
        std::string statusText;
        if (active->playbackStatus == "Playing") {
          statusText = i18n::tr("control-center.home.media.playing");
          m_mediaStatus->setColor(colorSpecFromRole(ColorRole::Primary));
        } else if (active->playbackStatus == "Paused") {
          statusText = i18n::tr("control-center.home.media.paused");
          m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        } else {
          statusText = active->playbackStatus;
          m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        }
        if (!progressText.empty()) {
          statusText = std::format("{} · {}", statusText, progressText);
        }
        if (m_mediaStatus->text() != statusText) {
          m_mediaStatus->setText(statusText);
          PanelManager::instance().requestLayout();
        }
      }
    }
  }
}

void HomeTab::syncShortcuts() {
  for (auto& pad : m_shortcutPads) {
    auto& sc = *pad.shortcut;
    const bool enabled = sc.enabled();
    const bool on = sc.isToggle() && sc.active();

    if (pad.button != nullptr) {
      applyShortcutButtonStyle(*pad.button, enabled, on, panelCardOpacity());
    }
    if (pad.glyph != nullptr) {
      pad.glyph->setGlyph(sc.displayIcon());
    }
    if (pad.button != nullptr && pad.label != nullptr) {
      const std::string label = sc.displayLabel();
      if (pad.label->text() != label) {
        pad.button->setText(label);
      }
      pad.label->setBaselineMode(LabelBaselineMode::InkCentered);
    }
  }
}
