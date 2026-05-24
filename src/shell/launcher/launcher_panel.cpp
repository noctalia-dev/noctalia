#include "shell/launcher/launcher_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/key_symbols.h"
#include "core/keybind_matcher.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "ui/builders.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/fuzzy_match.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>

namespace {

  constexpr std::size_t kMaxResults = 50;
  constexpr std::size_t kRowOverscan = 3;
  constexpr float kIconSize = 40.0f;
  constexpr double kUsageScorePerCount = 0.1;
  constexpr double kTypedUsageScoreCap = 0.5;

  double usageBoostForScore(double score, int usageCount, bool typedQuery) {
    if (usageCount <= 0) {
      return 0.0;
    }

    const double rawBoost = static_cast<double>(usageCount) * kUsageScorePerCount;
    if (!typedQuery) {
      return rawBoost;
    }
    if (!FuzzyMatch::isMatch(score)) {
      return 0.0;
    }

    // For typed searches, usage should nudge close matches without letting a
    // weak fuzzy hit outrank a much stronger lexical match.
    return std::min(rawBoost, kTypedUsageScoreCap);
  }

  float launcherRowHeight(float scale) {
    const float paddingY = Style::spaceXs * scale;
    const float textGap = Style::spaceXs * scale;
    const float titleHeight = Style::fontSizeBody * scale * 1.35f;
    const float subtitleHeight = Style::fontSizeCaption * scale * 1.25f;
    const float textHeight = titleHeight + textGap + subtitleHeight;
    return std::ceil(std::max(kIconSize * scale, textHeight) + paddingY * 2.0f);
  }

  class LauncherResultRow final : public Node {
  public:
    LauncherResultRow(float scale, AsyncTextureCache* asyncTextures)
        : m_scale(scale), m_rowHeight(launcherRowHeight(scale)), m_asyncTextures(asyncTextures) {
      auto row = ui::row(
          {.out = &m_row, .align = FlexAlign::Center, .gap = Style::spaceMd * scale, .configure = [scale](Flex& flex) {
             flex.setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
             flex.setRadius(Style::scaledRadiusMd(scale));
           }}
      );
      addChild(std::move(row));

      m_row->addChild(
          ui::label({
              .out = &m_actionLabel,
              .fontSize = kIconSize * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::image({
              .out = &m_image,
              .width = kIconSize * scale,
              .height = kIconSize * scale,
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = kIconSize * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this]() {
        if (m_actionTextVisible || m_iconPath.empty() || m_image == nullptr || m_glyph == nullptr ||
            !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_row->addChild(
          ui::column(
              {
                  .out = &m_textCol,
                  .align = FlexAlign::Start,
                  .gap = Style::spaceXs * 0.5f * scale,
                  .flexGrow = 1.0f,
              },
              ui::label({
                  .out = &m_title,
                  .fontSize = Style::fontSizeBody * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
                  .fontWeight = FontWeight::Bold,
              }),
              ui::label({
                  .out = &m_subtitle,
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .configure = [](Label& label) { label.setCaptionStyle(); },
              })
          )
      );
    }

    void bind(Renderer& renderer, const LauncherResult& result, float width, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      m_iconTargetSize = static_cast<int>(std::round(kIconSize * m_scale));
      m_actionTextVisible = !result.actionText.empty();

      setSize(width, m_rowHeight);
      m_row->setFrameSize(width, m_rowHeight);

      m_actionLabel->setVisible(false);
      m_image->setVisible(false);
      m_glyph->setVisible(false);

      if (m_actionTextVisible) {
        m_actionLabel->setText(result.actionText);
        m_actionLabel->setSize(kIconSize * m_scale, kIconSize * m_scale);
        m_actionLabel->setVisible(true);
        m_image->clear(renderer);
      } else if (!m_iconPath.empty()) {
        const bool ready = refreshAsyncIcon(renderer);
        m_image->setVisible(ready);
        m_glyph->setGlyph(m_fallbackGlyph);
        m_glyph->setVisible(!ready);
      } else {
        m_image->clear(renderer);
        m_glyph->setGlyph(m_fallbackGlyph);
        m_glyph->setVisible(true);
      }

      const float textWidth =
          std::max(0.0f, width - kIconSize * m_scale - Style::spaceSm * m_scale * 2.0f - Style::spaceMd * m_scale);
      m_title->setText(result.title);
      m_title->setMaxWidth(textWidth);

      if (result.subtitle.empty()) {
        m_subtitle->setVisible(false);
        m_subtitle->setText("");
      } else {
        m_subtitle->setVisible(true);
        m_subtitle->setText(result.subtitle);
        m_subtitle->setMaxWidth(textWidth);
      }

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (m_actionTextVisible || m_iconPath.empty()) {
        return false;
      }

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      m_image->setSize(kIconSize * m_scale, kIconSize * m_scale);
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (!m_actionTextVisible && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_selected) {
        m_row->setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
      } else if (m_hovered) {
        m_row->setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45f));
      } else {
        m_row->setFill(rgba(0, 0, 0, 0));
      }
    }

    float m_scale = 1.0f;
    float m_rowHeight = 0.0f;
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_row = nullptr;
    Label* m_actionLabel = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Flex* m_textCol = nullptr;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_actionTextVisible = false;
  };

} // namespace

class LauncherResultAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

  LauncherResultAdapter(float scale, AsyncTextureCache* cache) : m_scale(scale), m_cache(cache) {}

  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0u : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherResultRow>(m_scale, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* row = static_cast<LauncherResultRow*>(&tile);
    row->bind(*m_renderer, (*m_results)[index], tile.width(), selected, hovered);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  float m_scale;
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
};

LauncherPanel::LauncherPanel(ConfigService* config, AsyncTextureCache* asyncTextures)
    : m_config(config), m_asyncTextures(asyncTextures) {}

LauncherPanel::~LauncherPanel() = default;

PanelPlacement LauncherPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.launcherPlacement : PanelPlacement::Centered;
}

void LauncherPanel::addProvider(std::unique_ptr<LauncherProvider> provider) {
  provider->initialize();
  m_providers.push_back(std::move(provider));
}

void LauncherPanel::create() {
  const float scale = contentScale();
  auto container = ui::column({
      .out = &m_container,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  container->addChild(
      ui::input({
          .out = &m_input,
          .placeholder = i18n::tr("launcher.search-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceMd * scale,
          .clearButtonEnabled = true,
          .onChange = [this](const std::string& text) { onInputChanged(text); },
          .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
          .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
      })
  );

  container->addChild(
      ui::segmented({
          .out = &m_categoryFilter,
          .scale = scale,
          .compact = true,
          .equalSegmentWidths = true,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Segmented& segmented) { segmented.setAlign(FlexAlign::Center); },
      })
  );

  auto body = ui::column({
      .out = &m_body,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .flexGrow = 1.0f,
  });

  m_adapter = std::make_unique<LauncherResultAdapter>(scale, m_asyncTextures);
  m_adapter->setResults(&m_results);
  m_adapter->setOnActivate([this](std::size_t index) { activateAt(index); });
  m_adapter->setOnSecondaryActivate([this](std::size_t index, float ax, float ay) {
    openAppActionsMenu(index, ax, ay);
  });

  body->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .columns = 1,
          .cellHeight = launcherRowHeight(scale),
          .squareCells = false,
          .columnGap = 0.0f,
          .rowGap = 0.0f,
          .overscanRows = kRowOverscan,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0f,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (idx.has_value() && *idx < m_results.size()) {
                  m_selectedIndex = *idx;
                }
              },
          .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
      })
  );

  body->addChild(
      ui::label({
          .out = &m_emptyLabel,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Label& label) { label.setCaptionStyle(); },
      })
  );

  container->addChild(std::move(body));

  setRoot(std::move(container));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void LauncherPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr || m_input == nullptr) {
    return;
  }

  if (m_adapter != nullptr) {
    m_adapter->setRenderer(&renderer);
  }

  m_container->setSize(width, height);
  m_container->layout(renderer);
}

void LauncherPanel::onOpen(std::string_view context) {
  m_categoryFilterVisible = m_config != nullptr && m_config->config().shell.panel.launcherCategories;
  m_activeCategory.clear();
  m_currentCategories.clear();
  if (m_categoryFilter != nullptr) {
    m_categoryFilter->clearOptions();
    m_categoryFilter->setVisible(false);
    m_categoryFilter->setParticipatesInLayout(false);
  }

  const std::string initialValue(context);
  if (m_input != nullptr) {
    m_input->setValue(initialValue);
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0f);
  }
  onInputChanged(initialValue);
}

void LauncherPanel::onClose() {
  if (m_actionsMenu != nullptr && m_actionsMenu->isOpen()) {
    m_actionsMenu->close();
  }

  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures]() { asyncTextures->trimUnused(0); });
  }

  m_query.clear();
  m_results.clear();
  m_allResults.clear();
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_selectedIndex = 0;

  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  m_adapter.reset();

  // The scene tree (and all nodes) is destroyed by PanelManager after onClose().
  m_container = nullptr;
  m_input = nullptr;
  m_categoryFilter = nullptr;
  m_body = nullptr;
  m_grid = nullptr;
  m_emptyLabel = nullptr;
  clearReleasedRoot();
}

void LauncherPanel::onIconThemeChanged() {
  std::string selectedProvider;
  std::string selectedId;
  if (m_selectedIndex < m_results.size()) {
    selectedProvider = m_results[m_selectedIndex].providerName;
    selectedId = m_results[m_selectedIndex].id;
  }

  onInputChanged(m_query);

  if (!selectedId.empty()) {
    for (std::size_t i = 0; i < m_results.size(); ++i) {
      if (m_results[i].providerName == selectedProvider && m_results[i].id == selectedId) {
        m_selectedIndex = i;
        break;
      }
    }
  }
  refreshResults();
}

InputArea* LauncherPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

void LauncherPanel::onInputChanged(const std::string& text) {
  m_query = text;
  m_allResults.clear();

  // Route query to providers
  LauncherProvider* activeProvider = nullptr;
  std::string_view queryText = text;

  // Check for prefix match (longest first)
  for (auto& provider : m_providers) {
    auto prefix = provider->prefix();
    if (prefix.empty()) {
      continue;
    }
    if (text.size() >= prefix.size() && std::string_view(text).substr(0, prefix.size()) == prefix) {
      activeProvider = provider.get();
      queryText = std::string_view(text).substr(prefix.size());
      // Trim leading space after prefix
      if (!queryText.empty() && queryText.front() == ' ') {
        queryText = queryText.substr(1);
      }
      break;
    }
  }

  const bool typedQuery = !queryText.empty();

  auto applyUsageBoost = [&](std::vector<LauncherResult>& results, const LauncherProvider& provider) {
    if (!provider.trackUsage()) {
      return;
    }
    for (auto& result : results) {
      const int usageCount = m_usageTracker.getCount(provider.name(), result.id);
      result.score += usageBoostForScore(result.score, usageCount, typedQuery);
    }
  };

  std::vector<LauncherCategory> newCategories;

  if (activeProvider != nullptr) {
    m_allResults = activeProvider->query(queryText);
    applyUsageBoost(m_allResults, *activeProvider);
    for (auto& result : m_allResults) {
      result.providerName = activeProvider->name();
    }
    newCategories = activeProvider->categories();
  } else {
    // Query default providers (empty prefix)
    for (auto& provider : m_providers) {
      if (provider->prefix().empty()) {
        auto results = provider->query(queryText);
        applyUsageBoost(results, *provider);
        for (auto& result : results) {
          result.providerName = provider->name();
        }
        m_allResults.insert(
            m_allResults.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end())
        );
        auto providerCats = provider->categories();
        for (auto& cat : providerCats) {
          newCategories.push_back(std::move(cat));
        }
      }
    }
    // Stable sort by score descending — preserves provider order (e.g. alphabetical) for ties
    std::stable_sort(m_allResults.begin(), m_allResults.end(), [](const LauncherResult& a, const LauncherResult& b) {
      return a.score > b.score;
    });
  }

  const int iconTargetSize = static_cast<int>(std::round(kIconSize * contentScale()));
  for (auto& result : m_allResults) {
    if (result.iconPath.empty() && !result.iconName.empty()) {
      const std::string& resolved = m_iconResolver.resolve(result.iconName, iconTargetSize);
      if (!resolved.empty()) {
        result.iconPath = resolved;
      } else if (result.iconName != "application-x-executable") {
        const std::string& fallback = m_iconResolver.resolve("application-x-executable", iconTargetSize);
        if (!fallback.empty()) {
          result.iconPath = fallback;
        }
      }
      result.iconName.clear();
    }
  }

  bool categoriesChanged = newCategories.size() != m_currentCategories.size();
  if (!categoriesChanged) {
    for (std::size_t i = 0; i < newCategories.size(); ++i) {
      if (newCategories[i].label != m_currentCategories[i].label) {
        categoriesChanged = true;
        break;
      }
    }
  }
  if (categoriesChanged) {
    m_activeCategory.clear();
    rebuildCategoryFilter(newCategories);
  }

  applyActiveCategory();
}

void LauncherPanel::rebuildCategoryFilter(const std::vector<LauncherCategory>& categories) {
  m_currentCategories = categories;
  if (m_categoryFilter == nullptr) {
    return;
  }
  m_categoryFilter->clearOptions();
  if (categories.empty()) {
    setCategoryFilterVisible(false);
    return;
  }
  m_categoryFilter->addOption("", "layout-grid");
  m_categoryFilter->setOptionTooltip(0, i18n::tr("launcher.categories.all"));
  for (std::size_t i = 0; i < categories.size(); ++i) {
    m_categoryFilter->addOption("", categories[i].glyphName);
    m_categoryFilter->setOptionTooltip(i + 1, categories[i].label);
  }
  m_categoryFilter->setSelectedIndex(0);
  m_categoryFilter->setOnChange([this](std::size_t idx) {
    if (idx == 0) {
      m_activeCategory.clear();
    } else if (idx - 1 < m_currentCategories.size()) {
      m_activeCategory = m_currentCategories[idx - 1].label;
    }
    applyActiveCategory();
  });
  setCategoryFilterVisible(m_categoryFilterVisible);
}

void LauncherPanel::setCategoryFilterVisible(bool visible) {
  if (m_categoryFilter == nullptr) {
    return;
  }
  const bool show = visible && !m_currentCategories.empty();
  m_categoryFilter->setVisible(show);
  m_categoryFilter->setParticipatesInLayout(show);
  if (m_container != nullptr) {
    m_container->markLayoutDirty();
  }
}

void LauncherPanel::applyActiveCategory() {
  m_results.clear();
  if (m_activeCategory.empty()) {
    m_results = m_allResults;
  } else {
    for (const auto& r : m_allResults) {
      if (r.category == m_activeCategory) {
        m_results.push_back(r);
      }
    }
  }
  if (!m_query.empty() && m_results.size() > kMaxResults) {
    m_results.resize(kMaxResults);
  }
  m_selectedIndex = 0;
  refreshResults();
}

void LauncherPanel::refreshResults() {
  uiAssertNotRendering("LauncherPanel::refreshResults");
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }

  m_grid->notifyDataChanged();
  if (m_results.empty()) {
    m_grid->setSelectedIndex(std::nullopt);
    m_grid->scrollView().setScrollOffset(0.0f);
  } else {
    m_grid->setSelectedIndex(m_selectedIndex);
  }
  applyEmptyState();
}

void LauncherPanel::applyEmptyState() {
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }
  const bool empty = m_results.empty();
  m_grid->setVisible(!empty);
  m_grid->setParticipatesInLayout(!empty);
  m_emptyLabel->setVisible(empty);
  m_emptyLabel->setParticipatesInLayout(empty);
  if (empty) {
    m_emptyLabel->setText(
        m_query.empty() ? i18n::tr("launcher.empty.type-to-search") : i18n::tr("launcher.empty.no-results")
    );
  }
}

void LauncherPanel::openAppActionsMenu(std::size_t index, float anchorX, float anchorY) {
  if (index >= m_results.size()) {
    return;
  }
  const LauncherResult& base = m_results[index];

  const DesktopEntry* match = nullptr;
  for (const auto& e : desktopEntries()) {
    if (e.path == base.id) {
      match = &e;
      break;
    }
  }
  if (match == nullptr || match->actions.empty()) {
    return;
  }

  WaylandConnection* wl = PanelManager::instance().wayland();
  RenderContext* rc = PanelManager::instance().renderContext();
  if (wl == nullptr || rc == nullptr) {
    return;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return;
  }

  if (m_actionsMenu == nullptr) {
    m_actionsMenu = std::make_unique<ContextMenuPopup>(*wl, *rc);
  }

  std::vector<DesktopAction> actionsCopy = match->actions;

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(actionsCopy.size() + 1);
  entries.push_back(
      ContextMenuControlEntry{
          .id = -1,
          .label = i18n::tr("launcher.context-menu.open"),
          .enabled = true,
          .separator = false,
          .hasSubmenu = false,
      }
  );
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(actionsCopy.size()); ++i) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = i,
            .label = actionsCopy[static_cast<std::size_t>(i)].name,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }

  const float scale = contentScale();
  constexpr float kMenuWidth = 240.0f;
  const float menuWidth = kMenuWidth * scale;

  if (m_config != nullptr) {
    m_actionsMenu->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_actionsMenu.get());

  m_actionsMenu->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  m_actionsMenu->setOnActivate([this, base,
                                actionsCopy = std::move(actionsCopy)](const ContextMenuControlEntry& entry) {
    LauncherResult result = base;
    result.desktopActionId.clear();
    if (entry.id >= 0 && entry.id < static_cast<std::int32_t>(actionsCopy.size())) {
      result.desktopActionId = actionsCopy[static_cast<std::size_t>(entry.id)].id;
    } else if (entry.id != -1) {
      return;
    }

    for (auto& provider : m_providers) {
      if (provider->name() != std::string_view(result.providerName)) {
        continue;
      }
      if (!provider->activate(result)) {
        return;
      }
      if (provider->trackUsage()) {
        m_usageTracker.record(provider->name(), result.id);
      }
      PanelManager::instance().closePanel();
      return;
    }
  });

  const float inset = std::round(std::max(4.0f, Style::spaceXs * scale));
  const std::int32_t ax = static_cast<std::int32_t>(std::round(anchorX - inset));
  const std::int32_t ay = static_cast<std::int32_t>(std::round(anchorY - inset));
  const std::int32_t aw = static_cast<std::int32_t>(std::round(inset * 2.0f));
  const std::int32_t ah = static_cast<std::int32_t>(std::round(inset * 2.0f));

  m_actionsMenu->open(
      std::move(entries), menuWidth, 12, ax, ay, std::max(1, aw), std::max(1, ah), parentCtx->layerSurface,
      parentCtx->output
  );
}

void LauncherPanel::activateAt(std::size_t index) {
  if (index >= m_results.size()) {
    return;
  }
  m_selectedIndex = index;
  activateSelected();
}

void LauncherPanel::activateSelected() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }

  const auto& result = m_results[m_selectedIndex];

  // Dispatch only to the provider that produced this result. Providers can use
  // overlapping id shapes, so probing every provider risks side effects.
  for (auto& provider : m_providers) {
    if (provider->name() != std::string_view(result.providerName)) {
      continue;
    }

    if (!provider->activate(result)) {
      return;
    }

    if (provider->trackUsage()) {
      m_usageTracker.record(provider->name(), result.id);
    }
    PanelManager::instance().closePanel();
    return;
  }
}

bool LauncherPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  const auto moveSelection = [this](int delta) {
    if (m_results.empty()) {
      return;
    }
    const int last = static_cast<int>(m_results.size() - 1);
    const int next = std::clamp(static_cast<int>(m_selectedIndex) + delta, 0, last);
    if (next == static_cast<int>(m_selectedIndex)) {
      return;
    }
    m_selectedIndex = static_cast<std::size_t>(next);
    if (m_grid != nullptr) {
      m_grid->setSelectedIndex(m_selectedIndex);
    }
  };

  if (KeySymbol::isTab(sym) && !m_currentCategories.empty()) {
    m_categoryFilterVisible = !m_categoryFilterVisible;
    setCategoryFilterVisible(m_categoryFilterVisible);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (m_categoryFilter != nullptr && m_categoryFilter->visible() && m_categoryFilter->selectedIndex() > 0) {
      m_categoryFilter->setSelectedIndex(m_categoryFilter->selectedIndex() - 1);
      return true;
    }
    return false;
  }

  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (m_categoryFilter != nullptr && m_categoryFilter->visible()) {
      const std::size_t next = m_categoryFilter->selectedIndex() + 1;
      const std::size_t total = m_currentCategories.size() + 1;
      if (next < total) {
        m_categoryFilter->setSelectedIndex(next);
        return true;
      }
    }
    return false;
  }

  if (KeySymbol::isPageUp(sym)) {
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(-stride);
    return true;
  }

  if (KeySymbol::isPageDown(sym)) {
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(stride);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    moveSelection(-1);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    moveSelection(1);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}
