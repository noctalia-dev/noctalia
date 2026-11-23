#pragma once

#include "filemanager/file_manager_settings.h"
#include "filemanager/places_model.h"
#include "ui/controls/context_menu_popup.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Button;
class FileManagerTestAccess;
class FileOperationManager;
class Flex;
class Label;
class ProgressBar;
class ScrollView;
class ToplevelSurface;

namespace filemanager {

  struct PlacePathResolution {
    bool exists = false;
    bool isDirectory = false;
    std::filesystem::path canonicalPath;
  };

  class PlacesSidebarController {
  public:
    struct Callbacks {
      std::function<std::filesystem::path()> currentPath;
      std::function<bool()> isBrowsingTrash;
      std::function<bool()> isQuitting;
      std::function<float()> sidebarWidth;
      std::function<bool()> hasPendingClipboardOrRename;
      std::function<void(const std::filesystem::path&, bool recordHistory)> onNavigate;
      std::function<void(std::vector<ContextMenuControlEntry>, float x, float y)> onShowMenu;
      std::function<void(std::uint32_t serial)> onInputSerial;
      std::function<void(std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)>)>
          onTextPrompt;
      std::function<void(
          std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
          std::function<void(int)> response
      )>
          onShowDialog;
      std::function<void(std::string)> onStatus;
      std::function<void()> onUpdateNavigationControls;
      std::function<void()> onRequestLayout;
      std::function<void()> onClearDropPlace;
      std::function<void(const std::filesystem::path& mountPath)> onMountRemoved;
      std::function<void(const Place& place)> onOpenPlaceInBackgroundTab;
    };

    PlacesSidebarController(FileManagerSettings& settings, FileOperationManager& operations, Callbacks callbacks);
    ~PlacesSidebarController();

    void attachUi(ScrollView* scroll, Flex* content);
    void detachUi();
    void shutdown();

    void markDirty(bool refreshCapacity = false);
    [[nodiscard]] bool isDirty() const noexcept { return m_dirty; }
    void rebuild();

    void savePlace(const std::filesystem::path& path, bool verifiedDirectory = false);
    void removeSavedPlace(std::string_view identity);
    void persistSavedPlaces();
    [[nodiscard]] bool isPersonalPlaceSaved(const std::filesystem::path& path) const;

    [[nodiscard]] std::optional<std::string>
    placeInsertionAt(float x, float y, std::string_view group, bool edgesOnly) const;
    void showPlaceInsertion(const std::optional<std::string>& before);
    void reorderPlace(const std::string& identity, const std::string& before);
    void pinFolders(const std::vector<std::string>& paths, const std::string& before, bool verifiedDirectories = false);

    void openPlaceMenu(const Place& place, float x, float y);
    void openPlacesVisibilityMenu(float x, float y);
    void setPlaceHidden(const Place& place, bool hidden);
    void toggleShowHiddenMounts();
    void operatePlace(const Place& place, PlaceOperation operation);

    void refreshCapacities();
    void cancelCapacityQueries();
    void updateCapacityBarWidths(float sidebarWidth);
    void updateActiveHighlight(const std::filesystem::path& currentPath, bool browsingTrash);
    void invalidateMountCapacitiesForPath(const std::filesystem::path& path);

    void cancelPathResolutions();
    void resolvePathAsync(
        const std::filesystem::path& path, std::function<void(const PlacePathResolution&)> onResolved = {},
        bool refreshSidebar = true
    );
    void cacheResolvedPath(const std::filesystem::path& path, const PlacePathResolution& resolved);
    [[nodiscard]] bool samePersonalPlace(const std::filesystem::path& first, const std::filesystem::path& second) const;
    [[nodiscard]] std::optional<bool>
    sameDirectoryPathCached(const std::filesystem::path& first, const std::filesystem::path& second) const;
    void compareDirectoriesAsync(
        const std::filesystem::path& first, const std::filesystem::path& second, std::function<void(bool)> onResolved
    );

    [[nodiscard]] std::optional<std::filesystem::path> dropDestinationAt(float x, float y) const;
    [[nodiscard]] std::optional<std::size_t> rowIndexAt(float x, float y) const;
    [[nodiscard]] bool hasActiveDeviceOperations() const noexcept { return !m_deviceOperations.empty(); }
    [[nodiscard]] const std::optional<Place>& placeMenuTarget() const noexcept { return m_placeMenuTarget; }
    [[nodiscard]] const std::vector<std::pair<Button*, Place>>& placeRows() const noexcept { return m_placeRows; }
    [[nodiscard]] ScrollView* scrollView() const noexcept { return m_scroll; }

  private:
    friend class ::FileManagerTestAccess;

    struct PlaceCapacityView {
      std::string identity;
      std::string capacityIdentity;
      std::filesystem::path mountPath;
      Label* freeSpaceLabel = nullptr;
      ProgressBar* bar = nullptr;
      Button* row = nullptr;
    };

    struct CapacityInfo {
      std::uint64_t total = 0;
      std::uint64_t free = 0;
      std::string filesystemType;
      std::string filesystemId;
      std::chrono::steady_clock::time_point updatedAt;
    };

    void mountPassword(GMountOperation* op, GAskPasswordFlags flags, std::string user, std::string domain, unsigned step);
    void requestPlaceCapacity(const PlaceCapacityView& place, bool forceRefresh = false);
    void completePlaceCapacityQuery(
        const std::string& identity, const std::string& requestKey, GCancellable* cancellable, GFileInfo* info,
        bool cancelled
    );
    static void placeCapacityReady(GObject* source, GAsyncResult* result, gpointer data);

    FileManagerSettings& m_settings;
    FileOperationManager& m_operations;
    Callbacks m_callbacks;

    std::unique_ptr<PlacesModel> m_model;
    ScrollView* m_scroll = nullptr;
    Flex* m_content = nullptr;

    std::vector<SavedPlace> m_savedPlaces;
    bool m_savedPlacesLoaded = false;
    bool m_showHiddenMounts = false;
    bool m_dirty = false;
    bool m_refreshCapacity = false;

    std::vector<std::pair<std::string, Flex*>> m_insertionLines;
    std::optional<Place> m_placeMenuTarget;
    std::vector<std::pair<Button*, Place>> m_placeRows;
    std::vector<std::pair<Button*, std::filesystem::path>> m_placeButtons;
    std::vector<PlaceCapacityView> m_placeCapacities;
    std::unordered_map<std::string, CapacityInfo> m_capacityCache;
    std::unordered_map<std::string, std::string> m_placeFilesystemIds;
    std::unordered_map<std::string, GCancellable*> m_capacityQueries;
    std::unordered_set<std::string> m_deviceOperations;
    std::vector<std::shared_ptr<GMountOperation>> m_mountPrompts;

    std::unordered_map<std::string, PlacePathResolution> m_placePathCache;
    std::unordered_set<std::string> m_placePathPending;
    GCancellable* m_placePathCancellable = nullptr;
    std::uint64_t m_placePathGeneration = 0;
    std::function<bool(const std::filesystem::path&, std::function<void(PlacePathResolution)>)> m_placePathResolveHook;

    std::shared_ptr<int> m_placesAlive = std::make_shared<int>(0);
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
