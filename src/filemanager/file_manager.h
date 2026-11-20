#pragma once

#include "app/main_loop.h"
#include "config/config_poll_source.h"
#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/timer_manager.h"
#include "filemanager/checksum.h"
#include "filemanager/external_tools.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/gio_poll_source.h"
#include "filemanager/places_model.h"
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
  struct PendingMonitorChange {
    GFileMonitorEvent event;
    std::filesystem::path path;
    std::filesystem::path otherPath;
  };
  struct PlaceCapacityView {
    std::string identity;
    std::string capacityIdentity;
    std::filesystem::path mountPath;
    Label* freeSpaceLabel = nullptr;
    class ProgressBar* bar = nullptr;
    Button* row = nullptr;
  };
  struct CapacityInfo {
    std::uint64_t total = 0;
    std::uint64_t free = 0;
    std::string filesystemType;
    std::string filesystemId;
    std::chrono::steady_clock::time_point updatedAt;
  };
  void navigate(const std::filesystem::path& path, bool recordHistory = true);
  void enumerate(const std::filesystem::path& path);
  void cancelEnumeration();
  void navigateTrash();
  void restoreWastebinSelection(bool chooseDestination);
  void confirmEmptyWastebin();
  [[nodiscard]] GFile* browsingFile() const;
  void fetchNext(GFileEnumerator* enumerator, std::uint64_t generation);
  std::optional<std::uint64_t> folderItemCount(const FileEntry& entry);
  void startNextFolderCount();
  void clearFolderCounts();
  void sortEntries();
  void persistFileViewState();
  bool openApplicationMenu(float x, float y, std::size_t menuIndex = 0);
  [[nodiscard]] std::vector<ContextMenuControlEntry> fileMenuEntries() const;
  [[nodiscard]] std::vector<ContextMenuControlEntry> editMenuEntries() const;
  [[nodiscard]] std::vector<ContextMenuControlEntry> viewMenuEntries() const;
  [[nodiscard]] std::vector<ContextMenuControlEntry> toolsMenuEntries();
  [[nodiscard]] std::vector<std::string> externalToolTargets(filemanager::external_tools::Tool tool) const;
  void updateApplicationMenuUnderline();
  void persistApplicationMenuPreference();
  void launchNewWindow(const std::filesystem::path& path);
  void restoreSelection(bool chooseFirst = false);
  enum class SelectionChange { All, None, Invert };
  [[nodiscard]] bool canChangeSelection() const;
  void changeSelection(SelectionChange change);
  void applyPendingMonitorChanges();
  void handleMonitorEvent(GFile* file, GFile* otherFile, GFileMonitorEvent event);
  void queryMonitoredFile(
      const std::filesystem::path& path, const std::filesystem::path& replacedPath = std::filesystem::path{}
  );
  void removeMonitoredFile(const std::filesystem::path& path);
  void activateSelected();
  void openProperties(const std::filesystem::path& path);
  void activatePath(const std::filesystem::path& path);
  void activatePointerItem(std::size_t index);
  void setStatus(std::string text);
  void updateNavigationControls();
  void updateSelectionSummary();
  void updateStatusBar();
  void rebuildPlacesSidebar();
  void savePlace(const std::filesystem::path& path);
  void persistSavedPlaces();
  std::optional<std::string> placeInsertionAt(float x, float y, std::string_view group, bool edgesOnly) const;
  void showPlaceInsertion(const std::optional<std::string>& before);
  void reorderPlace(const std::string& identity, const std::string& before);
  void pinFolders(const std::vector<std::string>& paths, const std::string& before);
  void updateDragPreview(Renderer& renderer, bool nativeIcon = false);
  void scheduleNativeDragPreview();
  void clearNativeDragPreview();
  void cancelFileGesture();
  std::uint32_t updateProtocolDrag(wl_surface* surface, float x, float y, std::uint32_t actions);
  NativeDataTransfer::DropReceiver receiveProtocolDrop(std::uint32_t action);
  void textPrompt(
      std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
  );
  void mountPassword(GMountOperation*, GAskPasswordFlags, std::string, std::string, unsigned);
  bool m_textPrompt = false;
  bool m_passwordPrompt = false;
  void openPlaceMenu(const filemanager::Place& place, float x, float y);
  void openPlacesVisibilityMenu(float x, float y);
  void showPlacesMenu(std::vector<ContextMenuControlEntry> entries, float x, float y);
  void setPlaceHidden(const filemanager::Place& place, bool hidden);
  bool m_showHiddenMounts = false;
  void operatePlace(const filemanager::Place& place, filemanager::PlaceOperation operation);
  void applyViewScale();
  void adjustViewScale(float value);
  void setGridMode(bool gridMode);
  std::optional<std::filesystem::path> dropDestination(float x, float y, bool incoming = false) const;
  void clearDragFeedback();
  struct SavedPlace {
    std::string id;
    std::filesystem::path path;
    std::string label;
  };
  std::vector<SavedPlace> m_savedPlaces;
  bool m_savedPlacesLoaded = false;
  bool m_reordering = false;
  std::vector<std::pair<std::string, Flex*>> m_placeInsertionLines;
  std::optional<filemanager::Place> m_placeMenuTarget;
  std::vector<std::pair<Button*, filemanager::Place>> m_placeRows;
  std::optional<std::string> m_reorderId;
  std::optional<std::string> m_reorderBefore;
  std::optional<std::string> m_pinBefore;
  float m_dragStartX = 0, m_dragStartY = 0;
  bool m_draggingFiles = false;
  bool m_dragPending = false;
  std::uint32_t m_dragPressSerial = 0;
  bool m_protocolDragInside = false;
  std::optional<std::filesystem::path> m_protocolDropDestination;
  std::vector<std::string> m_protocolDropSources;
  std::optional<std::size_t> m_dragIndex;
  std::vector<std::string> m_dragSources;
  std::unique_ptr<RenderTarget> m_nativePreviewTarget;
  wl_surface* m_nativePreviewSurface = nullptr;
  bool m_nativePreviewQueued = false;
  std::uint64_t m_nativePreviewGeneration = 0;
  Flex* m_dragPreview = nullptr;
  class FileIcon* m_dragPreviewIcon = nullptr;
  Label* m_dragPreviewLabel = nullptr;
  Label* m_dragPreviewAction = nullptr;
  float m_dragPointerX = 0, m_dragPointerY = 0;
  bool m_dragTargetValid = false;
  std::unordered_set<std::string> m_rubberBaseline;
  std::optional<std::filesystem::path> m_rubberSelectedPath;
  std::optional<std::filesystem::path> m_rubberAnchor;
  std::uint32_t m_rubberModifiers = 0;
  Button* m_dropPlace = nullptr;
  std::optional<std::size_t> m_dropIndex;
  float m_viewScale = 1.0F;
  bool m_viewScaleLoaded = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  std::unordered_set<std::string> m_deviceOperations;
  std::vector<std::shared_ptr<GMountOperation>> m_mountPrompts;
  void handlePlacesChanged();
  void requestPlaceCapacity(const PlaceCapacityView& place, bool forceRefresh = false);
  void refreshPlaceCapacities();
  void completePlaceCapacityQuery(
      const std::string& identity, const std::string& requestKey, GCancellable* cancellable, GFileInfo* info,
      bool cancelled
  );
  void cancelPlaceCapacityQueries();
  static void placeCapacityReady(GObject* source, GAsyncResult* result, gpointer data);
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
  void prepareProgressFrame(bool needsUpdate, bool needsLayout);
  void syncProgressWindow();
  void handleProgressKeyboard(const KeyboardEvent& event);
  void handleDialogKeyboard(const KeyboardEvent& event);
  struct ChecksumJob;
  struct ChecksumDialog {
    enum class StatusTone : std::uint8_t { Neutral, Error, Success, Mismatch };
    std::filesystem::path target;
    std::filesystem::path manifest;
    filemanager::checksum::Algorithm algorithm = filemanager::checksum::Algorithm::Sha256;
    bool verify = false;
    StatusTone statusTone = StatusTone::Neutral;
    std::shared_ptr<ChecksumJob> job;
    Timer progressTimer;
    Timer copyFeedbackTimer;
    std::array<Button*, 3> algorithms{};
    Button* browse = nullptr;
    Button* paste = nullptr;
    Button* run = nullptr;
    Button* copy = nullptr;
    Button* close = nullptr;
    Input* expectedInput = nullptr;
    Input* calculated = nullptr;
    Label* manifestLabel = nullptr;
    Label* warning = nullptr;
    Glyph* statusIcon = nullptr;
    Label* status = nullptr;
    class ProgressBar* progress = nullptr;
  };
  [[nodiscard]] std::optional<std::filesystem::path> checksumTarget() const;
  [[nodiscard]] std::array<ContextMenuControlEntry, 2> checksumMenuEntries() const;
  void showChecksumDialog(bool verify);
  void buildChecksumFields(float width);
  void setChecksumStatus(ChecksumDialog::StatusTone tone, std::string_view text);
  void chooseChecksumManifest();
  void pasteExpectedChecksum();
  void setChecksumInput(std::string_view value);
  void resetChecksumResult();
  void updateChecksumControls();
  void startChecksum();
  void stopChecksum();
  void dismissChecksum();

  void requestQuit(ToplevelSurface* parent = nullptr);
  void showDialog(
      std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
      std::function<void(int)> response, ToplevelSurface* parent = nullptr, bool applyToAllCheckbox = false,
      bool alternativeName = false
  );
  void closeDialog(int response);
  void prepareDialogFrame(bool needsUpdate, bool needsLayout);
  void buildDialog(float dialogWidth);
  void updateDialogTheme();
  std::vector<Flex*> m_dialogCards;
  void monitorDirectory();
  void closeMonitor();
  void cancelStatusFreeSpaceQuery();
  void queryStatusFreeSpaceAsync(const std::filesystem::path& path);
  void cancelSelectionInfoQuery();
  void querySelectionInfoAsync(const std::filesystem::path& path);
  void cancelDirectoryWritableQuery();
  void queryDirectoryWritableAsync(const std::filesystem::path& path);
  [[nodiscard]] bool currentDirectoryWritable() const;
  struct PlacePathResolution {
    bool exists = false;
    bool isDirectory = false;
    std::filesystem::path canonicalPath;
  };
  void cancelPlacePathResolutions();
  void resolvePlacePathAsync(
      const std::filesystem::path& path, std::function<void(const PlacePathResolution&)> onResolved = {},
      bool refreshSidebar = true
  );
  [[nodiscard]] bool samePersonalPlace(const std::filesystem::path& first, const std::filesystem::path& second) const;
  [[nodiscard]] std::optional<bool>
  sameDirectoryPathCached(const std::filesystem::path& first, const std::filesystem::path& second) const;
  void compareDirectoriesAsync(
      const std::filesystem::path& first, const std::filesystem::path& second, std::function<void(bool)> onResolved
  );
  struct DropValidation {
    std::vector<std::string> sources;
    std::filesystem::path rawDestination;
    std::optional<std::filesystem::path> target;
    bool copyByDefault = true;
  };
  [[nodiscard]] bool dragSourcesAreFolders() const;
  void cancelDropValidation();
  void requestHoverDropValidation(const std::vector<std::string>& sources, const std::filesystem::path& destination);
  void validateDropAsync(
      std::vector<std::string> sources, std::filesystem::path destination, std::uint64_t navigation,
      std::uint64_t dragSession, std::function<void(DropValidation)> onComplete
  );
  [[nodiscard]] std::vector<std::string> selectedPaths() const;
  [[nodiscard]] std::optional<std::pair<std::filesystem::path, bool>> symlinkSelection() const;
  [[nodiscard]] ContextMenuControlEntry newItemMenu() const;
  void duplicateSelection();
  void createSelectionSymlink();
  void showSymlinkDialog();
  [[nodiscard]] bool submitSymlinkDialog();
  void pasteClipboard();
  bool recordPasteCompletion(const FileOperationManager::Operation& operation);
  void applyPendingPasteSelection();
  void finishPendingTransferReveal(Renderer& renderer);
  void copyToDesktopClipboard(bool cut);
  void writeDesktopClipboard(std::string payload, const char* mime);
  void beginRename();
  void beginRenamePath(const std::filesystem::path& path);
  void cancelRename();
  void openContextMenu(std::size_t index, float x, float y);
  void openApplicationChooser();
  void openEmptyContextMenu(float x, float y);
  void syncSortControls();
  bool applyListColumnWidths();
  void positionFileColumnMarkers();
  void updateFileColumnHeaders();
  void persistFileColumnLayout();
  bool handleFileHeaderPointer(const PointerEvent& event, float x, float y);
  void cancelFileHeaderGesture();
  [[nodiscard]] float listColumnAvailableWidth() const;

  ConfigService m_configService;
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
  std::unique_ptr<SearchPickerPopup> m_applicationPicker;
  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<ToplevelSurface> m_progressSurface;
  std::unique_ptr<ToplevelSurface> m_dialogSurface;
  std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;
  std::unique_ptr<PropertiesWindow> m_propertiesWindow;
  InputDispatcher m_dispatcher;
  InputDispatcher m_progressDispatcher;
  InputDispatcher m_dialogDispatcher;
  Timer m_selectionTimer;
  Timer m_statusMessageTimer;
  std::chrono::steady_clock::time_point m_selectionTick;
  std::unique_ptr<Flex> m_root;
  std::unique_ptr<Flex> m_progressRoot;
  std::unique_ptr<Flex> m_dialogRoot;
  std::unique_ptr<ChecksumDialog> m_checksumDialog;
  std::uint64_t m_checksumInputRevision = 0;
  Label* m_progressLabel = nullptr;
  Button* m_progressCancelButton = nullptr;
  bool m_progressWindowClosed = false;
  Flex* m_breadcrumbBar = nullptr;
  Flex* m_sidebar = nullptr;
  Flex* m_sidebarDivider = nullptr;
  Flex* m_statusDivider = nullptr;
  Flex* m_statusStrip = nullptr;
  Flex* m_fileHeader = nullptr;
  std::optional<filemanager::FileColumn> m_resizingFileColumn;
  std::optional<filemanager::FileColumn> m_fileColumnHoverBoundary;
  float m_fileColumnDragX = 0.0F;
  filemanager::FileColumns m_fileColumnStart{};
  filemanager::FileColumns m_listColumns{};
  filemanager::FileColumns m_preferredListColumns{};
  filemanager::FileColumnLayout m_fileColumnLayout;
  std::vector<filemanager::FileColumn> m_visibleFileColumns;
  float m_listHeaderLeftGutter = -1.0F;
  float m_listHeaderRightGutter = -1.0F;
  struct FileColumnDrag {
    filemanager::FileColumn column;
    float x;
    float y;
    std::optional<std::size_t> destination;
  };
  std::optional<FileColumnDrag> m_fileColumnDrag;
  bool m_resizingStatus = false;
  float m_statusDragY = 0.0F;
  float m_statusDragHeight = 42.0F;
  float m_statusHeight = 42.0F;
  std::chrono::steady_clock::time_point m_statusMessageUntil{};
  std::string m_statusMessage;
  Flex* m_placesContent = nullptr;
  ScrollView* m_placesScroll = nullptr;
  std::unique_ptr<filemanager::PlacesModel> m_placesModel;
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
  Flex* m_appMenuItems = nullptr;
  Button* m_fileMenuButton = nullptr;
  std::array<Button*, 5> m_applicationMenuButtons{};
  Box* m_applicationMenuUnderline = nullptr;
  AnimationManager::Id m_menuUnderlineAnimation = 0;
  float m_menuUnderlineStartX = 0.0F;
  float m_menuUnderlineStartWidth = 0.0F;
  std::optional<std::size_t> m_applicationMenuOpen;
  std::optional<std::size_t> m_applicationMenuHovered;
  Button* m_compactMenuButton = nullptr;
  bool m_showApplicationMenuBar = true;
  bool m_applicationMenuCollapsed = false;
  std::array<Button*, filemanager::kFileColumns.size()> m_fileColumnHeaders{};
  std::array<Separator*, filemanager::kFileColumns.size()> m_fileColumnMarkers{};
  Separator* m_fileColumnDropMarker = nullptr;
  std::unique_ptr<Adapter> m_adapter;
  std::filesystem::path m_path;
  // URI tokens retain history/selection identity, never local filesystem paths.
  bool m_browsingTrash = false;
  std::optional<std::string> m_operationResultAfterRefresh;
  std::unordered_map<std::string, std::string> m_trashUris;
  std::unordered_map<std::string, std::string> m_trashOriginalPaths;
  std::vector<std::filesystem::path> m_backHistory;
  std::vector<std::filesystem::path> m_forwardHistory;
  std::vector<FileEntry> m_entries;
  std::vector<FileEntry> m_loadingEntries;
  struct FolderCount {
    std::uint64_t id;
    std::optional<std::uint64_t> items;
  };
  std::unordered_map<std::string, FolderCount> m_folderCounts;
  std::vector<std::pair<std::string, std::uint64_t>> m_pendingFolderCounts;
  std::uint64_t m_nextFolderCountId = 0;
  GCancellable* m_folderCountCancellable = nullptr;
  bool m_folderCountRunning = false;
  std::vector<PendingMonitorChange> m_pendingMonitorChanges;
  std::size_t m_pendingMonitorQueries = 0;
  std::vector<std::pair<Button*, std::filesystem::path>> m_placeButtons;
  std::vector<PlaceCapacityView> m_placeCapacities;
  std::unordered_map<std::string, CapacityInfo> m_capacityCache;
  std::unordered_map<std::string, std::string> m_placeFilesystemIds;
  std::unordered_map<std::string, GCancellable*> m_capacityQueries;
  bool m_placesDirty = false;
  bool m_placesRefreshCapacity = false;
  std::shared_ptr<int> m_placesAlive = std::make_shared<int>(0);
  std::optional<std::filesystem::path> m_selectedPath;
  std::optional<std::filesystem::path> m_contextMenuTarget;
  std::vector<std::shared_ptr<GAppInfo>> m_openWithApplications;
  std::vector<std::pair<filemanager::external_tools::Tool, filemanager::external_tools::Choice>> m_externalTools;
  std::vector<std::shared_ptr<GAppInfo>> m_applicationChooserApps;
  std::string m_applicationChooserMime;
  std::unordered_set<std::string> m_selectedPaths;
  std::optional<std::filesystem::path> m_selectionAnchor;
  // Clipboard transfers, drops and explicit creation commands share result selection.
  struct PasteRequest {
    std::filesystem::path destination;
    std::uint64_t navigation = 0;
    std::optional<std::uint64_t> dropScrollGeneration;
  };
  struct PasteSelection {
    PasteRequest request;
    std::vector<FileOperationManager::CompletedEntry> entries;
  };
  std::uint64_t m_navigationGeneration = 0;
  std::unordered_map<std::uint64_t, PasteRequest> m_pasteRequests;
  std::optional<PasteSelection> m_pendingPasteSelection;
  std::unordered_set<std::string> m_identityCheckedPaths;
  std::optional<std::filesystem::path> m_pendingPasteRevealPath;
  std::optional<std::uint64_t> m_pendingDropScrollGeneration;
  std::unordered_map<std::uint64_t, std::uint64_t> m_cutOperations;
  std::optional<std::uint64_t> m_consumedCutSelection;
  std::unordered_map<std::uint64_t, NativeDataTransfer::FinishDrop> m_dropFinishes;
  std::size_t m_pendingClipboardReads = 0;
  std::uint32_t m_keyboardModifiers = 0;
  std::uint32_t m_lastInputSerial = 0;
  std::optional<std::uint64_t> m_inlineRenameOperation;
  std::optional<std::filesystem::path> m_renamePath;
  std::optional<std::filesystem::path> m_pendingInlineRenamePath;
  InputArea* m_inlineEditArea = nullptr;
  bool m_deleteConfirmation = false;
  bool m_deletePermanentConfirmation = false;
  bool m_windowOpen = true;
  bool m_backgroundConflictPending = false;
  bool m_quitConfirmation = false;
  bool m_quitting = false;
  std::function<void(int)> m_dialogResponse;
  std::mutex m_conflictMutex;
  std::string m_dialogTitle;
  std::string m_dialogMessage;
  bool m_dialogWarningStyle = false;
  bool m_dialogResizable = false;
  std::string m_dialogSizeStateKey;
  std::uint32_t m_dialogPreferredWidth = 0;
  std::uint32_t m_dialogPreferredHeight = 0;
  std::array<InputArea*, 8> m_dialogResizeEdges{};
  InputArea* m_dialogMoveHandle = nullptr;
  Flex* m_dialogTitleRow = nullptr;
  Flex* m_dialogSpacer = nullptr;
  Flex* m_dialogFooter = nullptr;
  Node* m_dialogDivider = nullptr;
  Image* m_deleteWarning = nullptr;
  DialogBackgroundArtwork* m_dialogTopAccent = nullptr;
  DialogBackgroundArtwork* m_dialogCornerAccent = nullptr;
  std::string m_dialogComparisonSummary;
  std::vector<std::pair<std::string, int>> m_dialogChoices;
  bool m_dialogCloseQueued = false;
  bool m_dialogApplyToAllCheckbox = false;
  bool m_dialogApplyToAllChecked = false;
  bool m_dialogAlternativeNameEnabled = false;
  std::string m_dialogAlternativeName;
  std::filesystem::path m_dialogDestination;
  Input* m_dialogAlternativeNameInput = nullptr;
  std::optional<PasteRequest> m_symlinkDialogDestination;
  Input* m_symlinkDialogName = nullptr;
  Input* m_symlinkDialogTarget = nullptr;
  Label* m_symlinkDialogError = nullptr;
  int m_dialogResultPending = -1;
  GFileMonitor* m_monitor = nullptr;
  FileDialogSortField m_sortField = FileDialogSortField::Name;
  FileDialogSortOrder m_sortOrder = FileDialogSortOrder::Ascending;
  bool m_showHidden = false;
  bool m_foldersFirst = true;
  bool m_hiddenLast = true;
  bool m_gridMode = false;
  bool m_editingLocation = false;
  std::uint64_t m_locationEditGeneration = 0;
  bool m_refreshAfterRename = false;
  bool m_restoringSelection = false;
  bool m_loading = false;
  bool m_chooseFirstAfterLoad = true;
  std::optional<std::size_t> m_lastPointerIndex;
  std::uint32_t m_lastPointerClickTime = 0;
  std::uint32_t m_pointerClickTime = 0;
  bool m_running = true;
  GCancellable* m_enumerationCancellable = nullptr;
  std::uint64_t m_enumerationGeneration = 0;

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

  GCancellable* m_directoryWritableCancellable = nullptr;
  std::uint64_t m_directoryWritableGeneration = 0;
  std::filesystem::path m_directoryWritablePath;
  std::optional<bool> m_directoryWritable;

  std::function<bool(const std::filesystem::path&, std::function<void(std::optional<std::uint64_t>)>)>
      m_freeSpaceQueryHook;
  std::function<bool(const std::filesystem::path&, std::function<void(std::optional<SelectionInfo>)>)>
      m_selectionInfoQueryHook;
  std::function<bool(const std::filesystem::path&, std::function<void(bool)>)> m_directoryWritableQueryHook;

  std::unordered_map<std::string, PlacePathResolution> m_placePathCache;
  std::unordered_set<std::string> m_placePathPending;
  GCancellable* m_placePathCancellable = nullptr;
  std::uint64_t m_placePathGeneration = 0;
  std::function<bool(const std::filesystem::path&, std::function<void(PlacePathResolution)>)> m_placePathResolveHook;

  std::optional<DropValidation> m_dropValidation;
  std::optional<std::pair<std::vector<std::string>, std::filesystem::path>> m_dropValidationPending;
  GCancellable* m_dropValidationCancellable = nullptr;
  std::uint64_t m_dropValidationGeneration = 0;
  std::uint64_t m_dragSessionGeneration = 0;
  std::function<
      bool(const std::vector<std::string>&, const std::filesystem::path&, std::function<void(DropValidation)>)>
      m_dropValidationHook;
};
