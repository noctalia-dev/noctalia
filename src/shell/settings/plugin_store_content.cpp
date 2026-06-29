#include "shell/settings/plugin_store_content.h"

#include "i18n/i18n.h"
#include "scripting/plugin_file_cache.h"
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
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace settings {

  namespace {

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

      void bindTile(Node& tile, std::size_t index, bool /*selected*/, bool hovered) override {
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
        t->bind(storeEntry.entry, storeEntry.source, onDisk, hovered, thumbPath, m_renderer, m_textureCache);
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
    collectTags();
    applyFilter();
  }

  PluginStoreContent::~PluginStoreContent() = default;

  void PluginStoreContent::setOnRebuildNeeded(std::function<void()> cb) { m_onRebuildNeeded = std::move(cb); }

  bool PluginStoreContent::isDetailView() const noexcept { return m_detailIndex.has_value(); }

  void PluginStoreContent::collectTags() {
    std::set<std::string> tagSet;
    for (const auto& entry : m_catalog) {
      for (const auto& tag : entry.entry.tags) {
        tagSet.insert(tag);
      }
      if (m_fileCache != nullptr) {
        std::string path = m_fileCache->resolve(entry.entry.id, entry.sourceConfig, "thumbnail.webp");
        if (!path.empty()) {
          m_thumbnailPaths[entry.entry.id] = path;
        }
      }
    }
    m_tags.assign(tagSet.begin(), tagSet.end());
  }

  void PluginStoreContent::applyFilter() {
    m_filteredIndices.clear();
    for (std::size_t i = 0; i < m_catalog.size(); ++i) {
      const auto& e = m_catalog[i];
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
              }
              if (m_countLabel != nullptr) {
                m_countLabel->setText(
                    i18n::tr("settings.plugins.store.results-count", "count", std::to_string(m_filteredIndices.size()))
                );
              }
            },
        })
    );

    if (!m_tags.empty()) {
      std::vector<std::string> allTags;
      allTags.push_back(i18n::tr("settings.plugins.store.category-all"));
      allTags.insert(allTags.end(), m_tags.begin(), m_tags.end());
      std::vector<std::unique_ptr<Button>> tagButtons;
      for (std::size_t i = 0; i < allTags.size(); ++i) {
        const bool selected = (i == 0 && m_selectedTag.empty()) || (i > 0 && m_tags[i - 1] == m_selectedTag);
        auto btn = ui::button({
            .text = allTags[i],
            .fontSize = Style::fontSizeCaption * scale,
            .variant = selected ? ButtonVariant::Default : ButtonVariant::Outline,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this, i]() {
              m_selectedTag = i == 0 ? std::string{} : m_tags[i - 1];
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
    grid->setCellHeight(260.0f * scale);
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

    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});

    auto thumbIt = m_thumbnailPaths.find(entry.id);
    if (thumbIt != m_thumbnailPaths.end() && !thumbIt->second.empty()) {
      auto img = ui::image({
          .fit = ImageFit::Cover,
          .radius = Style::scaledRadiusMd(scale),
          .width = 120.0f * scale,
          .height = 80.0f * scale,
      });
      if (textureCache != nullptr) {
        img->setSourceFileAsync(renderer, *textureCache, thumbIt->second);
      } else {
        img->setSourceFile(renderer, thumbIt->second);
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

    auto info = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0f});
    info->addChild(
        ui::label({
            .text = entry.name,
            .fontSize = Style::fontSizeHeader * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .fontWeight = FontWeight::Bold,
        })
    );
    if (!entry.author.empty()) {
      info->addChild(
          ui::label({
              .text = entry.author,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
    auto meta = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    if (!entry.version.empty()) {
      meta->addChild(
          ui::label({
              .text = "v" + entry.version,
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
    if (storeEntry.source == "official") {
      meta->addChild(
          ui::row(
              {.align = FlexAlign::Center,
               .paddingH = Style::spaceXs * scale,
               .fill = colorSpecFromRole(ColorRole::Primary, 0.15f),
               .radius = Style::scaledRadiusSm(scale)},
              ui::label({
                  .text = i18n::tr("settings.badges.official"),
                  .fontSize = Style::fontSizeMini * scale,
                  .color = colorSpecFromRole(ColorRole::Primary),
                  .fontWeight = FontWeight::Bold,
              })
          )
      );
    }
    info->addChild(std::move(meta));
    header->addChild(std::move(info));

    if (enabling) {
      header->addChild(
          ui::spinner({
              .spinnerSize = Style::controlHeightSm * scale * 0.7f,
              .spinning = true,
          })
      );
    } else if (!entry.compatible) {
      header->addChild(
          ui::button({
              .text = i18n::tr("settings.plugins.store.incompatible"),
              .fontSize = Style::fontSizeCaption * scale,
              .enabled = false,
              .variant = ButtonVariant::Default,
          })
      );
    } else if (!onDisk) {
      header->addChild(
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
    } else if (!entry.description.empty()) {
      dc->addChild(
          ui::label({
              .text = entry.description,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 0,
          })
      );
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
