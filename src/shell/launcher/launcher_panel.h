#pragma once

#include "launcher/launcher_provider.h"
#include "launcher/usage_tracker.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ContextMenuPopup;
class AccountsService;
class Button;
class Flex;
class Glyph;
class Image;
class Input;
class Label;
class LauncherResultAdapter;
class LauncherAppGridAdapter;
class LauncherSectionsAdapter;
class Renderer;
class Segmented;
class ScrollView;
class VirtualGridView;
class VirtualListView;
class ConfigService;
class AsyncTextureCache;

class LauncherPanel : public Panel {
public:
  LauncherPanel(ConfigService* config, AccountsService* accounts, AsyncTextureCache* asyncTextures);
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
  void syncUsageTrackingState();

  // Invoked after a terminal close when the activation copied text and the provider
  // supports auto-paste. The host schedules virtual-keyboard paste (clipboard path).
  void setCopiedActivationCallback(std::function<void()> callback) { m_onCopiedActivation = std::move(callback); }

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  enum ActiveCategoryType { All, Pinned, RecentlyUsed, Category };
  enum class ResultsTransition : std::uint8_t { None, Search, Navigation };

  struct CategoryFilterSlot {
    ActiveCategoryType type;
    std::size_t categoryIndex = 0;
  };

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
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
  [[nodiscard]] bool startsWithLauncherPrefix(std::string_view text) const;
  void applyProviderConfig(LauncherProvider& provider) const;
  void finishActivation(LauncherProvider& provider, const std::string& resultId, bool copied);
  [[nodiscard]] std::vector<LauncherResult> providerOverviewResults(std::string_view text) const;
  [[nodiscard]] bool openAppActionsMenu(std::size_t index, float anchorX, float anchorY);
  void updateCategoryFilterModel(const std::vector<LauncherCategory>& categories);
  void rebuildCategoryFilter();
  void rebuildLauncherNavigation();
  void updateCategorySelectionChrome();
  void updateLauncherSectionTitle();
  void setCategoryFilterVisible(bool visible);
  void setActiveCategorySlot(std::size_t slotIndex);
  void applyActiveCategory();
  void syncLauncherListStyle();
  void syncLauncherViewLayout(Renderer* renderer = nullptr);
  void queueResultsTransition(ResultsTransition transition);
  void animateResultsTransition();
  [[nodiscard]] bool shouldUseAppGrid() const;
  [[nodiscard]] bool shouldUseLauncherSections() const;
  void rebuildLauncherSections();
  void selectResult(std::size_t index, bool scrollToSelection);
  void refreshLauncherAppIconColorization();
  void updateLauncherGridMetrics(Renderer& renderer);
  void updatePinnedApplicationState();
  void applyPinnedApplicationOrder();
  void reorderPinnedApplication(std::string_view sourcePath, std::string_view targetPath);
  void syncLauncherAvatar(Renderer& renderer);
  void showProviderOverview();
  void showSessionActions();
  [[nodiscard]] bool usesBrowserLayout() const;
  [[nodiscard]] bool shouldTrackUsage() const;

  std::vector<std::unique_ptr<LauncherProvider>> m_providers;
  std::vector<LauncherResult> m_results;
  std::vector<LauncherResult> m_allResults;
  UsageTracker m_usageTracker;
  IconResolver m_iconResolver;

  Flex* m_container = nullptr;
  Input* m_input = nullptr;
  Segmented* m_categoryFilter = nullptr;
  Flex* m_body = nullptr;
  Flex* m_resultsViewport = nullptr;
  Flex* m_launcherToolbar = nullptr;
  Flex* m_launcherNavigation = nullptr;
  Label* m_launcherSectionTitle = nullptr;
  Image* m_launcherAvatar = nullptr;
  Glyph* m_launcherAvatarFallback = nullptr;
  Button* m_launcherSettingsButton = nullptr;
  Button* m_launcherPowerButton = nullptr;
  Button* m_launcherProvidersButton = nullptr;
  std::vector<Button*> m_launcherCategoryButtons;
  VirtualGridView* m_grid = nullptr;
  VirtualListView* m_launcherSections = nullptr;
  ScrollView* m_detailScroll = nullptr;
  Label* m_detailSubtitle = nullptr;
  Label* m_detailBody = nullptr;
  Label* m_emptyLabel = nullptr;
  bool m_anyProviderLoading = false;
  std::unique_ptr<LauncherResultAdapter> m_listAdapter;
  std::unique_ptr<LauncherAppGridAdapter> m_gridAdapter;
  std::unique_ptr<LauncherSectionsAdapter> m_launcherSectionsAdapter;

  std::string m_query;
  std::string m_scopedProviderId;
  std::string m_scopedPlaceholder;
  ActiveCategoryType m_activeCategoryType = All;
  std::string m_activeCategory;
  std::vector<LauncherCategory> m_currentCategories;
  std::vector<CategoryFilterSlot> m_categoryFilterSlots;
  bool m_hasRecentlyUsed = false;
  bool m_hasPinnedApplications = false;
  bool m_categoryFilterRebuildPending = false;
  std::size_t m_selectedIndex = 0;
  std::size_t m_activeCategorySlotIndex = 0;
  bool m_categoryFilterVisible = true;
  bool m_browserMode = false;
  bool m_showingProviderOverview = false;
  std::string m_launcherProviderViewId;
  bool m_launcherShowIcons = true;
  bool m_launcherShowAppOriginIndicator = true;
  bool m_launcherCompact = false;
  bool m_launcherAppGrid = false;
  bool m_usingAppGrid = false;
  ResultsTransition m_pendingResultsTransition = ResultsTransition::None;
  float m_launcherRowHeight = 0.0F;
  std::uint64_t m_desktopEntriesVersion = 0;
  ConfigService* m_config = nullptr;
  AccountsService* m_accounts = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::string m_launcherAvatarPath;
  std::unique_ptr<ContextMenuPopup> m_actionsMenu;
  Signal<>::ScopedConnection m_appIconColorizeConn;
  std::function<void()> m_onCopiedActivation;
};
