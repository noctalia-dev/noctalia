#include "filemanager/tab_manager.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace filemanager {

  TabManager::TabManager(std::filesystem::path initialPath, TabViewState initialViewState, Callbacks callbacks)
      : m_callbacks(std::move(callbacks)) {
    const TabId initialId = m_nextTabId++;
    auto tab = std::make_unique<BrowserTab>(
        initialId, initialPath.empty() ? std::filesystem::current_path() : std::move(initialPath), initialViewState,
        makeSessionCallbacks(initialId), [this](TabId changedId) {
          if (changedId == m_activeTabId && m_callbacks.onActiveStatusInfoChanged) {
            if (auto* active = findTab(changedId))
              m_callbacks.onActiveStatusInfoChanged(*active);
          }
        }
    );
    tab->active = true;
    tab->session.setActive(true);
    configureTabAdminSession(*tab);
    m_activeTabId = initialId;
    m_tabs.push_back(std::move(tab));
  }

  TabManager::~TabManager() { shutdownAll(); }

  TabId TabManager::openTab(
      const std::filesystem::path& path, const TabViewState& viewState, bool activate,
      std::optional<std::size_t> insertIndex, bool startNavigation, std::vector<std::filesystem::path> backHistory,
      std::vector<std::filesystem::path> forwardHistory
  ) {
    const TabId id = m_nextTabId++;
    std::filesystem::path targetPath = path;
    if (isTrashTabPath(targetPath))
      targetPath = "trash:/";
    else if (!targetPath.empty())
      targetPath = normalizeTabPath(targetPath);

    auto tab = std::make_unique<BrowserTab>(
        id, targetPath.empty() ? std::filesystem::current_path() : targetPath, viewState, makeSessionCallbacks(id),
        [this](TabId changedId) {
          if (changedId == m_activeTabId && m_callbacks.onActiveStatusInfoChanged) {
            if (auto* active = findTab(changedId))
              m_callbacks.onActiveStatusInfoChanged(*active);
          }
        }
    );
    tab->active = false;
    tab->session.setActive(false);
    configureTabAdminSession(*tab);
    if (!backHistory.empty() || !forwardHistory.empty())
      tab->session.setHistory(std::move(backHistory), std::move(forwardHistory));

    BrowserTab* tabPtr = tab.get();
    const std::size_t targetIndex = insertIndex ? std::min(*insertIndex, m_tabs.size()) : m_tabs.size();
    m_tabs.insert(m_tabs.begin() + static_cast<std::ptrdiff_t>(targetIndex), std::move(tab));

    if (activate) {
      BrowserTab* outgoing = findTab(m_activeTabId);
      const TabViewState previousViewState = outgoing != nullptr ? outgoing->viewState : viewState;
      if (outgoing != nullptr && outgoing != tabPtr) {
        if (m_callbacks.onBeforeTabSwitch)
          m_callbacks.onBeforeTabSwitch(*outgoing, *tabPtr);
        outgoing->active = false;
        outgoing->session.setActive(false);
      }
      m_activeTabId = id;
      tabPtr->active = true;
      tabPtr->session.setActive(true);
      if (m_callbacks.onActiveTabChanged)
        m_callbacks.onActiveTabChanged(*tabPtr, previousViewState);
      if (startNavigation)
        tabPtr->session.navigate(tabPtr->session.path(), false);
    } else if (startNavigation) {
      tabPtr->session.navigate(tabPtr->session.path(), false);
    }

    if (m_callbacks.onTabsChanged)
      m_callbacks.onTabsChanged();
    return id;
  }

  TabId TabManager::duplicateTab(TabId sourceTabId) {
    const BrowserTab* source = findTab(sourceTabId);
    const std::optional<std::size_t> sourceIndex = indexOf(sourceTabId);
    if (source == nullptr || !sourceIndex)
      return kInvalidTabId;
    return openTab(
        source->session.path(), source->viewState, true, *sourceIndex + 1, true, source->session.backHistory(),
        source->session.forwardHistory()
    );
  }

  bool TabManager::activateTab(TabId tabId) {
    if (tabId == m_activeTabId)
      return findTab(tabId) != nullptr;
    BrowserTab* incoming = findTab(tabId);
    BrowserTab* outgoing = findTab(m_activeTabId);
    if (incoming == nullptr || outgoing == nullptr)
      return false;

    const TabViewState previousViewState = outgoing->viewState;
    if (m_callbacks.onBeforeTabSwitch)
      m_callbacks.onBeforeTabSwitch(*outgoing, *incoming);

    outgoing->active = false;
    outgoing->session.setActive(false);

    m_activeTabId = incoming->id;
    incoming->active = true;
    incoming->session.setActive(true);

    if (m_callbacks.onActiveTabChanged)
      m_callbacks.onActiveTabChanged(*incoming, previousViewState);
    if (m_callbacks.onTabsChanged)
      m_callbacks.onTabsChanged();
    return true;
  }

  bool TabManager::activateRelative(int delta) {
    if (m_tabs.size() <= 1 || delta == 0)
      return false;
    const auto current = indexOf(m_activeTabId);
    if (!current)
      return false;
    const auto count = static_cast<std::ptrdiff_t>(m_tabs.size());
    const auto next = (static_cast<std::ptrdiff_t>(*current) + (delta % count) + count) % count;
    return activateTab(m_tabs[static_cast<std::size_t>(next)]->id);
  }

  bool TabManager::closeTab(TabId tabId) {
    const auto idxOpt = indexOf(tabId);
    if (!idxOpt)
      return false;
    const std::size_t idx = *idxOpt;
    if (m_tabs.size() <= 1) {
      if (m_callbacks.onRequestCloseLastTab)
        m_callbacks.onRequestCloseLastTab();
      return false;
    }

    BrowserTab* closingTab = m_tabs[idx].get();
    recordClosedTabSnapshot(*closingTab, idx);

    if (tabId == m_activeTabId) {
      const std::size_t replacementIdx = (idx + 1 < m_tabs.size()) ? (idx + 1) : (idx - 1);
      BrowserTab* replacement = m_tabs[replacementIdx].get();
      const TabViewState previousViewState = closingTab->viewState;
      if (m_callbacks.onBeforeTabSwitch)
        m_callbacks.onBeforeTabSwitch(*closingTab, *replacement);

      closingTab->active = false;
      closingTab->session.setActive(false);

      m_activeTabId = replacement->id;
      replacement->active = true;
      replacement->session.setActive(true);

      auto doomed = std::move(m_tabs[idx]);
      m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(idx));
      doomed->shutdown();
      doomed.reset();

      if (m_callbacks.onActiveTabChanged)
        m_callbacks.onActiveTabChanged(*replacement, previousViewState);
    } else {
      auto doomed = std::move(m_tabs[idx]);
      m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(idx));
      doomed->shutdown();
      doomed.reset();
    }

    if (m_callbacks.onTabsChanged)
      m_callbacks.onTabsChanged();
    return true;
  }

  bool TabManager::closeOtherTabs(TabId keepTabId) {
    if (findTab(keepTabId) == nullptr || m_tabs.size() <= 1)
      return false;
    if (m_activeTabId != keepTabId)
      activateTab(keepTabId);

    std::vector<std::unique_ptr<BrowserTab>> doomed;
    doomed.reserve(m_tabs.size() - 1);
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
      if (m_tabs[i]->id != keepTabId)
        recordClosedTabSnapshot(*m_tabs[i], i);
    }

    for (auto it = m_tabs.begin(); it != m_tabs.end();) {
      if ((*it)->id != keepTabId) {
        doomed.push_back(std::move(*it));
        it = m_tabs.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& tab : doomed)
      tab->shutdown();
    doomed.clear();

    if (m_callbacks.onTabsChanged)
      m_callbacks.onTabsChanged();
    return true;
  }

  bool TabManager::closeTabsToRight(TabId pivotTabId) {
    auto pivotIdx = indexOf(pivotTabId);
    if (!pivotIdx || *pivotIdx + 1 >= m_tabs.size())
      return false;

    if (const auto activeIdx = indexOf(m_activeTabId); activeIdx && *activeIdx > *pivotIdx)
      activateTab(pivotTabId);

    pivotIdx = indexOf(pivotTabId);
    if (!pivotIdx || *pivotIdx + 1 >= m_tabs.size())
      return false;

    std::vector<std::unique_ptr<BrowserTab>> doomed;
    for (std::size_t i = *pivotIdx + 1; i < m_tabs.size(); ++i)
      recordClosedTabSnapshot(*m_tabs[i], i);
    for (std::size_t i = *pivotIdx + 1; i < m_tabs.size(); ++i)
      doomed.push_back(std::move(m_tabs[i]));
    m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(*pivotIdx + 1), m_tabs.end());
    for (auto& tab : doomed)
      tab->shutdown();
    doomed.clear();

    if (m_callbacks.onTabsChanged)
      m_callbacks.onTabsChanged();
    return true;
  }

  TabId TabManager::reopenClosedTab() {
    if (m_closedTabs.empty())
      return kInvalidTabId;
    ClosedTabSnapshot snapshot = std::move(m_closedTabs.back());
    m_closedTabs.pop_back();
    return openTab(
        snapshot.path, snapshot.viewState, true, snapshot.index, true, std::move(snapshot.backHistory),
        std::move(snapshot.forwardHistory)
    );
  }

  bool TabManager::reorderTab(TabId tabId, std::size_t targetIndex) {
    if (m_tabs.size() <= 1)
      return false;
    const auto oldIdxOpt = indexOf(tabId);
    if (!oldIdxOpt)
      return false;
    const std::size_t oldIdx = *oldIdxOpt;
    const std::size_t clampedTarget = std::min(targetIndex, m_tabs.size() - 1);
    if (clampedTarget == oldIdx)
      return false;

    if (oldIdx < clampedTarget) {
      std::rotate(
          m_tabs.begin() + static_cast<std::ptrdiff_t>(oldIdx),
          m_tabs.begin() + static_cast<std::ptrdiff_t>(oldIdx + 1),
          m_tabs.begin() + static_cast<std::ptrdiff_t>(clampedTarget + 1)
      );
    } else {
      std::rotate(
          m_tabs.begin() + static_cast<std::ptrdiff_t>(clampedTarget),
          m_tabs.begin() + static_cast<std::ptrdiff_t>(oldIdx), m_tabs.begin() + static_cast<std::ptrdiff_t>(oldIdx + 1)
      );
    }

    if (m_callbacks.onTabsChanged)
      m_callbacks.onTabsChanged();
    return true;
  }

  void TabManager::shutdownAll() {
    for (auto& tab : m_tabs)
      tab->shutdown();
  }

  void TabManager::setAdminTransportLauncherForTesting(AdminTransportLauncher launcher) {
    m_adminTransportLauncherForTesting = std::move(launcher);
    for (auto& tab : m_tabs) {
      if (!tab->adminSession.isActive() && !tab->adminSession.isAuthenticating())
        tab->adminSession.setTransportLauncher(m_adminTransportLauncherForTesting);
    }
  }

  void TabManager::configureTabAdminSession(BrowserTab& tab) {
    const TabId tabId = tab.id;
    std::weak_ptr<bool> weakAlive = tab.alive;
    if (m_adminTransportLauncherForTesting)
      tab.adminSession.setTransportLauncher(m_adminTransportLauncherForTesting);
    tab.adminSession.setStateChangedCallback([this, tabId, weakAlive](const AdminSessionStatus& status) {
      const auto locked = weakAlive.lock();
      if (!locked || !*locked || status.tabId != tabId)
        return;
      auto* current = findTab(tabId);
      if (current == nullptr)
        return;
      if (m_callbacks.onTabMetadataChanged)
        m_callbacks.onTabMetadataChanged(tabId);
      if (m_callbacks.onTabAdminSessionChanged)
        m_callbacks.onTabAdminSessionChanged(*current, status);
    });
  }

  void
  TabManager::handleMountRemoved(const std::filesystem::path& mountPath, const std::filesystem::path& fallbackPath) {
    const auto normalizedMount = normalizeTabPath(mountPath);
    if (normalizedMount.empty())
      return;
    std::vector<TabId> affected;
    for (const auto& tab : m_tabs) {
      if (tab->session.browsingTrash())
        continue;
      const auto current = normalizeTabPath(tab->session.path());
      const auto rel = current.lexically_relative(normalizedMount);
      if (current == normalizedMount || (!rel.empty() && *rel.begin() != ".." && !rel.is_absolute()))
        affected.push_back(tab->id);
    }
    for (const TabId tabId : affected) {
      if (auto* tab = findTab(tabId))
        tab->session.navigate(fallbackPath, false);
    }
  }

  BrowserTab& TabManager::activeTab() noexcept {
    assert(!m_tabs.empty());
    if (auto* found = findTab(m_activeTabId))
      return *found;
    return *m_tabs.front();
  }

  const BrowserTab& TabManager::activeTab() const noexcept {
    assert(!m_tabs.empty());
    if (const auto* found = findTab(m_activeTabId))
      return *found;
    return *m_tabs.front();
  }

  BrowserTab* TabManager::findTab(TabId tabId) noexcept {
    for (const auto& tab : m_tabs) {
      if (tab->id == tabId)
        return tab.get();
    }
    return nullptr;
  }

  const BrowserTab* TabManager::findTab(TabId tabId) const noexcept {
    for (const auto& tab : m_tabs) {
      if (tab->id == tabId)
        return tab.get();
    }
    return nullptr;
  }

  std::optional<std::size_t> TabManager::indexOf(TabId tabId) const noexcept {
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
      if (m_tabs[i]->id == tabId)
        return i;
    }
    return std::nullopt;
  }

  void TabManager::recordClosedTabSnapshot(const BrowserTab& tab, std::size_t index) {
    if (m_closedTabs.size() >= kMaxClosedTabs)
      m_closedTabs.erase(m_closedTabs.begin());
    m_closedTabs.push_back(
        ClosedTabSnapshot{
            .path = tab.session.path(),
            .backHistory = tab.session.backHistory(),
            .forwardHistory = tab.session.forwardHistory(),
            .viewState = tab.viewState,
            .index = index,
        }
    );
  }

  DirectorySession::Callbacks TabManager::makeSessionCallbacks(TabId tabId) {
    return DirectorySession::Callbacks{
        .sortField =
            [this, tabId] {
              if (const auto* tab = findTab(tabId))
                return tab->viewState.sortField;
              return FileDialogSortField::Name;
            },
        .sortOrder =
            [this, tabId] {
              if (const auto* tab = findTab(tabId))
                return tab->viewState.sortOrder;
              return FileDialogSortOrder::Ascending;
            },
        .isSizeColumnVisible =
            [this] { return m_callbacks.isSizeColumnVisible ? m_callbacks.isSizeColumnVisible() : false; },
        .isRenaming = [this, tabId] { return m_callbacks.isRenaming ? m_callbacks.isRenaming(tabId) : false; },
        .onDeferRefreshDuringRename =
            [this, tabId] {
              if (m_callbacks.onDeferRefreshDuringRename)
                m_callbacks.onDeferRefreshDuringRename(tabId);
            },
        .onCancelFileGesture =
            [this, tabId] {
              if (tabId == m_activeTabId && m_callbacks.onCancelFileGesture)
                m_callbacks.onCancelFileGesture();
            },
        .onBeforeNavigate =
            [this, tabId](const std::filesystem::path& targetPath, bool pathChanged) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              tab->operationResultAfterRefresh.reset();
              tab->needsRefresh = false;
              tab->lastStatusMessage.clear();
              if (tabId == m_activeTabId && m_callbacks.onActiveBeforeNavigate)
                m_callbacks.onActiveBeforeNavigate(*tab, targetPath, pathChanged);
            },
        .onBeforePathChange =
            [this, tabId](const std::filesystem::path& newPath) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              tab->selectedPath.reset();
              tab->selectedPaths.clear();
              tab->selectionAnchor.reset();
              tab->pendingScrollOffsetAfterRefresh.reset();
              if (m_callbacks.onAnyBeforePathChange)
                m_callbacks.onAnyBeforePathChange(*tab, newPath);
              if (tabId == m_activeTabId && m_callbacks.onActiveBeforePathChange)
                m_callbacks.onActiveBeforePathChange(*tab, newPath);
            },
        .onAfterNavigatePathSet =
            [this, tabId](const std::filesystem::path& path, bool trash) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              if (m_callbacks.onInvalidateMountCapacities)
                m_callbacks.onInvalidateMountCapacities(path);
              if (m_callbacks.onTabMetadataChanged)
                m_callbacks.onTabMetadataChanged(tabId);
              if (tabId == m_activeTabId && m_callbacks.onActiveAfterNavigatePathSet)
                m_callbacks.onActiveAfterNavigatePathSet(*tab, path, trash);
            },
        .onEnumerateStarted =
            [this, tabId](const std::filesystem::path& path, bool trash) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              tab->cancelSelectionInfoQuery();
              if (m_callbacks.onAnyEnumerateStarted)
                m_callbacks.onAnyEnumerateStarted(*tab, path, trash);
              tab->lastStatusMessage = "Loading…";
              if (tabId == m_activeTabId) {
                if (!trash) {
                  tab->queryStatusFreeSpaceAsync(path);
                  if (!tab->hasActiveAdminSession() && m_callbacks.onResolvePathAsync)
                    m_callbacks.onResolvePathAsync(path);
                } else {
                  tab->cancelStatusFreeSpaceQuery();
                }
                if (m_callbacks.onActiveEnumerateStarted)
                  m_callbacks.onActiveEnumerateStarted(*tab, path, trash);
              } else {
                tab->cancelStatusFreeSpaceQuery();
              }
            },
        .onEntriesLoaded =
            [this, tabId](bool chooseFirst) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              if (!tab->selectedPaths.empty()) {
                const auto selected = tab->selectedPathsOrdered();
                tab->selectedPaths = std::unordered_set<std::string>(selected.begin(), selected.end());
              }
              if (tab->selectedPath
                  && std::ranges::find(tab->session.entries(), *tab->selectedPath, &FileEntry::absPath)
                      == tab->session.entries().end()) {
                tab->selectedPath.reset();
              }
              if (tab->selectionAnchor
                  && std::ranges::find(tab->session.entries(), *tab->selectionAnchor, &FileEntry::absPath)
                      == tab->session.entries().end()) {
                tab->selectionAnchor.reset();
              }
              if (tabId == m_activeTabId && m_callbacks.onActiveEntriesLoaded)
                m_callbacks.onActiveEntriesLoaded(*tab, chooseFirst);
            },
        .onAfterEntriesLoaded =
            [this, tabId] {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              if (m_callbacks.onApplyPendingPasteSelection)
                m_callbacks.onApplyPendingPasteSelection(*tab);
              if (tabId == m_activeTabId && m_callbacks.onTabMetadataChanged)
                m_callbacks.onTabMetadataChanged(tabId);
            },
        .onEntryMonitoredRenamed =
            [this, tabId](const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              if (tab->selectedPath == oldPath)
                tab->selectedPath = newPath;
              if (tab->selectionAnchor == oldPath)
                tab->selectionAnchor = newPath;
              if (tab->selectedPaths.erase(oldPath.string()))
                tab->selectedPaths.insert(newPath.string());
            },
        .onEntryMonitoredRemoved =
            [this, tabId](const std::filesystem::path& path) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              if (tab->selectedPath == path)
                tab->selectedPath.reset();
              tab->selectedPaths.erase(path.string());
              if (tab->selectionAnchor == path)
                tab->selectionAnchor.reset();
            },
        .onEntriesChanged =
            [this, tabId] {
              auto* tab = findTab(tabId);
              if (tab != nullptr && tabId == m_activeTabId && m_callbacks.onActiveEntriesChanged)
                m_callbacks.onActiveEntriesChanged(*tab);
            },
        .onEntryChangedAt =
            [this, tabId](std::size_t index) {
              auto* tab = findTab(tabId);
              if (tab != nullptr && tabId == m_activeTabId && m_callbacks.onActiveEntryChangedAt)
                m_callbacks.onActiveEntryChangedAt(*tab, index);
            },
        .onMonitorQueryCompleted =
            [this, tabId] {
              auto* tab = findTab(tabId);
              if (tab != nullptr && tabId == m_activeTabId && m_callbacks.onActiveMonitorQueryCompleted)
                m_callbacks.onActiveMonitorQueryCompleted(*tab);
            },
        .onBackgroundMonitorDirty =
            [this, tabId] {
              if (auto* tab = findTab(tabId))
                tab->needsRefresh = true;
            },
        .onStatus =
            [this, tabId](std::string text) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              tab->lastStatusMessage = text;
              if (tabId == m_activeTabId && m_callbacks.onActiveStatus)
                m_callbacks.onActiveStatus(*tab, std::move(text));
            },
        .onEnumerateFailed =
            [this, tabId](std::string detail) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              std::string msg = tab->operationResultAfterRefresh
                  ? *tab->operationResultAfterRefresh + "; refresh failed: " + detail
                  : detail;
              tab->operationResultAfterRefresh.reset();
              tab->lastStatusMessage = msg;
              if (tabId == m_activeTabId && m_callbacks.onActiveStatus)
                m_callbacks.onActiveStatus(*tab, std::move(msg));
            },
        .onEnumerateCompletedStatus =
            [this, tabId](bool empty) {
              auto* tab = findTab(tabId);
              if (tab == nullptr)
                return;
              std::string msg = tab->operationResultAfterRefresh.value_or(empty ? "This folder is empty" : "");
              tab->operationResultAfterRefresh.reset();
              tab->lastStatusMessage = msg;
              if (tabId == m_activeTabId && m_callbacks.onActiveStatus)
                m_callbacks.onActiveStatus(*tab, std::move(msg));
            },
    };
  }

} // namespace filemanager
