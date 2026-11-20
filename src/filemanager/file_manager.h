#pragma once

#include "app/main_loop.h"
#include "config/config_poll_source.h"
#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/timer_manager.h"
#include "filemanager/checksum.h"
#include "filemanager/dialogs/checksum_dialog.h"
#include "filemanager/dialogs/modal_dialog_host.h"
#include "filemanager/dialogs/progress_window.h"
#include "filemanager/directory_session.h"
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
  void activateContextMenuEntry(const ContextMenuControlEntry& entry);
  class Adapter;
  void navigate(const std::filesystem::path& path, bool recordHistory = true) { m_session.navigate(path, recordHistory); }
  void enumerate(const std::filesystem::path& path) { m_session.enumerate(path); }
  void navigateTrash() { m_session.navigateTrash(); }
  void restoreWastebinSelection(bool chooseDestination);
  void confirmEmptyWastebin();
  void sortEntries() { m_session.sortEntries(); }
  void persistFileViewState();
  bool openApplicationMenu(float x, float y, std::size_t menuIndex = 0) {
    return m_menus.openApplicationMenu(x, y, menuIndex);
  }
  [[nodiscard]] std::vector<ContextMenuControlEntry> fileMenuEntries() const { return m_menus.fileMenuEntries(); }
  [[nodiscard]] std::vector<ContextMenuControlEntry> editMenuEntries() const { return m_menus.editMenuEntries(); }
  [[nodiscard]] std::vector<ContextMenuControlEntry> viewMenuEntries() const { return m_menus.viewMenuEntries(); }
  [[nodiscard]] std::vector<ContextMenuControlEntry> toolsMenuEntries() { return m_menus.toolsMenuEntries(); }
  void launchNewWindow(const std::filesystem::path& path);
  void restoreSelection(bool chooseFirst = false);
  enum class SelectionChange { All, None, Invert };
  [[nodiscard]] bool canChangeSelection() const;
  void changeSelection(SelectionChange change);
  void activateSelected();
  void openProperties(const std::filesystem::path& path);
  void activatePath(const std::filesystem::path& path);
  void activatePointerItem(std::size_t index);
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
  using PasteRequest = filemanager::FileTransferController::PasteRequest;
  using PasteSelection = filemanager::FileTransferController::PasteSelection;
  using DropValidation = filemanager::FileTransferController::DropValidation;
  bool m_reordering = false;
  std::optional<std::string> m_reorderId;
  std::optional<std::string> m_reorderBefore;
  float m_dragStartX = 0, m_dragStartY = 0;
  std::unordered_set<std::string> m_rubberBaseline;
  std::optional<std::filesystem::path> m_rubberSelectedPath;
  std::optional<std::filesystem::path> m_rubberAnchor;
  std::uint32_t m_rubberModifiers = 0;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  void rebuildBreadcrumbs();
  void setLocationEditMode(bool editing, bool preserveOutsideClickFocus = false);
  void deferCancelLocationEdit();
  void setSortField(FileDialogSortField field);
  void toggleSortOrder();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void handlePointer(const PointerEvent& event);
  void dispatchPointer(const PointerEvent& event);
  void syncTooltipHover();
  void handleKeyboard(const KeyboardEvent& event);
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
  void cancelStatusFreeSpaceQuery();
  void queryStatusFreeSpaceAsync(const std::filesystem::path& path);
  void cancelSelectionInfoQuery();
  void querySelectionInfoAsync(const std::filesystem::path& path);
  [[nodiscard]] bool currentDirectoryWritable() const { return m_session.currentDirectoryWritable(); }
  [[nodiscard]] std::vector<std::string> selectedPaths() const;
  [[nodiscard]] std::optional<std::pair<std::filesystem::path, bool>> symlinkSelection() const;
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
          .writeClipboardText = [this](std::string text) { writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8"); },
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
                  m_session.clearFolderCounts();
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
          .currentPath = [this] { return m_session.path(); },
          .isBrowsingTrash = [this] { return m_session.browsingTrash(); },
          .isQuitting = [this] { return m_quitting; },
          .sidebarWidth = [this] { return m_sidebarWidth; },
          .hasPendingClipboardOrRename =
              [this] {
                return m_transfers.pendingClipboardReads() > 0
                    || m_renamePath.has_value()
                    || m_pendingInlineRenamePath.has_value();
              },
          .onNavigate = [this](const std::filesystem::path& path, bool recordHistory) { navigate(path, recordHistory); },
          .onShowMenu =
              [this](std::vector<ContextMenuControlEntry> entries, float x, float y) {
                m_menus.showPlacesMenu(std::move(entries), x, y);
              },
          .onInputSerial = [this](std::uint32_t serial) { m_lastInputSerial = serial; },
          .onTextPrompt =
              [this](
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
      },
  };
  Timer m_selectionTimer;
  Timer m_statusMessageTimer;
  std::chrono::steady_clock::time_point m_selectionTick;
  std::unique_ptr<Flex> m_root;
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
  filemanager::DirectorySession m_session;
  filemanager::FileMenuController m_menus;
  filemanager::FileTransferController m_transfers;
  std::optional<std::string> m_operationResultAfterRefresh;
  std::optional<std::filesystem::path> m_selectedPath;
  std::unordered_set<std::string> m_selectedPaths;
  std::optional<std::filesystem::path> m_selectionAnchor;
  std::uint32_t m_keyboardModifiers = 0;
  std::uint32_t m_lastInputSerial = 0;
  std::optional<std::uint64_t> m_inlineRenameOperation;
  std::optional<std::filesystem::path> m_renamePath;
  std::optional<std::filesystem::path> m_pendingInlineRenamePath;
  InputArea* m_inlineEditArea = nullptr;
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
  bool m_refreshAfterRename = false;
  bool m_restoringSelection = false;
  std::optional<std::size_t> m_lastPointerIndex;
  std::uint32_t m_lastPointerClickTime = 0;
  std::uint32_t m_pointerClickTime = 0;
  bool m_running = true;

  struct SelectionInfo {
    std::filesystem::path path;
    std::string type;
    std::string target;
    bool symlink = false;
    bool hasSize = false;
    std::uint64_t size = 0;
  };
  GCancellable* m_statusFreeSpaceCancellable = nullptr;
  std::uint64_t m_statusFreeSpaceGeneration = 0;
  std::filesystem::path m_statusFreeSpacePath;
  std::optional<std::uint64_t> m_statusFreeSpaceBytes;

  GCancellable* m_selectionInfoCancellable = nullptr;
  std::uint64_t m_selectionInfoGeneration = 0;
  std::filesystem::path m_selectionInfoPendingPath;
  std::optional<SelectionInfo> m_selectionInfo;

  std::function<bool(const std::filesystem::path&, std::function<void(std::optional<std::uint64_t>)>)>
      m_freeSpaceQueryHook;
  std::function<bool(const std::filesystem::path&, std::function<void(std::optional<SelectionInfo>)>)>
      m_selectionInfoQueryHook;
};
