#include "shell/settings/plugin_store_content.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
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
#include "ui/controls/separator.h"
#include "ui/controls/spinner.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"

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
      std::vector<StoreCatalogEntry> catalog, std::unordered_set<std::string> onDiskIds, PluginStoreCallbacks callbacks,
      scripting::PluginFileCache* fileCache
  )
      : m_catalog(std::move(catalog)), m_onDiskIds(std::move(onDiskIds)), m_callbacks(std::move(callbacks)),
        m_fileCache(fileCache) {
    collectThumbnails();
    collectSources();
    applyFilter();
  }

  PluginStoreContent::~PluginStoreContent() = default;

  void PluginStoreContent::setOnRebuildNeeded(std::function<void()> cb) { m_onRebuildNeeded = std::move(cb); }

  bool PluginStoreContent::isDetailView() const noexcept { return m_detailIndex.has_value(); }

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

  std::vector<std::string> PluginStoreContent::availableTags() const {
    std::set<std::string> tagSet;
    for (const auto& entry : m_catalog) {
      if (!m_selectedSource.empty() && entry.source != m_selectedSource) {
        continue;
      }
      for (const auto& tag : entry.entry.tags) {
        tagSet.insert(tag);
      }
    }
    return {tagSet.begin(), tagSet.end()};
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

  void PluginStoreContent::applyFilter() {
    m_filteredIndices.clear();
    for (std::size_t i = 0; i < m_catalog.size(); ++i) {
      const auto& e = m_catalog[i];
      if (!m_selectedSource.empty() && e.source != m_selectedSource) {
        continue;
      }
      if (!m_selectedTag.empty()) {
        if (!std::ranges::contains(e.entry.tags, m_selectedTag)) {
          continue;
        }
      }
      if (!m_searchQuery.empty()) {
        const std::string haystack = e.entry.name + " " + e.entry.description + " " + e.entry.author;
        if (!containsIgnoreCase(haystack, m_searchQuery)) {
          continue;
        }
      }
      m_filteredIndices.push_back(i);
    }
    std::ranges::sort(m_filteredIndices, [this](std::size_t a, std::size_t b) {
      return m_catalog[a].entry.name < m_catalog[b].entry.name;
    });
    if (m_filteredIndices.empty()) {
      m_selectedIndex.reset();
    } else if (m_selectedIndex.has_value() && *m_selectedIndex >= m_filteredIndices.size()) {
      m_selectedIndex = m_filteredIndices.size() - 1;
    }
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
              if (m_grid != nullptr) {
                m_grid->notifyDataChanged();
                m_grid->setSelectedIndex(m_selectedIndex);
              }
              if (m_countLabel != nullptr) {
                m_countLabel->setText(
                    i18n::tr("settings.plugins.store.results-count", "count", std::to_string(m_filteredIndices.size()))
                );
              }
            },
        })
    );

    if (m_sources.size() > 1) {
      body.addChild(
          ui::label({
              .text = i18n::tr("settings.plugins.store.sources"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );

      std::vector<std::string> allSources;
      allSources.push_back(i18n::tr("settings.plugins.store.source-all"));
      allSources.insert(allSources.end(), m_sources.begin(), m_sources.end());
      std::vector<std::unique_ptr<Button>> sourceButtons;
      for (std::size_t i = 0; i < allSources.size(); ++i) {
        const bool selected = (i == 0 && m_selectedSource.empty()) || (i > 0 && m_sources[i - 1] == m_selectedSource);
        const std::string text = i == 0 ? allSources[i] : sourceDisplayName(m_sources[i - 1]);
        sourceButtons.push_back(
            ui::button({
                .text = text,
                .fontSize = Style::fontSizeCaption * scale,
                .variant = selected ? ButtonVariant::Primary : ButtonVariant::Default,
                .radius = Style::scaledRadiusMd(scale),
                .onClick = [this, i]() {
                  m_selectedSource = i == 0 ? std::string{} : m_sources[i - 1];
                  if (!m_selectedTag.empty() && !std::ranges::contains(availableTags(), m_selectedTag)) {
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
      auto sourceRows = wrapButtonsIntoRows(
          renderer, sourceButtons, body.width() > 0 ? body.width() : 700.0f * scale, Style::spaceXs * scale
      );
      for (auto& row : sourceRows) {
        auto rowFlex = ui::row(
            {.align = FlexAlign::Center,
             .justify = FlexJustify::Center,
             .gap = Style::spaceXs * scale,
             .fillWidth = true}
        );
        for (auto& btn : row) {
          rowFlex->addChild(std::move(btn));
        }
        body.addChild(std::move(rowFlex));
      }
    }

    const std::vector<std::string> tags = availableTags();
    if (!tags.empty()) {
      auto tagsHeader = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .fillWidth = true});
      tagsHeader->addChild(
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

      const std::string selectedTag =
          m_selectedTag.empty() ? i18n::tr("settings.plugins.store.category-all") : m_selectedTag;
      tagsHeader->addChild(
          ui::label({
              .text = selectedTag,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      body.addChild(std::move(tagsHeader));

      if (!m_tagFiltersCollapsed) {
        std::vector<std::string> allTags;
        allTags.push_back(i18n::tr("settings.plugins.store.category-all"));
        allTags.insert(allTags.end(), tags.begin(), tags.end());
        std::vector<std::unique_ptr<Button>> tagButtons;
        for (std::size_t i = 0; i < allTags.size(); ++i) {
          const std::string tag = i == 0 ? std::string{} : tags[i - 1];
          const bool selected = tag == m_selectedTag;
          auto btn = ui::button({
              .text = allTags[i],
              .fontSize = Style::fontSizeCaption * scale,
              .variant = selected ? ButtonVariant::Primary : ButtonVariant::Default,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this, tag]() {
                m_selectedTag = tag;
                applyFilter();
                if (m_onRebuildNeeded) {
                  m_onRebuildNeeded();
                }
              },
          });
          tagButtons.push_back(std::move(btn));
        }
        auto rows = wrapButtonsIntoRows(
            renderer, tagButtons, body.width() > 0 ? body.width() : 700.0f * scale, Style::spaceXs * scale
        );
        for (auto& row : rows) {
          auto rowFlex = ui::row(
              {.align = FlexAlign::Center,
               .justify = FlexJustify::Center,
               .gap = Style::spaceXs * scale,
               .fillWidth = true}
          );
          for (auto& btn : row) {
            rowFlex->addChild(std::move(btn));
          }
          body.addChild(std::move(rowFlex));
        }
      }
    }

    body.addChild(
        ui::label({
            .out = &m_countLabel,
            .text = i18n::tr("settings.plugins.store.results-count", "count", std::to_string(m_filteredIndices.size())),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

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

    auto grid = std::make_unique<VirtualGridView>();
    grid->setMinCellWidth(200.0f * scale);
    grid->setCellHeight(215.0f * scale);
    grid->setSquareCells(false);
    grid->setColumnGap(Style::spaceSm * scale);
    grid->setRowGap(Style::spaceSm * scale);
    grid->setFillWidth(true);
    // The sheet hosts the store without an outer ScrollView, so the grid's own scroll fills the
    // available height and scrolls the catalog. No minimum height: a floor would overflow the
    // sheet bottom (and clip nothing) when the dialog is shorter than the floor.
    grid->setFlexGrow(1.0f);
    grid->setAdapter(adapterPtr);
    m_grid = grid.get();
    m_grid->setOnSelectionChanged([this](std::optional<std::size_t> index) { m_selectedIndex = index; });
    if (m_selectedIndex.has_value()) {
      m_grid->setSelectedIndex(m_selectedIndex);
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
    if (!entry.version.empty()) {
      addMetaText("v" + entry.version);
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
    m_selectedIndex = filteredIndex;
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
      m_selectedIndex.reset();
      if (m_grid != nullptr) {
        m_grid->setSelectedIndex(std::nullopt);
      }
      return;
    }
    m_selectedIndex = std::min(index, m_filteredIndices.size() - 1);
    if (m_grid != nullptr) {
      m_grid->setSelectedIndex(m_selectedIndex);
    }
  }

  void PluginStoreContent::moveSelection(int delta) {
    if (m_filteredIndices.empty()) {
      return;
    }
    if (!m_selectedIndex.has_value()) {
      selectIndex(delta >= 0 ? 0 : m_filteredIndices.size() - 1);
      return;
    }
    const int last = static_cast<int>(m_filteredIndices.size() - 1);
    const int next = std::clamp(static_cast<int>(*m_selectedIndex) + delta, 0, last);
    selectIndex(static_cast<std::size_t>(next));
  }

  bool PluginStoreContent::activateSelection() {
    if (m_filteredIndices.empty()) {
      return false;
    }
    if (!m_selectedIndex.has_value() || *m_selectedIndex >= m_filteredIndices.size()) {
      selectIndex(0);
    }
    openDetail(*m_selectedIndex);
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
