#pragma once

#include "filemanager/admin_session_client.h"
#include "filemanager/directory_session.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/tab_presentation.h"
#include "ui/controls/scroll_view.h"

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

class FileManagerTestAccess;

namespace filemanager {

  struct SelectionInfo {
    std::filesystem::path path;
    std::string type;
    std::string target;
    bool symlink = false;
    bool hasSize = false;
    std::uint64_t size = 0;
  };

  struct PasteRequest {
    TabId tabId = kInvalidTabId;
    std::filesystem::path destination;
    std::uint64_t navigation = 0;
    std::optional<std::uint64_t> dropScrollGeneration;
  };

  struct PasteSelection {
    PasteRequest request;
    std::vector<FileOperationManager::CompletedEntry> entries;
  };

  class BrowserTab {
  public:
    using FreeSpaceQueryHook =
        std::function<bool(const std::filesystem::path&, std::function<void(std::optional<std::uint64_t>)>)>;
    using SelectionInfoQueryHook =
        std::function<bool(const std::filesystem::path&, std::function<void(std::optional<SelectionInfo>)>)>;

    BrowserTab(
        TabId tabId, std::filesystem::path initialPath, TabViewState initialViewState,
        DirectorySession::Callbacks sessionCallbacks, std::function<void(TabId)> onStatusInfoChanged
    );
    ~BrowserTab();

    BrowserTab(const BrowserTab&) = delete;
    BrowserTab& operator=(const BrowserTab&) = delete;
    BrowserTab(BrowserTab&&) = delete;
    BrowserTab& operator=(BrowserTab&&) = delete;

    void shutdown();

    [[nodiscard]] TabCapabilities capabilities() const noexcept;
    [[nodiscard]] bool hasActiveAdminSession() const noexcept { return adminSession.isActive(); }
    [[nodiscard]] AdminSessionState adminSessionState() const noexcept { return adminSession.state(); }
    void leaveAdministratorMode();
    [[nodiscard]] std::vector<std::string> selectedPathsOrdered() const;
    [[nodiscard]] std::optional<std::pair<std::filesystem::path, bool>> symlinkSelection() const;

    void cancelStatusFreeSpaceQuery();
    void queryStatusFreeSpaceAsync(const std::filesystem::path& path);
    void cancelSelectionInfoQuery();
    void querySelectionInfoAsync(const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& statusFreeSpacePath() const noexcept { return m_statusFreeSpacePath; }
    [[nodiscard]] const std::optional<std::uint64_t>& freeSpaceBytes() const noexcept { return m_statusFreeSpaceBytes; }
    [[nodiscard]] const std::optional<SelectionInfo>& selectionInfo() const noexcept { return m_selectionInfo; }
    [[nodiscard]] const std::filesystem::path& selectionInfoPendingPath() const noexcept {
      return m_selectionInfoPendingPath;
    }

    TabId id = kInvalidTabId;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    DirectorySession session;
    AdminSessionClient adminSession;
    TabViewState viewState;
    ScrollViewState scrollState;
    std::optional<float> pendingScrollOffsetAfterRefresh;

    std::optional<std::filesystem::path> selectedPath;
    std::unordered_set<std::string> selectedPaths;
    std::optional<std::filesystem::path> selectionAnchor;

    std::optional<PasteSelection> pendingPasteSelection;
    std::unordered_set<std::string> identityCheckedPaths;
    std::optional<std::filesystem::path> pendingPasteRevealPath;
    std::optional<std::uint64_t> pendingDropScrollGeneration;
    bool pendingRevealOnActivate = false;

    std::optional<std::string> operationResultAfterRefresh;
    std::string lastStatusMessage;
    bool needsRefresh = false;
    bool active = false;
    bool administratorBrowsing = false;

  private:
    friend class ::FileManagerTestAccess;

    std::function<void(TabId)> m_onStatusInfoChanged;

    GCancellable* m_statusFreeSpaceCancellable = nullptr;
    AdminRequestId m_statusFreeSpaceAdminReqId = kInvalidAdminRequestId;
    std::uint64_t m_statusFreeSpaceGeneration = 0;
    std::filesystem::path m_statusFreeSpacePath;
    std::optional<std::uint64_t> m_statusFreeSpaceBytes;

    GCancellable* m_selectionInfoCancellable = nullptr;
    AdminRequestId m_selectionInfoAdminReqId = kInvalidAdminRequestId;
    std::uint64_t m_selectionInfoGeneration = 0;
    std::filesystem::path m_selectionInfoPendingPath;
    std::optional<SelectionInfo> m_selectionInfo;

    FreeSpaceQueryHook m_freeSpaceQueryHook;
    SelectionInfoQueryHook m_selectionInfoQueryHook;
  };

} // namespace filemanager
