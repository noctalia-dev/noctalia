#include "shell/settings/template_store_content.h"

#include "config/config_service.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "i18n/i18n.h"
#include "shell/settings/template_store_tile.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace settings {

  namespace {

    // Sentinel for the Enabled chip — not a real catalog category value.
    constexpr std::string_view kEnabledFilter = "__enabled__";

    [[nodiscard]] bool containsInsensitive(std::string_view haystack, std::string_view needle) {
      if (needle.empty()) {
        return true;
      }
      return !std::ranges::search(haystack, needle, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
              }).empty();
    }

    class TemplateStoreAdapter final : public VirtualGridAdapter {
    public:
      explicit TemplateStoreAdapter(float scale) : m_scale(scale) {}

      void setContent(TemplateStoreContent* content) { m_content = content; }
      void setFilteredIndices(const std::vector<std::size_t>* indices) { m_indices = indices; }
      void setCatalog(const std::vector<noctalia::theme::AvailableTemplate>* catalog) { m_catalog = catalog; }
      void setSelectedIds(const std::unordered_set<std::string>* ids) { m_selectedIds = ids; }

      [[nodiscard]] std::size_t itemCount() const override { return m_indices != nullptr ? m_indices->size() : 0; }

      [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<TemplateStoreTile>(m_scale); }

      void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
        if (m_indices == nullptr || m_catalog == nullptr || index >= m_indices->size()) {
          return;
        }
        auto* t = static_cast<TemplateStoreTile*>(&tile);
        const auto& entry = (*m_catalog)[(*m_indices)[index]];
        const bool enabled = m_selectedIds != nullptr && m_selectedIds->contains(entry.id);
        t->bind(entry.displayName, entry.category, enabled, selected, hovered, [this, index](bool nextEnabled) {
          if (m_content != nullptr) {
            m_content->setEnabledAtFilteredIndex(index, nextEnabled);
          }
        });
      }

      [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
        if (m_indices == nullptr || m_catalog == nullptr || index >= m_indices->size()) {
          return {};
        }
        return noctalia::theme::formatTemplateTooltip((*m_catalog)[(*m_indices)[index]]);
      }

      void onActivate(std::size_t index) override {
        if (m_content != nullptr) {
          m_content->toggleAtFilteredIndex(index);
        }
      }

    private:
      float m_scale;
      TemplateStoreContent* m_content = nullptr;
      const std::vector<std::size_t>* m_indices = nullptr;
      const std::vector<noctalia::theme::AvailableTemplate>* m_catalog = nullptr;
      const std::unordered_set<std::string>* m_selectedIds = nullptr;
    };

  } // namespace

  TemplateStoreContent::TemplateStoreContent(
      std::vector<noctalia::theme::AvailableTemplate> catalog, std::unordered_set<std::string> selectedIds,
      ConfigService* config, TemplateStoreCallbacks callbacks
  )
      : m_catalog(std::move(catalog)), m_selectedIds(std::move(selectedIds)), m_config(config),
        m_callbacks(std::move(callbacks)) {
    if (m_config != nullptr) {
      if (const std::optional<std::string> sort = m_config->stateString("template_store", "sort")) {
        m_sortMode = sortModeFromState(*sort);
      }
    }
    collectCategories();
    applyFilter();
  }

  TemplateStoreContent::~TemplateStoreContent() = default;

  void TemplateStoreContent::setOnRebuildNeeded(std::function<void()> cb) { m_onRebuildNeeded = std::move(cb); }

  void TemplateStoreContent::collectCategories() {
    std::set<std::string> categories;
    for (const auto& entry : m_catalog) {
      if (!entry.category.empty()) {
        categories.insert(entry.category);
      }
    }
    m_allCategories.assign(categories.begin(), categories.end());
  }

  void TemplateStoreContent::sortFiltered() {
    std::ranges::sort(m_filteredIndices, [this](std::size_t a, std::size_t b) {
      const auto& left = m_catalog[a];
      const auto& right = m_catalog[b];
      const int nameOrder = StringUtils::naturalCaseInsensitiveCompare(left.displayName, right.displayName);
      const int categoryOrder = StringUtils::naturalCaseInsensitiveCompare(left.category, right.category);
      switch (m_sortMode) {
      case TemplateSortMode::NameDesc:
        return nameOrder > 0;
      case TemplateSortMode::CategoryAsc:
        return categoryOrder != 0 ? categoryOrder < 0 : nameOrder < 0;
      case TemplateSortMode::CategoryDesc:
        return categoryOrder != 0 ? categoryOrder > 0 : nameOrder < 0;
      case TemplateSortMode::NameAsc:
      default:
        return nameOrder < 0;
      }
    });
  }

  void TemplateStoreContent::cycleSortMode() {
    TemplateSortMode next = TemplateSortMode::NameAsc;
    switch (m_sortMode) {
    case TemplateSortMode::NameAsc:
      next = TemplateSortMode::NameDesc;
      break;
    case TemplateSortMode::NameDesc:
      next = TemplateSortMode::CategoryAsc;
      break;
    case TemplateSortMode::CategoryAsc:
      next = TemplateSortMode::CategoryDesc;
      break;
    case TemplateSortMode::CategoryDesc:
      next = TemplateSortMode::NameAsc;
      break;
    }
    setSortMode(next);
  }

  void TemplateStoreContent::setSortMode(TemplateSortMode mode) {
    if (m_sortMode == mode) {
      return;
    }
    m_sortMode = mode;
    if (m_config != nullptr) {
      (void)m_config->setStateString("template_store", "sort", std::string(sortModeStateValue(mode)));
    }
    syncSortButtonGlyph();
    sortFiltered();
    syncGridSelection();
    if (m_grid != nullptr) {
      m_grid->notifyDataChanged();
    }
  }

  TemplateSortMode TemplateStoreContent::sortModeFromState(std::string_view value) {
    if (value == "name_desc") {
      return TemplateSortMode::NameDesc;
    }
    if (value == "category_asc") {
      return TemplateSortMode::CategoryAsc;
    }
    if (value == "category_desc") {
      return TemplateSortMode::CategoryDesc;
    }
    return TemplateSortMode::NameAsc;
  }

  std::string_view TemplateStoreContent::sortModeStateValue(TemplateSortMode mode) {
    switch (mode) {
    case TemplateSortMode::NameDesc:
      return "name_desc";
    case TemplateSortMode::CategoryAsc:
      return "category_asc";
    case TemplateSortMode::CategoryDesc:
      return "category_desc";
    case TemplateSortMode::NameAsc:
    default:
      return "name_asc";
    }
  }

  std::string_view TemplateStoreContent::sortModeGlyph(TemplateSortMode mode) {
    switch (mode) {
    case TemplateSortMode::NameDesc:
      return "sort-z-a";
    case TemplateSortMode::CategoryAsc:
      return "sort-ascending-2";
    case TemplateSortMode::CategoryDesc:
      return "sort-descending-2";
    case TemplateSortMode::NameAsc:
    default:
      return "sort-a-z";
    }
  }

  const char* TemplateStoreContent::sortModeTooltipKey(TemplateSortMode mode) {
    switch (mode) {
    case TemplateSortMode::NameDesc:
      return "settings.templates.store.sort-name-desc";
    case TemplateSortMode::CategoryAsc:
      return "settings.templates.store.sort-category-asc";
    case TemplateSortMode::CategoryDesc:
      return "settings.templates.store.sort-category-desc";
    case TemplateSortMode::NameAsc:
    default:
      return "settings.templates.store.sort-name-asc";
    }
  }

  void TemplateStoreContent::syncSortButtonGlyph() {
    if (m_sortButton == nullptr) {
      return;
    }
    m_sortButton->setGlyph(sortModeGlyph(m_sortMode));
    m_sortButton->setTooltip(i18n::tr(sortModeTooltipKey(m_sortMode)));
  }

  void TemplateStoreContent::applyFilter() {
    m_filteredIndices.clear();
    m_filteredIndices.reserve(m_catalog.size());
    const bool enabledOnly = m_selectedCategory == kEnabledFilter;
    for (std::size_t i = 0; i < m_catalog.size(); ++i) {
      const auto& entry = m_catalog[i];
      if (enabledOnly) {
        if (!m_selectedIds.contains(entry.id)) {
          continue;
        }
      } else if (!m_selectedCategory.empty() && entry.category != m_selectedCategory) {
        continue;
      }
      if (!m_searchQuery.empty()) {
        const bool match = containsInsensitive(entry.displayName, m_searchQuery)
            || containsInsensitive(entry.id, m_searchQuery)
            || containsInsensitive(entry.category, m_searchQuery);
        if (!match) {
          continue;
        }
      }
      m_filteredIndices.push_back(i);
    }
    sortFiltered();
  }

  void TemplateStoreContent::commitSelection() {
    if (!m_callbacks.setSelected) {
      return;
    }
    std::vector<std::string> ordered;
    ordered.reserve(m_selectedIds.size());
    for (const auto& entry : m_catalog) {
      if (m_selectedIds.contains(entry.id)) {
        ordered.push_back(entry.id);
      }
    }
    m_callbacks.setSelected(std::move(ordered));
  }

  void TemplateStoreContent::toggleAtFilteredIndex(std::size_t filteredIndex) {
    if (filteredIndex >= m_filteredIndices.size()) {
      return;
    }
    const auto& entry = m_catalog[m_filteredIndices[filteredIndex]];
    setEnabledAtFilteredIndex(filteredIndex, !m_selectedIds.contains(entry.id));
  }

  void TemplateStoreContent::setEnabledAtFilteredIndex(std::size_t filteredIndex, bool enabled) {
    if (filteredIndex >= m_filteredIndices.size()) {
      return;
    }
    const auto& entry = m_catalog[m_filteredIndices[filteredIndex]];
    if (enabled == m_selectedIds.contains(entry.id)) {
      return;
    }
    if (enabled) {
      m_selectedIds.insert(entry.id);
    } else {
      m_selectedIds.erase(entry.id);
    }
    m_selectedTemplateId = entry.id;
    commitSelection();

    if (m_selectedCategory == kEnabledFilter) {
      applyFilter();
      if (m_onRebuildNeeded) {
        m_onRebuildNeeded();
      } else if (m_grid != nullptr) {
        m_grid->notifyDataChanged();
        syncGridSelection();
      }
      return;
    }

    if (m_grid != nullptr) {
      m_grid->notifyItemChanged(filteredIndex);
      m_grid->setSelectedIndex(filteredIndex);
    }
    if (m_countLabel != nullptr) {
      m_countLabel->setText(
          i18n::tr("settings.templates.store.results-count", "count", std::to_string(m_filteredIndices.size()))
      );
    }
  }

  void TemplateStoreContent::populateBody(Flex& body, Renderer& renderer) {
    const float scale = m_callbacks.scale;
    m_countLabel = nullptr;
    m_sortButton = nullptr;
    m_grid = nullptr;
    m_adapter.reset();

    body.setDirection(FlexDirection::Vertical);
    body.setAlign(FlexAlign::Stretch);
    body.setGap(Style::spaceSm * scale);
    body.setPadding(Style::spaceMd * scale);

    body.addChild(
        ui::input({
            .value = m_searchQuery,
            .placeholder = i18n::tr("settings.templates.store.search-placeholder"),
            .fontSize = Style::fontSizeBody * scale,
            .onChange = [this](const std::string& text) {
              m_searchQuery = text;
              applyFilter();
              syncGridSelection();
              if (m_countLabel != nullptr) {
                m_countLabel->setText(
                    i18n::tr(
                        "settings.templates.store.results-count", "count", std::to_string(m_filteredIndices.size())
                    )
                );
              }
              if (m_grid != nullptr) {
                m_grid->notifyDataChanged();
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

    std::vector<std::unique_ptr<Button>> filterButtons;
    toolbar->addChild(
        ui::button({
            .text = i18n::tr("settings.templates.store.categories"),
            .glyph = m_categoryFiltersCollapsed ? std::string("chevron-right") : std::string("chevron-down"),
            .fontSize = Style::fontSizeCaption * scale,
            .glyphSize = Style::fontSizeCaption * scale,
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Ghost,
            .onClick = [this]() {
              m_categoryFiltersCollapsed = !m_categoryFiltersCollapsed;
              if (m_onRebuildNeeded) {
                m_onRebuildNeeded();
              }
            },
        })
    );

    if (!m_categoryFiltersCollapsed) {
      const auto makeFilterChip = [&](const std::string& label, std::string filterKey) {
        const bool selected = m_selectedCategory == filterKey;
        filterButtons.push_back(
            ui::button({
                .text = label,
                .fontSize = Style::fontSizeCaption * scale,
                .variant = selected ? ButtonVariant::Primary : ButtonVariant::Default,
                .radius = Style::scaledRadiusMd(scale),
                .onClick = [this, filterKey = std::move(filterKey)]() {
                  m_selectedCategory = filterKey;
                  applyFilter();
                  if (m_onRebuildNeeded) {
                    m_onRebuildNeeded();
                  }
                },
            })
        );
      };

      makeFilterChip(i18n::tr("settings.templates.store.category-all"), {});
      makeFilterChip(i18n::tr("settings.templates.store.filter-enabled"), std::string(kEnabledFilter));
      for (const auto& category : m_allCategories) {
        makeFilterChip(category, category);
      }
    }

    Flex& summaryRow = m_categoryFiltersCollapsed ? *toolbar : *toolbarBottom;
    summaryRow.addChild(ui::spacer());
    summaryRow.addChild(
        ui::label({
            .out = &m_countLabel,
            .text =
                i18n::tr("settings.templates.store.results-count", "count", std::to_string(m_filteredIndices.size())),
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

    if (!m_categoryFiltersCollapsed) {
      auto chipRow = ui::row({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .wrap = true,
          .gap = Style::spaceXs * scale,
          .fillWidth = true,
      });
      for (auto& btn : filterButtons) {
        chipRow->addChild(std::move(btn));
      }
      body.addChild(std::move(chipRow));
      body.addChild(std::move(toolbarBottom));
    }

    auto adapter = std::make_unique<TemplateStoreAdapter>(scale);
    auto* adapterPtr = adapter.get();
    adapterPtr->setContent(this);
    adapterPtr->setFilteredIndices(&m_filteredIndices);
    adapterPtr->setCatalog(&m_catalog);
    adapterPtr->setSelectedIds(&m_selectedIds);
    m_adapter = std::move(adapter);

    // Intrinsic height of the same card layout as built-in templates (padding + checkbox/text).
    auto probe = std::make_unique<TemplateStoreTile>(scale);
    probe->bind("Mg", "category", false, false, false, {});
    const float cardHeight = std::ceil(probe->measure(renderer, LayoutConstraints{}).height);
    // Floor above checkbox+tight padding — two caption lines need more than controlHeightSm alone.
    const float minCardHeight = (Style::controlHeightSm + Style::spaceSm * 2.0F) * scale;
    auto grid = ui::virtualGridView({
        .out = &m_grid,
        .minCellWidth = 152.0F * scale,
        .cellHeight = std::max(cardHeight, minCardHeight),
        .squareCells = false,
        .columnGap = Style::spaceSm * scale,
        .rowGap = Style::spaceSm * scale,
        .adapter = adapterPtr,
        .flexGrow = 1.0F,
        .onSelectionChanged =
            [this](std::optional<std::size_t> index) {
              m_selectedTemplateId = index.has_value() && *index < m_filteredIndices.size()
                  ? std::optional{m_catalog[m_filteredIndices[*index]].id}
                  : std::nullopt;
            },
        .configure = [](VirtualGridView& view) { view.setFillWidth(true); },
    });
    if (const auto index = indexOfTemplateId(m_selectedTemplateId.value_or("")); index.has_value()) {
      m_grid->setSelectedIndex(index);
    }
    body.addChild(std::move(grid));

    if (m_filteredIndices.empty()) {
      body.addChild(
          ui::label({
              .text = m_catalog.empty() ? i18n::tr("settings.schema.templates.community-ids.empty")
                                        : i18n::tr("settings.templates.store.empty"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
  }

  std::optional<std::size_t> TemplateStoreContent::indexOfTemplateId(std::string_view id) const {
    if (id.empty()) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < m_filteredIndices.size(); ++i) {
      if (m_catalog[m_filteredIndices[i]].id == id) {
        return i;
      }
    }
    return std::nullopt;
  }

  void TemplateStoreContent::syncGridSelection() {
    if (m_grid == nullptr) {
      return;
    }
    if (const auto index = indexOfTemplateId(m_selectedTemplateId.value_or("")); index.has_value()) {
      m_grid->setSelectedIndex(index);
    } else {
      m_grid->setSelectedIndex(std::nullopt);
    }
  }

  void TemplateStoreContent::selectIndex(std::size_t index) {
    if (index >= m_filteredIndices.size() || m_grid == nullptr) {
      return;
    }
    m_selectedTemplateId = m_catalog[m_filteredIndices[index]].id;
    m_grid->setSelectedIndex(index);
    m_grid->scrollToIndex(index);
  }

  void TemplateStoreContent::moveSelection(int delta) {
    if (m_filteredIndices.empty()) {
      return;
    }
    const auto current = indexOfTemplateId(m_selectedTemplateId.value_or(""));
    if (!current.has_value()) {
      selectIndex(0);
      return;
    }
    const int next = std::clamp(static_cast<int>(*current) + delta, 0, static_cast<int>(m_filteredIndices.size()) - 1);
    selectIndex(static_cast<std::size_t>(next));
  }

  bool TemplateStoreContent::activateSelection() {
    const auto index = indexOfTemplateId(m_selectedTemplateId.value_or(""));
    if (!index.has_value()) {
      return false;
    }
    toggleAtFilteredIndex(*index);
    return true;
  }

  bool TemplateStoreContent::handleKeyEvent(
      std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit, InputArea* focused
  ) {
    if (!pressed || preedit) {
      return false;
    }

    const InputArea* gridFocus = m_grid != nullptr ? m_grid->focusArea() : nullptr;
    if (focused != nullptr && focused != gridFocus) {
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

} // namespace settings
