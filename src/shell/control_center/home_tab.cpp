#include "shell/control_center/home_tab.h"

#include "config/config_service.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/wallpaper/wallpaper.h"
#include "system/distro_info.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/grid_view.h"
#include "ui/dialogs/file_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center");

  constexpr float kHomeAvatarScale = 2.6f;
  // Bottom row split: media/clock column grows more than the shortcuts column so the row feels balanced
  // (tweak either value slightly if needed).
  constexpr float kHomeMainColumnFlexGrow = 1.66f;
  constexpr float kHomeShortcutsFlexGrow = 1.0f;
  constexpr std::size_t kHomeShortcutGridColumns = 2;
  // At or below this count the shortcuts stack in a single narrow column instead of the 2-column grid.
  constexpr std::size_t kHomeStackedShortcutMax = 2;
  // Square tiles are trimmed slightly (height = width * trim) so the grid stays compact.
  constexpr float kHomeShortcutSquareTrim = 0.82f;
  constexpr auto kHomeTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kHomeTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kHomeTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kHomeTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr int kHomeMediaArtLayoutPassLimit = 8;

  float homeAvatarSize(float scale) { return Style::controlHeightLg * kHomeAvatarScale * scale; }

  // Width for the stacked (single-column) shortcuts so each tile keeps the same width it would have
  // as one cell of the standard 2-column grid, rather than stretching across the whole column.
  float homeStackedShortcutsWidth(float contentWidth, float bottomRowGap, const GridView& grid) {
    const float totalGrow = kHomeMainColumnFlexGrow + kHomeShortcutsFlexGrow;
    const float fullGridWidth = std::max(1.0f, contentWidth - bottomRowGap) * (kHomeShortcutsFlexGrow / totalGrow);
    const float horizontalPadding = grid.paddingLeft() + grid.paddingRight();
    const float fullGridInnerWidth = std::max(1.0f, fullGridWidth - horizontalPadding);
    const float cellWidth = std::max(
        1.0f,
        (fullGridInnerWidth - grid.columnGap() * static_cast<float>(kHomeShortcutGridColumns - 1))
            / static_cast<float>(kHomeShortcutGridColumns)
    );
    return cellWidth + horizontalPadding;
  }

  // The 2-column shortcuts grid's natural width = its flex share of the bottom row.
  float homeShortcutsGridNaturalWidth(float contentWidth, float bottomRowGap) {
    const float totalGrow = kHomeMainColumnFlexGrow + kHomeShortcutsFlexGrow;
    return std::max(1.0f, contentWidth - bottomRowGap) * (kHomeShortcutsFlexGrow / totalGrow);
  }

  std::filesystem::path avatarStartDirectory(const AccountsService* accounts, const ConfigService* config) {
    const std::string currentPath =
        config != nullptr ? shell::resolvedAvatarPath(accounts, config->config()) : std::string{};
    const std::filesystem::path current(currentPath);
    std::error_code ec;
    if (!current.empty() && std::filesystem::exists(current, ec) && current.has_parent_path()) {
      return current.parent_path();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / "Pictures";
    }
    return {};
  }

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

  std::string userHostLine() { return std::format("{}@{}", sessionDisplayName(), hostName()); }

  std::string noctaliaVersionLine() { return std::format("Noctalia {}", noctalia::build_info::displayVersion()); }

  void applyHomeCardStyle(Flex& card, float scale, float fillOpacity, bool showBorder) {
    applySectionCardStyle(card, scale, fillOpacity, showBorder);
    card.setGap(Style::spaceSm * scale);
  }

  void applyShortcutButtonStyle(Button& button, bool enabled, bool active, float fillOpacity) {
    const bool on = enabled && active;
    button.setVariant(on ? ButtonVariant::Primary : ButtonVariant::Default);
    button.setSurfaceOpacity(on ? 1.0f : fillOpacity);
    button.setEnabled(enabled);
  }

  // The whole home cards are clickable; on hover swap the card outline to the hover colour. No fill
  // change — the user card's fill sits behind the wallpaper, so a thin hover border is the one hover
  // signal that reads consistently across all three cards.
  void applyHomeCardHover(Flex& card, bool hovered, bool baseBorders) {
    if (hovered) {
      card.setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
    } else if (baseBorders) {
      card.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
    } else {
      card.clearBorder();
    }
  }

  void applyAvatarChrome(Image* avatar, bool highlighted) {
    if (avatar == nullptr) {
      return;
    }
    const float borderWidth = Style::borderWidth * 3.0f;
    if (highlighted) {
      avatar->setBorder(colorSpecFromRole(ColorRole::Hover), borderWidth);
      return;
    }
    avatar->setBorder(colorSpecFromRole(ColorRole::Primary), borderWidth);
  }

} // namespace

HomeTab::HomeTab(const ControlCenterServices& services)
    : m_mpris(services.mpris), m_httpClient(services.httpClient), m_weather(services.weather),
      m_config(services.config), m_accounts(services.accounts), m_wallpaper(services.wallpaper),
      m_thumbnails(services.thumbnails), m_services(services.shortcutServices()) {
  if (m_thumbnails != nullptr) {
    m_thumbnailPendingSub = m_thumbnails->subscribePendingUpload([this]() {
      if (m_wallpaperBg == nullptr) {
        return;
      }
      PanelManager::instance().requestUpdateOnly();
    });
  }

  // Pre-warm the wallpaper preview thumbnail as soon as the wallpaper changes,
  // even while the control center is closed, so it is already decoded by the
  // time the panel opens. Uses the last preview size the home card requested.
  if (m_wallpaper != nullptr) {
    m_wallpaperChangedConn = m_wallpaper->changed().connect([this]() {
      if (m_thumbnails != nullptr && m_loadedWallpaperSize > 0) {
        ensureWallpaperThumbnail(m_wallpaper->currentPath(), m_loadedWallpaperSize);
      }
      if (m_wallpaperBg != nullptr) {
        PanelManager::instance().requestUpdateOnly();
      }
    });
  }
}

HomeTab::~HomeTab() {
  if (m_thumbnails != nullptr && !m_loadedWallpaperPath.empty()) {
    m_thumbnails->release(m_loadedWallpaperPath, m_loadedWallpaperSize);
  }
}

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
    const float wallpaperRadius = std::max(0.0f, Style::scaledRadiusXl(scale) - Style::borderWidth);
    userCard->addChild(
        ui::image({
            .out = &m_wallpaperPlaceholder,
            .fit = ImageFit::Cover,
            .radius = wallpaperRadius,
            .participatesInLayout = false,
            .configure = [](Image& image) { image.setZIndex(-2); },
        })
    );

    userCard->addChild(
        ui::image({
            .out = &m_wallpaperBg,
            .fit = ImageFit::Cover,
            .radius = wallpaperRadius,
            .participatesInLayout = false,
            .configure = [](Image& image) {
              image.setZIndex(-1);
              image.setOpacity(0.0f);
            },
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
  const auto openAvatarPicker = [this]() {
    if (m_config == nullptr) {
      return;
    }

    FileDialogOptions options;
    options.mode = FileDialogMode::Open;
    options.defaultViewMode = FileDialogViewMode::Grid;
    options.title = i18n::tr("control-center.home.select-avatar");
    options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif"};
    options.startDirectory = avatarStartDirectory(m_accounts, m_config);

    (void)FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> pickedPath) {
      if (!pickedPath.has_value() || m_config == nullptr) {
        return;
      }
      const auto applyResult = shell::applyAvatarPath(m_accounts, m_config, pickedPath->string());
      if (applyResult.success()) {
        m_loadedAvatarPath.clear();
        DeferredCall::callLater([]() {
          PanelManager::instance().refresh();
          PanelManager::instance().requestRedraw();
        });
        return;
      }
      notify::error(
          "Noctalia", i18n::tr("control-center.home.avatar-error-title"),
          i18n::tr(shell::avatarApplyErrorTranslationKey(applyResult.error))
      );
    });
  };

  auto avatarArea = std::make_unique<InputArea>();
  avatarArea->setSize(avatarSize, avatarSize);
  avatarArea->setHitShape(InputArea::HitShape::Circle);
  avatarArea->setFocusable(true);
  avatarArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  avatarArea->setOnClick([openAvatarPicker](const InputArea::PointerData&) { openAvatarPicker(); });
  avatarArea->setOnKeyDown([openAvatarPicker](const InputArea::KeyData& key) {
    if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      openAvatarPicker();
    }
  });
  m_userAvatarArea = avatarArea.get();
  avatarArea->addChild(
      ui::image({
          .out = &m_userAvatar,
          .fit = ImageFit::Cover,
          .radius = avatarSize * 0.5f,
          .padding = 1.0f * scale,
          .width = avatarSize,
          .height = avatarSize,
          .configure = [](Image& image) {
            image.setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * 3.0f);
            image.setHitTestVisible(false);
          },
      })
  );
  const auto syncAvatarChrome = [this]() {
    const bool highlighted =
        m_userAvatarArea != nullptr && (m_userAvatarArea->focused() || m_userAvatarArea->hovered());
    applyAvatarChrome(m_userAvatar, highlighted);
    PanelManager::instance().requestRedraw();
  };
  avatarArea->setOnEnter([syncAvatarChrome](const InputArea::PointerData&) { syncAvatarChrome(); });
  avatarArea->setOnLeave([syncAvatarChrome]() { syncAvatarChrome(); });
  avatarArea->setOnFocusGain(syncAvatarChrome);
  avatarArea->setOnFocusLoss(syncAvatarChrome);
  const auto configureUserDetailLabel = [scale](Label& label) {
    label.setShadow(Color{0.0f, 0.0f, 0.0f, 0.36f}, 0.0f, 1.0f * scale);
  };
  auto userRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale}, std::move(avatarArea),
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
              .out = &m_userHost,
              .text = userHostLine(),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          }),
          ui::label({
              .out = &m_userUptime,
              .text = "…",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          }),
          ui::label({
              .out = &m_userVersion,
              .text = noctaliaVersionLine(),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          })
      )
  );
  userCard->addChild(std::move(userRow));

  // Wallpaper panel: full-card keyboard target; carved pointer target leaves the avatar clickable.
  const auto openWallpaperPanel = []() { PanelManager::instance().togglePanel("wallpaper"); };
  m_userCardKeyboardArea =
      addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = true, .pointerHitTest = false});
  m_userCardArea = addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = false, .pointerHitTest = true});

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

  // Clicking anywhere on the media card opens the media tab.
  m_mediaCardArea = addCardOverlay(*m_mediaCard, []() { openControlCenterTab("media"); });

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

  // Clicking anywhere on the clock/weather card opens the weather tab.
  m_dateTimeCardArea = addCardOverlay(*m_dateTimeCard, []() { openControlCenterTab("weather"); });

  leftColumn->addChild(std::move(mediaCard));
  leftColumn->addChild(std::move(dateTimeCard));
  bottomRow->addChild(std::move(leftColumn));

  // --- Shortcuts (right of media + clock) ---
  const auto& shortcuts =
      m_config != nullptr ? m_config->config().controlCenter.shortcuts : std::vector<ShortcutConfig>{};
  const std::size_t count = std::min(shortcuts.size(), std::size_t{6});

  auto grid = std::make_unique<GridView>();
  grid->setColumns(kHomeShortcutGridColumns);
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
        .radius = Style::scaledRadiusXl(scale),
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
    if (auto* ia = btnPtr->inputArea(); ia != nullptr) {
      ia->setOnAxisHandler([this, padIdx](const InputArea::PointerData& data) -> bool {
        if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || padIdx >= m_shortcutPads.size()) {
          return false;
        }
        // Scroll up moves forward (toward performance); Wayland reports up as a negative delta.
        m_shortcutPads[padIdx].shortcut->onScroll(data.scrollDelta(1.0f) > 0 ? -1 : 1);
        return true;
      });
    }
    ShortcutPad pad;
    pad.shortcut = std::move(shortcut);
    pad.button = btnPtr;
    pad.glyph = btnPtr->glyph();
    pad.label = btnPtr->label();
    m_shortcutPads.push_back(std::move(pad));
    grid->addChild(std::move(btn));
  }

  if (m_shortcutPads.size() <= kHomeStackedShortcutMax) {
    grid->setColumns(1);
    grid->setFlexGrow(0.0f);
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
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_sessionButton,
          .glyph = "shutdown",
          .onClick = []() { PanelManager::instance().togglePanel("session"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
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
  if (m_shortcutsGrid != nullptr && !m_shortcutPads.empty()) {
    const float scale = contentScale();
    const float bottomRowGap = m_bottomRow != nullptr ? m_bottomRow->gap() : 0.0f;
    const bool stacked = m_shortcutPads.size() <= kHomeStackedShortcutMax;
    const std::size_t cols = stacked ? 1u : kHomeShortcutGridColumns;
    const std::size_t rows = (m_shortcutPads.size() + cols - 1) / cols;
    const float padH = m_shortcutsGrid->paddingLeft() + m_shortcutsGrid->paddingRight();
    const float padV = m_shortcutsGrid->paddingTop() + m_shortcutsGrid->paddingBottom();
    const float colGap = m_shortcutsGrid->columnGap();
    const float rowGap = m_shortcutsGrid->rowGap();

    float gridWidth = stacked ? homeStackedShortcutsWidth(contentWidth, bottomRowGap, *m_shortcutsGrid)
                              : homeShortcutsGridNaturalWidth(contentWidth, bottomRowGap);

    // A wide control center would otherwise grow the square tiles tall enough to crush the
    // user card (which content-overflows). Reserve the user card's natural height and cap the
    // square side to fit; cap the grid width to keep tiles square, handing the freed width to
    // the media/clock column.
    const float userCardReserve = homeAvatarSize(scale) + 2.0f * (Style::spaceSm + Style::spaceXs) * scale;
    const float rootGap = m_rootLayout->gap();
    const float availForGrid = std::max(1.0f, bodyHeight - userCardReserve - rootGap);
    const float maxCellSide =
        std::max(1.0f, (availForGrid - static_cast<float>(rows - 1) * rowGap - padV) / static_cast<float>(rows));
    const float maxGridWidth = static_cast<float>(cols) * (maxCellSide / kHomeShortcutSquareTrim)
        + static_cast<float>(cols - 1) * colGap
        + padH;

    const bool capped = gridWidth > maxGridWidth;
    if (capped) {
      gridWidth = maxGridWidth;
    }
    // Pin the width (flexGrow 0) for the stacked column or whenever capped, so the left column
    // absorbs the slack; otherwise let the 2-column grid flex-grow normally.
    if (stacked || capped) {
      m_shortcutsGrid->setFlexGrow(0.0f);
      m_shortcutsGrid->setSize(gridWidth, m_shortcutsGrid->height());
    } else {
      m_shortcutsGrid->setFlexGrow(kHomeShortcutsFlexGrow);
    }
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
        1.0f,
        dateTimeRightWrap
            - (m_weatherGlyph != nullptr ? m_weatherGlyph->width() : 0.0f)
            - Style::spaceXs * contentScale()
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

  if (m_userCard != nullptr) {
    const float userWrap = innerWidth(m_userCard);
    for (Label* label : {m_userHost, m_userUptime, m_userVersion}) {
      if (label != nullptr) {
        label->setMaxWidth(userWrap);
        label->setMaxLines(1);
      }
    }
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
    // doesn't tower over the user card area. The width was capped earlier so this stays bounded.
    const float cellSide = cellWidth * kHomeShortcutSquareTrim;
    const float gridH = static_cast<float>(rows) * cellSide
        + static_cast<float>(rows > 0 ? rows - 1 : 0) * m_shortcutsGrid->rowGap()
        + m_shortcutsGrid->paddingTop()
        + m_shortcutsGrid->paddingBottom();
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
  layoutCardOverlays();
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->measure(renderer);
  }
}

InputArea* HomeTab::addCardOverlay(Flex& card, std::function<void()> onActivate) {
  return addCardOverlay(card, std::move(onActivate), CardOverlayOptions{});
}

InputArea* HomeTab::addCardOverlay(Flex& card, std::function<void()> onActivate, CardOverlayOptions options) {
  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setZIndex(3);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  if (!options.pointerHitTest) {
    area->setHitTestVisible(false);
  }
  if (options.keyboardFocus) {
    area->setFocusable(true);
  } else {
    area->setFocusable(false);
    area->setTabStop(false);
  }

  Flex* cardPtr = &card;
  const bool borders = panelBordersEnabled();
  InputArea* areaPtr = area.get();
  std::function<void()> activate = std::move(onActivate);

  const auto setHovered = [cardPtr, borders](bool hovered) {
    applyHomeCardHover(*cardPtr, hovered, borders);
    PanelManager::instance().requestRedraw();
  };

  if (options.pointerHitTest) {
    area->setOnEnter([setHovered](const InputArea::PointerData&) { setHovered(true); });
    area->setOnLeave([setHovered, areaPtr]() {
      if (areaPtr->focused()) {
        return;
      }
      setHovered(false);
    });
    area->setOnClick([activate](const InputArea::PointerData&) { activate(); });
  }
  if (options.keyboardFocus) {
    area->setOnFocusGain([setHovered]() { setHovered(true); });
    area->setOnFocusLoss([setHovered, areaPtr]() {
      if (areaPtr->hovered()) {
        return;
      }
      setHovered(false);
    });
    area->setOnKeyDown([activate](const InputArea::KeyData& key) {
      if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
        activate();
      }
    });
  }

  return static_cast<InputArea*>(card.addChild(std::move(area)));
}

void HomeTab::layoutCardOverlays() {
  const auto cover = [](Flex* card, InputArea* area) {
    if (card == nullptr || area == nullptr) {
      return;
    }
    area->setPosition(0.0f, 0.0f);
    area->setSize(card->width(), card->height());
  };
  cover(m_mediaCard, m_mediaCardArea);
  cover(m_dateTimeCard, m_dateTimeCardArea);
  cover(m_userCard, m_userCardKeyboardArea);

  // The pointer overlay must not swallow the avatar's own click, so start it just past the
  // avatar's right edge; the avatar (a nested InputArea) keeps the carved-out left region.
  if (m_userCard != nullptr && m_userCardArea != nullptr) {
    float left = 0.0f;
    if (m_userAvatar != nullptr) {
      float ax = 0.0f, ay = 0.0f, cx = 0.0f, cy = 0.0f;
      Node::absolutePosition(m_userAvatar, ax, ay);
      Node::absolutePosition(m_userCard, cx, cy);
      left = std::max(0.0f, (ax - cx) + m_userAvatar->width() + Style::spaceMd * contentScale());
    }
    m_userCardArea->setPosition(left, 0.0f);
    m_userCardArea->setSize(std::max(0.0f, m_userCard->width() - left), m_userCard->height());
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
  if (m_wallpaperPlaceholder != nullptr) {
    m_wallpaperPlaceholder->setPosition(bw, bw);
    m_wallpaperPlaceholder->setSize(cw, ch);
  }

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

void HomeTab::ensureWallpaperThumbnail(const std::string& path, int targetPx) {
  if (m_thumbnails == nullptr) {
    return;
  }
  if (path == m_loadedWallpaperPath && targetPx == m_loadedWallpaperSize) {
    return;
  }
  if (!m_loadedWallpaperPath.empty() && m_loadedWallpaperSize > 0) {
    m_thumbnails->release(m_loadedWallpaperPath, m_loadedWallpaperSize);
  }
  if (!path.empty() && targetPx > 0) {
    (void)m_thumbnails->acquire(path, targetPx);
  }
  m_loadedWallpaperPath = path;
  m_loadedWallpaperSize = targetPx;
}

void HomeTab::syncWallpaperBackground(Renderer& renderer) {
  if (m_wallpaperBg == nullptr || m_wallpaperPlaceholder == nullptr) {
    return;
  }

  const std::string path = m_wallpaper != nullptr ? m_wallpaper->currentPath() : std::string{};
  const float renderScale = std::max(1.0f, renderer.renderScale());
  const int targetPx =
      static_cast<int>(std::lround(std::max(m_wallpaperBg->width(), m_wallpaperBg->height()) * renderScale));

  ensureWallpaperThumbnail(path, targetPx);

  if (path.empty()) {
    m_wallpaperPlaceholder->setVisible(false);
    m_wallpaperBg->setVisible(false);
    cancelCrispFade();
    m_wallpaperBg->setOpacity(0.0f);
    m_crispWorkingPath.clear();
    m_crispWorkingSize = 0;
    m_crispShown = false;
    m_crispNeedsFade = false;
    return;
  }

  // Instant placeholder: show the resident full-screen wallpaper texture (already
  // in VRAM, mipmapped) so the correct wallpaper appears with no decode wait.
  const TextureHandle resident = m_wallpaper != nullptr ? m_wallpaper->currentTexture() : TextureHandle{};
  if (resident.valid()) {
    m_wallpaperPlaceholder->setExternalTexture(renderer, resident);
    m_wallpaperPlaceholder->setVisible(true);
  } else {
    m_wallpaperPlaceholder->setVisible(false);
  }

  // Reset the crisp layer when the wallpaper identity or target size changes; the
  // placeholder carries the view until the new card-sized thumbnail is decoded.
  if (path != m_crispWorkingPath || targetPx != m_crispWorkingSize) {
    m_crispWorkingPath = path;
    m_crispWorkingSize = targetPx;
    m_crispShown = false;
    m_crispNeedsFade = false;
    cancelCrispFade();
    m_wallpaperBg->setOpacity(0.0f);
    m_wallpaperBg->setVisible(false);
  }

  if (m_thumbnails == nullptr || targetPx <= 0 || m_crispShown) {
    return;
  }

  (void)m_thumbnails->uploadPending(renderer.textureManager());
  const TextureHandle crisp = m_thumbnails->peek(path, targetPx);
  if (!crisp.valid()) {
    // Still decoding: keep the placeholder; fade the crisp layer in once it lands.
    m_crispNeedsFade = true;
    return;
  }

  m_wallpaperBg->setExternalTexture(renderer, crisp);
  m_wallpaperBg->setVisible(true);
  m_crispShown = true;
  if (m_crispNeedsFade) {
    startCrispFade();
  } else {
    // Ready on the first look (cached) — snap in without a crossfade.
    cancelCrispFade();
    m_wallpaperBg->setOpacity(1.0f);
  }
}

void HomeTab::startCrispFade() {
  if (m_wallpaperBg == nullptr) {
    return;
  }
  AnimationManager* animations = m_wallpaperBg->animationManager();
  if (animations == nullptr) {
    m_wallpaperBg->setOpacity(1.0f);
    return;
  }
  cancelCrispFade();
  Image* crisp = m_wallpaperBg;
  m_wallpaperCrispAnimId = animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [crisp](float v) { crisp->setOpacity(v); }, [this]() { m_wallpaperCrispAnimId = 0; }, crisp
  );
}

void HomeTab::cancelCrispFade() {
  if (m_wallpaperCrispAnimId != 0 && m_wallpaperBg != nullptr) {
    if (AnimationManager* animations = m_wallpaperBg->animationManager()) {
      animations->cancel(m_wallpaperCrispAnimId);
    }
  }
  m_wallpaperCrispAnimId = 0;
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
  m_userHost = nullptr;
  m_userUptime = nullptr;
  m_userVersion = nullptr;
  m_settingsButton = nullptr;
  m_sessionButton = nullptr;
  m_userCardKeyboardArea = nullptr;
  m_userCardArea = nullptr;
  m_mediaCardArea = nullptr;
  m_dateTimeCardArea = nullptr;
  m_loadedAvatarPath.clear();
  m_loadedAvatarSize = 0;
  // The crisp fade animation is tagged with the m_wallpaperBg node as owner, so
  // it is cancelled automatically when the node tree is destroyed on close.
  m_wallpaperCrispAnimId = 0;
  m_crispWorkingPath.clear();
  m_crispWorkingSize = 0;
  m_crispShown = false;
  m_crispNeedsFade = false;
  m_wallpaperPlaceholder = nullptr;
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
  (void)opacity;
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
  for (Label* label : {m_userHost, m_userUptime, m_userVersion}) {
    if (label != nullptr) {
      label->setFontSize(Style::fontSizeCaption * s);
    }
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
    const std::string displayPath = shell::avatarDisplayPath(m_accounts, m_config->config());
    const int avatarSize = static_cast<int>(std::round(m_userAvatar->width()));
    if (displayPath != m_loadedAvatarPath || avatarSize != m_loadedAvatarSize) {
      if (displayPath.empty()) {
        m_userAvatar->clear(renderer);
      } else {
        // Decode at the avatar's final on-screen size with no mipmaps: layout grows the
        // avatar to match the user text block, and trilinear mipmap sampling softens an
        // image displayed near 1:1. Both made the avatar look blurry.
        (void)m_userAvatar->setSourceFile(renderer, displayPath, avatarSize, false);
      }
      m_loadedAvatarPath = displayPath;
      m_loadedAvatarSize = avatarSize;
    }
  }

  if (m_userHost != nullptr) {
    m_userHost->setText(userHostLine());
  }
  if (m_userUptime != nullptr) {
    const auto uptime = systemUptime();
    const std::string uptimeText =
        uptime.has_value() ? formatDuration(*uptime) : i18n::tr("control-center.home.unknown");
    m_userUptime->setText(i18n::tr("control-center.home.uptime", "uptime", uptimeText));
  }
  if (m_userVersion != nullptr) {
    m_userVersion->setText(noctaliaVersionLine());
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
              m_mediaPositionSampleAt != std::chrono::steady_clock::time_point{}
              && now - m_mediaPositionSampleAt <= kHomeTransientPositionRegressionWindow;
          const bool preserveDisplayedPosition = sameDisplayedTrack
              && m_mediaLastPlaybackStatus == "Playing"
              && active->playbackStatus == "Playing"
              && m_mediaPositionUs >= kHomeTransientPositionRegressionFloorUs
              && livePositionUs <= kHomeTransientPositionRegressionCeilingUs
              && livePositionUs + kHomeTransientPositionRegressionDeltaUs < m_mediaPositionUs
              && withinTransientRegressionWindow;
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
            const std::string artPath = mpris::resolveArtworkSource(
                m_httpClient, m_pendingArtDownloads, artUrl,
                [this] {
                  m_loadedMediaArtUrl.clear();
                  PanelManager::instance().refresh();
                },
                m_aliveGuard
            );
            bool loaded = false;
            if (!artPath.empty()) {
              const int decodeSize = static_cast<int>(std::round(Style::controlHeightLg * 2.6f * contentScale()));
              loaded = m_mediaArt->setSourceFile(renderer, artPath, decodeSize, true, true);
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
