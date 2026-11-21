#pragma once

#include "filemanager/dialogs/checksum_dialog.h"
#include "filemanager/dialogs/help_dialog.h"
#include "filemanager/directory_session.h"
#include "filemanager/external_tools.h"
#include "filemanager/file_list_header.h"
#include "filemanager/file_manager_settings.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/places_sidebar.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/dialogs/search_picker_popup.h"
#include "wayland/native_data_transfer.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class Box;
class Button;
class ConfigService;
class FileManagerTestAccess;
class Flex;
class LayerPopupHostRegistry;
class RenderContext;

namespace filemanager {

  namespace menu_id {
    inline constexpr int kManualSymlink = 117;
    inline constexpr int kSelectionSymlink = 118;
    inline constexpr int kDuplicate = 119;
    inline constexpr int kSelectAll = 120;
    inline constexpr int kDeselectAll = 121;
    inline constexpr int kInvertSelection = 122;
    inline constexpr int kUndo = 123;
    inline constexpr int kRedo = 124;
    inline constexpr int kCompactEditMenu = 125;
    inline constexpr int kCompactViewMenu = 126;
    inline constexpr int kListView = 127;
    inline constexpr int kGridView = 128;
    inline constexpr int kShowColumns = 129;
    inline constexpr int kZoomIn = 130;
    inline constexpr int kZoomOut = 131;
    inline constexpr int kResetZoom = 132;
    inline constexpr int kFirstColumnAction = 133;
    inline constexpr int kShowMenuBar = 30008;
    inline constexpr int kCompactToolsMenu = 30100;
    inline constexpr int kFirstToolMenuRow = 30101;
    inline constexpr int kCalculateChecksum = ChecksumDialogController::kCalculateMenuId;
    inline constexpr int kVerifyChecksum = ChecksumDialogController::kVerifyMenuId;
    inline constexpr int kCompactHelpMenu = 30200;
    inline constexpr int kKeyboardShortcuts = HelpDialogController::kKeyboardShortcutsMenuId;
    inline constexpr int kReportBug = HelpDialogController::kReportBugMenuId;
    inline constexpr int kGitHub = HelpDialogController::kGitHubMenuId;
    inline constexpr int kAboutFiles = HelpDialogController::kAboutMenuId;
    inline constexpr int kFirstExternalToolApp = 40000;
    inline constexpr std::size_t kToolsMenuIndex = 3;
    inline constexpr std::size_t kHelpMenuIndex = 4;
  } // namespace menu_id

  class FileMenuController {
  public:
    struct Callbacks {
      std::function<ToplevelSurface*()> mainSurface;
      std::function<std::uint32_t()> lastInputSerial;
      std::function<void(std::uint32_t)> setLastInputSerial;
      std::function<bool()> isModalOpen;
      std::function<bool()> isRenaming;
      std::function<std::size_t()> pendingClipboardReads;
      std::function<bool()> canChangeSelection;
      std::function<std::optional<std::filesystem::path>()> selectedPath;
      std::function<const std::unordered_set<std::string>&()> selectedPaths;
      std::function<std::vector<std::string>()> selectedPathsOrdered;
      std::function<std::optional<std::pair<std::filesystem::path, bool>>()> symlinkSelection;
      std::function<std::array<ContextMenuControlEntry, 2>()> checksumMenuEntries;
      std::function<void(std::size_t index)> onSelectSingleForContextMenu;
      std::function<void()> onClearSelectionForEmptyMenu;
      std::function<void(std::string)> onStatus;
      std::function<void()> onRequestLayout;
    };

    FileMenuController(
        WaylandConnection& wayland, RenderContext& renderContext, AnimationManager& menuAnimations,
        InputDispatcher& dispatcher, FileManagerSettings& settings, FileOperationManager& operations,
        DirectorySession& session, FileListHeaderController& listHeader, PlacesSidebarController& places,
        ChecksumDialogController& checksumDialog, NativeDataTransfer& transfer, Callbacks callbacks
    );
    ~FileMenuController();

    void loadInitialPreference();
    void initializePopups(
        ConfigService& configService, LayerPopupHostRegistry& pickerHosts,
        std::function<void(const ContextMenuControlEntry&)> onActivate
    );
    void attachMenuBarUi(Flex& appBar, float toolbarHeight);
    void detachUi();
    void closeAll();
    void closeContextMenu();
    void closeApplicationPicker();

    [[nodiscard]] bool isContextMenuOpen() const;
    [[nodiscard]] bool isApplicationPickerOpen() const;
    [[nodiscard]] bool hasOpenPopup() const;

    bool handleAppPickerPointer(const PointerEvent& event);
    bool handlePointer(const PointerEvent& event);
    bool handleKeyboard(const KeyboardEvent& event);

    void syncResponsiveMenuBar(int surfaceWidth);
    void toggleMenuBarVisibility();
    void updateApplicationMenuUnderline();

    bool openApplicationMenu(float x, float y, std::size_t menuIndex = 0);
    void openContextMenu(std::size_t index, float x, float y);
    void openEmptyContextMenu(float x, float y);
    void showPlacesMenu(std::vector<ContextMenuControlEntry> entries, float x, float y);
    void openColumnHeaderMenu(std::vector<ContextMenuControlEntry> entries, float x, float y);
    void openApplicationChooser();

    [[nodiscard]] ContextMenuControlEntry newItemMenu() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> fileMenuEntries() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> editMenuEntries() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> viewMenuEntries() const;
    [[nodiscard]] std::vector<ContextMenuControlEntry> toolsMenuEntries();
    [[nodiscard]] std::vector<ContextMenuControlEntry> helpMenuEntries() const;
    [[nodiscard]] std::optional<std::vector<ContextMenuControlEntry>> applicationMenuEntries(std::size_t menuIndex);

    bool launchOpenWithApp(std::size_t index);
    bool launchExternalTool(std::size_t index);

    [[nodiscard]] const std::optional<std::filesystem::path>& contextMenuTarget() const noexcept {
      return m_contextMenuTarget;
    }
    void setContextMenuTarget(std::optional<std::filesystem::path> target) { m_contextMenuTarget = std::move(target); }

    [[nodiscard]] static bool
    launchApplicationForFile(GAppInfo* app, const std::filesystem::path& path, GError** error);

  private:
    friend class ::FileManagerTestAccess;

    [[nodiscard]] std::vector<std::string> externalToolTargets(external_tools::Tool tool) const;
    void persistApplicationMenuPreference();

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    AnimationManager& m_menuAnimations;
    InputDispatcher& m_dispatcher;
    FileManagerSettings& m_settings;
    FileOperationManager& m_operations;
    DirectorySession& m_session;
    FileListHeaderController& m_listHeader;
    PlacesSidebarController& m_places;
    ChecksumDialogController& m_checksumDialog;
    NativeDataTransfer& m_transfer;
    Callbacks m_callbacks;

    std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;
    std::unique_ptr<SearchPickerPopup> m_applicationPicker;

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

    std::optional<std::filesystem::path> m_contextMenuTarget;
    std::vector<std::shared_ptr<GAppInfo>> m_openWithApplications;
    std::vector<std::pair<external_tools::Tool, external_tools::Choice>> m_externalTools;
    std::vector<std::shared_ptr<GAppInfo>> m_applicationChooserApps;
    std::string m_applicationChooserMime;
  };

} // namespace filemanager
