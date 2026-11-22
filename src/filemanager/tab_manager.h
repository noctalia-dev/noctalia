#pragma once

#include "filemanager/browser_tab.h"
#include "filemanager/tab_presentation.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class FileManagerTestAccess;

namespace filemanager {

  struct ClosedTabSnapshot {
    std::filesystem::path path;
    std::vector<std::filesystem::path> backHistory;
    std::vector<std::filesystem::path> forwardHistory;
    TabViewState viewState;
    std::size_t index = 0;
  };

  class TabManager {
  public:
    static constexpr std::size_t kMaxClosedTabs = 16;

    struct Callbacks {
      std::function<void(BrowserTab& outgoing, BrowserTab& incoming)> onBeforeTabSwitch;
      std::function<void(BrowserTab& active, const TabViewState& previousViewState)> onActiveTabChanged;
      std::function<void()> onTabsChanged;
      std::function<void(TabId tabId)> onTabMetadataChanged;
      std::function<void()> onRequestCloseLastTab;

      std::function<bool()> isSizeColumnVisible;
      std::function<bool(TabId tabId)> isRenaming;
      std::function<void(TabId tabId)> onDeferRefreshDuringRename;
      std::function<void()> onCancelFileGesture;

      std::function<void(BrowserTab& active, const std::filesystem::path& targetPath, bool pathChanged)>
          onActiveBeforeNavigate;
      std::function<void(BrowserTab& active, const std::filesystem::path& newPath)> onActiveBeforePathChange;
      std::function<void(BrowserTab& active, const std::filesystem::path& path, bool trash)>
          onActiveAfterNavigatePathSet;
      std::function<void(BrowserTab& active, const std::filesystem::path& path, bool trash)> onActiveEnumerateStarted;
      std::function<void(BrowserTab& active, bool chooseFirst)> onActiveEntriesLoaded;
      std::function<void(BrowserTab& active)> onActiveEntriesChanged;
      std::function<void(BrowserTab& active, std::size_t index)> onActiveEntryChangedAt;
      std::function<void(BrowserTab& active)> onActiveMonitorQueryCompleted;
      std::function<void(BrowserTab& active, std::string status)> onActiveStatus;
      std::function<void(BrowserTab& active)> onActiveStatusInfoChanged;
      std::function<void(BrowserTab& tab)> onApplyPendingPasteSelection;

      std::function<void(BrowserTab& tab, const std::filesystem::path& newPath)> onAnyBeforePathChange;
      std::function<void(BrowserTab& tab, const std::filesystem::path& path, bool trash)> onAnyEnumerateStarted;
      std::function<void(BrowserTab& tab, const AdminSessionStatus& status)> onTabAdminSessionChanged;
      std::function<void(const std::filesystem::path& path)> onInvalidateMountCapacities;
      std::function<void(const std::filesystem::path& path)> onResolvePathAsync;
    };

    TabManager(std::filesystem::path initialPath, TabViewState initialViewState, Callbacks callbacks);
    ~TabManager();

    TabManager(const TabManager&) = delete;
    TabManager& operator=(const TabManager&) = delete;
    TabManager(TabManager&&) = delete;
    TabManager& operator=(TabManager&&) = delete;

    TabId openTab(
        const std::filesystem::path& path, const TabViewState& viewState, bool activate = true,
        std::optional<std::size_t> insertIndex = std::nullopt, bool startNavigation = true,
        std::vector<std::filesystem::path> backHistory = {}, std::vector<std::filesystem::path> forwardHistory = {}
    );
    TabId duplicateTab(TabId sourceTabId);
    bool activateTab(TabId tabId);
    bool activateRelative(int delta);
    bool closeTab(TabId tabId);
    bool closeOtherTabs(TabId keepTabId);
    bool closeTabsToRight(TabId pivotTabId);
    TabId reopenClosedTab();
    bool reorderTab(TabId tabId, std::size_t targetIndex);

    void shutdownAll();
    void setAdminTransportLauncherForTesting(AdminTransportLauncher launcher);
    void handleMountRemoved(const std::filesystem::path& mountPath, const std::filesystem::path& fallbackPath);

    [[nodiscard]] bool canReopenClosedTab() const noexcept { return !m_closedTabs.empty(); }
    [[nodiscard]] const std::vector<ClosedTabSnapshot>& closedTabs() const noexcept { return m_closedTabs; }

    [[nodiscard]] std::size_t count() const noexcept { return m_tabs.size(); }
    [[nodiscard]] TabId activeTabId() const noexcept { return m_activeTabId; }
    [[nodiscard]] BrowserTab& activeTab() noexcept;
    [[nodiscard]] const BrowserTab& activeTab() const noexcept;
    [[nodiscard]] BrowserTab* findTab(TabId tabId) noexcept;
    [[nodiscard]] const BrowserTab* findTab(TabId tabId) const noexcept;
    [[nodiscard]] std::optional<std::size_t> indexOf(TabId tabId) const noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<BrowserTab>>& tabs() const noexcept { return m_tabs; }

  private:
    friend class ::FileManagerTestAccess;

    [[nodiscard]] DirectorySession::Callbacks makeSessionCallbacks(TabId tabId);
    void configureTabAdminSession(BrowserTab& tab);
    void recordClosedTabSnapshot(const BrowserTab& tab, std::size_t index);

    Callbacks m_callbacks;
    AdminTransportLauncher m_adminTransportLauncherForTesting;
    std::vector<std::unique_ptr<BrowserTab>> m_tabs;
    TabId m_activeTabId = kInvalidTabId;
    TabId m_nextTabId = 1;
    std::vector<ClosedTabSnapshot> m_closedTabs;
  };

} // namespace filemanager
