#pragma once

#include "launcher/launcher_provider.h"
#include "launcher/usage_tracker.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class ContextMenuPopup;
class Flex;
class Input;
class Label;
class LauncherResultAdapter;
class LauncherAppGridAdapter;
class Renderer;
class Segmented;
class ScrollView;
class VirtualGridView;
class ConfigService;
class AsyncTextureCache;

class LauncherPanel : public Panel {
public:
  LauncherPanel(ConfigService* config, AsyncTextureCache* asyncTextures);
  ~LauncherPanel() override;

  void addProvider(std::unique_ptr<LauncherProvider> provider);
  // Drop every dynamically-registered (plugin-backed) provider, so the enabled
  // plugin set can be re-applied without disturbing the built-in providers.
  void clearDynamicProviders();
  // Drop providers whose stable id starts with `prefix` (e.g. config-driven "dmenu.").
  void clearProvidersWithIdPrefix(std::string_view prefix);
  // Restrict the next open to a single provider (stdin/dmenu session). When set,
  // onInputChanged queries only that provider and skips prefix routing/overview.
  // Cleared on close.
  void setScopedProvider(std::string_view providerId, std::string_view placeholder = {});

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onIconThemeChanged() override;

  void clearUsage();

  [[nodiscard]] float preferredWidth() const override { return scaled(560.0f); }
  [[nodiscard]] float preferredHeight() const override { return scaled(500.0f); }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  enum ActiveCategoryType { All, RecentlyUsed, Category };

  struct CategoryFilterSlot {
    ActiveCategoryType type;
    std::size_t categoryIndex = 0;
  };

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void onInputChanged(const std::string& text);
  void setQuery(std::string query);
  // Re-gather the current query, preserving the selected result by identity.
  void reapplyCurrentQuery();
  // A plugin provider delivered fresh async results — re-gather if the panel is open.
  void onProviderResultsChanged();
  void refreshResults();
  void activateAt(std::size_t index);
  void activateSelected();
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void applyEmptyState();
  void bindDetailResult();
  [[nodiscard]] bool shouldUseDetailPresentation() const;
  [[nodiscard]] std::vector<LauncherResult> providerOverviewResults(std::string_view text) const;
  void openAppActionsMenu(std::size_t index, float anchorX, float anchorY);
  void rebuildCategoryFilter(const std::vector<LauncherCategory>& categories);
  void setCategoryFilterVisible(bool visible);
  void setActiveCategorySlot(std::size_t slotIndex);
  void applyActiveCategory();
  void syncLauncherListStyle();
  void syncLauncherViewLayout(Renderer* renderer = nullptr);
  [[nodiscard]] bool shouldUseAppGrid() const;
  void refreshLauncherAppIconColorization();
  void updateLauncherGridMetrics(Renderer& renderer);

  std::vector<std::unique_ptr<LauncherProvider>> m_providers;
  std::vector<LauncherResult> m_results;
  std::vector<LauncherResult> m_allResults;
  UsageTracker m_usageTracker;
  IconResolver m_iconResolver;

  Flex* m_container = nullptr;
  Input* m_input = nullptr;
  Segmented* m_categoryFilter = nullptr;
  Flex* m_body = nullptr;
  VirtualGridView* m_grid = nullptr;
  ScrollView* m_detailScroll = nullptr;
  Label* m_detailSubtitle = nullptr;
  Label* m_detailBody = nullptr;
  Label* m_emptyLabel = nullptr;
  std::unique_ptr<LauncherResultAdapter> m_listAdapter;
  std::unique_ptr<LauncherAppGridAdapter> m_gridAdapter;

  std::string m_query;
  std::string m_scopedProviderId;
  std::string m_scopedPlaceholder;
  ActiveCategoryType m_activeCategoryType = All;
  std::string m_activeCategory;
  std::vector<LauncherCategory> m_currentCategories;
  std::vector<CategoryFilterSlot> m_categoryFilterSlots;
  bool m_hasRecentlyUsed = false;
  std::size_t m_selectedIndex = 0;
  bool m_categoryFilterVisible = true;
  bool m_launcherShowIcons = true;
  bool m_launcherCompact = false;
  bool m_launcherAppGrid = false;
  bool m_usingAppGrid = false;
  float m_launcherRowHeight = 0.0f;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::unique_ptr<ContextMenuPopup> m_actionsMenu;
  Signal<>::ScopedConnection m_appIconColorizeConn;
};
