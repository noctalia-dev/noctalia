#pragma once

#include "config/config_types.h"
#include "scripting/plugin_catalog.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class AsyncTextureCache;
class Flex;
class InputArea;
class Label;
class Renderer;
class VirtualGridAdapter;
class VirtualGridView;

namespace scripting {
  class PluginFileCache;
}

namespace settings {

  struct StoreCatalogEntry {
    scripting::CatalogEntry entry;
    std::string source;
    PluginSourceConfig sourceConfig;
  };

  struct PluginStoreCallbacks {
    std::function<void(std::string id, bool enable)> setEnabled;
    std::function<bool(const std::string& id)> isEnabling;
    float scale = 1.0f;
  };

  class PluginStoreContent {
  public:
    PluginStoreContent(
        std::vector<StoreCatalogEntry> catalog, std::unordered_set<std::string> onDiskIds,
        PluginStoreCallbacks callbacks, scripting::PluginFileCache* fileCache
    );
    ~PluginStoreContent();

    void populateBody(Flex& body, Renderer& renderer, AsyncTextureCache* textureCache);

    void updateOnDiskIds(std::unordered_set<std::string> ids);
    void onFileReady(const std::string& pluginId, const std::string& filename, const std::string& path);

    void setOnRebuildNeeded(std::function<void()> cb);

    [[nodiscard]] bool isDetailView() const noexcept;
    [[nodiscard]] std::optional<std::string> detailPageUrl() const;
    [[nodiscard]] std::optional<std::string> detailSourceUrl() const;
    void openDetail(std::size_t filteredIndex);
    void closeDetail();

    // Arrow/page/validate navigation for the catalog grid and detail install action.
    // Returns true when the event was consumed. Pass the sheet's focused InputArea so
    // chrome controls (search, category chips) keep their own Enter/arrow handling.
    [[nodiscard]] bool
    handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit, InputArea* focused);

  private:
    void buildGridView(Flex& body, Renderer& renderer, AsyncTextureCache* textureCache);
    void buildDetailView(Flex& body, Renderer& renderer, AsyncTextureCache* textureCache);
    void applyFilter();
    void collectThumbnails();
    void collectSources();
    // Tags present in the catalog under the active source filter (so category chips never offer a
    // category with zero results for the selected source).
    [[nodiscard]] std::vector<std::string> availableTags() const;
    void selectIndex(std::size_t index);
    void moveSelection(int delta);
    [[nodiscard]] bool activateSelection();
    [[nodiscard]] bool installDetailIfAvailable();

    std::vector<StoreCatalogEntry> m_catalog;
    std::vector<std::size_t> m_filteredIndices;
    std::unordered_set<std::string> m_onDiskIds;
    std::vector<std::string> m_sources;
    bool m_tagFiltersCollapsed = true;
    std::string m_searchQuery;
    std::string m_selectedTag;
    std::string m_selectedSource;
    PluginStoreCallbacks m_callbacks;
    scripting::PluginFileCache* m_fileCache = nullptr;

    std::optional<std::size_t> m_detailIndex;
    std::string m_detailReadme;
    bool m_detailReadmeLoading = false;

    VirtualGridView* m_grid = nullptr;
    Label* m_countLabel = nullptr;
    std::optional<std::size_t> m_selectedIndex;
    std::function<void()> m_onRebuildNeeded;

    std::unordered_map<std::string, std::string> m_thumbnailPaths;

    Renderer* m_renderer = nullptr;
    AsyncTextureCache* m_textureCache = nullptr;
    std::unique_ptr<VirtualGridAdapter> m_adapter;
  };

} // namespace settings
