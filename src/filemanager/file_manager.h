#pragma once

#include "app/main_loop.h"
#include "config/config_poll_source.h"
#include "config/config_service.h"
#include "core/files/directory_scanner.h"
#include "core/timer_manager.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/gio_poll_source.h"
#include "filemanager/places_model.h"
#include "net/http_client.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "theme/theme_service.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/virtual_grid_view.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

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
class Flex;
class Input;
class InputArea;
class Label;
class ScrollView;

class FileManager {
public:
  explicit FileManager(std::string initialPath);
  ~FileManager();
  int run();

private:
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
  void navigateTrash();
  [[nodiscard]] GFile* browsingFile() const;
  void fetchNext(GFileEnumerator* enumerator, std::uint64_t generation);
  void sortEntries();
  void restoreSelection(bool chooseFirst = false);
  void applyPendingMonitorChanges();
  void handleMonitorEvent(GFile* file, GFile* otherFile, GFileMonitorEvent event);
  void queryMonitoredFile(
      const std::filesystem::path& path, const std::filesystem::path& replacedPath = std::filesystem::path{}
  );
  void removeMonitoredFile(const std::filesystem::path& path);
  void activateSelected();
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
  void updateDragPreview(Renderer& renderer);
  void cancelFileGesture();
  void textPrompt(
      std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
  );
  void mountPassword(GMountOperation*, GAskPasswordFlags, std::string, std::string, unsigned);
  bool m_textPrompt = false;
  bool m_passwordPrompt = false;
  void openPlaceMenu(const filemanager::Place& place, float x, float y);
  void operatePlace(const filemanager::Place& place, filemanager::PlaceOperation operation);
  void applyViewScale();
  std::optional<std::filesystem::path> dropDestination(float x, float y) const;
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
  std::optional<std::size_t> m_dragIndex;
  std::vector<std::string> m_dragSources;
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
  void setLocationEditMode(bool editing);
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
  [[nodiscard]] std::vector<std::string> selectedPaths() const;
  void pasteClipboard();
  void copyToDesktopClipboard(bool cut);
  void acceptClipboardPayload(std::string payload, bool uriListFallback, bool kdeCutSelection = false);
  void readUriListClipboard();
  void beginRename();
  void beginRenamePath(const std::filesystem::path& path);
  void cancelRename();
  void openContextMenu(std::size_t index, float x, float y);
  void openEmptyContextMenu(float x, float y);
  void syncSortControls();

  ConfigService m_configService;
  HttpClient m_httpClient;
  nythera::theme::ThemeService m_themeService{m_configService, m_httpClient};
  ConfigPollSource m_configPollSource{m_configService};
  WaylandConnection m_wayland{WaylandConnection::Purpose::ApplicationClient};
  GlSharedContext m_sharedContext;
  RenderContext m_renderContext;
  GioPollSource m_gioPollSource;
  FileOperationManager m_operations;
  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<ToplevelSurface> m_progressSurface;
  std::unique_ptr<ToplevelSurface> m_dialogSurface;
  std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;
  InputDispatcher m_dispatcher;
  InputDispatcher m_progressDispatcher;
  InputDispatcher m_dialogDispatcher;
  Timer m_selectionTimer;
  Timer m_statusMessageTimer;
  std::chrono::steady_clock::time_point m_selectionTick;
  std::unique_ptr<Flex> m_root;
  std::unique_ptr<Flex> m_progressRoot;
  std::unique_ptr<Flex> m_dialogRoot;
  Label* m_progressLabel = nullptr;
  Button* m_progressCancelButton = nullptr;
  bool m_progressWindowClosed = false;
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
  Button* m_locationEditButton = nullptr;
  Button* m_backButton = nullptr;
  Button* m_forwardButton = nullptr;
  Button* m_upButton = nullptr;
  Button* m_homeButton = nullptr;
  Button* m_refreshButton = nullptr;
  Button* m_viewButton = nullptr;
  Button* m_hiddenButton = nullptr;
  Button* m_nameHeaderButton = nullptr;
  Button* m_sizeHeaderButton = nullptr;
  Button* m_modifiedHeaderButton = nullptr;
  std::unique_ptr<Adapter> m_adapter;
  std::filesystem::path m_path;
  // URI tokens retain history/selection identity, never local filesystem paths.
  bool m_browsingTrash = false;
  std::unordered_map<std::string, std::string> m_trashUris;
  std::unordered_map<std::string, std::string> m_trashOriginalPaths;
  std::vector<std::filesystem::path> m_backHistory;
  std::vector<std::filesystem::path> m_forwardHistory;
  std::vector<FileEntry> m_entries;
  std::vector<FileEntry> m_loadingEntries;
  std::vector<PendingMonitorChange> m_pendingMonitorChanges;
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
  std::vector<std::string> m_fileClipboard;
  std::unordered_set<std::string> m_selectedPaths;
  std::optional<std::filesystem::path> m_selectionAnchor;
  bool m_clipboardCut = false;
  std::size_t m_pendingClipboardReads = 0;
  std::uint32_t m_keyboardModifiers = 0;
  std::uint32_t m_lastInputSerial = 0;
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
  std::string m_dialogComparisonSummary;
  std::vector<std::pair<std::string, int>> m_dialogChoices;
  bool m_dialogCloseQueued = false;
  bool m_dialogApplyToAllCheckbox = false;
  bool m_dialogApplyToAllChecked = false;
  bool m_dialogAlternativeNameEnabled = false;
  std::string m_dialogAlternativeName;
  std::filesystem::path m_dialogDestination;
  Input* m_dialogAlternativeNameInput = nullptr;
  int m_dialogResultPending = -1;
  GFileMonitor* m_monitor = nullptr;
  FileDialogSortField m_sortField = FileDialogSortField::Name;
  FileDialogSortOrder m_sortOrder = FileDialogSortOrder::Ascending;
  bool m_showHidden = false;
  bool m_gridMode = false;
  bool m_editingLocation = false;
  bool m_refreshAfterRename = false;
  bool m_restoringSelection = false;
  bool m_loading = false;
  bool m_chooseFirstAfterLoad = true;
  std::optional<std::size_t> m_lastPointerIndex;
  std::uint32_t m_lastPointerClickTime = 0;
  std::uint32_t m_pointerClickTime = 0;
  bool m_running = true;
  std::uint64_t m_enumerationGeneration = 0;
};
