#pragma once

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "scripting/plugin_file_cache.h"
#include "scripting/plugin_manager.h"
#include "shell/settings/config_export_dialog_popup.h"
#include "shell/settings/search_picker_popup.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_sheet_popup.h"
#include "shell/settings/widget_add_popup.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/toplevel_surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Box;
class Button;
class AccountsService;
class ConfigService;
class CompositorPlatform;
class DependencyService;
class Flex;
class IdleManager;
class Input;
class Label;
class RenderContext;
class UPowerService;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;

namespace settings {
  struct SettingsContentContext;
} // namespace settings

// Standalone xdg-toplevel settings UI (same binary as the shell; shares RenderContext).
class SettingsWindow {
public:
  ~SettingsWindow();

  void initialize(
      WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, DependencyService* dependencies,
      UPowerService* upower, IdleManager* idleManager, CompositorPlatform* platform, AccountsService* accounts = nullptr
  );

  void open(std::string context = "");
  void openToBarWidget(std::string barName, std::string widgetName);
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }
  [[nodiscard]] wl_surface* wlSurface() const noexcept {
    return m_surface != nullptr ? m_surface->wlSurface() : nullptr;
  }
  [[nodiscard]] bool ownsKeyboardSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const;

  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void onThemeChanged();
  void onFontChanged();
  void requestRedraw();
  void onExternalOptionsChanged();
  void onPluginsChanged();
  void setOpenDesktopWidgetEditor(std::function<void()> callback) { m_openDesktopWidgetEditor = std::move(callback); }
  void setOpenLockscreenWidgetEditor(std::function<void()> callback) {
    m_openLockscreenWidgetEditor = std::move(callback);
  }
  void setOpenWallpaperPanel(std::function<void()> callback) { m_openWallpaperPanel = std::move(callback); }
  void setPluginManager(scripting::PluginManager* manager) { m_pluginManager = manager; }
  void setSyncGreeterAppearance(std::function<void()> callback) { m_syncGreeterAppearance = std::move(callback); }
  void setResetLauncherUsage(std::function<void()> callback) { m_resetLauncherUsage = std::move(callback); }
  void setResetScreenTime(std::function<void()> callback) { m_resetScreenTime = std::move(callback); }
  void setSaveWallpaperPaletteAsCustom(std::function<void()> callback) {
    m_saveWallpaperPaletteAsCustom = std::move(callback);
  }
  void setConnectCalendarAccount(std::function<void(std::string, std::string)> callback) {
    m_connectCalendarAccount = std::move(callback);
  }

  void onSecondTick();
  void onIdleLiveStatusChanged();
  void markSettingsWriteSuccess(bool requestRebuild = true);
  void markSettingsWriteError(std::string message);
  void showTransientStatus(std::string message, bool isError = false);

private:
  void destroyWindow();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(std::uint32_t width, std::uint32_t height);
  void rebuildSettingsContent();
  [[nodiscard]] settings::RegistryEnvironment buildRegistryEnvironment() const;
  void refreshSettingsRegistry(const Config& cfg);
  void syncSelectedBarState(const Config& cfg, const std::vector<std::string>& availableBars);
  [[nodiscard]] std::unique_ptr<Flex> buildHeaderRow(float scale);
  [[nodiscard]] std::unique_ptr<Flex>
  buildFilterRow(float scale, const std::string& resetPageScope, std::vector<std::vector<std::string>> resetPagePaths);
  [[nodiscard]] std::unique_ptr<Flex> buildStatusRow(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildBody(
      float scale, const Config& cfg, const std::vector<settings::SettingsSection>& sections,
      const std::vector<std::string>& availableBars
  );
  [[nodiscard]] std::vector<settings::SelectOption> batteryDeviceOptions() const;
  [[nodiscard]] settings::SettingsContentContext makeContentContext(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride
  );
  [[nodiscard]] std::vector<std::vector<std::string>> currentPageResetPaths() const;
  [[nodiscard]] bool
  tryPatchSettingsRegistryValue(const std::vector<std::string>& path, const ConfigOverrideValue& value);
  [[nodiscard]] bool tryPatchSettingsRegistryOverrides(
      const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides
  );
  [[nodiscard]] bool tryPatchSettingsRegistryResetValues(const std::vector<std::vector<std::string>>& paths);
  void rebuildFilterRow(float scale);
  void requestSceneRebuild();
  void
  requestContentRebuild(bool refreshRegistry = false, bool refreshFilterRow = false, bool rebuildEditorSheet = false);
  void markPluginListDirty();
  void refreshPluginListIfNeeded();
  void maybeOpenPendingWidgetInspector();
  void applyPendingContentScrollTarget(float margin);
  void scrollFocusedAreaIntoView(class InputArea* area);
  void scrollSidebarNodeIntoView(const Node* node);
  void clearStatusMessage();
  void clearTransientSettingsState();
  void finishSettingsWrite(
      bool changed, bool forceSceneRebuild, bool pageResetPathsChanged, bool registryAlreadyCurrent,
      bool rebuildWhenUnchanged = false
  );
  void openActionsMenu();
  void openConfigExportDialog();
  void openBarWidgetAddPopup(const std::vector<std::string>& lanePath);
  // Request is taken by value because opening the popup can close the sheet that owns the forwarding control.
  void openSearchPickerPopup(settings::SearchPickerOpenRequest request);
  void openSessionActionEntryEditor(std::size_t index);
  void syncSessionActionInlineSummary(std::size_t index, const SessionPanelActionConfig& row);
  void openIdleBehaviorEntryEditor(std::size_t index);
  void openIdleBehaviorCreateEditor();
  void openNotificationFilterEntryEditor(std::size_t index);
  void openNotificationFilterCreateEditor();
  void openCalendarAccountEditor(std::optional<std::string> accountId);
  void openWidgetInspectorEditor(std::vector<std::string> laneListPath, std::string widgetName);
  void openCapsuleGroupEditor(std::vector<std::string> laneListPath, std::string groupId);
  void openPluginSourceCreateEditor(std::optional<PluginSourceConfig> existing = std::nullopt);
  void openPluginSettingsEditor(std::string pluginId);
  void openPluginStore();
  void openBarWidgetEditorSheet(
      std::string title, std::function<void(Flex&)> populate, std::function<void()> removeAction = nullptr
  );
  void closeWidgetInspectorPopup();
  void refreshIdleLiveStatusText();
  void saveSupportReport();
  void saveConfigExport(settings::ConfigExportMode mode);
  [[nodiscard]] bool headerDragRegionContains(float sceneX, float sceneY) const;
  void setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value);
  void setSettingOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides);
  void clearSettingOverride(std::vector<std::string> path);
  void clearSettingOverrides(std::vector<std::vector<std::string>> paths);
  void renameWidgetInstance(
      std::string oldName, std::string newName,
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
  );
  void createBar(std::string name);
  void renameBar(std::string oldName, std::string newName);
  void deleteBar(std::string name);
  void moveBar(std::string name, int direction);
  void createMonitorOverride(std::string barName, std::string match);
  void renameMonitorOverride(std::string barName, std::string oldMatch, std::string newMatch);
  void deleteMonitorOverride(std::string barName, std::string match);
  [[nodiscard]] float uiScale() const;

  [[nodiscard]] std::optional<LayerPopupParentContext> topmostPopupParentContext() const;
  void dismissOpenSelectDropdown();

  WaylandConnection* m_wayland = nullptr;
  CompositorPlatform* m_platform = nullptr;
  IdleManager* m_idleManager = nullptr;
  ConfigService* m_config = nullptr;
  scripting::PluginManager* m_pluginManager = nullptr;
  // Cached PluginManager::list() — discovery can spawn git, so refresh it off the UI path.
  std::vector<scripting::PluginStatus> m_pluginList;
  bool m_pluginListDirty = true;
  bool m_pluginListRefreshInFlight = false;
  std::uint64_t m_pluginListRefreshGeneration = 0;
  scripting::PluginFileCache m_pluginFileCache;
  RenderContext* m_renderContext = nullptr;
  DependencyService* m_dependencies = nullptr;
  UPowerService* m_upower = nullptr;
  AccountsService* m_accounts = nullptr;
  Label* m_idleLiveStatusLabel = nullptr;
  std::vector<Label*> m_sessionActionSummaryLabels;
  std::shared_ptr<std::vector<SessionPanelActionConfig>> m_sessionActionsEditState;

  // m_sceneRoot must be destroyed before m_animations — ~Node() calls cancelForOwner().
  AnimationManager m_animations;
  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  Flex* m_mainContainer = nullptr; // Outer Flex inside m_sceneRoot, sized to the window
  Box* m_panelBackground = nullptr;
  Node* m_headerRow = nullptr;
  Node* m_filterRow = nullptr;
  Button* m_actionsMenuButton = nullptr;
  Flex* m_contentContainer = nullptr;
  ScrollView* m_contentScrollView = nullptr;
  ScrollView* m_sidebarScrollView = nullptr;
  RovingListNavHost* m_sidebarNav = nullptr;
  std::unique_ptr<ContextMenuPopup> m_actionsMenuPopup;
  std::unique_ptr<settings::WidgetAddPopup> m_widgetAddPopup;
  std::unique_ptr<settings::ConfigExportDialogPopup> m_configExportDialogPopup;
  std::unique_ptr<settings::SearchPickerPopup> m_searchPickerPopup;
  std::unique_ptr<settings::SettingsSheetPopup> m_editorSheetPopup;
  std::unique_ptr<settings::SettingsControlFactory> m_editorSheetFactory;
  std::vector<std::string> m_editorSheetListPath;
  InputDispatcher m_inputDispatcher;
  std::unique_ptr<SelectDropdownPopup> m_selectPopup;
  bool m_pointerInside = false;
  wl_output* m_output = nullptr;

  std::uint32_t m_lastSceneWidth = 0;
  std::uint32_t m_lastSceneHeight = 0;
  ScrollViewState m_sidebarScrollState;
  ScrollViewState m_contentScrollState;
  std::vector<settings::SettingEntry> m_settingsRegistry;
  bool m_rebuildRequested = false;
  bool m_contentRebuildRequested = false;
  bool m_settingsRegistryRefreshRequested = false;
  bool m_filterRowRefreshRequested = false;
  bool m_focusSearchOnRebuild = false;
  Input* m_settingsSearchInput = nullptr;
  bool m_scrollToPendingContentTarget = false;
  Node* m_pendingContentScrollTarget = nullptr;
  std::string m_searchQuery;
  Timer m_searchDebounceTimer;
  // Set by openToBarWidget (e.g. middle-click on a bar widget); consumed once the window holds
  // keyboard focus so the sheet's grab popup gets a serial the compositor accepts.
  std::string m_pendingOpenWidgetInspectorName;
  int m_pendingOpenWidgetInspectorFrames = 0;
  // When the editor sheet is opened programmatically (bar middle-click) there is no grab-valid serial,
  // so open it without an xdg_popup grab. Consumed by openBarWidgetEditorSheet.
  bool m_pendingEditorSheetNoGrab = false;
  std::string m_editingWidgetName;
  std::string m_editingCapsuleGroupId;
  std::vector<std::string> m_selectedLaneWidgets;
  std::string m_pendingDeleteWidgetName;
  std::string m_pendingDeleteWidgetSettingPath;
  std::string m_renamingWidgetName;
  std::string m_creatingBarName;
  std::string m_renamingBarName;
  std::string m_pendingDeleteBarName;
  std::string m_creatingMonitorOverrideBarName;
  std::string m_creatingMonitorOverrideMatch;
  std::string m_renamingMonitorOverrideBarName;
  std::string m_renamingMonitorOverrideMatch;
  std::string m_pendingDeleteMonitorOverrideBarName;
  std::string m_pendingDeleteMonitorOverrideMatch;
  std::string m_pendingDeletePluginId;
  std::string m_selectedBarName;
  std::string m_selectedMonitorOverride;
  std::string m_selectedSection;
  std::string m_statusMessage;
  std::string m_pendingResetPageScope;
  bool m_forceEnTranslation = false;
  bool m_showAdvanced = false;
  bool m_showOverriddenOnly = false;
  bool m_statusIsError = false;
  std::function<void()> m_openDesktopWidgetEditor;
  std::function<void()> m_openLockscreenWidgetEditor;
  std::function<void()> m_openWallpaperPanel;
  std::function<void()> m_syncGreeterAppearance;
  std::function<void()> m_resetLauncherUsage;
  std::function<void()> m_resetScreenTime;
  std::function<void()> m_saveWallpaperPaletteAsCustom;
  std::function<void(std::string, std::string)> m_connectCalendarAccount;
};
