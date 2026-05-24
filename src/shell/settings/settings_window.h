#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/settings/config_export_dialog_popup.h"
#include "shell/settings/search_picker_popup.h"
#include "shell/settings/session_actions_editor_popup.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/widget_add_popup.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/toplevel_surface.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Box;
class Button;
class ConfigService;
class DependencyService;
class Flex;
class IdleManager;
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
      UPowerService* upower, IdleManager* idleManager
  );

  void open();
  void openToBarWidget(std::string barName, std::string widgetName);
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }
  [[nodiscard]] wl_surface* wlSurface() const noexcept {
    return m_surface != nullptr ? m_surface->wlSurface() : nullptr;
  }
  [[nodiscard]] bool ownsKeyboardSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const;

  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void onThemeChanged();
  void onFontChanged();
  void onExternalOptionsChanged();
  void setOpenDesktopWidgetEditor(std::function<void()> callback) { m_openDesktopWidgetEditor = std::move(callback); }
  void setOpenWallpaperPanel(std::function<void()> callback) { m_openWallpaperPanel = std::move(callback); }

  void onSecondTick();
  void onIdleLiveStatusChanged();

private:
  void destroyWindow();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene(std::uint32_t width, std::uint32_t height);
  void rebuildSettingsContent();
  [[nodiscard]] settings::RegistryEnvironment buildRegistryEnvironment() const;
  void syncSelectedBarState(const Config& cfg, const std::vector<std::string>& availableBars);
  [[nodiscard]] std::unique_ptr<Flex> buildHeaderRow(float scale);
  [[nodiscard]] std::unique_ptr<Flex>
  buildFilterRow(float scale, const std::string& resetPageScope, std::vector<std::vector<std::string>> resetPagePaths);
  [[nodiscard]] std::unique_ptr<Flex> buildStatusRow(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildBody(
      float scale, const Config& cfg, const std::vector<std::string>& sections,
      const std::vector<std::string>& availableBars
  );
  [[nodiscard]] std::vector<settings::SelectOption> batteryDeviceOptions() const;
  [[nodiscard]] settings::SettingsContentContext makeContentContext(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride
  );
  void requestSceneRebuild();
  void requestContentRebuild();
  void applyPendingContentScrollTarget(float margin);
  void clearStatusMessage();
  void clearTransientSettingsState();
  void openActionsMenu();
  void openConfigExportDialog();
  void openBarWidgetAddPopup(const std::vector<std::string>& lanePath);
  void openSearchPickerPopup(
      const std::string& title, const std::vector<settings::SelectOption>& options, const std::string& selectedValue,
      const std::string& placeholder, const std::string& emptyText, const std::vector<std::string>& settingPath
  );
  void openSessionActionEntryEditor(std::size_t index);
  void openIdleBehaviorEntryEditor(std::size_t index);
  void openIdleBehaviorCreateEditor();
  void refreshIdleLiveStatusText();
  void saveSupportReport();
  void saveConfigExport(settings::ConfigExportMode mode);
  [[nodiscard]] bool headerDragRegionContains(float sceneX, float sceneY) const;
  void setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value);
  void setSettingOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides);
  void clearSettingOverride(std::vector<std::string> path);
  void clearSettingOverrides(std::vector<std::vector<std::string>> paths);
  void markSettingsWriteSuccess(bool requestRebuild = true);
  void markSettingsWriteError(std::string message);
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

  WaylandConnection* m_wayland = nullptr;
  IdleManager* m_idleManager = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  DependencyService* m_dependencies = nullptr;
  UPowerService* m_upower = nullptr;
  Label* m_idleLiveStatusLabel = nullptr;

  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  Flex* m_mainContainer = nullptr; // Outer Flex inside m_sceneRoot, sized to the window
  Box* m_panelBackground = nullptr;
  Node* m_headerRow = nullptr;
  Button* m_actionsMenuButton = nullptr;
  Flex* m_contentContainer = nullptr;
  ScrollView* m_contentScrollView = nullptr;
  std::unique_ptr<ContextMenuPopup> m_actionsMenuPopup;
  std::unique_ptr<settings::WidgetAddPopup> m_widgetAddPopup;
  std::unique_ptr<settings::ConfigExportDialogPopup> m_configExportDialogPopup;
  std::unique_ptr<settings::SearchPickerPopup> m_searchPickerPopup;
  std::unique_ptr<settings::SessionActionsEditorPopup> m_sessionActionsEditorPopup;
  InputDispatcher m_inputDispatcher;
  AnimationManager m_animations;
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
  bool m_focusSearchOnRebuild = false;
  bool m_scrollToPendingContentTarget = false;
  Node* m_pendingContentScrollTarget = nullptr;
  std::string m_searchQuery;
  std::string m_editingWidgetName;
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
  std::string m_selectedBarName;
  std::string m_selectedMonitorOverride;
  std::string m_selectedSection;
  std::string m_statusMessage;
  std::string m_pendingResetPageScope;
  bool m_showAdvanced = false;
  bool m_showOverriddenOnly = false;
  bool m_statusIsError = false;
  std::function<void()> m_openDesktopWidgetEditor;
  std::function<void()> m_openWallpaperPanel;
};
