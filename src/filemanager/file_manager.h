#pragma once

#include "app/main_loop.h"
#include "config/config_poll_source.h"
#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/timer_manager.h"
#include "filemanager/checksum.h"
#include "filemanager/dialogs/checksum_dialog.h"
#include "filemanager/dialogs/help_dialog.h"
#include "filemanager/dialogs/modal_dialog_host.h"
#include "filemanager/dialogs/progress_window.h"
#include "filemanager/dialogs/trash_fallback_dialog.h"
#include "filemanager/external_tools.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_list_header.h"
#include "filemanager/file_manager_settings.h"
#include "filemanager/file_menu_controller.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/file_transfer_controller.h"
#include "filemanager/gio_poll_source.h"
#include "filemanager/places_model.h"
#include "filemanager/places_sidebar.h"
#include "filemanager/properties_window.h"
#include "filemanager/tab_bar_controller.h"
#include "filemanager/tab_manager.h"
#include "net/http_client.h"
#include "render/animation/animation_manager.h"
#include "render/core/thumbnail_service.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "theme/theme_service.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_dialog_popup.h"
#include "ui/dialogs/search_picker_popup.h"
#include "wayland/native_data_transfer.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gio/gio.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Button;
class Box;
class DialogBackgroundArtwork;
class Separator;
class Flex;
class Glyph;
class Input;
class InputArea;
class Label;
class ScrollView;
class Image;

class FileManager {
public:
  explicit FileManager(std::string initialPath);
  ~FileManager();
  int run();

private:
  friend class FileManagerTestAccess;
  class NativeTextClipboard;
  void installInputHooks();
  void uninstallInputHooks();
  void activateContextMenuEntry(const ContextMenuControlEntry& entry);
  class Adapter;
  [[nodiscard]] filemanager::BrowserTab& activeTab() noexcept { return m_tabs.activeTab(); }
  [[nodiscard]] const filemanager::BrowserTab& activeTab() const noexcept { return m_tabs.activeTab(); }
  [[nodiscard]] filemanager::DirectorySession& activeSession() noexcept { return activeTab().session; }
  [[nodiscard]] const filemanager::DirectorySession& activeSession() const noexcept { return activeTab().session; }
  void navigate(const std::filesystem::path& path, bool recordHistory = true) {
    auto& tab = activeTab();
    if (tab.hasActiveAdminSession() && filemanager::isTrashTabPath(path)) {
      setStatus("Wastebin is unavailable in Administrator Mode");
      return;
    }
    tab.session.setNextEnumerateUserInitiated(tab.hasActiveAdminSession());
    tab.session.navigate(path, recordHistory);
  }
  void enumerate(const std::filesystem::path& path) {
    auto& tab = activeTab();
    recordAdminTabUserActivity(tab);
    tab.session.enumerate(path);
  }
  void navigateTrash() {
    if (activeTab().hasActiveAdminSession()) {
      setStatus("Wastebin is unavailable in Administrator Mode");
      return;
    }
    activeSession().navigateTrash();
  }
  void restoreWastebinSelection(bool chooseDestination);
  void confirmEmptyWastebin();
  void emptyWastebinAfterConfirmation(filemanager::TabId originatingTab, int response);
  void pinPlace(const std::filesystem::path& path);
  void sortEntries() { activeSession().sortEntries(); }
  void persistFileViewState();
  void updateWindowTitle();
  [[nodiscard]] std::unique_ptr<Flex> buildHeaderAppBar();
  void updateAdminHeaderUi(int surfaceWidth = 0);
  bool enterAdministratorMode(
      filemanager::TabId tabId, std::optional<std::filesystem::path> targetDirectory = std::nullopt
  );
  void finishAdministratorMode(filemanager::TabId tabId, std::string statusMessage = {});
  void recordAdminTabUserActivity(filemanager::BrowserTab& tab);
  [[nodiscard]] filemanager::TabManager::Callbacks makeTabManagerCallbacks();
  [[nodiscard]] static std::filesystem::path defaultTabPath();
  filemanager::TabId openNewTab();
  filemanager::TabId openFolderInNewTab(const std::filesystem::path& path, bool activate);
  filemanager::TabId duplicateTab(filemanager::TabId tabId);
  bool activateTab(filemanager::TabId tabId);
  bool activateRelativeTab(int delta);
  bool closeTab(filemanager::TabId tabId);
  bool closeOtherTabs(filemanager::TabId keepTabId);
  bool closeTabsToRight(filemanager::TabId pivotTabId);
  filemanager::TabId reopenClosedTab();
  void setAlwaysShowTabBar(bool show);
  void setOpenTabsInBackground(bool background);
  bool openApplicationMenu(float x, float y, std::size_t menuIndex = 0) {
    return m_menus.openApplicationMenu(x, y, menuIndex);
  }
  [[nodiscard]] std::vector<ContextMenuControlEntry> fileMenuEntries() const { return m_menus.fileMenuEntries(); }
  [[nodiscard]] std::vector<ContextMenuControlEntry> editMenuEntries() const { return m_menus.editMenuEntries(); }
  [[nodiscard]] std::vector<ContextMenuControlEntry> viewMenuEntries() const { return m_menus.viewMenuEntries(); }
  [[nodiscard]] std::vector<ContextMenuControlEntry> toolsMenuEntries() { return m_menus.toolsMenuEntries(); }
  [[nodiscard]] std::vector<ContextMenuControlEntry> helpMenuEntries() const { return m_menus.helpMenuEntries(); }
  void launchNewWindow(const std::filesystem::path& path);
  void restoreSelection(bool chooseFirst = false);
  enum class SelectionChange { All, None, Invert };
  [[nodiscard]] bool canChangeSelection() const;
  void changeSelection(SelectionChange change);
  void activateSelected();
  void openProperties(const std::filesystem::path& path);
  void activatePath(const std::filesystem::path& path);
  void activatePointerItem(std::size_t index);
  void activatePointerMiddleItem(std::size_t index);
  void setStatus(std::string text);
  void updateNavigationControls();
  void updateSelectionSummary();
  void updateStatusBar();
  void cancelFileGesture() { m_transfers.cancelFileGesture(); }
  void textPrompt(
      std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
  );
  void applyViewScale();
  void adjustViewScale(float value);
  void setGridMode(bool gridMode);
  std::optional<std::filesystem::path> dropDestination(float x, float y, bool incoming = false) const {
    return m_transfers.dropDestination(x, y, incoming);
  }
  using SavedPlace = filemanager::SavedPlace;
  using PlacePathResolution = filemanager::PlacePathResolution;
  using SelectionInfo = filemanager::SelectionInfo;
  using PasteRequest = filemanager::PasteRequest;
  using PasteSelection = filemanager::PasteSelection;
  using DropValidation = filemanager::FileTransferController::DropValidation;
  struct CreateRequest {
    filemanager::TabId tabId = filemanager::kInvalidTabId;
    std::uint64_t navigation = 0;
    std::filesystem::path destination;
  };
  struct InlineRenameTarget {
    std::uint64_t operationId = 0;
    filemanager::TabId tabId = filemanager::kInvalidTabId;
    std::filesystem::path source;
  };
  bool m_reordering = false;
  std::optional<std::string> m_reorderId;
  std::optional<std::string> m_reorderBefore;
  float m_dragStartX = 0, m_dragStartY = 0;
  std::unordered_set<std::string> m_rubberBaseline;
  std::optional<std::filesystem::path> m_rubberSelectedPath;
  std::optional<std::filesystem::path> m_rubberAnchor;
  std::uint32_t m_rubberModifiers = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  struct ProtectedCopy {
    filemanager::TabId tabId;
    std::filesystem::path original;
    std::filesystem::path temporary;
    std::string revision;
    ~ProtectedCopy();
  };
  using ProtectedCopyReady = std::function<void(std::shared_ptr<ProtectedCopy>, const std::string&)>;
  std::vector<std::shared_ptr<ProtectedCopy>> m_protectedEdits;
  [[nodiscard]] std::shared_ptr<ProtectedCopy>
  protectedEdit(filemanager::TabId tab, const std::filesystem::path& path) const;
  std::function<bool()> requestProtectedCopy(
      filemanager::BrowserTab& tab, const std::filesystem::path& path, ProtectedCopyReady ready, bool thumbnail = false
  );
  void openProtectedFile(filemanager::TabId tabId, const std::filesystem::path& path,
                         std::function<void(const std::filesystem::path&)> launch);
  void saveProtectedChanges(const std::filesystem::path& path);
  void launchFileUri(filemanager::TabId tab, GFile* file, std::string success = {});
  struct ProtectedThumbnail {
    std::filesystem::path path;
    std::shared_ptr<ProtectedCopy> copy;
  };
  std::unordered_map<std::size_t, ProtectedThumbnail> m_protectedThumbnails;
  filemanager::TabId m_protectedThumbnailTab = filemanager::kInvalidTabId;
  std::uint64_t m_protectedThumbnailListing = 0;
  std::uint64_t m_protectedThumbnailRevision = 0;
  std::function<bool()> m_cancelProtectedThumbnail;
  bool m_protectedThumbnailPending = false;
  Timer m_protectedThumbnailRetry;
  void clearProtectedThumbnails();
  std::optional<std::string> protectedThumbnailPath(filemanager::BrowserTab& tab, const FileEntry& entry, std::size_t index);
  void rebuildBreadcrumbs();
  void setLocationEditMode(bool editing, bool preserveOutsideClickFocus = false);
  void deferCancelLocationEdit();
  void pasteLocationFromClipboard();
  void setSortField(FileDialogSortField field);
  void toggleSortOrder();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void handlePointer(const PointerEvent& event);
  void dispatchPointer(const PointerEvent& event);
  void syncTooltipHover();
  void handleKeyboard(const KeyboardEvent& event);
  void handleWindowKeyEvent(const KeyboardEvent& event);
  [[nodiscard]] bool navigateFileViewByKeyboard(std::uint32_t sym, std::uint32_t mods);
  [[nodiscard]] bool handleFileViewSpaceSelection(std::uint32_t mods);
  [[nodiscard]] bool handleFileViewTypeahead(std::uint32_t utf32);
  [[nodiscard]] bool handleSidebarKeyboardNavigation(std::uint32_t sym, std::uint32_t mods);
  void openLocationContextMenu(float x, float y, bool highlightOnOpen);
  void openFocusedContextMenu();
  void syncProgressWindow();
  [[nodiscard]] std::optional<std::filesystem::path> checksumTarget() const;
  [[nodiscard]] std::array<ContextMenuControlEntry, 2> checksumMenuEntries() const;
  void showChecksumDialog(bool verify);

  void requestQuit(ToplevelSurface* parent = nullptr);
  void showDialog(
      std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
      std::function<void(int)> response, ToplevelSurface* parent = nullptr, bool applyToAllCheckbox = false,
      bool alternativeName = false
  );
  void closeDialog(int response);
  void cancelStatusFreeSpaceQuery() { activeTab().cancelStatusFreeSpaceQuery(); }
  void queryStatusFreeSpaceAsync(const std::filesystem::path& path) { activeTab().queryStatusFreeSpaceAsync(path); }
  void cancelSelectionInfoQuery() { activeTab().cancelSelectionInfoQuery(); }
  void querySelectionInfoAsync(const std::filesystem::path& path) { activeTab().querySelectionInfoAsync(path); }
  [[nodiscard]] bool currentDirectoryWritable() const { return activeSession().currentDirectoryWritable(); }
  [[nodiscard]] std::vector<std::string> selectedPaths() const { return activeTab().selectedPathsOrdered(); }
  [[nodiscard]] std::optional<std::pair<std::filesystem::path, bool>> symlinkSelection() const {
    return activeTab().symlinkSelection();
  }
  void duplicateSelection();
  void createSelectionSymlink();
  void showSymlinkDialog();
  void pasteClipboard() { m_transfers.pasteClipboard(); }
  bool recordPasteCompletion(const FileOperationManager::Operation& operation) {
    return m_transfers.recordPasteCompletion(operation);
  }
  void applyPendingPasteSelection() { m_transfers.applyPendingPasteSelection(); }
  void finishPendingTransferReveal(Renderer& renderer) { m_transfers.finishPendingTransferReveal(renderer); }
  void copyToDesktopClipboard(bool cut) { m_transfers.copyToDesktopClipboard(cut); }
  void writeDesktopClipboard(std::string payload, const char* mime) {
    m_transfers.writeDesktopClipboard(std::move(payload), mime);
  }
  void beginRename();
  void beginRenamePath(const std::filesystem::path& path);
  void cancelRename();
  void syncSortControls();

  ConfigService m_configService;
  filemanager::FileManagerSettings m_settings{m_configService};
  HttpClient m_httpClient;
  nythera::theme::ThemeService m_themeService{m_configService, m_httpClient};
  ConfigPollSource m_configPollSource{m_configService};
  NativeDataTransfer m_transfer;
  std::unique_ptr<NativeTextClipboard> m_textClipboard;
  TextInputService m_textInputService;
  WaylandConnection m_wayland{WaylandConnection::Purpose::ApplicationClient};
  GlSharedContext m_sharedContext;
  RenderContext m_renderContext;
  AnimationManager m_menuAnimations;
  GioPollSource m_gioPollSource;
  FileOperationManager m_operations;
  ThumbnailService m_pickerThumbnails;
  ThumbnailService::Subscription m_thumbnailPendingSub;
  LayerPopupHostRegistry m_pickerHosts;
  std::unique_ptr<FileDialogPopup> m_destinationPicker;
  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<PropertiesWindow> m_propertiesWindow;
  filemanager::ProgressWindow m_progressWindow{
      m_wayland, m_renderContext, m_operations, [this](ToplevelSurface* parent) { requestQuit(parent); }
  };
  filemanager::ModalDialogHost m_modalDialog{
      m_wayland,
      m_renderContext,
      m_configService,
      m_settings,
      {
          .defaultParent = [this] { return m_surface ? m_surface.get() : m_progressWindow.surface(); },
          .isMainWindowOpen = [this] { return m_windowOpen; },
          .mainSurface = [this] { return m_surface.get(); },
          .progressSurface = [this] { return m_progressWindow.surface(); },
          .syncProgressWindow = [this] { syncProgressWindow(); },
          .setStatus = [this](std::string text) { setStatus(std::move(text)); },
      },
  };
  filemanager::ChecksumDialogController m_checksumDialog{
      m_modalDialog,
      m_transfer,
      m_wayland,
      {
          .writeClipboardText =
              [this](std::string text) { writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8"); },
          .isPickerOpen = [this] { return m_destinationPicker && m_destinationPicker->isOpen(); },
          .closePicker =
              [this] {
                if (m_destinationPicker && m_destinationPicker->isOpen()) {
                  m_destinationPicker->closeFileDialogWithoutResult();
                  FileDialog::cancelIfPending();
                }
              },
      },
  };
  filemanager::HelpDialogController m_helpDialog{
      m_modalDialog,
      m_wayland,
      {
          .mainSurface = [this] { return m_surface.get(); },
          .onStatus = [this](std::string text) { setStatus(std::move(text)); },
      },
  };
  InputDispatcher m_dispatcher;
  filemanager::FileListHeaderController m_listHeader{
      m_settings,
      m_wayland,
      m_dispatcher,
      {
          .onViewStateChanged = [this] { persistFileViewState(); },
          .onSortChanged =
              [this] {
                sortEntries();
                if (m_grid != nullptr) {
                  m_grid->notifyDataChanged();
                  restoreSelection();
                }
                updateNavigationControls();
              },
          .onViewModeOrScaleChanged =
              [this] {
                if (m_listHeader.gridMode())
                  activeSession().clearFolderCounts();
                activeTab().viewState.gridMode = m_listHeader.gridMode();
                activeTab().viewState.viewScale = m_listHeader.viewScale();
                applyViewScale();
              },
          .onRequestLayout =
              [this] {
                if (m_surface)
                  m_surface->requestLayout();
              },
          .onRequestRedraw =
              [this] {
                if (m_surface)
                  m_surface->requestRedraw();
              },
          .onStatus = [this](std::string text) { setStatus(std::move(text)); },
          .canHandlePointer =
              [this] {
                return !m_transfers.isDragPending()
                    && !m_transfers.isDraggingFiles()
                    && !m_transfer.dragging()
                    && !m_reorderId
                    && !m_tabBar.gestureActive()
                    && !m_resizingSidebar
                    && !m_resizingStatus
                    && !(m_grid && m_grid->rubberBandActive());
              },
          .canAdjustScale = [this] { return !m_renamePath; },
          .pointerSerial = [this] { return m_pointerSerial; },
      },
  };
  filemanager::PlacesSidebarController m_places{
      m_settings,
      m_operations,
      {
          .currentPath = [this] { return activeSession().path(); },
          .isBrowsingTrash = [this] { return activeSession().browsingTrash(); },
          .isQuitting = [this] { return m_quitting; },
          .sidebarWidth = [this] { return m_sidebarWidth; },
          .hasPendingClipboardOrRename =
              [this] {
                return m_transfers.pendingClipboardReads() > 0
                    || m_renamePath.has_value()
                    || m_pendingInlineRenamePath.has_value();
              },
          .onNavigate =
              [this](const std::filesystem::path& path, bool recordHistory) { navigate(path, recordHistory); },
          .onShowMenu = [this](
                            std::vector<ContextMenuControlEntry> entries, float x, float y
                        ) { m_menus.showPlacesMenu(std::move(entries), x, y); },
          .onInputSerial = [this](std::uint32_t serial) { m_lastInputSerial = serial; },
          .onTextPrompt = [this](
                              std::string title, std::string message, bool password,
                              std::function<void(std::optional<std::string>)> response
                          ) { textPrompt(std::move(title), std::move(message), password, std::move(response)); },
          .onShowDialog =
              [this](
                  std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
                  std::function<void(int)> response
              ) { showDialog(std::move(title), std::move(message), std::move(choices), std::move(response)); },
          .onStatus = [this](std::string text) { setStatus(std::move(text)); },
          .onUpdateNavigationControls = [this] { updateNavigationControls(); },
          .onRequestLayout =
              [this] {
                if (m_surface)
                  m_surface->requestLayout();
              },
          .onClearDropPlace = [this] { m_transfers.clearDropPlace(); },
          .onMountRemoved = [this](
                                const std::filesystem::path& mountPath
                            ) { m_tabs.handleMountRemoved(mountPath, defaultTabPath()); },
          .onOpenPlaceInBackgroundTab =
              [this](const filemanager::Place& place) {
                if (place.identity == "personal:trash")
                  openFolderInNewTab(std::filesystem::path("trash:/"), false);
                else if (place.mounted && !place.path.empty())
                  openFolderInNewTab(place.path, false);
              },
      },
  };
  Timer m_selectionTimer;
  Timer m_statusMessageTimer;
  std::chrono::steady_clock::time_point m_selectionTick;
  std::unique_ptr<Flex> m_root;
  Flex* m_appBar = nullptr;
  Flex* m_appBrandContainer = nullptr;
  Flex* m_adminHeaderContainer = nullptr;
  Box* m_adminHeaderSeparator = nullptr;
  Glyph* m_adminHeaderShield = nullptr;
  Label* m_adminHeaderLabel = nullptr;
  Button* m_adminFinishButton = nullptr;
  int m_lastHeaderWidth = 1120;
  std::function<std::optional<bool>(const std::filesystem::path&)> m_unprivilegedDirectoryAccessibleHook;
  Flex* m_breadcrumbBar = nullptr;
  Flex* m_sidebar = nullptr;
  Flex* m_sidebarDivider = nullptr;
  Flex* m_statusDivider = nullptr;
  Flex* m_statusStrip = nullptr;
  bool m_resizingStatus = false;
  float m_statusDragY = 0.0F;
  float m_statusDragHeight = 42.0F;
  float m_statusHeight = 42.0F;
  std::chrono::steady_clock::time_point m_statusMessageUntil{};
  std::string m_statusMessage;
  float m_sidebarWidth = 220.0F;
  bool m_resizingSidebar = false;
  float m_sidebarDragOffset = 0.0F;
  float m_sidebarDragWidth = 0.0F;
  std::uint32_t m_pointerSerial = 0;
  VirtualGridView* m_grid = nullptr;
  Input* m_locationInput = nullptr;
  Label* m_statusLabel = nullptr;
  Label* m_statusDetailLabel = nullptr;
  Label* m_statusFreeLabel = nullptr;
  Button* m_backButton = nullptr;
  Button* m_forwardButton = nullptr;
  Button* m_upButton = nullptr;
  Button* m_refreshButton = nullptr;
  std::unique_ptr<Adapter> m_adapter;
  filemanager::TabManager m_tabs;
  filemanager::FileMenuController m_menus;
  filemanager::TabBarController m_tabBar;
  filemanager::FileTransferController m_transfers;
  filemanager::TrashFallbackDialogController m_trashFallbackDialog{m_modalDialog};
  std::uint32_t m_keyboardModifiers = 0;
  std::uint32_t m_lastInputSerial = 0;
  std::optional<InlineRenameTarget> m_inlineRenameTarget;
  std::unordered_map<std::uint64_t, CreateRequest> m_createRequests;
  std::optional<std::filesystem::path> m_renamePath;
  std::optional<std::filesystem::path> m_pendingInlineRenamePath;
  InputArea* m_inlineEditArea = nullptr;
  std::string m_windowTitle = "Nythera Files";
  bool m_alwaysShowTabBar = false;
  bool m_openTabsInBackground = false;
  bool m_deleteConfirmation = false;
  bool m_deletePermanentConfirmation = false;
  bool m_windowOpen = true;
  bool m_quitConfirmation = false;
  bool m_quitting = false;
  using ConflictDialogKind = filemanager::ConflictDialogKind;
  using ConflictItemInfo = filemanager::ConflictItemInfo;
  using ConflictDialogData = filemanager::ConflictDialogData;
  [[nodiscard]] static std::string sanitizeConflictDisplayText(std::string_view text) {
    return filemanager::ModalDialogHost::sanitizeConflictDisplayText(text);
  }
  bool m_editingLocation = false;
  std::uint64_t m_locationEditGeneration = 0;
  std::function<void(std::function<void(std::optional<std::string>)>)> m_readLocationClipboardForTesting;
  bool m_refreshAfterRename = false;
  bool m_restoringSelection = false;
  bool m_fileViewFocusVisible = false;
  std::string m_typeaheadQuery;
  std::chrono::steady_clock::time_point m_typeaheadLastTime{};
  std::optional<std::size_t> m_lastPointerIndex;
  std::uint32_t m_lastPointerClickTime = 0;
  std::uint32_t m_pointerClickTime = 0;
  bool m_running = true;
};
