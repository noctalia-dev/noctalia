#include "shell/settings/plugin_store_content.h"

#include "config/config_service.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_file_cache.h"
#include "scripting/plugin_id.h"
#include "shell/settings/plugin_store_tile.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/markdown_view.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace settings {

  namespace {

    constexpr float kSourceBadgeMaxWidth = 120.0f;
    constexpr float kTagBadgeMaxWidth = 120.0f;

    // Display label for a source filter value: official/community are localized badge names,
    // custom source names show verbatim.
    std::string sourceDisplayName(const std::string& source) {
      if (source == "official") {
        return i18n::tr("settings.badges.official");
      }
      if (source == "community") {
        return i18n::tr("settings.badges.community");
      }
      return source;
    }

    // Ordering for the source filter chips: official first, community second, custom sorted after.
    int sourceRank(const std::string& source) {
      if (source == "official") {
        return 0;
      }
      if (source == "community") {
        return 1;
      }
      return 2;
    }

    bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
      if (needle.empty()) {
        return true;
      }
      return !std::ranges::search(haystack, needle, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
              }).empty();
    }

    class PluginStoreAdapter final : public VirtualGridAdapter {
    public:
      explicit PluginStoreAdapter(float scale) : m_scale(scale) {}

      void setContent(PluginStoreContent* content) { m_content = content; }
      void setFilteredIndices(const std::vector<std::size_t>* indices) { m_indices = indices; }
      void setCatalog(const std::vector<StoreCatalogEntry>* catalog) { m_catalog = catalog; }
      void setOnDiskIds(const std::unordered_set<std::string>* ids) { m_onDiskIds = ids; }
      void setCallbacks(const PluginStoreCallbacks* callbacks) { m_callbacks = callbacks; }
      void setThumbnailPaths(const std::unordered_map<std::string, std::string>* paths) { m_thumbnailPaths = paths; }
      void setRenderer(Renderer* r) { m_renderer = r; }
      void setTextureCache(AsyncTextureCache* c) { m_textureCache = c; }

      [[nodiscard]] std::size_t itemCount() const override { return m_indices != nullptr ? m_indices->size() : 0; }

      [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<PluginStoreTile>(m_scale); }

      void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
        if (m_indices == nullptr || m_catalog == nullptr || index >= m_indices->size()) {
          return;
        }
        auto* t = static_cast<PluginStoreTile*>(&tile);
        const auto& storeEntry = (*m_catalog)[(*m_indices)[index]];
        const bool onDisk = m_onDiskIds != nullptr && m_onDiskIds->contains(storeEntry.entry.id);
        std::string thumbPath;
        if (m_thumbnailPaths != nullptr) {
          auto it = m_thumbnailPaths->find(storeEntry.entry.id);
          if (it != m_thumbnailPaths->end()) {
            thumbPath = it->second;
          }
        }
        t->bind(storeEntry.entry, storeEntry.source, onDisk, selected, hovered, thumbPath, m_renderer, m_textureCache);
      }

      void onActivate(std::size_t index) override {
        if (m_content != nullptr && m_indices != nullptr && index < m_indices->size()) {
          m_content->openDetail(index);
        }
      }

    private:
      float m_scale;
      PluginStoreContent* m_content = nullptr;
      const std::vector<std::size_t>* m_indices = nullptr;
      const std::vector<StoreCatalogEntry>* m_catalog = nullptr;
      const std::unordered_set<std::string>* m_onDiskIds = nullptr;
      const PluginStoreCallbacks* m_callbacks = nullptr;
      const std::unordered_map<std::string, std::string>* m_thumbnailPaths = nullptr;
      Renderer* m_renderer = nullptr;
      AsyncTextureCache* m_textureCache = nullptr;
    };

  } // namespace

  PluginStoreContent::PluginStoreContent(
      std::vector<StoreCatalogEntry> catalog, ConfigService* config, std::unordered_set<std::string> onDiskIds,
      PluginStoreCallbacks callbacks, scripting::PluginFileCache* fileCache
  )
      : m_catalog(std::move(catalog)), m_config(config), m_onDiskIds(std::move(onDiskIds)),
        m_callbacks(std::move(callbacks)), m_fileCache(fileCache) {
    if (m_config != nullptr) {
      if (const std::optional<std::string> sort = m_config->stateString("plugin_store", "sort")) {
        m_sortMode = sortModeFromState(*sort);
      }
    }
    collectThumbnails();
    collectSources();
    collectTags();
    applyFilter();
  }

  PluginStoreContent::~PluginStoreContent() = default;

  void PluginStoreContent::setOnRebuildNeeded(std::function<void()> cb) { m_onRebuildNeeded = std::move(cb); }

  bool PluginStoreContent::isDetailView() const noexcept { return m_detailIndex.has_value(); }

  void PluginStoreContent::cycleSortMode() {
    SortMode next = SortMode::NameAsc;
    switch (m_sortMode) {
    case SortMode::NameAsc:
      next = SortMode::NameDesc;
      break;
    case SortMode::NameDesc:
      next = SortMode::UpdatedAtDesc;
      break;
    case SortMode::UpdatedAtDesc:
      next = SortMode::UpdatedAtAsc;
      break;
    case SortMode::UpdatedAtAsc:
      next = SortMode::AddedAtDesc;
      break;
    case SortMode::AddedAtDesc:
      next = SortMode::AddedAtAsc;
      break;
    case SortMode::AddedAtAsc:
      next = SortMode::NameAsc;
      break;
    }
    setSortMode(next);
  }

  void PluginStoreContent::setSortMode(SortMode mode) {
    if (m_sortMode == mode) {
      return;
    }
    m_sortMode = mode;
    if (m_config != nullptr) {
      (void)m_config->setStateString("plugin_store", "sort", std::string(sortModeStateValue(mode)));
    }

    syncSortButtonGlyph();
    sortEntries();
    syncGridSelection();
  }

  SortMode PluginStoreContent::sortModeFromState(std::string_view value) {
    if (value == "name_desc") {
      return SortMode::NameDesc;
    }
    if (value == "updated_at_asc") {
      return SortMode::UpdatedAtAsc;
    }
    if (value == "updated_at_desc") {
      return SortMode::UpdatedAtDesc;
    }
    if (value == "added_at_asc") {
      return SortMode::AddedAtAsc;
    }
    if (value == "added_at_desc") {
      return SortMode::AddedAtDesc;
    }
    return SortMode::NameAsc;
  }

  std::string_view PluginStoreContent::sortModeStateValue(SortMode mode) {
    switch (mode) {
    case SortMode::NameDesc:
      return "name_desc";
    case SortMode::UpdatedAtAsc:
      return "updated_at_asc";
    case SortMode::UpdatedAtDesc:
      return "updated_at_desc";
    case SortMode::AddedAtAsc:
      return "added_at_asc";
    case SortMode::AddedAtDesc:
      return "added_at_desc";
    case SortMode::NameAsc:
    default:
      return "name_asc";
    }
  }

  std::string_view PluginStoreContent::sortModeGlyph(SortMode mode) {
    switch (mode) {
    case SortMode::NameDesc:
      return "sort-z-a";
    case SortMode::UpdatedAtAsc:
      return "sort-ascending-2";
    case SortMode::UpdatedAtDesc:
      return "sort-descending-2";
    case SortMode::AddedAtAsc:
      return "sort-ascending-2-filled";
    case SortMode::AddedAtDesc:
      return "sort-descending-2-filled";
    case SortMode::NameAsc:
    default:
      return "sort-a-z";
    }
  }

  const char* PluginStoreContent::sortModeTooltipKey(SortMode mode) {
    switch (mode) {
    case SortMode::NameDesc:
      return "settings.plugins.store.sort-name-desc";
    case SortMode::UpdatedAtAsc:
      return "settings.plugins.store.sort-updated-at-asc";
    case SortMode::UpdatedAtDesc:
      return "settings.plugins.store.sort-updated-at-desc";
    case SortMode::AddedAtAsc:
      return "settings.plugins.store.sort-added-at-asc";
    case SortMode::AddedAtDesc:
      return "settings.plugins.store.sort-added-at-desc";
    case SortMode::NameAsc:
    default:
      return "settings.plugins.store.sort-name-asc";
    }
  }

  void PluginStoreContent::syncSortButtonGlyph() {
    if (m_sortButton == nullptr) {
      return;
    }
    m_sortButton->setGlyph(sortModeGlyph(m_sortMode));
    // Retargets a tooltip already on screen: setTooltip notifies TooltipManager.
    m_sortButton->setTooltip(i18n::tr(sortModeTooltipKey(m_sortMode)));
  }

  std::optional<std::string> PluginStoreContent::detailPageUrl() const {
    if (!m_detailIndex.has_value() || *m_detailIndex >= m_filteredIndices.size()) {
      return std::nullopt;
    }
    const auto& storeEntry = m_catalog[m_filteredIndices[*m_detailIndex]];
    return scripting::pluginWebsitePageUrl(storeEntry.source, storeEntry.entry.id);
  }

  std::optional<std::string> PluginStoreContent::detailSourceUrl() const {
    if (!m_detailIndex.has_value() || *m_detailIndex >= m_filteredIndices.size()) {
      return std::nullopt;
    }
    const auto& storeEntry = m_catalog[m_filteredIndices[*m_detailIndex]];
    if (storeEntry.sourceConfig.kind != PluginSourceKind::Git) {
      return std::nullopt;
    }
    if (storeEntry.source == "official" || storeEntry.source == "community") {
      return storeEntry.sourceConfig.location
          + "/tree/main/"
          + scripting::pluginSubdirFromId(storeEntry.entry.id).value();
    }
    return storeEntry.sourceConfig.location;
  }

  void PluginStoreContent::collectThumbnails() {
    if (m_fileCache == nullptr) {
      return;
    }
    for (const auto& entry : m_catalog) {
      std::string path = m_fileCache->resolve(entry.entry.id, entry.sourceConfig, "thumbnail.webp");
      if (!path.empty()) {
        m_thumbnailPaths[entry.entry.id] = path;
      }
    }
  }

  void PluginStoreContent::collectTags() {
    std::set<std::string> tagSet;
    for (const auto& entry : m_catalog) {
      if (!m_selectedSource.empty() && entry.source != m_selectedSource) {
        continue;
      }
      for (const auto& tag : entry.entry.tags) {
        tagSet.insert(tag);
      }
    }
    m_allTags = {tagSet.begin(), tagSet.end()};
  }

  void PluginStoreContent::collectSources() {
    std::set<std::string> sourceSet;
    for (const auto& entry : m_catalog) {
      if (!entry.source.empty()) {
        sourceSet.insert(entry.source);
      }
    }
    m_sources.assign(sourceSet.begin(), sourceSet.end());
    std::ranges::sort(m_sources, [](const std::string& a, const std::string& b) {
      const int ra = sourceRank(a);
      const int rb = sourceRank(b);
      return ra != rb ? ra < rb : a < b;
    });
  }

  void PluginStoreContent::sortEntries() {
    std::ranges::sort(m_filteredIndices, [this](std::size_t a, std::size_t b) {
      const int nameOrder =
          StringUtils::naturalCaseInsensitiveCompare(m_catalog[a].entry.name, m_catalog[b].entry.name);
      // Timestamps tie constantly (whole-day granularity, and every entry when a
      // source predates the catalog date fields), so name breaks the tie.
      const auto byDate = [&](auto aTime, auto bTime, bool ascending) {
        if (aTime != bTime) {
          return ascending ? aTime < bTime : aTime > bTime;
        }
        return nameOrder < 0;
      };
      switch (m_sortMode) {
      case SortMode::NameDesc:
        return nameOrder > 0;
      case SortMode::UpdatedAtAsc:
        return byDate(m_catalog[a].entry.updatedAt, m_catalog[b].entry.updatedAt, true);
      case SortMode::UpdatedAtDesc:
        return byDate(m_catalog[a].entry.updatedAt, m_catalog[b].entry.updatedAt, false);
      case SortMode::AddedAtAsc:
        return byDate(m_catalog[a].entry.addedAt, m_catalog[b].entry.addedAt, true);
      case SortMode::AddedAtDesc:
        return byDate(m_catalog[a].entry.addedAt, m_catalog[b].entry.addedAt, false);
      case SortMode::NameAsc:
      default:
        return nameOrder < 0;
      }
    });
  }

  void PluginStoreContent::applyFilter() {
    m_filteredIndices.clear();
    for (std::size_t i = 0; i < m_catalog.size(); ++i) {
      const auto& e = m_catalog[i];
      if (!m_selectedSource.empty() && e.source != m_selectedSource) {
        continue;
      }
      if (!m_selectedTag.empty() && !std::ranges::contains(e.entry.tags, m_selectedTag)) {
        continue;
      }
      if (!m_searchQuery.empty()) {
        const std::string haystack = e.entry.name + " " + e.entry.description + " " + e.entry.author;
        if (!containsIgnoreCase(haystack, m_searchQuery)) {
          continue;
        }
      }
      m_filteredIndices.push_back(i);
    }
    sortEntries();
  }

  void PluginStoreContent::populateBody(Flex& body, Renderer& renderer, AsyncTextureCache* textureCache) {
    if (m_detailIndex.has_value()) {
      buildDetailView(body, renderer, textureCache);
    } else {
      buildGridView(body, renderer, textureCache);
    }
  }

  void PluginStoreContent::buildGridView(Flex& body, Renderer& renderer, AsyncTextureCache* textureCache) {
    m_renderer = &renderer;
    m_textureCache = textureCache;
    const float scale = m_callbacks.scale;

    body.addChild(
        ui::input({
            .placeholder = i18n::tr("settings.plugins.store.search-placeholder"),
            .fontSize = Style::fontSizeBody * scale,
            .onChange = [this](const std::string& text) {
              m_searchQuery = text;
              applyFilter();
              syncGridSelection();
              if (m_countLabel != nullptr) {
                m_countLabel->setText(
                    i18n::tr("settings.plugins.store.results-count", "count", std::to_string(m_filteredIndices.size()))
                );
              }
            },
        })
    );

    auto toolbar = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    // Holds the count and sort button when the category chips are expanded, so the chips
    // sit between the filters and the row that summarizes them.
    auto toolbarBottom = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });

    if (m_sources.size() > 1) {
      std::vector<ui::SegmentedOption> allSources;
      allSources.push_back({i18n::tr("settings.plugins.store.source-all"), "", ""});
      for (const auto& source : m_sources) {
        allSources.push_back({sourceDisplayName(source), "", ""});
      }
      const auto selectedSource = std::ranges::find(m_sources, m_selectedSource);
      const std::size_t selectedSourceIndex =
          selectedSource == m_sources.end() ? 0 : static_cast<std::size_t>(selectedSource - m_sources.begin()) + 1;

      toolbar->addChild(
          ui::segmented({
              .options = allSources,
              .selectedIndex = selectedSourceIndex,
              .fontSize = Style::fontSizeCaption * scale,
              .compact = true,
              .onChange = [this](std::size_t i) {
                m_selectedSource = i == 0 ? std::string{} : m_sources[i - 1];

                // The tag list is scoped to the selected source, so keep the chosen
                // category only when the new source still offers it.
                collectTags();
                if (!std::ranges::contains(m_allTags, m_selectedTag)) {
                  m_selectedTag.clear();
                }

                applyFilter();
                if (m_onRebuildNeeded) {
                  m_onRebuildNeeded();
                }
              },
          })
      );
    }

    std::vector<std::string> allTags;
    allTags.push_back(i18n::tr("settings.plugins.store.category-all"));
    allTags.insert(allTags.end(), m_allTags.begin(), m_allTags.end());
    std::vector<std::unique_ptr<Button>> tagButtons;

    if (allTags.size() > 1) {
      toolbar->addChild(
          ui::button({
              .text = i18n::tr("settings.plugins.store.categories"),
              .glyph = m_tagFiltersCollapsed ? std::string("chevron-right") : std::string("chevron-down"),
              .fontSize = Style::fontSizeCaption * scale,
              .glyphSize = Style::fontSizeCaption * scale,
              .contentAlign = ButtonContentAlign::Start,
              .variant = ButtonVariant::Ghost,
              .onClick = [this]() {
                m_tagFiltersCollapsed = !m_tagFiltersCollapsed;
                if (m_onRebuildNeeded) {
                  m_onRebuildNeeded();
                }
              },
          })
      );

      if (!m_tagFiltersCollapsed) {
        for (std::size_t i = 0; i < allTags.size(); ++i) {
          // Index 0 is the "all" chip, which clears the filter rather than naming a tag.
          std::string tag = i == 0 ? std::string{} : m_allTags[i - 1];
          const bool selected = tag == m_selectedTag;
          auto btn = ui::button({
              .text = allTags[i],
              .fontSize = Style::fontSizeCaption * scale,
              .variant = selected ? ButtonVariant::Primary : ButtonVariant::Default,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this, tag = std::move(tag)]() {
                m_selectedTag = tag;
                applyFilter();
                if (m_onRebuildNeeded) {
                  m_onRebuildNeeded();
                }
              },
          });
          tagButtons.push_back(std::move(btn));
        }
      }
    }

    Flex& summaryRow = m_tagFiltersCollapsed ? *toolbar : *toolbarBottom;

    summaryRow.addChild(ui::spacer());

    summaryRow.addChild(
        ui::label({
            .out = &m_countLabel,
            .text = i18n::tr("settings.plugins.store.results-count", "count", std::to_string(m_filteredIndices.size())),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    summaryRow.addChild(
        ui::button({
            .out = &m_sortButton,
            .glyph = std::string(sortModeGlyph(m_sortMode)),
            .glyphSize = Style::fontSizeTitle * scale,
            .variant = ButtonVariant::Default,
            .tooltip = i18n::tr(sortModeTooltipKey(m_sortMode)),
            .minWidth = Style::controlHeight * scale,
            .minHeight = Style::controlHeight * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this]() { cycleSortMode(); },
        })
    );

    body.addChild(std::move(toolbar));

    if (!m_tagFiltersCollapsed) {
      auto tagRows = ui::row({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .wrap = true,
          .gap = Style::spaceXs * scale,
          .fillWidth = true,
      });
      for (auto& btn : tagButtons) {
        tagRows->addChild(std::move(btn));
      }
      body.addChild(std::move(tagRows));

      body.addChild(std::move(toolbarBottom));
    }

    auto adapter = std::make_unique<PluginStoreAdapter>(scale);
    auto* adapterPtr = adapter.get();
    adapterPtr->setContent(this);
    adapterPtr->setFilteredIndices(&m_filteredIndices);
    adapterPtr->setCatalog(&m_catalog);
    adapterPtr->setOnDiskIds(&m_onDiskIds);
    adapterPtr->setCallbacks(&m_callbacks);
    adapterPtr->setThumbnailPaths(&m_thumbnailPaths);
    adapterPtr->setRenderer(&renderer);
    adapterPtr->setTextureCache(textureCache);
    m_adapter = std::move(adapter);

    auto grid = ui::virtualGridView({
        .out = &m_grid,
        .minCellWidth = 200.0f * scale,
        .cellHeight = 215.0f * scale,
        .squareCells = false,
        .columnGap = Style::spaceSm * scale,
        .rowGap = Style::spaceSm * scale,
        .adapter = adapterPtr,
        .flexGrow = 1.0f,
        .onSelectionChanged =
            [this](std::optional<std::size_t> index) {
              m_selectedPluginId = index.has_value() && *index < m_filteredIndices.size()
                  ? std::optional{m_catalog[m_filteredIndices[*index]].entry.id}
                  : std::nullopt;
            },
        .configure = [](VirtualGridView& view) { view.setFillWidth(true); },
    });
    // The sheet hosts the store without an outer ScrollView, so the grid's own scroll fills the
    // available height and scrolls the catalog. No minimum height: a floor would overflow the
    // sheet bottom (and clip nothing) when the dialog is shorter than the floor.
    if (const auto index = indexOfPluginId(m_selectedPluginId.value_or("")); index.has_value()) {
      m_grid->setSelectedIndex(index);
    }
    body.addChild(std::move(grid));

    if (m_filteredIndices.empty()) {
      body.addChild(
          ui::label({
              .text = i18n::tr("settings.plugins.store.empty"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
  }

  void PluginStoreContent::buildDetailView(Flex& body, Renderer& renderer, AsyncTextureCache* textureCache) {
    if (!m_detailIndex.has_value() || *m_detailIndex >= m_filteredIndices.size()) {
      return;
    }
    const auto& storeEntry = m_catalog[m_filteredIndices[*m_detailIndex]];
    const auto& entry = storeEntry.entry;
    const float scale = m_callbacks.scale;
    const bool onDisk = m_onDiskIds.contains(entry.id);
    const bool enabling = m_callbacks.isEnabling && m_callbacks.isEnabling(entry.id);

    // The sheet hosts the store without an outer ScrollView, so the detail view scrolls its own
    // content (header + README can exceed the sheet height).
    auto scroll = ui::scrollView({
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0f,
        .viewportPaddingV = 0.0f,
        .flexGrow = 1.0f,
        .configure = [](ScrollView& sv) {
          sv.clearFill();
          sv.clearBorder();
        },
    });
    Flex* dc = scroll->content();
    dc->setDirection(FlexDirection::Vertical);
    dc->setAlign(FlexAlign::Stretch);
    dc->setGap(Style::spaceMd * scale);

    auto header = ui::row({.align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .fillWidth = true});

    auto pill = [&](const std::string& text, ColorRole fg, ColorRole bg, float bgAlpha, float maxWidth = 0.0f) {
      Label* label = nullptr;
      auto badge = ui::row(
          {.align = FlexAlign::Center,
           .paddingH = Style::spaceXs * scale,
           .fill = colorSpecFromRole(bg, bgAlpha),
           .radius = Style::scaledRadiusSm(scale)},
          ui::label({
              .out = &label,
              .text = text,
              .fontSize = Style::fontSizeMini * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(fg),
          })
      );
      if (maxWidth > 0.0f) {
        badge->setMaxWidth(maxWidth * scale);
        label->setMaxWidth((maxWidth - (Style::spaceXs * 2.0f)) * scale);
        label->setMaxLines(1);
        label->setEllipsize(TextEllipsize::End);
      }
      return badge;
    };

    // Left side: plugin thumbnail (Contain-fit so it shows uncropped), or glyph fallback.
    auto thumbIt = m_thumbnailPaths.find(entry.id);
    if (thumbIt != m_thumbnailPaths.end() && !thumbIt->second.empty()) {
      auto img = ui::image({
          .fit = ImageFit::Contain,
          .radius = Style::scaledRadiusMd(scale),
          .width = 320.0f * scale,
          .height = 200.0f * scale,
      });
      const int thumbTargetSize = static_cast<int>(std::ceil(320.0f * scale));
      if (textureCache != nullptr) {
        img->setSourceFileAsync(renderer, *textureCache, thumbIt->second, thumbTargetSize, true);
      } else {
        img->setSourceFile(renderer, thumbIt->second, thumbTargetSize, true);
      }
      header->addChild(std::move(img));
    } else {
      header->addChild(
          ui::glyph({
              .glyph = entry.icon.empty() ? std::string("apps") : entry.icon,
              .glyphSize = Style::fontSizeHeader * 2.0f * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .width = 80.0f * scale,
              .height = 80.0f * scale,
          })
      );
    }

    // Right side: plugin info (name, author, tags, version/license/badges, description, action),
    // left-aligned and filling the space next to the thumbnail.
    auto info = ui::column(
        {.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .paddingV = Style::spaceSm * scale, .flexGrow = 1.0f}
    );
    auto title = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale, .fillWidth = true});
    title->addChild(
        ui::label({
            .text = entry.name,
            .fontSize = Style::fontSizeHeader * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    for (const auto& tag : entry.tags) {
      title->addChild(pill(tag, ColorRole::OnSurfaceVariant, ColorRole::SurfaceVariant, 1.0f, kTagBadgeMaxWidth));
    }
    info->addChild(std::move(title));
    auto meta = ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale, .fillWidth = true});
    bool hasMeta = false;
    const auto addMetaItem = [&](std::unique_ptr<Node> item) {
      auto group = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
      if (hasMeta) {
        group->addChild(
            ui::label({
                .text = "·",
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        );
      }
      group->addChild(std::move(item));
      meta->addChild(std::move(group));
      hasMeta = true;
    };
    const auto addMetaText = [&](const std::string& text) {
      addMetaItem(
          ui::label({
              .text = text,
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    };
    if (!entry.author.empty()) {
      addMetaText(entry.author);
    }
    if (!entry.resolvedVersion.empty()) {
      addMetaText("v" + entry.resolvedVersion);
    }
    if (!entry.license.empty()) {
      addMetaText(entry.license);
    }
    if (storeEntry.source == "official") {
      addMetaItem(pill(
          i18n::tr("settings.badges.official"), ColorRole::Primary, ColorRole::Primary, 0.15f, kSourceBadgeMaxWidth
      ));
    } else if (storeEntry.source == "community") {
      addMetaItem(pill(
          i18n::tr("settings.badges.community"), ColorRole::Secondary, ColorRole::Secondary, 0.15f, kSourceBadgeMaxWidth
      ));
    } else {
      addMetaItem(pill(storeEntry.source, ColorRole::Tertiary, ColorRole::Tertiary, 0.15f, kSourceBadgeMaxWidth));
    }
    if (entry.deprecated) {
      addMetaItem(pill(i18n::tr("settings.badges.deprecated"), ColorRole::Error, ColorRole::Error, 0.15f));
    }
    if (entry.heldBack) {
      addMetaItem(pill(i18n::tr("settings.plugins.store.held-back"), ColorRole::Tertiary, ColorRole::Tertiary, 0.15f));
    }
    info->addChild(std::move(meta));

    if (!entry.description.empty()) {
      info->addChild(
          ui::label({
              .text = entry.description,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 4,
              .ellipsize = TextEllipsize::End,
          })
      );
    }

    // Name the version this build gets and why, so the store never silently installs
    // something other than the newest published version.
    if (entry.heldBack) {
      info->addChild(
          ui::label({
              .text = i18n::tr(
                  "settings.plugins.store.held-back-hint", "version", entry.resolvedVersion, "resolved",
                  entry.resolvedPluginApiVersion, "latest", entry.version, "latestRequired", entry.pluginApiVersion,
                  "current", scripting::kCurrentPluginApiVersion
              ),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::Tertiary),
              .maxLines = 3,
          })
      );
    } else if (!entry.compatible) {
      info->addChild(
          ui::label({
              .text = i18n::tr(
                  "settings.plugins.store.incompatible-hint", "required", entry.pluginApiVersion, "oldest",
                  scripting::kOldestSupportedPluginApiVersion, "current", scripting::kCurrentPluginApiVersion
              ),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::Error),
              .maxLines = 3,
          })
      );
    }

    info->addChild(ui::spacer());

    if (enabling) {
      info->addChild(
          ui::spinner({
              .spinnerSize = Style::controlHeightSm * scale * 0.7f,
              .spinning = true,
          })
      );
    } else if (!entry.compatible) {
      info->addChild(
          ui::button({
              .text = i18n::tr("settings.plugins.store.incompatible"),
              .fontSize = Style::fontSizeCaption * scale,
              .enabled = false,
              .variant = ButtonVariant::Default,
          })
      );
    } else if (!onDisk) {
      info->addChild(
          ui::button({
              .text = i18n::tr("settings.plugins.store.add"),
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Primary,
              .onClick = [this, id = entry.id]() {
                if (m_callbacks.setEnabled) {
                  m_callbacks.setEnabled(id, true);
                }
              },
          })
      );
    }
    header->addChild(std::move(info));

    dc->addChild(std::move(header));

    dc->addChild(ui::separator({.spacing = Style::spaceSm * scale}));

    if (m_detailReadmeLoading) {
      dc->addChild(
          ui::spinner({
              .spinnerSize = Style::controlHeightSm * scale,
              .spinning = true,
          })
      );
    } else if (!m_detailReadme.empty()) {
      auto md = std::make_unique<MarkdownView>();
      md->setMarkdown(m_detailReadme, scale);
      dc->addChild(std::move(md));
    } else {
      dc->addChild(
          ui::label({
              .text = i18n::tr("settings.plugins.store.no-readme"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }

    body.addChild(std::move(scroll));
  }

  void PluginStoreContent::openDetail(std::size_t filteredIndex) {
    m_detailIndex = filteredIndex;
    m_selectedPluginId = m_catalog[m_filteredIndices[filteredIndex]].entry.id;
    m_detailReadme.clear();
    m_detailReadmeLoading = false;

    if (filteredIndex < m_filteredIndices.size()) {
      const auto& storeEntry = m_catalog[m_filteredIndices[filteredIndex]];
      if (m_fileCache != nullptr) {
        m_detailReadmeLoading = true;
        std::string path = m_fileCache->resolve(storeEntry.entry.id, storeEntry.sourceConfig, "README.md");
        if (!path.empty()) {
          std::ifstream f(path);
          if (f.is_open()) {
            m_detailReadme = std::string(std::istreambuf_iterator<char>(f), {});
          }
          m_detailReadmeLoading = false;
        }
      }
    }

    if (m_onRebuildNeeded) {
      m_onRebuildNeeded();
    }
  }

  void PluginStoreContent::closeDetail() {
    m_detailIndex.reset();
    m_detailReadme.clear();
    m_detailReadmeLoading = false;
    if (m_onRebuildNeeded) {
      m_onRebuildNeeded();
    }
  }

  void PluginStoreContent::selectIndex(std::size_t index) {
    if (m_filteredIndices.empty()) {
      m_selectedPluginId.reset();
      if (m_grid != nullptr) {
        m_grid->setSelectedIndex(std::nullopt);
      }
      return;
    }
    index = std::min(index, m_filteredIndices.size() - 1);
    if (m_grid != nullptr) {
      m_grid->setSelectedIndex(index);
    }
    m_selectedPluginId = m_catalog[m_filteredIndices[index]].entry.id;
  }

  std::optional<std::size_t> PluginStoreContent::indexOfPluginId(std::string_view id) const {
    if (id.empty()) {
      return std::nullopt;
    }
    const auto it = std::ranges::find_if(m_filteredIndices, [&](std::size_t i) { return m_catalog[i].entry.id == id; });
    if (it == m_filteredIndices.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(it - m_filteredIndices.begin());
  }

  void PluginStoreContent::syncGridSelection() {
    if (m_grid == nullptr) {
      return;
    }
    m_grid->notifyDataChanged();
    m_grid->setSelectedIndex(indexOfPluginId(m_selectedPluginId.value_or("")));
  }

  void PluginStoreContent::moveSelection(int delta) {
    if (m_filteredIndices.empty()) {
      return;
    }
    const auto index = indexOfPluginId(m_selectedPluginId.value_or(""));
    if (!index.has_value()) {
      selectIndex(delta >= 0 ? 0 : m_filteredIndices.size() - 1);
      return;
    }
    const int last = static_cast<int>(m_filteredIndices.size() - 1);
    const int next = std::clamp(static_cast<int>(*index) + delta, 0, last);
    selectIndex(static_cast<std::size_t>(next));
  }

  bool PluginStoreContent::activateSelection() {
    if (m_filteredIndices.empty()) {
      return false;
    }
    std::optional<std::size_t> index = indexOfPluginId(m_selectedPluginId.value_or(""));
    if (!index.has_value()) {
      selectIndex(0);
      index = 0;
    }
    openDetail(*index);
    return true;
  }

  bool PluginStoreContent::installDetailIfAvailable() {
    if (!m_detailIndex.has_value() || *m_detailIndex >= m_filteredIndices.size()) {
      return false;
    }
    const auto& storeEntry = m_catalog[m_filteredIndices[*m_detailIndex]];
    const auto& entry = storeEntry.entry;
    if (!entry.compatible || m_onDiskIds.contains(entry.id)) {
      return false;
    }
    if (m_callbacks.isEnabling && m_callbacks.isEnabling(entry.id)) {
      return false;
    }
    if (!m_callbacks.setEnabled) {
      return false;
    }
    m_callbacks.setEnabled(entry.id, true);
    return true;
  }

  bool PluginStoreContent::handleKeyEvent(
      std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit, InputArea* focused
  ) {
    if (!pressed || preedit) {
      return false;
    }

    // Search, category chips, and detail actions own Enter/arrows while focused.
    const InputArea* gridFocus = m_grid != nullptr ? m_grid->focusArea() : nullptr;
    if (focused != nullptr && focused != gridFocus) {
      return false;
    }

    if (isDetailView()) {
      if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
        return installDetailIfAvailable();
      }
      return false;
    }

    if (m_filteredIndices.empty()) {
      return false;
    }

    const int columns = m_grid != nullptr ? static_cast<int>(std::max<std::size_t>(1, m_grid->layoutColumnCount())) : 1;

    if (KeySymbol::isPageUp(sym)) {
      const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : columns;
      moveSelection(-stride);
      return true;
    }
    if (KeySymbol::isPageDown(sym)) {
      const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : columns;
      moveSelection(stride);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
      moveSelection(-1);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
      moveSelection(1);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
      moveSelection(-columns);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
      moveSelection(columns);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
      return activateSelection();
    }
    return false;
  }

  void PluginStoreContent::updateOnDiskIds(std::unordered_set<std::string> ids) {
    m_onDiskIds = std::move(ids);
    if (m_grid != nullptr && !isDetailView()) {
      m_grid->notifyDataChanged();
    }
  }

  void
  PluginStoreContent::onFileReady(const std::string& pluginId, const std::string& filename, const std::string& path) {
    if (filename == "thumbnail.webp") {
      m_thumbnailPaths[pluginId] = path;
      if (m_grid != nullptr && !isDetailView()) {
        m_grid->notifyDataChanged();
      }
    } else if (filename == "README.md" && isDetailView()) {
      const auto& storeEntry = m_catalog[m_filteredIndices[*m_detailIndex]];
      if (storeEntry.entry.id == pluginId) {
        std::ifstream f(path);
        if (f.is_open()) {
          m_detailReadme = std::string(std::istreambuf_iterator<char>(f), {});
        }
        m_detailReadmeLoading = false;
        if (m_onRebuildNeeded) {
          m_onRebuildNeeded();
        }
      }
    }
  }

} // namespace settings
