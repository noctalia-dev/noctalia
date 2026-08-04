#pragma once

#include "theme/builtin_templates.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class Button;
class ConfigService;
class Flex;
class InputArea;
class Label;
class Renderer;
class VirtualGridAdapter;
class VirtualGridView;

namespace settings {

  enum class TemplateSortMode : std::uint8_t {
    NameAsc,
    NameDesc,
    CategoryAsc,
    CategoryDesc,
  };

  struct TemplateStoreCallbacks {
    // Called with catalog-ordered enabled ids whenever the selection changes.
    std::function<void(std::vector<std::string> ids)> setSelected;
    float scale = 1.0F;
  };

  class TemplateStoreContent {
  public:
    TemplateStoreContent(
        std::vector<noctalia::theme::AvailableTemplate> catalog, std::unordered_set<std::string> selectedIds,
        ConfigService* config, TemplateStoreCallbacks callbacks
    );
    ~TemplateStoreContent();

    void populateBody(Flex& body, Renderer& renderer);

    void setOnRebuildNeeded(std::function<void()> cb);

    // Arrow/page/validate navigation for the catalog grid.
    // Returns true when the event was consumed. Pass the sheet's focused InputArea so
    // chrome controls (search, category chips) keep their own Enter/arrow handling.
    [[nodiscard]] bool
    handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit, InputArea* focused);

    void toggleAtFilteredIndex(std::size_t filteredIndex);
    void setEnabledAtFilteredIndex(std::size_t filteredIndex, bool enabled);

  private:
    void collectCategories();
    void applyFilter();
    void sortFiltered();
    void cycleSortMode();
    void setSortMode(TemplateSortMode mode);
    void syncSortButtonGlyph();
    [[nodiscard]] static TemplateSortMode sortModeFromState(std::string_view value);
    [[nodiscard]] static std::string_view sortModeStateValue(TemplateSortMode mode);
    [[nodiscard]] static std::string_view sortModeGlyph(TemplateSortMode mode);
    [[nodiscard]] static const char* sortModeTooltipKey(TemplateSortMode mode);
    void commitSelection();
    void selectIndex(std::size_t index);
    [[nodiscard]] std::optional<std::size_t> indexOfTemplateId(std::string_view id) const;
    void syncGridSelection();
    void moveSelection(int delta);
    [[nodiscard]] bool activateSelection();

    std::vector<noctalia::theme::AvailableTemplate> m_catalog;
    std::unordered_set<std::string> m_selectedIds;
    std::vector<std::size_t> m_filteredIndices;
    std::vector<std::string> m_allCategories;
    bool m_categoryFiltersCollapsed = true;
    TemplateSortMode m_sortMode = TemplateSortMode::NameAsc;
    std::string m_searchQuery;
    // Empty = all templates. `kEnabledFilter` shows only enabled. Otherwise a catalog category.
    std::string m_selectedCategory;
    ConfigService* m_config = nullptr;
    TemplateStoreCallbacks m_callbacks;

    Label* m_countLabel = nullptr;
    Button* m_sortButton = nullptr;
    VirtualGridView* m_grid = nullptr;
    std::unique_ptr<VirtualGridAdapter> m_adapter;
    std::optional<std::string> m_selectedTemplateId;
    std::function<void()> m_onRebuildNeeded;
  };

} // namespace settings
