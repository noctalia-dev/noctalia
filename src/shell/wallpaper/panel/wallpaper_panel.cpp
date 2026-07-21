#include "shell/wallpaper/panel/wallpaper_panel.h"

#include "config/config_service.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "shell/wallpaper/panel/wallpaper_tile.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "theme/builtin_palettes.h"
#include "theme/community_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/theme_service.h"
#include "ui/builders.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr)
      return false;
    for (const Node* p = node->parent(); p != nullptr; p = p->parent()) {
      if (p == ancestor)
        return true;
    }
    return false;
  }

  constexpr Logger kLog("wp-panel");
  constexpr auto kFilterDebounceInterval = std::chrono::milliseconds(120);
  constexpr float kMinTileWidth = 180.0f;
  constexpr float kMonitorSelectMinWidth = 136.0f;
  constexpr float kFavoriteSelectMinWidth = 168.0f;
  constexpr float kFavoritesMetaRowGap = Style::spaceSm;
  constexpr float kTileAspect = 0.78f; // height / width — leaves room for label under widescreen thumb

  [[nodiscard]] std::size_t themeModeSegmentIndex(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Dark:
      return 0;
    case ThemeMode::Light:
      return 1;
    case ThemeMode::Auto:
    default:
      return 2;
    }
  }

  [[nodiscard]] ThemeMode themeModeFromSegmentIndex(std::size_t index) {
    switch (index) {
    case 0:
      return ThemeMode::Dark;
    case 1:
      return ThemeMode::Light;
    default:
      return ThemeMode::Auto;
    }
  }

  [[nodiscard]] std::vector<std::string> wallpaperSchemeOptions() {
    return {
        i18n::tr("theme.scheme.m3-content"),     i18n::tr("theme.scheme.m3-tonal-spot"),
        i18n::tr("theme.scheme.m3-fruit-salad"), i18n::tr("theme.scheme.m3-rainbow"),
        i18n::tr("theme.scheme.m3-monochrome"),  i18n::tr("theme.scheme.vibrant"),
        i18n::tr("theme.scheme.faithful"),       i18n::tr("theme.scheme.soft"),
        i18n::tr("theme.scheme.dysfunctional"),  i18n::tr("theme.scheme.muted"),
    };
  }

  [[nodiscard]] std::vector<std::string> wallpaperSchemeValues() {
    return {
        "m3-content", "m3-tonal-spot", "m3-fruit-salad", "m3-rainbow",    "m3-monochrome",
        "vibrant",    "faithful",      "soft",           "dysfunctional", "muted",
    };
  }

  [[nodiscard]] std::optional<ThemeMode> favoriteThemeBadge(const WallpaperFavorite* favorite) {
    if (favorite == nullptr) {
      return std::nullopt;
    }
    if (favorite->themeMode == ThemeMode::Light || favorite->themeMode == ThemeMode::Dark) {
      return favorite->themeMode;
    }
    return std::nullopt;
  }

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  std::string colorWallpaperPath(const Color& color) { return "color:" + formatRgbHex(color); }

  bool wallpaperEntryIsCurrent(const WallpaperEntry& entry, std::string_view currentWallpaperPath) {
    if (entry.isDir) {
      return false;
    }
    return FileUtils::normalizeWallpaperPath(entry.absPath.string())
        == FileUtils::normalizeWallpaperPath(currentWallpaperPath);
  }

  [[nodiscard]] WallpaperFavorite wallpaperFavoriteFromTheme(const ThemeConfig& theme) {
    WallpaperFavorite favorite;
    favorite.themeMode = theme.mode;
    favorite.paletteSource = theme.source;
    favorite.builtinPalette = theme.builtinPalette;
    favorite.communityPalette = theme.communityPalette;
    favorite.customPalette = theme.customPalette;
    favorite.wallpaperScheme = theme.wallpaperScheme;
    return favorite;
  }

  [[nodiscard]] std::string paletteSelectionValue(const WallpaperFavorite& theme) {
    switch (theme.paletteSource.value_or(PaletteSource::Builtin)) {
    case PaletteSource::Builtin:
      return theme.builtinPalette;
    case PaletteSource::Wallpaper:
      return theme.wallpaperScheme;
    case PaletteSource::Community:
      return theme.communityPalette;
    case PaletteSource::Custom:
      return theme.customPalette;
    }
    return {};
  }

  [[nodiscard]] std::filesystem::path canonicalWallpaperDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) {
      return {};
    }
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(dir, ec);
    return ec ? dir.lexically_normal() : canonical;
  }

  [[nodiscard]] bool wallpaperDirectoriesEquivalent(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) {
      return a.empty() && b.empty();
    }
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) {
      return true;
    }
    return canonicalWallpaperDirectory(a) == canonicalWallpaperDirectory(b);
  }

  [[nodiscard]] bool
  wallpaperPathInsideDirectory(const std::filesystem::path& file, const std::filesystem::path& directory) {
    const std::filesystem::path dir = canonicalWallpaperDirectory(directory);
    if (dir.empty()) {
      return false;
    }
    std::error_code ec;
    const auto filePath = std::filesystem::weakly_canonical(file, ec);
    if (ec) {
      return false;
    }
    const auto rel = std::filesystem::relative(filePath.parent_path(), dir, ec);
    if (ec || rel.empty()) {
      return wallpaperDirectoriesEquivalent(filePath.parent_path(), dir);
    }
    const std::string relText = rel.generic_string();
    return !relText.starts_with("..");
  }

  [[nodiscard]] int caseInsensitiveNameOrder(std::string_view a, std::string_view b) {
    return StringUtils::naturalCaseInsensitiveCompare(a, b);
  }

  [[nodiscard]] std::optional<std::filesystem::file_time_type> entryModifiedTime(const WallpaperEntry& entry) {
    if (entry.isDir || entry.absPath.string().starts_with("color:")) {
      return std::nullopt;
    }
    // Scanned entries carry the mtime captured during the directory walk, so the
    // sort comparator never stats. Pinned favorites lack it and stat once here.
    if (entry.hasMtime) {
      return entry.mtime;
    }
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(entry.absPath, ec);
    if (ec) {
      return std::nullopt;
    }
    return mtime;
  }

  [[nodiscard]] bool favoriteVisibleInBrowseContext(
      std::string_view favoritePath, const std::filesystem::path& activeDir, const std::filesystem::path& rootDir,
      bool flatten
  ) {
    if (favoritePath.starts_with("color:")) {
      return wallpaperDirectoriesEquivalent(activeDir, rootDir);
    }

    const std::filesystem::path favoriteFile = std::filesystem::path(FileUtils::normalizeWallpaperPath(favoritePath));
    if (wallpaperDirectoriesEquivalent(activeDir, rootDir)) {
      return true;
    }
    if (!flatten) {
      return wallpaperDirectoriesEquivalent(favoriteFile.parent_path(), activeDir);
    }
    return wallpaperPathInsideDirectory(favoriteFile, activeDir);
  }

} // namespace

class WallpaperGridAdapter : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(const WallpaperEntry&)>;
  using StarCallback = std::function<void(const WallpaperEntry&)>;

  explicit WallpaperGridAdapter(float scale) : m_scale(scale) {}

  void setEntries(const std::vector<WallpaperEntry>* entries) { m_entries = entries; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setConfig(ConfigService* config) { m_config = config; }
  void setCurrentWallpaperPath(std::string path) { m_currentWallpaperPath = std::move(path); }
  void setThumbnailService(ThumbnailService* service) {
    m_thumbnails = service;
    for (WallpaperTile* tile : m_pool) {
      if (tile != nullptr) {
        tile->setThumbnailService(service);
      }
    }
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnStarToggle(StarCallback callback) { m_onStarToggle = std::move(callback); }

  void refreshVisibleThumbnails(Renderer& renderer) {
    for (WallpaperTile* tile : m_pool) {
      if (tile != nullptr && tile->visible()) {
        tile->refreshThumbnail(renderer);
      }
    }
  }

  [[nodiscard]] std::size_t itemCount() const override { return m_entries == nullptr ? 0U : m_entries->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    auto tile = std::make_unique<WallpaperTile>(0.0f, 0.0f, m_scale);
    tile->setThumbnailService(m_thumbnails);
    tile->setOnStarClick([this](const WallpaperEntry& entry) {
      if (m_onStarToggle) {
        m_onStarToggle(entry);
      }
    });
    m_pool.push_back(tile.get());
    return tile;
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    auto* wt = static_cast<WallpaperTile*>(&tile);
    wt->setCellSize(wt->width(), wt->height());
    if (m_renderer != nullptr && m_entries != nullptr && index < m_entries->size()) {
      const auto& entry = (*m_entries)[index];
      wt->setEntry(entry, *m_renderer);
      if (m_config != nullptr && !entry.isDir) {
        const std::string path = entry.absPath.string();
        wt->setFavoriteState(
            m_config->isWallpaperFavorite(path), favoriteThemeBadge(m_config->wallpaperFavorite(path))
        );
      } else {
        wt->setFavoriteState(false, std::nullopt);
      }
    }
    wt->setSelected(selected);
    wt->setCurrent(
        m_entries != nullptr
        && index < m_entries->size()
        && wallpaperEntryIsCurrent((*m_entries)[index], m_currentWallpaperPath)
    );
    wt->setHoveredVisual(hovered && !selected);
  }

  // Grid overlay owns clicks; star toggles dispatch here.
  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (m_entries == nullptr || index >= m_entries->size()) {
      return false;
    }
    const auto& entry = (*m_entries)[index];
    if (entry.isDir) {
      return false;
    }
    if (!WallpaperTile::hitTestStarRegion(cellWidth, cellHeight, m_scale, cellLocalX, cellLocalY)) {
      return false;
    }
    if (m_onStarToggle) {
      m_onStarToggle(entry);
    }
    return true;
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight
  ) const override {
    if (m_entries == nullptr || index >= m_entries->size()) {
      return false;
    }
    if ((*m_entries)[index].isDir) {
      return false;
    }
    return WallpaperTile::hitTestStarRegion(cellWidth, cellHeight, m_scale, cellLocalX, cellLocalY);
  }

  void applyOverlayHover(Node& tile, bool hovered) override {
    static_cast<WallpaperTile&>(tile).setStarHovered(hovered);
  }

  void onActivate(std::size_t index) override {
    if (!m_onActivate || m_entries == nullptr || index >= m_entries->size()) {
      return;
    }
    m_onActivate((*m_entries)[index]);
  }

private:
  float m_scale;
  const std::vector<WallpaperEntry>* m_entries = nullptr;
  std::string m_currentWallpaperPath;
  Renderer* m_renderer = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  ConfigService* m_config = nullptr;

  std::vector<WallpaperTile*> m_pool;
  ActivateCallback m_onActivate;
  StarCallback m_onStarToggle;
};

WallpaperPanel::WallpaperPanel(
    WaylandConnection* wayland, ConfigService* config, ThumbnailService* thumbnails, WallpaperScanner* scanner,
    noctalia::theme::ThemeService* themeService
)
    : m_wayland(wayland), m_config(config), m_thumbnails(thumbnails), m_scanner(scanner), m_themeService(themeService) {
  if (m_config != nullptr) {
    m_flatten = m_config->stateBool("wallpaper_panel", "flatten").value_or(false);
    if (const std::optional<std::string> sort = m_config->stateString("wallpaper_panel", "sort")) {
      m_sortMode = sortModeFromState(*sort);
    }
  }
  if (m_scanner != nullptr) {
    m_scanner->setOnComplete([this]() { onScanComplete(); });
  }
}

WallpaperPanel::~WallpaperPanel() {
  if (m_scanner != nullptr) {
    m_scanner->setOnComplete(nullptr);
  }
}

PanelPlacement WallpaperPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.wallpaperPlacement;
}

void WallpaperPanel::create() {
  const float scale = contentScale();

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto toolbar = ui::row({
      .out = &m_toolbar,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });

  toolbar->addChild(
      ui::label({
          .out = &m_title,
          .text = i18n::tr("wallpaper.panel.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );

  toolbar->addChild(
      ui::input({
          .out = &m_filterInput,
          .placeholder = i18n::tr("wallpaper.panel.filter-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .horizontalPadding = Style::spaceMd * scale,
          .surfaceOpacity = panelCardOpacity(),
          .width = 210.0f * scale,
          .height = 0.0f,
          .onChange =
              [this](const std::string& text) {
                if (text == m_pendingFilterQuery) {
                  return;
                }
                m_pendingFilterQuery = text;
                m_filterDebounceTimer.start(kFilterDebounceInterval, [this]() {
                  if (m_pendingFilterQuery == m_filterQuery) {
                    return;
                  }
                  m_filterQuery = m_pendingFilterQuery;
                  applyFilter();
                  resetSelection();
                  rebindGrid();
                  syncBrowseChrome();
                  m_dirty = true;
                  PanelManager::instance().refresh();
                });
              },
          .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
      })
  );

  toolbar->addChild(
      ui::button({
          .out = &m_backButton,
          .glyph = "arrow-big-up",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Secondary,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { navigateUp(); },
      })
  );

  toolbar->addChild(ui::spacer());

  toolbar->addChild(
      ui::label({
          .out = &m_flattenLabel,
          .text = i18n::tr("wallpaper.panel.flatten"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  toolbar->addChild(
      ui::toggle({
          .out = &m_flattenToggle,
          .checked = m_flatten,
          .onChange = [this](bool checked) {
            m_flatten = checked;
            if (m_config != nullptr) {
              (void)m_config->setStateBool("wallpaper_panel", "flatten", checked);
            }
            refreshVisibleEntries();
            resetSelection();
            rebindGrid();
            syncBrowseChrome();
            m_dirty = true;
            PanelManager::instance().refresh();
          },
      })
  );

  toolbar->addChild(
      ui::select({
          .out = &m_monitorSelect,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .surfaceOpacity = panelCardOpacity(),
          .onSelectionChanged =
              [this](std::size_t idx, std::string_view) {
                m_selectedMonitorIndex = idx;
                m_navStack.clear();
                refreshVisibleEntries();
                resetSelection();
                rebindGrid();
                syncBackButton();
                syncBrowseChrome();
                m_dirty = true;
                PanelManager::instance().refresh();
              },
          .configure = [scale](Select& select) { select.setMinWidth(kMonitorSelectMinWidth * scale); },
      })
  );

  toolbar->addChild(
      ui::button({
          .out = &m_colorButton,
          .glyph = "color-picker",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { applyColorWallpaper(); },
      })
  );

  toolbar->addChild(
      ui::button({
          .out = &m_sortButton,
          .glyph = std::string(sortModeGlyph(m_sortMode)),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { cycleSortMode(); },
      })
  );

  toolbar->addChild(
      ui::button({
          .out = &m_refreshButton,
          .glyph = "refresh",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() {
            if (m_scanner != nullptr) {
              m_scanner->invalidate();
            }
            refreshVisibleEntries();
            resetSelection();
            rebindGrid();
            syncBrowseChrome();
            m_dirty = true;
            PanelManager::instance().refresh();
          },
      })
  );

  toolbar->addChild(
      ui::button({
          .out = &m_closeButton,
          .glyph = "close",
          .glyphSize = Style::fontSizeBody * scale,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = []() { PanelManager::instance().close(); },
      })
  );

  root->addChild(std::move(toolbar));

  const float favoritesControlHeight = Style::controlHeightSm * scale;
  const float favoritesLabelFontSize = Style::fontSizeCaption * scale;

  auto favoritesOptions = ui::row({
      .out = &m_favoritesOptionsColumn,
      .align = FlexAlign::Center,
      .gap = kFavoritesMetaRowGap * scale,
      .fillWidth = true,
  });

  // Only offer palette sources that actually have palettes — Community/Custom are empty
  // when nothing is fetched/installed, and selecting them would do nothing.
  m_paletteSourceOrder.clear();
  std::vector<ui::SegmentedOption> paletteSourceOptions;
  const auto addPaletteSource = [&](PaletteSource source, const char* labelKey) {
    m_paletteSourceOrder.push_back(source);
    paletteSourceOptions.push_back({.label = i18n::tr(labelKey)});
  };
  addPaletteSource(PaletteSource::Builtin, "settings.options.theme.source.built-in");
  addPaletteSource(PaletteSource::Wallpaper, "settings.options.theme.source.wallpaper");
  if (!noctalia::theme::availableCommunityPalettes().empty()) {
    addPaletteSource(PaletteSource::Community, "settings.options.theme.source.community");
  }
  if (!noctalia::theme::availableCustomPalettes().empty()) {
    addPaletteSource(PaletteSource::Custom, "settings.options.theme.source.custom");
  }

  favoritesOptions->addChild(
      ui::segmented({
          .out = &m_favoritePaletteSourceSegmented,
          .options = std::move(paletteSourceOptions),
          .selectedIndex = static_cast<std::size_t>(0),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t index) {
            if (m_syncingFavoriteControls || m_config == nullptr) {
              return;
            }
            const std::optional<PaletteSource> source = paletteSourceForSegment(index);
            if (!source.has_value()) {
              return;
            }
            // Repopulate the palette picker for the newly chosen source, then apply.
            WallpaperFavorite seed = activeThemeSettings();
            seed.paletteSource = source;
            if (m_favoriteThemeSegmented != nullptr) {
              seed.themeMode = themeModeFromSegmentIndex(m_favoriteThemeSegmented->selectedIndex());
            }
            m_syncingFavoriteControls = true;
            rebuildFavoritePaletteDetailSelect(&seed);
            m_syncingFavoriteControls = false;

            applyThemeFromControls();
            m_dirty = true;
            PanelManager::instance().refresh();
          },
      })
  );

  favoritesOptions->addChild(
      ui::select({
          .out = &m_favoritePaletteDetailSelect,
          .fontSize = favoritesLabelFontSize,
          .controlHeight = favoritesControlHeight,
          .horizontalPadding = Style::spaceSm * scale,
          .glyphSize = Style::fontSizeCaption * scale,
          .surfaceOpacity = panelCardOpacity(),
          .visible = false,
          .onSelectionChanged =
              [this](std::size_t index, std::string_view) {
                if (m_syncingFavoriteControls || m_config == nullptr || index >= m_favoritePaletteDetailValues.size()) {
                  return;
                }
                applyThemeFromControls();
                m_dirty = true;
                PanelManager::instance().refresh();
              },
          .configure = [scale](Select& select) { select.setMinWidth(kFavoriteSelectMinWidth * scale); },
      })
  );

  favoritesOptions->addChild(ui::spacer());

  favoritesOptions->addChild(
      ui::segmented({
          .out = &m_favoriteThemeSegmented,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.options.theme.mode.dark")},
                  {.label = i18n::tr("settings.options.theme.mode.light")},
                  {.label = i18n::tr("common.states.auto")},
              },
          .selectedIndex = static_cast<std::size_t>(0),
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .onChange = [this](std::size_t) {
            if (m_syncingFavoriteControls || m_config == nullptr) {
              return;
            }
            applyThemeFromControls();
            rebindGrid();
            m_dirty = true;
            PanelManager::instance().refresh();
          },
      })
  );

  root->addChild(std::move(favoritesOptions));

  // ── Body: virtualized scrolling grid ──────────────────────────────────
  m_adapter = std::make_unique<WallpaperGridAdapter>(scale);
  m_adapter->setThumbnailService(m_thumbnails);
  m_adapter->setConfig(m_config);
  m_adapter->setEntries(&m_visibleEntries);
  m_adapter->setOnActivate([this](const WallpaperEntry& entry) {
    if (entry.isDir) {
      navigateInto(entry.absPath);
    } else {
      applyWallpaperFromEntry(entry);
    }
  });
  m_adapter->setOnStarToggle([this](const WallpaperEntry& entry) {
    if (!entry.isDir) {
      toggleFavoriteForPath(entry.absPath.string());
    }
  });

  root->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .minCellWidth = kMinTileWidth * scale,
          .squareCells = false,
          .columnGap = Style::spaceMd * scale,
          .rowGap = Style::spaceMd * scale,
          .overscanRows = 2,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0f,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (m_syncingGridSelectionVisual) {
                  return;
                }
                if (idx.has_value() && *idx < m_visibleEntries.size()) {
                  m_selectedVisibleIndex = *idx;
                } else {
                  m_selectedVisibleIndex = kNoVisibleSelection;
                }
                syncThemeControls();
              },
          .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
      })
  );

  if (m_grid != nullptr && m_grid->focusArea() != nullptr) {
    m_grid->focusArea()->setOnFocusGain([this]() {
      m_gridKeyboardActive = true;
      if (m_grid != nullptr && hasVisibleSelection()) {
        m_syncingGridSelectionVisual = true;
        m_grid->setSelectedIndex(m_selectedVisibleIndex);
        m_syncingGridSelectionVisual = false;
      }
    });
    m_grid->focusArea()->setOnFocusLoss([this]() {
      m_gridKeyboardActive = false;
      if (m_grid != nullptr) {
        m_syncingGridSelectionVisual = true;
        m_grid->setSelectedIndex(std::nullopt);
        m_syncingGridSelectionVisual = false;
      }
    });
  }

  // Loading state shown while the directory scan runs on the worker thread.
  // Occupies the body in place of the grid (only one is visible at a time).
  root->addChild(
      ui::column(
          {
              .out = &m_loadingBox,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
              .gap = Style::spaceMd * scale,
              .fillWidth = true,
              .flexGrow = 1.0f,
              .visible = false,
          },
          ui::spinner({
              .out = &m_spinner,
              .spinnerSize = Style::fontSizeTitle * scale * 1.6f,
              .spinning = false,
          }),
          ui::label({
              .text = i18n::tr("wallpaper.panel.loading"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      )
  );

  setRoot(std::move(root));
  if (m_animations != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }

  if (m_thumbnails != nullptr) {
    m_thumbnailPendingSub = m_thumbnails->subscribePendingUpload([this]() {
      if (m_rootLayout == nullptr) {
        return;
      }
      m_thumbnailRefreshPending = true;
      PanelManager::instance().requestUpdateOnly();
    });
  }
}

void WallpaperPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_lastWidth = width;
  m_lastHeight = height;

  if (m_thumbnails != nullptr) {
    (void)m_thumbnails->uploadPending(renderer.textureManager());
    m_thumbnailRefreshPending = false;
  }

  if (m_adapter != nullptr) {
    m_adapter->setRenderer(&renderer);
  }

  // Drive cell height from current tile width via VirtualGridView's resolved
  // geometry: configure the cell height to follow the chosen tile aspect.
  if (m_grid != nullptr) {
    m_grid->setCellHeight(kMinTileWidth * contentScale() * kTileAspect);
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  m_dirty = false;
}

void WallpaperPanel::doUpdate(Renderer& renderer) {
  if (m_rootLayout == nullptr) {
    return;
  }

  if (m_thumbnailRefreshPending && m_thumbnails != nullptr) {
    const bool changed = m_thumbnails->uploadPending(renderer.textureManager());
    m_thumbnailRefreshPending = false;
    if (changed && m_adapter != nullptr) {
      m_adapter->setRenderer(&renderer);
      m_adapter->refreshVisibleThumbnails(renderer);
    }
  }
}

void WallpaperPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_filterInput != nullptr) {
    m_filterInput->setSurfaceOpacity(opacity);
  }
  if (m_monitorSelect != nullptr) {
    m_monitorSelect->setSurfaceOpacity(opacity);
  }
  if (m_favoriteThemeSegmented != nullptr) {
    m_favoriteThemeSegmented->setSurfaceOpacity(opacity);
  }
  if (m_favoritePaletteSourceSegmented != nullptr) {
    m_favoritePaletteSourceSegmented->setSurfaceOpacity(opacity);
  }
  if (m_favoritePaletteDetailSelect != nullptr) {
    m_favoritePaletteDetailSelect->setSurfaceOpacity(opacity);
  }
}

void WallpaperPanel::onOpen(std::string_view /*context*/) {
  m_filterQuery.clear();
  m_pendingFilterQuery.clear();
  m_filterDebounceTimer.stop();
  if (m_filterInput != nullptr) {
    m_filterInput->setValue("");
  }
  if (m_flattenToggle != nullptr) {
    m_flattenToggle->setCheckedImmediate(m_flatten);
  }
  m_navStack.clear();
  populateMonitorChoices();
  syncSortButtonGlyph();
  refreshVisibleEntries();
  resetSelection();
  rebindGrid();
  syncBrowseChrome();
  syncBackButton();
  m_dirty = true;
}

void WallpaperPanel::onClose() {
  m_filterDebounceTimer.stop();
  m_pendingFilterQuery.clear();
  m_filterQuery.clear();

  m_visibleEntries.clear();

  // Detach adapter from grid before either is destroyed; the pool tiles were
  // minted by the adapter.
  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  m_adapter.reset();
  m_thumbnailPendingSub.disconnect();

  m_rootLayout = nullptr;
  m_toolbar = nullptr;
  m_favoritesOptionsColumn = nullptr;
  m_title = nullptr;
  m_backButton = nullptr;
  m_monitorSelect = nullptr;
  m_filterInput = nullptr;
  m_flattenToggle = nullptr;
  m_flattenLabel = nullptr;
  m_sortButton = nullptr;
  m_refreshButton = nullptr;
  m_colorButton = nullptr;
  m_closeButton = nullptr;
  m_favoriteThemeSegmented = nullptr;
  m_favoritePaletteSourceSegmented = nullptr;
  m_favoritePaletteDetailSelect = nullptr;
  m_grid = nullptr;
  m_loadingBox = nullptr;
  m_spinner = nullptr;
  m_scanPending = false;

  clearReleasedRoot();
  m_lastWidth = 0.0f;
  m_lastHeight = 0.0f;
  m_thumbnailRefreshPending = false;
}

bool WallpaperPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  auto& dispatcher = PanelManager::instance().inputDispatcher();
  InputArea* focused = dispatcher.focusedArea();

  if (m_favoriteThemeSegmented != nullptr && focused == m_favoriteThemeSegmented->focusArea()) {
    const bool moveIntoGrid = KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)
        || KeybindMatcher::matches(KeybindAction::TabNext, sym, modifiers);
    if (moveIntoGrid && m_grid != nullptr && m_grid->focusArea() != nullptr) {
      dispatcher.setFocus(m_grid->focusArea());
      if (!hasVisibleSelection() && !m_visibleEntries.empty()) {
        selectVisibleIndex(0);
      }
      return true;
    }
  }

  if (m_favoritePaletteSourceSegmented != nullptr && focused == m_favoritePaletteSourceSegmented->focusArea()) {
    const bool moveIntoGrid = KeybindMatcher::matches(KeybindAction::Down, sym, modifiers);
    if (moveIntoGrid && m_grid != nullptr && m_grid->focusArea() != nullptr) {
      dispatcher.setFocus(m_grid->focusArea());
      if (!hasVisibleSelection() && !m_visibleEntries.empty()) {
        selectVisibleIndex(0);
      }
      return true;
    }
  }

  if (focused != nullptr && !isDescendantOf(focused, m_grid)) {
    return false;
  }

  return handleKeyEvent(sym, modifiers);
}

InputArea* WallpaperPanel::initialFocusArea() const {
  return m_filterInput != nullptr ? m_filterInput->inputArea() : nullptr;
}

void WallpaperPanel::populateMonitorChoices() {
  m_monitorChoices.clear();
  m_monitorChoices.push_back({"", i18n::tr("wallpaper.panel.all-monitors")});
  if (m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (out.connectorName.empty()) {
        continue;
      }
      m_monitorChoices.push_back({out.connectorName, out.connectorName});
    }
  }

  if (m_selectedMonitorIndex >= m_monitorChoices.size()) {
    m_selectedMonitorIndex = 0;
  }

  if (m_monitorSelect != nullptr) {
    std::vector<std::string> labels;
    labels.reserve(m_monitorChoices.size());
    for (const auto& c : m_monitorChoices) {
      labels.push_back(c.label);
    }
    m_monitorSelect->setOptions(std::move(labels));
    m_monitorSelect->setSelectedIndex(m_selectedMonitorIndex);
  }
}

std::filesystem::path WallpaperPanel::rootDirectoryForSelection() const {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return {};
  }
  const auto& wp = m_config->config().wallpaper;
  const ThemeMode configured = m_config->config().theme.mode;
  const bool isLight = m_themeService != nullptr ? m_themeService->isLightMode() : configured == ThemeMode::Light;
  const ThemeMode mode = wallpaper::effectiveThemeMode(configured, isLight);

  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  if (choice.connector.empty() || !wp.perMonitorDirectories) {
    return std::filesystem::path(wallpaper::resolveGlobalWallpaperDirectory(wp, mode));
  }

  if (m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (out.connectorName == choice.connector) {
        return std::filesystem::path(wallpaper::resolveWallpaperDirectory(wp, out, mode));
      }
    }
  }
  return std::filesystem::path(wallpaper::resolveGlobalWallpaperDirectory(wp, mode));
}

std::filesystem::path WallpaperPanel::activeDirectoryForSelection() const {
  if (!m_navStack.empty()) {
    return m_navStack.back();
  }
  return rootDirectoryForSelection();
}

std::string WallpaperPanel::currentWallpaperPathForSelection() const {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return {};
  }

  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  return choice.connector.empty() ? m_config->getDefaultWallpaperPath() : m_config->getWallpaperPath(choice.connector);
}

std::optional<Color> WallpaperPanel::selectedFillColor() const {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return std::nullopt;
  }

  const auto& wp = m_config->config().wallpaper;
  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  Color sourceColor;
  const std::string currentPath =
      choice.connector.empty() ? m_config->getDefaultWallpaperPath() : m_config->getWallpaperPath(choice.connector);
  if (parseColorWallpaperPath(currentPath, sourceColor)) {
    return sourceColor;
  }

  if (!choice.connector.empty()) {
    for (const auto& ovr : wp.monitorOverrides) {
      if (ovr.match == choice.connector && ovr.fillColor.has_value()) {
        return resolveColorSpec(*ovr.fillColor);
      }
    }
  }

  if (wp.fillColor.has_value()) {
    return resolveColorSpec(*wp.fillColor);
  }
  return std::nullopt;
}

std::vector<std::string> WallpaperPanel::allMonitorConnectors() const {
  std::vector<std::string> connectors;
  if (m_wayland == nullptr) {
    return connectors;
  }
  for (const auto& out : m_wayland->outputs()) {
    if (!out.connectorName.empty()) {
      connectors.push_back(out.connectorName);
    }
  }
  return connectors;
}

std::string WallpaperPanel::displayNameForWallpaperPath(std::string_view path) {
  if (path.starts_with("color:")) {
    return std::string(path.substr(std::string_view("color:").size()));
  }
  const std::filesystem::path filePath(path);
  if (!filePath.filename().empty()) {
    return filePath.filename().string();
  }
  return std::string(path);
}

void WallpaperPanel::syncBrowseChrome() {
  if (m_backButton != nullptr) {
    m_backButton->setVisible(!m_navStack.empty());
  }
  syncThemeControls();
}

std::string WallpaperPanel::selectedWallpaperPath() const {
  if (m_visibleEntries.empty() || m_selectedVisibleIndex >= m_visibleEntries.size()) {
    return {};
  }
  const auto& entry = m_visibleEntries[m_selectedVisibleIndex];
  if (entry.isDir) {
    return {};
  }
  return entry.absPath.string();
}

void WallpaperPanel::appendFilteredFavoriteEntries(
    std::vector<WallpaperEntry>& out, std::unordered_set<std::string>& favoritePaths,
    const std::filesystem::path& activeDir, const std::filesystem::path& rootDir
) const {
  if (m_config == nullptr) {
    return;
  }

  const std::string filterNeedle = StringUtils::toLower(m_filterQuery);
  for (const auto& favorite : m_config->wallpaperFavorites()) {
    if (!favoriteVisibleInBrowseContext(favorite.path, activeDir, rootDir, m_flatten)) {
      continue;
    }

    const std::string displayName = displayNameForWallpaperPath(favorite.path);
    if (!filterNeedle.empty() && !StringUtils::toLower(displayName).contains(filterNeedle)) {
      continue;
    }

    const std::string normalized = FileUtils::normalizeWallpaperPath(favorite.path);
    if (!favoritePaths.insert(normalized).second) {
      continue;
    }

    WallpaperEntry entry;
    entry.absPath = std::filesystem::path(favorite.path);
    entry.name = displayName;
    entry.isDir = false;
    out.push_back(std::move(entry));
  }
}

void WallpaperPanel::rebuildFavoritePaletteDetailSelect(const WallpaperFavorite* favorite) {
  if (m_favoritePaletteDetailSelect == nullptr) {
    return;
  }

  m_favoritePaletteDetailValues.clear();
  std::vector<std::string> labels;
  std::size_t selectedIndex = 0;
  std::string selectedValue;

  if (favorite != nullptr) {
    const PaletteSource source = favorite->paletteSource.value_or(PaletteSource::Builtin);
    switch (source) {
    case PaletteSource::Builtin:
      for (const auto& builtin : noctalia::theme::builtinPalettes()) {
        m_favoritePaletteDetailValues.emplace_back(builtin.name);
        labels.emplace_back(builtin.name);
      }
      selectedValue = favorite->builtinPalette;
      break;
    case PaletteSource::Wallpaper:
      m_favoritePaletteDetailValues = wallpaperSchemeValues();
      labels = wallpaperSchemeOptions();
      selectedValue = favorite->wallpaperScheme;
      break;
    case PaletteSource::Community:
      for (const auto& community : noctalia::theme::availableCommunityPalettes()) {
        m_favoritePaletteDetailValues.push_back(community.name);
        labels.push_back(community.name);
      }
      selectedValue = favorite->communityPalette;
      break;
    case PaletteSource::Custom:
      for (const auto& custom : noctalia::theme::availableCustomPalettes()) {
        m_favoritePaletteDetailValues.push_back(custom.name);
        labels.push_back(custom.name);
      }
      selectedValue = favorite->customPalette;
      break;
    }
  }

  const bool visible = !labels.empty();
  m_favoritePaletteDetailSelect->setVisible(visible);
  if (!visible) {
    m_favoritePaletteDetailSelect->setOptions({});
    m_favoritePaletteDetailSelect->clearSelection();
    return;
  }

  m_favoritePaletteDetailSelect->setOptions(std::move(labels));
  if (!selectedValue.empty()) {
    for (std::size_t i = 0; i < m_favoritePaletteDetailValues.size(); ++i) {
      if (m_favoritePaletteDetailValues[i] == selectedValue) {
        selectedIndex = i;
        break;
      }
    }
  }
  m_favoritePaletteDetailSelect->setSelectedIndex(selectedIndex);
}

std::optional<PaletteSource> WallpaperPanel::paletteSourceForSegment(std::size_t index) const {
  if (index >= m_paletteSourceOrder.size()) {
    return std::nullopt;
  }
  return m_paletteSourceOrder[index];
}

std::size_t WallpaperPanel::segmentForPaletteSource(PaletteSource source) const {
  for (std::size_t i = 0; i < m_paletteSourceOrder.size(); ++i) {
    if (m_paletteSourceOrder[i] == source) {
      return i;
    }
  }
  return 0;
}

WallpaperFavorite WallpaperPanel::activeThemeSettings() const {
  // A selected favorite wallpaper's saved preset is the source of truth; otherwise the live global theme is.
  WallpaperFavorite theme =
      (m_config != nullptr) ? wallpaperFavoriteFromTheme(m_config->config().theme) : WallpaperFavorite{};
  if (m_config != nullptr) {
    if (const std::string path = selectedWallpaperPath(); !path.empty()) {
      if (const WallpaperFavorite* favorite = m_config->wallpaperFavorite(path); favorite != nullptr) {
        theme = *favorite;
      }
    }
  }
  return theme;
}

void WallpaperPanel::applyThemeFromControls() {
  if (m_config == nullptr) {
    return;
  }
  const WallpaperFavorite theme = themeFromControls();
  const std::string path = selectedWallpaperPath();

  if (!path.empty() && m_config->isWallpaperFavorite(path)) {
    // Edit the selected wallpaper's favorite preset and apply it live (also re-asserts the wallpaper).
    m_config->setWallpaperFavoriteThemeMode(path, theme.themeMode);
    m_config->setWallpaperFavoritePaletteSource(path, theme.paletteSource);
    m_config->setWallpaperFavoritePaletteSelection(path, paletteSelectionValue(theme));
    applyWallpaperPath(path, &theme);
    return;
  }

  // No favorite target — behave like the Settings window: change the global theme only.
  m_config->setThemeMode(theme.themeMode);
  if (theme.paletteSource.has_value()) {
    (void)m_config->setThemeColorScheme(*theme.paletteSource, paletteSelectionValue(theme));
  }
}

void WallpaperPanel::syncThemeControls() {
  if (m_favoriteThemeSegmented == nullptr || m_config == nullptr) {
    return;
  }

  const WallpaperFavorite themeSettings = activeThemeSettings();

  m_syncingFavoriteControls = true;

  m_favoriteThemeSegmented->setSelectedIndex(themeModeSegmentIndex(themeSettings.themeMode));

  if (m_favoritePaletteSourceSegmented != nullptr) {
    const std::size_t sourceIndex =
        themeSettings.paletteSource.has_value() ? segmentForPaletteSource(*themeSettings.paletteSource) : 0;
    m_favoritePaletteSourceSegmented->setSelectedIndex(sourceIndex);
  }

  rebuildFavoritePaletteDetailSelect(&themeSettings);
  m_syncingFavoriteControls = false;
}

void WallpaperPanel::refreshScan() {
  const auto dir = activeDirectoryForSelection();
  if (dir.empty() || m_scanner == nullptr) {
    m_scanPending = false;
    return;
  }
  // requestScan() returns false when a worker scan was queued — the entries
  // arrive later via onScanComplete(). A cached/fresh dir returns true.
  m_scanPending = !m_scanner->requestScan(dir, m_flatten);
}

void WallpaperPanel::refreshVisibleEntries() {
  refreshScan();
  applyFilter();
  syncLoadingState();
}

void WallpaperPanel::onScanComplete() {
  // The scanner outlives the panel's UI; ignore late results after teardown.
  if (m_rootLayout == nullptr) {
    return;
  }
  const auto dir = activeDirectoryForSelection();
  m_scanPending = !dir.empty() && m_scanner != nullptr && m_scanner->scanning(dir, m_flatten);

  applyFilter();
  rebindGrid();
  syncBrowseChrome();
  syncLoadingState();
  m_dirty = true;
  PanelManager::instance().refresh();
}

void WallpaperPanel::syncLoadingState() {
  const bool loading = m_scanPending;
  if (m_loadingBox != nullptr) {
    m_loadingBox->setVisible(loading);
  }
  if (m_grid != nullptr) {
    m_grid->setVisible(!loading);
  }
  if (m_spinner != nullptr) {
    if (loading) {
      m_spinner->start();
    } else {
      m_spinner->stop();
    }
  }
}

void WallpaperPanel::applyFilter() {
  m_visibleEntries.clear();
  std::unordered_set<std::string> favoritePaths;
  const auto dir = activeDirectoryForSelection();
  const auto rootDir = rootDirectoryForSelection();
  appendFilteredFavoriteEntries(m_visibleEntries, favoritePaths, dir, rootDir);
  m_pinnedFavoriteCount = m_visibleEntries.size();

  const WallpaperScanResult* result =
      (dir.empty() || m_scanner == nullptr) ? nullptr : m_scanner->cached(dir, m_flatten);
  if (result != nullptr) {
    const std::string needle = StringUtils::toLower(m_filterQuery);
    const bool filterActive = !needle.empty();
    m_visibleEntries.reserve(m_visibleEntries.size() + result->entries.size());
    for (const auto& entry : result->entries) {
      if (!entry.isDir) {
        const std::string normalized = FileUtils::normalizeWallpaperPath(entry.absPath.string());
        if (favoritePaths.contains(normalized)) {
          continue;
        }
      }
      if (filterActive && !StringUtils::toLower(entry.name).contains(needle)) {
        continue;
      }
      m_visibleEntries.push_back(entry);
    }
  }

  sortVisibleEntries();
  if (m_selectedVisibleIndex >= m_visibleEntries.size()) {
    resetSelection();
  }
}

void WallpaperPanel::rebindGrid(bool resetScroll) {
  if (m_grid == nullptr) {
    return;
  }
  if (m_adapter != nullptr) {
    m_adapter->setCurrentWallpaperPath(currentWallpaperPathForSelection());
  }
  m_grid->notifyDataChanged();
  if (resetScroll || m_visibleEntries.empty()) {
    m_grid->scrollView().setScrollOffset(0.0f);
  }
  if (m_visibleEntries.empty() || !hasVisibleSelection() || !m_gridKeyboardActive) {
    m_grid->setSelectedIndex(std::nullopt);
  } else {
    m_grid->setSelectedIndex(m_selectedVisibleIndex);
  }
}

void WallpaperPanel::resetSelection() { m_selectedVisibleIndex = kNoVisibleSelection; }

bool WallpaperPanel::hasVisibleSelection() const { return m_selectedVisibleIndex < m_visibleEntries.size(); }

void WallpaperPanel::toggleFavoriteForPath(const std::string& path) {
  if (m_config == nullptr || path.empty()) {
    return;
  }

  if (m_config->isWallpaperFavorite(path)) {
    m_config->removeWallpaperFavorite(path);
  } else {
    std::optional<WallpaperFavorite> preset;
    const std::string currentPath = currentWallpaperPathForSelection();
    if (!currentPath.empty()
        && FileUtils::normalizeWallpaperPath(path) == FileUtils::normalizeWallpaperPath(currentPath)) {
      preset = wallpaperFavoriteFromTheme(m_config->config().theme);
    }
    m_config->addWallpaperFavorite(path, preset);
  }

  refreshVisibleEntries();
  rebindGrid();
  syncBrowseChrome();
  m_dirty = true;
  PanelManager::instance().refresh();
}

void WallpaperPanel::applyWallpaperPath(const std::string& path, const WallpaperFavorite* applyTheme) {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return;
  }

  const auto& choice = m_monitorChoices[m_selectedMonitorIndex];
  const std::optional<std::string> connector =
      choice.connector.empty() ? std::optional<std::string>{} : std::optional<std::string>{choice.connector};
  m_config->applyWallpaperSelection(connector, path, applyTheme, allMonitorConnectors());
  rebindGrid();
}

const WallpaperFavorite* WallpaperPanel::favoriteThemeToApply(std::string_view path) const {
  if (m_config == nullptr || !m_config->isWallpaperFavorite(path)) {
    return nullptr;
  }
  return m_config->wallpaperFavorite(path);
}

WallpaperFavorite WallpaperPanel::themeFromControls() const {
  WallpaperFavorite theme;
  if (m_favoriteThemeSegmented != nullptr) {
    theme.themeMode = themeModeFromSegmentIndex(m_favoriteThemeSegmented->selectedIndex());
  }
  if (m_favoritePaletteSourceSegmented != nullptr) {
    theme.paletteSource = paletteSourceForSegment(m_favoritePaletteSourceSegmented->selectedIndex());
  }
  if (m_favoritePaletteDetailSelect != nullptr && !m_favoritePaletteDetailValues.empty()) {
    const std::size_t index = m_favoritePaletteDetailSelect->selectedIndex();
    if (index < m_favoritePaletteDetailValues.size()) {
      const std::string& value = m_favoritePaletteDetailValues[index];
      switch (theme.paletteSource.value_or(PaletteSource::Builtin)) {
      case PaletteSource::Builtin:
        theme.builtinPalette = value;
        break;
      case PaletteSource::Wallpaper:
        theme.wallpaperScheme = value;
        break;
      case PaletteSource::Community:
        theme.communityPalette = value;
        break;
      case PaletteSource::Custom:
        theme.customPalette = value;
        break;
      }
    }
  }
  return theme;
}

void WallpaperPanel::selectVisibleIndex(std::size_t index) {
  if (m_visibleEntries.empty() || index >= m_visibleEntries.size()) {
    return;
  }

  m_selectedVisibleIndex = index;
  if (m_grid != nullptr) {
    m_grid->setSelectedIndex(index);
    m_grid->scrollToIndex(index);
  }
  syncThemeControls();

  m_dirty = true;
  PanelManager::instance().refresh();
}

void WallpaperPanel::activateSelectedEntry() {
  if (!hasVisibleSelection()) {
    return;
  }

  const auto& entry = m_visibleEntries[m_selectedVisibleIndex];
  if (entry.isDir) {
    navigateInto(entry.absPath);
  } else {
    applyWallpaperFromEntry(entry);
  }
}

bool WallpaperPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (m_visibleEntries.empty()) {
    return false;
  }

  // Approximate column count from current grid layout. VirtualGridView does
  // not expose its column count directly, so we recompute from viewport width.
  std::size_t columns = 1;
  if (m_grid != nullptr) {
    const float viewportW = m_grid->scrollView().contentViewportWidth();
    const float cellW = kMinTileWidth * contentScale();
    const float gap = Style::spaceMd * contentScale();
    if (cellW > 0.0f) {
      columns = std::max<std::size_t>(1, static_cast<std::size_t>((viewportW + gap) / (cellW + gap)));
    }
  }

  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      return true;
    }
    if (m_selectedVisibleIndex > 0) {
      selectVisibleIndex(m_selectedVisibleIndex - 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      selectVisibleIndex(0);
    } else if (m_selectedVisibleIndex + 1 < m_visibleEntries.size()) {
      selectVisibleIndex(m_selectedVisibleIndex + 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      selectVisibleIndex(0);
    } else if (m_selectedVisibleIndex >= columns) {
      selectVisibleIndex(m_selectedVisibleIndex - columns);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (!hasVisibleSelection()) {
      selectVisibleIndex(0);
    } else {
      const std::size_t nextIndex = m_selectedVisibleIndex + columns;
      if (nextIndex < m_visibleEntries.size()) {
        selectVisibleIndex(nextIndex);
      }
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelectedEntry();
    return true;
  }

  return false;
}

void WallpaperPanel::syncBackButton() {
  uiAssertNotRendering("WallpaperPanel::syncBackButton");
  if (m_backButton == nullptr) {
    return;
  }
  const bool canNavigateUp = !m_navStack.empty();
  m_backButton->setEnabled(canNavigateUp);
  m_backButton->setVisible(canNavigateUp);
}

void WallpaperPanel::navigateInto(const std::filesystem::path& dir) {
  m_navStack.push_back(dir);
  refreshVisibleEntries();
  resetSelection();
  rebindGrid(true);
  syncBackButton();
  syncBrowseChrome();
  m_dirty = true;
  PanelManager::instance().refresh();
}

void WallpaperPanel::navigateUp() {
  if (m_navStack.empty()) {
    return;
  }
  m_navStack.pop_back();
  refreshVisibleEntries();
  resetSelection();
  rebindGrid(true);
  syncBackButton();
  syncBrowseChrome();
  m_dirty = true;
  PanelManager::instance().refresh();
}

WallpaperPanel::SortMode WallpaperPanel::sortModeFromState(std::string_view value) {
  if (value == "name_desc") {
    return SortMode::NameDesc;
  }
  if (value == "date_asc") {
    return SortMode::DateAsc;
  }
  if (value == "date_desc") {
    return SortMode::DateDesc;
  }
  return SortMode::NameAsc;
}

std::string_view WallpaperPanel::sortModeStateValue(SortMode mode) {
  switch (mode) {
  case SortMode::NameDesc:
    return "name_desc";
  case SortMode::DateAsc:
    return "date_asc";
  case SortMode::DateDesc:
    return "date_desc";
  case SortMode::NameAsc:
  default:
    return "name_asc";
  }
}

std::string_view WallpaperPanel::sortModeGlyph(SortMode mode) {
  switch (mode) {
  case SortMode::NameDesc:
    return "sort-z-a";
  case SortMode::DateAsc:
    return "sort-ascending-2";
  case SortMode::DateDesc:
    return "sort-descending-2";
  case SortMode::NameAsc:
  default:
    return "sort-a-z";
  }
}

const char* WallpaperPanel::sortModeTooltipKey(SortMode mode) {
  switch (mode) {
  case SortMode::NameDesc:
    return "wallpaper.panel.sort-name-desc";
  case SortMode::DateAsc:
    return "wallpaper.panel.sort-date-asc";
  case SortMode::DateDesc:
    return "wallpaper.panel.sort-date-desc";
  case SortMode::NameAsc:
  default:
    return "wallpaper.panel.sort-name-asc";
  }
}

void WallpaperPanel::syncSortButtonGlyph() {
  if (m_sortButton == nullptr) {
    return;
  }
  m_sortButton->setGlyph(sortModeGlyph(m_sortMode));
  m_sortButton->setTooltip(i18n::tr(sortModeTooltipKey(m_sortMode)));

  if (!m_sortButton->hovered()) {
    return;
  }
  InputArea* area = m_sortButton->inputArea();
  if (area == nullptr || !area->hasTooltip()) {
    return;
  }
  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value() || parentCtx->layerSurface == nullptr || parentCtx->output == nullptr) {
    return;
  }
  TooltipManager::instance().onHoverChange(area, parentCtx->layerSurface, parentCtx->output);
}

void WallpaperPanel::cycleSortMode() {
  SortMode next = SortMode::NameAsc;
  switch (m_sortMode) {
  case SortMode::NameAsc:
    next = SortMode::NameDesc;
    break;
  case SortMode::NameDesc:
    next = SortMode::DateAsc;
    break;
  case SortMode::DateAsc:
    next = SortMode::DateDesc;
    break;
  case SortMode::DateDesc:
    next = SortMode::NameAsc;
    break;
  }
  setSortMode(next);
}

void WallpaperPanel::setSortMode(SortMode mode) {
  if (m_sortMode == mode) {
    return;
  }
  m_sortMode = mode;
  if (m_config != nullptr) {
    (void)m_config->setStateString("wallpaper_panel", "sort", std::string(sortModeStateValue(mode)));
  }
  syncSortButtonGlyph();
  sortVisibleEntries();
  rebindGrid();
  m_dirty = true;
  PanelManager::instance().refresh();
}

void WallpaperPanel::sortVisibleEntries() {
  if (m_visibleEntries.empty()) {
    return;
  }

  const auto compareEntries = [this](const WallpaperEntry& a, const WallpaperEntry& b) {
    switch (m_sortMode) {
    case SortMode::NameDesc: {
      const int order = caseInsensitiveNameOrder(a.name, b.name);
      return order > 0;
    }
    case SortMode::DateAsc: {
      const auto aTime = entryModifiedTime(a);
      const auto bTime = entryModifiedTime(b);
      if (aTime.has_value() && bTime.has_value()) {
        if (*aTime != *bTime) {
          return *aTime < *bTime;
        }
      } else if (aTime.has_value() != bTime.has_value()) {
        return !aTime.has_value();
      }
      return caseInsensitiveNameOrder(a.name, b.name) < 0;
    }
    case SortMode::DateDesc: {
      const auto aTime = entryModifiedTime(a);
      const auto bTime = entryModifiedTime(b);
      if (aTime.has_value() && bTime.has_value()) {
        if (*aTime != *bTime) {
          return *aTime > *bTime;
        }
      } else if (aTime.has_value() != bTime.has_value()) {
        return aTime.has_value();
      }
      return caseInsensitiveNameOrder(a.name, b.name) < 0;
    }
    case SortMode::NameAsc:
    default: {
      const int order = caseInsensitiveNameOrder(a.name, b.name);
      return order < 0;
    }
    }
  };

  const auto sortRange = [&](const std::size_t beginIndex, const std::size_t endIndex, const bool foldersFirst) {
    if (beginIndex >= endIndex || beginIndex >= m_visibleEntries.size()) {
      return;
    }
    const std::size_t end = std::min(endIndex, m_visibleEntries.size());
    const auto begin = m_visibleEntries.begin() + static_cast<std::ptrdiff_t>(beginIndex);
    const auto endIt = m_visibleEntries.begin() + static_cast<std::ptrdiff_t>(end);
    if (foldersFirst) {
      const auto folderEnd = std::partition(begin, endIt, [](const WallpaperEntry& entry) { return entry.isDir; });
      std::sort(begin, folderEnd, compareEntries);
      std::sort(folderEnd, endIt, compareEntries);
    } else {
      std::sort(begin, endIt, compareEntries);
    }
  };

  sortRange(0, m_pinnedFavoriteCount, false);
  sortRange(m_pinnedFavoriteCount, m_visibleEntries.size(), !m_flatten);
}

void WallpaperPanel::applyWallpaperFromEntry(const WallpaperEntry& entry) {
  if (entry.isDir) {
    return;
  }

  const std::string path = entry.absPath.string();
  applyWallpaperPath(path, favoriteThemeToApply(path));
  kLog.info(
      "applied wallpaper {} to {}", path,
      m_selectedMonitorIndex == 0 ? "ALL" : m_monitorChoices[m_selectedMonitorIndex].connector
  );
}

void WallpaperPanel::applyColorWallpaper() {
  if (m_config == nullptr || m_selectedMonitorIndex >= m_monitorChoices.size()) {
    return;
  }

  ColorPickerDialogOptions options;
  options.title = i18n::tr("wallpaper.panel.color-title");
  if (auto color = selectedFillColor()) {
    options.initialColor = *color;
  } else if (auto last = ColorPickerDialog::lastResult()) {
    options.initialColor = *last;
  }

  (void)ColorPickerDialog::open(std::move(options), [this](std::optional<Color> result) {
    if (!result.has_value() || m_config == nullptr) {
      return;
    }

    Color rgb = *result;
    rgb.a = 1.0f;
    applyWallpaperPath(colorWallpaperPath(rgb), nullptr);
    syncBrowseChrome();
    kLog.info("applied color wallpaper {}", colorWallpaperPath(rgb));
  });
}
