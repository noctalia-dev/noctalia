#include "filemanager/file_manager.h"

#include "app/timer_poll_source.h"
#include "core/deferred_call.h"
#include "core/files/display_text.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_breadcrumbs.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_creation.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_sort.h"
#include "filemanager/file_transfer_payload.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/folder_size.h"
#include "filemanager/permanent_delete_prompt.h"
#include "filemanager/property_operations.h"
#include "filemanager/startup_path.h"
#include "i18n/i18n_service.h"
#include "render/core/renderer.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/dialogs/dialog_background_artwork.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"
#include "ui/dialogs/file_icon.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstring>
#include <future>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <sstream>
#include <sys/stat.h>
#include <tuple>
#include <utility>
#include <wayland-client-protocol.h>
#include <xdg-shell-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  using namespace filemanager::menu_id;

  using filemanager::columnIndex;
  using filemanager::FileColumn;

} // namespace

class FileManager::Adapter final : public VirtualGridAdapter {
public:
  explicit Adapter(FileManager& owner) : m_owner(owner) {}
  std::size_t itemCount() const override { return m_owner.activeSession().entries().size(); }
  std::optional<bool> selectedState(std::size_t index) const override {
    const auto& tab = m_owner.activeTab();
    if (index >= tab.session.entries().size())
      return false;
    return tab.selectedPaths.contains(tab.session.entries()[index].absPath.string());
  }
  std::unique_ptr<Node> createTile() override {
    if (m_owner.m_listHeader.gridMode()) {
      auto tile = std::make_unique<FileEntryTile>(
          1.0F, &m_owner.m_pickerThumbnails, filemanager::fileViewMetrics(m_owner.m_listHeader.viewScale()).gridIcon
      );
      tile->setSelectionRoles(ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer);
      return tile;
    }
    auto row = std::make_unique<FileEntryRow>(
        1.0F, filemanager::fileViewMetrics(m_owner.m_listHeader.viewScale()).listIcon, &m_owner.m_pickerThumbnails
    );
    row->setSelectionRoles(ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer);
    return row;
  }
  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    const auto& tab = m_owner.activeTab();
    auto& session = m_owner.activeSession();
    if (index >= session.entries().size() || m_renderer == nullptr)
      return;
    const auto& entry = session.entries()[index];
    const bool allowThumbnails = tab.capabilities().canGenerateThumbnails;
    const bool cut = !session.browsingTrash() && tab.capabilities().canMutate && m_owner.m_transfers.isPathCut(entry.absPath);
    if (m_owner.m_listHeader.gridMode()) {
      auto& tileNode = static_cast<FileEntryTile&>(tile);
      tileNode.setAllowThumbnailsAndMetadata(allowThumbnails);
      tileNode.bind(
          *m_renderer, entry, index, tile.width(), tile.height(), selected,
          hovered || m_owner.m_transfers.dropIndex() == index, false, cut
      );
    } else {
      auto& row = static_cast<FileEntryRow&>(tile);
      row.setAllowThumbnailsAndMetadata(allowThumbnails);
      row.setColumns(m_owner.m_listHeader.visibleColumns(), m_owner.m_listHeader.listColumns());
      row.bind(
          *m_renderer, entry, index, tile.width(), selected, hovered || m_owner.m_transfers.dropIndex() == index, false,
          session.folderItemCount(entry), cut
      );
    }
  }
  void onActivate(std::size_t index) override {
    if (index < m_owner.activeSession().entries().size())
      m_owner.activatePointerItem(index);
  }
  void onMiddleActivate(std::size_t index) override { m_owner.activatePointerMiddleItem(index); }
  std::string itemTooltip(std::size_t index) const override {
    const auto& session = m_owner.activeSession();
    if (index >= session.entries().size())
      return {};
    const auto& entry = session.entries()[index];
    auto found = session.trashOriginalPaths().find(entry.absPath.string());
    const auto description = guessFileTypeDescription(entry.absPath, entry.isDir, entry.type);
    const auto tooltip = filemanager::sanitizeFilenameForDisplay(entry.name) + "\n" + description;
    return found == session.trashOriginalPaths().end()
        ? tooltip
        : tooltip + "\nOriginal location: " + filemanager::sanitizeFilenameForDisplay(found->second);
  }
  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    m_owner.m_menus.openContextMenu(index, anchorX, anchorY);
  }
  void onSecondaryEmptyActivate(float anchorX, float anchorY) override {
    m_owner.m_menus.openEmptyContextMenu(anchorX, anchorY);
  }
  InputArea* beginInlineEdit(
      Node& tile, const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
      std::function<void()> cancel
  ) override {
    if (m_owner.m_listHeader.gridMode())
      return static_cast<FileEntryTile&>(tile).beginInlineEdit(path, name, std::move(submit), std::move(cancel));
    return static_cast<FileEntryRow&>(tile).beginInlineEdit(path, name, std::move(submit), std::move(cancel));
  }
  void endInlineEdit(Node& tile, const std::string& path) override {
    if (m_owner.m_listHeader.gridMode())
      static_cast<FileEntryTile&>(tile).endInlineEdit(path);
    else
      static_cast<FileEntryRow&>(tile).endInlineEdit(path);
  }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }

private:
  FileManager& m_owner;
  Renderer* m_renderer = nullptr;
};

namespace {
  constexpr float kToolbarHeight = 32.0F;
  constexpr float kSidebarMinWidth = 150.0F;
  constexpr float kSidebarMaxWidth = 360.0F;
  constexpr float kStatusMinHeight = 30.0F;
  constexpr float kStatusMaxHeight = 76.0F;

  struct CompactSize {
    double value = 0.0;
    std::string unit;
    std::size_t unitIndex = 0;
  };

  CompactSize compactSize(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 7> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= 1024.0 && index + 1 < units.size()) {
      value /= 1024.0;
      ++index;
    }
    if (value >= 10.0)
      value = std::round(value);
    else
      value = std::round(value * 10.0) / 10.0;
    return {value, std::string(units[index]), index};
  }

  void applyNytheraBevel(Flex& panel, float radius) {
    panel.setRadius(radius);
    panel.setCornerShapes(
        CornerShapes{
            .tl = CornerShape::Chamfered,
            .tr = CornerShape::Chamfered,
            .br = CornerShape::Chamfered,
            .bl = CornerShape::Chamfered,
        }
    );
  }

  void useSubtleSelection(Button& button, bool muted = false) {
    auto palette = Button::defaultPalette(button.variant());
    palette.selected = Button::ButtonStateColors{
        .bg = colorSpecFromRole(ColorRole::PrimaryContainer, 0.42F),
        .border = clearColorSpec(),
        .label = colorSpecFromRole(ColorRole::OnPrimaryContainer),
    };
    if (muted) {
      for (auto* state : {&palette.normal, &palette.hover, &palette.pressed})
        state->label = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    }
    button.setCustomPalette(std::move(palette));
  }

  std::unique_ptr<Button> makeButton(std::string_view text, std::function<void()> action, Button** out = nullptr) {
    auto button = std::make_unique<Button>();
    button->setText(text);
    button->setVariant(ButtonVariant::Ghost);
    button->setControlHeight(kToolbarHeight);
    button->setPadding(5.0F);
    button->setOnClick(std::move(action));
    if (out != nullptr)
      *out = button.get();
    return button;
  }

  filemanager::TabViewState loadInitialTabViewState(filemanager::FileManagerSettings& settings) {
    const auto view = settings.loadFileView();
    return filemanager::TabViewState{
        .gridMode = view.gridMode,
        .showHidden = view.showHidden,
        .foldersFirst = view.foldersFirst,
        .hiddenLast = view.hiddenLast,
        .sortField = view.sortField,
        .sortOrder = view.sortOrder,
        .viewScale = settings.loadViewScale(filemanager::kFileViewDefaultScale),
    };
  }

} // namespace

filemanager::TabManager::Callbacks FileManager::makeTabManagerCallbacks() {
  return filemanager::TabManager::Callbacks{
      .onBeforeTabSwitch =
          [this](filemanager::BrowserTab& outgoing, filemanager::BrowserTab&) {
            if (m_refreshAfterRename) {
              outgoing.needsRefresh = true;
              m_refreshAfterRename = false;
            }
            if (m_renamePath)
              cancelRename();
            m_pendingInlineRenamePath.reset();
            m_inlineRenameTarget.reset();
            if (m_adminInlineRenameTarget && m_adminInlineRenameTarget->tabId == outgoing.id)
              m_adminInlineRenameTarget.reset();
            setLocationEditMode(false);
            cancelFileGesture();
            m_listHeader.cancelGesture();
            m_menus.closeContextMenu();
            m_lastPointerIndex.reset();
            m_rubberBaseline.clear();
            m_rubberSelectedPath.reset();
            m_rubberAnchor.reset();
            if (m_grid != nullptr)
              m_grid->resetTransientStateForDataSwap();
          },
      .onActiveTabChanged =
          [this](filemanager::BrowserTab& active, const filemanager::TabViewState& previousViewState) {
            const bool modeOrScaleChanged = previousViewState.gridMode != active.viewState.gridMode
                || previousViewState.viewScale != active.viewState.viewScale;
            m_listHeader.applyTabViewState(active.viewState);
            if (m_grid != nullptr) {
              if (modeOrScaleChanged) {
                const auto metrics = filemanager::fileViewMetrics(m_listHeader.viewScale());
                m_grid->setAdapter(nullptr);
                m_grid->setAdapter(m_adapter.get());
                m_grid->setColumns(m_listHeader.gridMode() ? 0 : 1);
                m_grid->setMinCellWidth(metrics.gridCellWidth);
                m_grid->setCellHeight(m_listHeader.gridMode() ? metrics.gridCellHeight : metrics.listRowHeight);
                m_grid->setRowGap(m_listHeader.gridMode() ? 3.0F : 1.0F);
              }
              m_grid->bindScrollState(&active.scrollState);
            }
            m_statusMessage = active.lastStatusMessage;
            m_statusMessageTimer.stop();
            if (!m_statusMessage.empty())
              m_statusMessageUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            else
              m_statusMessageUntil = {};
            if (active.needsRefresh) {
              active.pendingScrollOffsetAfterRefresh = active.scrollState.offset;
              active.needsRefresh = false;
              active.session.refreshPreservingSelection();
            } else {
              if (m_grid != nullptr) {
                m_grid->notifyDataChanged();
                restoreSelection(false);
              }
              if (active.pendingPasteSelection && !active.session.loading())
                m_transfers.applyPendingPasteSelection(active);
              if (active.pendingRevealOnActivate) {
                active.pendingRevealOnActivate = false;
                if (m_grid != nullptr
                    && active.selectedPath
                    && (!active.pendingDropScrollGeneration
                        || *active.pendingDropScrollGeneration == active.scrollState.userScrollGeneration)) {
                  const auto entry =
                      std::ranges::find(active.session.entries(), *active.selectedPath, &FileEntry::absPath);
                  if (entry != active.session.entries().end())
                    m_grid->scrollToIndex(static_cast<std::size_t>(entry - active.session.entries().begin()));
                }
              }
            }
            rebuildBreadcrumbs();
            if (m_locationInput != nullptr)
              m_locationInput->setValue(active.session.path().string());
            updateNavigationControls();
            updateStatusBar();
            updateWindowTitle();
            updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
            m_tabBar.markDirty();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onTabsChanged =
          [this] {
            std::erase_if(m_adminMutationsInFlight, [this](const auto& item) {
              return m_tabs.findTab(item.first) == nullptr;
            });
            if (m_adminInlineRenameTarget && m_tabs.findTab(m_adminInlineRenameTarget->tabId) == nullptr)
              m_adminInlineRenameTarget.reset();
            updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
            m_tabBar.markDirty();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onTabMetadataChanged =
          [this](filemanager::TabId tabId) {
            m_tabBar.markDirty();
            if (tabId == m_tabs.activeTabId()) {
              updateWindowTitle();
              updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
            }
            if (m_surface)
              m_surface->requestLayout();
          },
      .onRequestCloseLastTab = [this] { requestQuit(m_surface.get()); },
      .isSizeColumnVisible = [this] { return m_listHeader.columnLayout().visible[columnIndex(FileColumn::Size)]; },
      .isRenaming =
          [this](filemanager::TabId tabId) { return tabId == m_tabs.activeTabId() && m_renamePath.has_value(); },
      .onDeferRefreshDuringRename =
          [this](filemanager::TabId tabId) {
            if (tabId == m_tabs.activeTabId())
              m_refreshAfterRename = true;
            else if (auto* tab = m_tabs.findTab(tabId))
              tab->needsRefresh = true;
          },
      .onCancelFileGesture = [this] { cancelFileGesture(); },
      .onActiveBeforeNavigate =
          [this](filemanager::BrowserTab&, const std::filesystem::path&, bool pathChanged) {
            if (pathChanged) {
              if (m_renamePath)
                cancelRename();
              m_pendingInlineRenamePath.reset();
              if (m_adminInlineRenameTarget && m_adminInlineRenameTarget->tabId == m_tabs.activeTabId())
                m_adminInlineRenameTarget.reset();
            }
            m_lastPointerIndex.reset();
          },
      .onActiveBeforePathChange =
          [this](filemanager::BrowserTab&, const std::filesystem::path&) {
            if (m_grid != nullptr) {
              m_grid->setSelectedIndex(std::nullopt, false);
              m_grid->notifyDataChanged();
            }
          },
      .onActiveAfterNavigatePathSet =
          [this](filemanager::BrowserTab&, const std::filesystem::path& path, bool) {
            if (m_locationInput != nullptr)
              m_locationInput->setValue(path.string());
            rebuildBreadcrumbs();
            updateNavigationControls();
            updateWindowTitle();
            m_tabBar.markDirty();
          },
      .onActiveEnumerateStarted =
          [this](filemanager::BrowserTab&, const std::filesystem::path& path, bool) {
            setStatus("Loading…");
            rebuildBreadcrumbs();
            if (m_locationInput != nullptr)
              m_locationInput->setValue(path.string());
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveEntriesLoaded =
          [this](filemanager::BrowserTab& active, bool chooseFirst) {
            if (m_grid != nullptr) {
              m_grid->notifyDataChanged();
              if (active.pendingScrollOffsetAfterRefresh.has_value()) {
                const float savedOffset = *active.pendingScrollOffsetAfterRefresh;
                active.pendingScrollOffsetAfterRefresh.reset();
                m_grid->scrollView().requestScrollToOffset(savedOffset);
              }
            }
            restoreSelection(chooseFirst);
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveEntriesChanged =
          [this](filemanager::BrowserTab&) {
            if (m_grid != nullptr) {
              m_grid->notifyDataChanged();
              restoreSelection();
            }
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveEntryChangedAt =
          [this](filemanager::BrowserTab&, std::size_t index) {
            if (m_grid != nullptr) {
              m_grid->notifyItemChanged(index);
              if (m_surface)
                m_surface->requestLayout();
            }
          },
      .onActiveMonitorQueryCompleted =
          [this](filemanager::BrowserTab& active) {
            if (active.pendingPasteRevealPath && m_surface)
              m_surface->requestLayout();
          },
      .onActiveStatus =
          [this](filemanager::BrowserTab&, std::string text) {
            setStatus(std::move(text));
            if (m_surface)
              m_surface->requestLayout();
          },
      .onActiveStatusInfoChanged =
          [this](filemanager::BrowserTab&) {
            updateStatusBar();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onApplyPendingPasteSelection =
          [this](filemanager::BrowserTab& tab) { m_transfers.applyPendingPasteSelection(tab); },
      .onAnyBeforePathChange =
          [this](filemanager::BrowserTab& tab, const std::filesystem::path&) {
            (void)tab.adminSession.cancelAuthentication();
            m_transfers.onBeforePathChange(tab);
          },
      .onAnyEnumerateStarted =
          [this](filemanager::BrowserTab& tab, const std::filesystem::path&, bool) {
            m_transfers.onEnumerateStarted(tab);
            m_tabBar.markDirty();
            if (m_surface)
              m_surface->requestLayout();
          },
      .onTabAdminSessionChanged =
          [this](filemanager::BrowserTab& tab, const filemanager::AdminSessionStatus& status) {
            if (status.state == filemanager::AdminSessionState::Failed && tab.administratorBrowsing) {
              // If the session was already active and failed (inactivity expiry, unexpected termination,
              // or protocol failure), recover the tab back to unprivileged browsing.
              finishAdministratorMode(
                  tab.id,
                  status.message.empty() ? "Administrator Mode ended." : status.message
              );
              return;
            }
            m_tabBar.markDirty();
            if (tab.id == m_tabs.activeTabId()) {
              updateWindowTitle();
              updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
            }
            if (m_surface)
              m_surface->requestLayout();
          },
      .onInvalidateMountCapacities =
          [this](const std::filesystem::path& path) { m_places.invalidateMountCapacitiesForPath(path); },
      .onResolvePathAsync = [this](const std::filesystem::path& path) { m_places.resolvePathAsync(path, {}, false); },
  };
}

FileManager::FileManager(std::string initialPath)
    : m_operations([this](const FileOperationManager::Operation& operation) {
        if (operation.adminCopy) {
          const auto& origin = *operation.adminCopy;
          auto* tab = m_tabs.findTab(origin.tabId);
          const bool sameContext = m_windowOpen
              && tab
              && tab->hasActiveAdminSession()
              && tab->adminSession.sessionGeneration() == origin.sessionGeneration
              && tab->session.navigationGeneration() == origin.navigation
              && tab->session.path() == operation.destination;
          if (operation.finished) {
            m_transfers.onOperationFinished(operation);
            if (operation.mutated)
              m_places.refreshCapacities();
            if (!m_operations.active() && ((!m_windowOpen && m_transfers.pendingClipboardReads() == 0) || m_quitting))
              m_running = false;
          }
          if (sameContext) {
            if (tab->active)
              setStatus(operation.status);
            else if (operation.finished)
              tab->lastStatusMessage = operation.status;
            if (operation.finished && operation.mutated) {
              tab->operationResultAfterRefresh = operation.status;
              if (tab->active)
                enumerate(tab->session.path());
              else
                tab->needsRefresh = true;
            }
          }
          syncProgressWindow();
          return;
        }
        if (operation.finished) {
          setStatus(operation.status);
          if (operation.succeeded && operation.kind == FileOperationManager::Kind::Rename) {
            for (const auto& tabPtr : m_tabs.tabs()) {
              if (tabPtr->selectedPaths.erase(operation.source))
                tabPtr->selectedPaths.insert(operation.destination);
              if (tabPtr->selectedPath && *tabPtr->selectedPath == operation.source)
                tabPtr->selectedPath = operation.destination;
              if (tabPtr->selectionAnchor && *tabPtr->selectionAnchor == operation.source)
                tabPtr->selectionAnchor = operation.destination;
            }
          }
          if (m_inlineRenameTarget && m_inlineRenameTarget->operationId == operation.id) {
            const auto renameTarget = *m_inlineRenameTarget;
            m_inlineRenameTarget.reset();
            if (operation.succeeded
                && renameTarget.tabId == m_tabs.activeTabId()
                && m_renamePath == std::filesystem::path(operation.source)) {
              // The completion below refreshes once, after selection follows the new path.
              activeSession().clearPendingMonitorChanges();
              m_refreshAfterRename = false;
              cancelRename();
            }
          }
          if (const auto createIt = m_createRequests.find(operation.id); createIt != m_createRequests.end()) {
            const auto createReq = createIt->second;
            m_createRequests.erase(createIt);
            if (operation.succeeded
                && (operation.kind == FileOperationManager::Kind::CreateFile
                    || operation.kind == FileOperationManager::Kind::CreateFolder)) {
              if (auto* originTab = m_tabs.findTab(createReq.tabId)) {
                if (originTab->session.navigationGeneration() == createReq.navigation
                    && std::filesystem::path(operation.destination).parent_path() == originTab->session.path()) {
                  originTab->selectedPath = operation.destination;
                  originTab->selectedPaths.clear();
                  originTab->selectedPaths.insert(operation.destination);
                  if (originTab->id == m_tabs.activeTabId())
                    m_pendingInlineRenamePath = operation.destination;
                }
              }
            }
          }
          const bool pastedEntries = m_transfers.onOperationFinished(operation);
          const bool wastebinOperation = operation.kind == FileOperationManager::Kind::Restore
              || operation.kind == FileOperationManager::Kind::EmptyWastebin;
          const bool anyTrashChange = wastebinOperation
              || operation.kind == FileOperationManager::Kind::Trash
              || operation.kind == FileOperationManager::Kind::TrashToHome;
          if (!operation.succeeded && !operation.cancelled && operation.trashFallback.has_value() && m_windowOpen) {
            m_trashFallbackDialog.show(
                *operation.trashFallback,
                [this](std::vector<std::string> sources) {
                  if (!sources.empty() && m_windowOpen)
                    m_operations.trashToHomeWastebin(std::move(sources));
                },
                [this](std::vector<std::string> sources) {
                  if (sources.empty() || !m_windowOpen)
                    return;
                  const auto prompt = filemanager::permanentDeletePrompt(sources, sources.size());
                  showDialog(
                      "Permanently delete?", prompt, {{"Cancel", 0}, {"Delete", 1}},
                      [this, sources = std::move(sources)](int response) mutable {
                        if (response == 1 && m_windowOpen)
                          m_operations.removePermanently(std::move(sources));
                      }
                  );
                }
            );
          }
          const bool dirtyChange = operation.succeeded || operation.mutated || wastebinOperation || pastedEntries;
          if (dirtyChange && m_windowOpen) {
            if (wastebinOperation || operation.undoUnavailable || !operation.succeeded)
              activeTab().operationResultAfterRefresh = operation.status;
            enumerate(activeSession().path());
          }
          if (dirtyChange) {
            const auto activeId = m_tabs.activeTabId();
            const bool historyReplay = operation.kind == FileOperationManager::Kind::Undo
                || operation.kind == FileOperationManager::Kind::Redo;
            const auto matchesDir = [this](const std::filesystem::path& tabPath, const std::filesystem::path& dir) {
              if (dir.empty())
                return false;
              if (filemanager::normalizeTabPath(tabPath) == filemanager::normalizeTabPath(dir))
                return true;
              const auto same = m_places.sameDirectoryPathCached(tabPath, dir);
              return same.value_or(true);
            };
            const auto srcParent = operation.source.empty() ? std::filesystem::path{}
                                                            : std::filesystem::path(operation.source).parent_path();
            const std::filesystem::path dstPath(operation.destination);
            const auto dstParent = dstPath.empty() ? std::filesystem::path{} : dstPath.parent_path();
            for (const auto& tabPtr : m_tabs.tabs()) {
              if (tabPtr->id == activeId)
                continue;
              if (tabPtr->session.browsingTrash()) {
                if (anyTrashChange || historyReplay)
                  tabPtr->needsRefresh = true;
                continue;
              }
              if (historyReplay
                  || matchesDir(tabPtr->session.path(), srcParent)
                  || matchesDir(tabPtr->session.path(), dstPath)
                  || matchesDir(tabPtr->session.path(), dstParent)) {
                tabPtr->needsRefresh = true;
                continue;
              }
              for (const auto& completed : operation.completedEntries) {
                if (matchesDir(tabPtr->session.path(), std::filesystem::path(completed.path).parent_path())) {
                  tabPtr->needsRefresh = true;
                  break;
                }
              }
            }
          }
          if (operation.succeeded || operation.mutated)
            m_places.refreshCapacities();
          if (!m_operations.active() && !m_windowOpen && m_transfers.pendingClipboardReads() == 0)
            m_running = false;
          if (!m_operations.active() && m_quitting)
            m_running = false;
        } else {
          setStatus(operation.status);
        }
        syncProgressWindow();
      }),
      m_adapter(std::make_unique<Adapter>(*this)),
      m_tabs(
          initialPath.empty() ? std::filesystem::current_path() : std::filesystem::path(initialPath),
          loadInitialTabViewState(m_settings), makeTabManagerCallbacks()
      ),
      m_menus(
          m_wayland, m_renderContext, m_menuAnimations, m_dispatcher, m_settings, m_operations, m_tabs, m_listHeader,
          m_places, m_checksumDialog, m_transfer,
          {
              .mainSurface = [this] { return m_surface.get(); },
              .lastInputSerial = [this] { return m_lastInputSerial; },
              .setLastInputSerial = [this](std::uint32_t serial) { m_lastInputSerial = serial; },
              .isModalOpen = [this] { return m_modalDialog.isOpen(); },
              .isRenaming = [this] { return m_renamePath.has_value(); },
              .alwaysShowTabBar = [this] { return m_alwaysShowTabBar; },
              .openTabsInBackground = [this] { return m_openTabsInBackground; },
              .pendingClipboardReads = [this] { return m_transfers.pendingClipboardReads(); },
              .canChangeSelection = [this] { return canChangeSelection(); },
              .selectedPath = [this] { return activeTab().selectedPath; },
              .selectedPaths = [this]() -> const std::unordered_set<std::string>& { return activeTab().selectedPaths; },
              .selectedPathsOrdered = [this] { return selectedPaths(); },
              .symlinkSelection = [this] { return symlinkSelection(); },
              .checksumMenuEntries = [this] { return checksumMenuEntries(); },
              .onSelectSingleForContextMenu =
                  [this](std::size_t index) {
                    m_keyboardModifiers = 0;
                    auto& tab = activeTab();
                    tab.selectedPaths.clear();
                    tab.selectedPaths.insert(tab.session.entries()[index].absPath.string());
                    tab.selectedPath = tab.session.entries()[index].absPath;
                    m_grid->setSelectedIndex(index, false);
                    m_grid->notifyDataChanged();
                  },
              .onClearSelectionForEmptyMenu =
                  [this] {
                    auto& tab = activeTab();
                    tab.selectedPaths.clear();
                    tab.selectedPath.reset();
                    tab.selectionAnchor.reset();
                    if (m_grid)
                      m_grid->setSelectedIndex(std::nullopt, false);
                  },
              .onStatus = [this](std::string text) { setStatus(std::move(text)); },
              .onRequestLayout =
                  [this] {
                    if (m_surface)
                      m_surface->requestLayout();
                  },
          }
      ),
      m_tabBar(
          m_tabs, m_menuAnimations, m_dispatcher, m_wayland,
          {
              .isAlwaysShowTabBar = [this] { return m_alwaysShowTabBar; },
              .homePath = [] { return std::filesystem::path(g_get_home_dir()); },
              .onNewTab = [this] { openNewTab(); },
              .onActivateTab = [this](filemanager::TabId tabId) { activateTab(tabId); },
              .onCloseTab = [this](filemanager::TabId tabId) { closeTab(tabId); },
              .onReorderTab =
                  [this](filemanager::TabId tabId, std::size_t targetIndex) { m_tabs.reorderTab(tabId, targetIndex); },
              .onOpenTabContextMenu = [this](
                                          std::vector<ContextMenuControlEntry> entries, float x, float y
                                      ) { m_menus.openTabContextMenu(std::move(entries), x, y); },
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
              .canHandlePointer =
                  [this] {
                    return !m_transfers.isDragPending()
                        && !m_transfers.isDraggingFiles()
                        && !m_transfer.dragging()
                        && !m_reorderId
                        && !m_listHeader.gestureActive()
                        && !m_resizingSidebar
                        && !m_resizingStatus
                        && !(m_grid && m_grid->rubberBandActive());
                  },
              .pointerSerial = [this] { return m_pointerSerial; },
          }
      ),
      m_transfers(
          m_wayland, m_renderContext, m_dispatcher, m_transfer, m_operations, m_tabs, m_places,
          {
              .mainSurface = [this] { return m_surface.get(); },
              .sceneRoot = [this] { return m_root.get(); },
              .grid = [this] { return m_grid; },
              .isWindowOpen = [this] { return m_windowOpen; },
              .isQuitting = [this] { return m_quitting; },
              .isRenaming = [this] { return m_renamePath.has_value(); },
              .keyboardModifiers = [this] { return m_keyboardModifiers; },
              .pointerSerial = [this] { return m_pointerSerial; },
              .selectedPathsOrdered = [this] { return selectedPaths(); },
              .selectedPath = [this] { return activeTab().selectedPath; },
              .onApplyPasteSelection =
                  [this](
                      filemanager::TabId tabId, std::unordered_set<std::string> paths, std::filesystem::path primary
                  ) {
                    if (auto* tab = m_tabs.findTab(tabId)) {
                      tab->selectedPaths = std::move(paths);
                      tab->selectedPath = primary;
                      tab->selectionAnchor = tab->selectedPath;
                      if (tabId == m_tabs.activeTabId())
                        restoreSelection();
                    }
                  },
              .onClickUnmovedDragItem =
                  [this](std::size_t index, std::uint32_t clickTime) {
                    if (!(m_keyboardModifiers & (KeyMod::Ctrl | KeyMod::Shift))) {
                      auto& tab = activeTab();
                      tab.selectedPaths = {tab.session.entries()[index].absPath.string()};
                      tab.selectedPath = tab.session.entries()[index].absPath;
                      if (m_grid)
                        m_grid->notifyDataChanged();
                      m_pointerClickTime = clickTime;
                      activatePointerItem(index);
                    } else {
                      m_lastPointerIndex.reset();
                    }
                  },
              .onCancelPlaceReorder =
                  [this] {
                    m_reorderId.reset();
                    m_reorderBefore.reset();
                    m_reordering = false;
                  },
              .onClearLastPointerIndex = [this] { m_lastPointerIndex.reset(); },
              .onSynthesizeMotion = [this](const PointerEvent& motion) { handlePointer(motion); },
              .onUpdateNavigationControls = [this] { updateNavigationControls(); },
              .onUpdateSelectionSummary = [this] { updateSelectionSummary(); },
              .onCutStateChanged =
                  [this] {
                    if (m_grid)
                      m_grid->notifyDataChanged();
                    if (m_surface)
                      m_surface->requestRedraw();
                  },
              .onStatus = [this](std::string text) { setStatus(std::move(text)); },
              .onMaybeStopMainLoop =
                  [this] {
                    if ((!m_windowOpen || m_quitting)
                        && !m_operations.active()
                        && m_transfers.pendingClipboardReads() == 0)
                      m_running = false;
                  },
          }
      ) {
  m_menus.loadInitialPreference();
  m_alwaysShowTabBar = m_settings.alwaysShowTabBar();
  m_openTabsInBackground = m_settings.openTabsInBackground();
  m_listHeader.loadInitialState();
  m_listHeader.applyTabViewState(activeTab().viewState);
  updateWindowTitle();
  m_operations.setConflictResolver(m_modalDialog.makeConflictResolver());
}

bool FileManager::canChangeSelection() const {
  return m_grid
      && !activeSession().loading()
      && !m_renamePath
      && !m_transfers.isDragPending()
      && !m_transfers.isDraggingFiles()
      && !m_transfer.dragging()
      && !m_reorderId
      && !m_listHeader.gestureActive()
      && !m_tabBar.gestureActive()
      && !m_resizingSidebar
      && !m_resizingStatus
      && !m_grid->rubberBandActive();
}

void FileManager::changeSelection(SelectionChange change) {
  if (!canChangeSelection())
    return;
  auto& tab = activeTab();
  std::unordered_set<std::string> paths;
  std::optional<std::filesystem::path> first;
  if (change != SelectionChange::None) {
    paths.reserve(tab.session.entries().size());
    for (const auto& entry : tab.session.entries()) {
      const auto path = entry.absPath.string();
      if (change == SelectionChange::Invert && tab.selectedPaths.contains(path))
        continue;
      paths.insert(path);
      if (!first)
        first = entry.absPath;
    }
  }
  if (!tab.selectedPath || !paths.contains(tab.selectedPath->string()))
    tab.selectedPath = std::move(first);
  tab.selectedPaths = std::move(paths);
  tab.selectionAnchor = tab.selectedPath;
  tab.session.clearChooseFirstAfterLoad();
  restoreSelection();
  m_grid->notifyDataChanged();
  updateSelectionSummary();
  if (m_surface)
    m_surface->requestRedraw();
}

void FileManager::duplicateSelection() {
  const auto& tab = activeTab();
  if (!tab.capabilities().canCreate
      || tab.session.browsingTrash()
      || tab.session.loading()
      || !currentDirectoryWritable())
    return;
  auto paths = selectedPaths();
  if (paths.empty())
    return;
  if (const auto id = m_operations.duplicate(std::move(paths), tab.session.path().string()))
    m_transfers.registerPasteRequest(id, PasteRequest{tab.id, tab.session.path(), tab.session.navigationGeneration()});
}

void FileManager::createSelectionSymlink() {
  const auto& tab = activeTab();
  if (!tab.capabilities().canCreate || tab.session.browsingTrash() || !currentDirectoryWritable())
    return;
  const auto selection = symlinkSelection();
  if (!selection) {
    setStatus("Select one regular file or folder to create a symlink");
    return;
  }
  const auto& [path, directory] = *selection;
  auto name = path.filename().string();
  // Treat compound filename extensions as a unit and retain a leading dot.
  const auto extension = directory ? std::string::npos : name.find('.', 1);
  name.insert(extension == std::string::npos ? name.size() : extension, "-link");
  if (const auto id = m_operations.createSymlink(path.string(), (tab.session.path() / name).string(), true))
    m_transfers.registerPasteRequest(id, PasteRequest{tab.id, tab.session.path(), tab.session.navigationGeneration()});
}

void FileManager::showSymlinkDialog() {
  const auto& tab = activeTab();
  if (!tab.capabilities().canCreate
      || m_modalDialog.isOpen()
      || tab.session.browsingTrash()
      || !currentDirectoryWritable())
    return;
  const PasteRequest request{tab.id, tab.session.path(), tab.session.navigationGeneration()};
  m_modalDialog.showSymlinkDialog(
      [this, request](
          const std::string& name, const std::string& target
      ) -> std::optional<filemanager::ModalDialogHost::SymlinkError> {
        using Field = filemanager::ModalDialogHost::SymlinkField;
        using Err = filemanager::ModalDialogHost::SymlinkError;
        if (m_places.hasActiveDeviceOperations() || m_operations.replaying())
          return Err{"Wait for the current operation to finish.", Field::None};
        const auto* currentTab = m_tabs.findTab(request.tabId);
        if (currentTab == nullptr
            || request.tabId != m_tabs.activeTabId()
            || request.navigation != currentTab->session.navigationGeneration()
            || currentTab->session.browsingTrash()
            || !currentTab->capabilities().canCreate)
          return Err{"The displayed folder changed. Reopen the dialog in the destination folder.", Field::None};
        if (name.empty())
          return Err{"Enter a symlink name.", Field::Name};
        if (!filemanager::validFileName(name))
          return Err{"Enter a single valid filename.", Field::Name};
        if (target.empty() || target.find('\0') != std::string::npos)
          return Err{"Enter a target path.", Field::Target};
        const auto destination = request.destination / name;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(destination, error);
        if (std::filesystem::exists(status))
          return Err{"An item with that name already exists.", Field::Name};
        if (error && error != std::errc::no_such_file_or_directory)
          return Err{error.message(), Field::Name};
        if (request.destination == currentTab->session.path() && !currentTab->session.currentDirectoryWritable())
          return Err{"The destination folder does not support file creation.", Field::None};
        const auto id =
            m_operations.createSymlink(filemanager::expandHomePath(target, g_get_home_dir()), destination.string());
        if (id == 0)
          return Err{"The symlink operation could not be started.", Field::None};
        m_transfers.registerPasteRequest(id, request);
        return std::nullopt;
      }
  );
}

void FileManager::beginRename() {
  if (!activeTab().capabilities().canRename) {
    if (activeSession().browsingTrash())
      setStatus("This action is unavailable in Wastebin");
    else if (activeTab().hasActiveAdminSession() && activeSession().loading())
      setStatus("Wait for the folder listing to finish");
    else if (!currentDirectoryWritable())
      setStatus("This directory is read-only");
    else
      setStatus("This action is currently unavailable");
    return;
  }
  const auto paths = selectedPaths();
  if (!paths.empty())
    beginRenamePath(paths.front());
}

void FileManager::beginRenamePath(const std::filesystem::path& path) {
  auto& tab = activeTab();
  auto& session = tab.session;
  const auto tabId = tab.id;
  if (session.browsingTrash()) {
    setStatus("This action is unavailable in Wastebin");
    return;
  }
  if (!tab.capabilities().canRename) {
    if (tab.hasActiveAdminSession() && session.loading()) {
      m_pendingInlineRenamePath = path;
      return;
    }
    setStatus(!currentDirectoryWritable() ? "This directory is read-only" : "This action is currently unavailable");
    return;
  }
  if (!m_grid || !m_surface || m_inlineRenameTarget.has_value() || m_adminInlineRenameTarget.has_value())
    return;
  const bool isAdminTab = tab.hasActiveAdminSession();
  if (isAdminTab && m_adminMutationsInFlight.contains(tabId)) {
    setStatus("Wait for the current Administrator operation to finish");
    return;
  }
  if (path.parent_path() != session.path()) {
    m_pendingInlineRenamePath.reset();
    return;
  }
  const auto found = std::ranges::find(session.entries(), path, &FileEntry::absPath);
  if (found == session.entries().end()) {
    if (session.loading())
      m_pendingInlineRenamePath = path;
    else
      setStatus("The item is no longer available");
    return;
  }
  const auto adminSessionGen = tab.adminSession.sessionGeneration();
  const auto navGen = session.navigationGeneration();
  const bool entryHasIdentity = found->hasIdentity;
  const std::uint64_t entryDevice = found->device;
  const std::uint64_t entryInode = found->inode;
  const bool entryIsDir = found->isDir;
  const bool entryIsSymlink = found->isSymlink;
  const auto index = static_cast<std::size_t>(found - session.entries().begin());
  m_grid->setSelectedIndex(index, false);
  m_grid->scrollToIndex(index);
  m_renamePath = path;
  const auto stablePath = path.string();
  InputArea* editor = m_grid->beginInlineEdit(
      index, stablePath, path.filename().string(),
      [this, path, tabId, isAdminTab, adminSessionGen, navGen, entryHasIdentity, entryDevice, entryInode, entryIsDir,
       entryIsSymlink](const std::string& newName) {
        if (!filemanager::validFileName(newName)) {
          setStatus("Choose a valid filename");
          return;
        }
        if (m_inlineRenameTarget.has_value() || m_adminInlineRenameTarget.has_value())
          return;
        const auto destination = path.parent_path() / newName;
        if (destination == path) {
          cancelRename();
          return;
        }
        if (isAdminTab) {
          auto* originTab = m_tabs.findTab(tabId);
          if (!originTab
              || tabId != m_tabs.activeTabId()
              || !originTab->hasActiveAdminSession()
              || originTab->adminSession.sessionGeneration() != adminSessionGen
              || !originTab->capabilities().canRename
              || originTab->session.navigationGeneration() != navGen
              || originTab->session.path() != path.parent_path()) {
            cancelRename();
            setStatus("Administrator session is no longer active");
            return;
          }
          if (m_adminMutationsInFlight.contains(tabId)) {
            setStatus("Wait for the current Administrator operation to finish");
            return;
          }
          if (!entryHasIdentity || entryDevice == 0 || entryInode == 0) {
            setStatus("Could not verify the selected item identity; refresh and try again");
            return;
          }
          const std::weak_ptr<bool> alive = m_alive;
          const std::string sourceName = path.filename().string();
          const auto reqId = originTab->adminSession.renameEntryAsync(
              path.parent_path(), sourceName, newName, entryDevice, entryInode, entryIsDir, entryIsSymlink,
              [this, alive, tabId, adminSessionGen, navGen, path, sourceName](
                  bool ok, const filemanager::AdminMutationRecord& rec, filemanager::AdminStatusCode code,
                  const std::string& detail
              ) {
                const auto locked = alive.lock();
                if (!locked || !*locked)
                  return;
                if (m_adminInlineRenameTarget
                    && m_adminInlineRenameTarget->tabId == tabId
                    && m_adminInlineRenameTarget->sessionGeneration == adminSessionGen) {
                  m_adminInlineRenameTarget.reset();
                }
                if (const auto it = m_adminMutationsInFlight.find(tabId);
                    it != m_adminMutationsInFlight.end() && it->second.sessionGeneration == adminSessionGen) {
                  m_adminMutationsInFlight.erase(it);
                }

                auto* completedTab = m_tabs.findTab(tabId);
                if (!completedTab
                    || !completedTab->hasActiveAdminSession()
                    || completedTab->adminSession.sessionGeneration() != adminSessionGen) {
                  return;
                }

                if (!ok) {
                  const std::string errorMsg = adminMutationErrorMessage(code, detail);
                  if (tabId == m_tabs.activeTabId())
                    setStatus(errorMsg);
                  else
                    completedTab->lastStatusMessage = errorMsg;
                  return;
                }

                const std::filesystem::path renamedPath = path.parent_path() / rec.name;
                const std::string oldStr = path.string();
                const std::string newStr = renamedPath.string();
                for (const auto& tabPtr : m_tabs.tabs()) {
                  if (tabPtr->selectedPaths.erase(oldStr))
                    tabPtr->selectedPaths.insert(newStr);
                  if (tabPtr->selectedPath && *tabPtr->selectedPath == path)
                    tabPtr->selectedPath = renamedPath;
                  if (tabPtr->selectionAnchor && *tabPtr->selectionAnchor == path)
                    tabPtr->selectionAnchor = renamedPath;
                }

                if (tabId == m_tabs.activeTabId() && m_renamePath && *m_renamePath == path) {
                  activeSession().clearPendingMonitorChanges();
                  m_refreshAfterRename = false;
                  cancelRename();
                }

                const std::string statusMsg = "Renamed \""
                    + filemanager::sanitizeFilenameForDisplay(sourceName)
                    + "\" to \""
                    + filemanager::sanitizeFilenameForDisplay(rec.name)
                    + "\"";
                if (tabId == m_tabs.activeTabId()) {
                  setStatus(statusMsg);
                  completedTab->operationResultAfterRefresh = statusMsg;
                } else {
                  completedTab->lastStatusMessage = statusMsg;
                  completedTab->operationResultAfterRefresh = statusMsg;
                }

                if (completedTab->session.navigationGeneration() == navGen
                    && completedTab->session.path() == path.parent_path()) {
                  if (tabId == m_tabs.activeTabId()) {
                    completedTab->session.setNextEnumerateUserInitiated(false);
                    completedTab->session.refreshPreservingSelection();
                  } else {
                    completedTab->needsRefresh = true;
                  }
                }
                markOtherTabsDirtyForDirectory(tabId, path.parent_path());
              }
          );
          if (reqId != filemanager::kInvalidAdminRequestId) {
            m_adminInlineRenameTarget = AdminInlineRenameTarget{reqId, tabId, adminSessionGen, path, destination};
            m_adminMutationsInFlight[tabId] = AdminMutationRequest{reqId, adminSessionGen};
          }
          return;
        }
        if (activeTab().hasActiveAdminSession()) {
          cancelRename();
          return;
        }
        struct stat state{};
        if (lstat(path.c_str(), &state) != 0) {
          setStatus("Could not inspect the item: " + std::string(g_strerror(errno)));
          return;
        }
        // Keep the editor and the proposed name until success. Checked rename
        // rejects collisions without offering to overwrite another item.
        if (const auto id =
                m_operations.renameChecked(path.string(), destination.string(), state.st_dev, state.st_ino, {})) {
          m_inlineRenameTarget = InlineRenameTarget{id, tabId, path};
        }
      },
      [this] { cancelRename(); }
  );
  if (editor != nullptr) {
    m_inlineEditArea = editor;
    m_dispatcher.setFocus(editor);
    m_surface->requestLayout();
  } else {
    m_pendingInlineRenamePath = path;
    m_surface->requestLayout();
  }
}

void FileManager::cancelRename() {
  if (m_renamePath && m_grid)
    m_grid->endInlineEdit(m_renamePath->string());
  m_renamePath.reset();
  m_inlineEditArea = nullptr;
  if (m_grid)
    m_dispatcher.setFocus(m_grid->focusArea());
  if (m_surface)
    m_surface->requestLayout();
  auto& session = activeSession();
  if (!session.loading() && session.hasPendingMonitorChanges())
    session.applyPendingMonitorChanges();
  if (m_refreshAfterRename) {
    m_refreshAfterRename = false;
    enumerate(session.path());
  }
}

FileManager::~FileManager() {
  *m_alive = false;
  m_operations.shutdown();
  m_checksumDialog.dismiss();
  m_helpDialog.dismiss();
  m_menuAnimations.cancelAll();
  m_tabBar.detachUi();
  m_menus.closeContextMenu();
  m_transfers.shutdown();
  m_menus.closeApplicationPicker();
  if (m_destinationPicker) {
    m_destinationPicker->closeFileDialogWithoutResult();
    FileDialog::cancelIfPending();
    FileDialog::setPresenter(nullptr);
    m_destinationPicker.reset();
  }
  m_places.shutdown();
  m_tabs.shutdownAll();
  m_dispatcher.setSceneRoot(nullptr);
  if (m_surface)
    m_surface->setSceneRoot(nullptr);
  m_selectionTimer.stop();
  TooltipManager::instance().shutdown();
  m_renderContext.cleanup();
  m_sharedContext.cleanup();
}

int FileManager::run() {
  for (const auto action :
       {KeybindAction::Validate, KeybindAction::Cancel, KeybindAction::Up, KeybindAction::Down, KeybindAction::Left,
        KeybindAction::Right}) {
    KeybindMatcher::setMatcher(
        action, [this, alive = std::weak_ptr<bool>(m_alive), action](std::uint32_t sym, std::uint32_t modifiers) {
          const auto locked = alive.lock();
          return locked && *locked && m_configService.matchesKeybind(action, sym, modifiers);
        }
    );
  }
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");
  i18n::Service::instance().init();
  Style::setRtl(i18n::Service::instance().rtl());
  m_configService.addReloadCallback(
      [this] {
        m_themeService.apply();
        IconResolver::invalidateThemeContent();
        if (m_grid)
          m_grid->notifyDataChanged();
        m_places.markDirty(false);
        m_tabBar.updateTheme();
        updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
        m_modalDialog.updateTheme();
        if (m_surface)
          m_surface->requestLayout();
        if (m_modalDialog.isOpen())
          m_modalDialog.requestLayout();
      },
      "file-manager-theme"
  );
  m_themeService.apply();
  m_wayland.setNativeDataTransfer(&m_transfer);
  m_transfers.setupDragHandlers();
  m_wayland.connect();
  m_transfer.setDragIconResources(m_wayland.compositor(), m_wayland.shm());
  if (!m_wayland.hasXdgShell() || m_wayland.compositor() == nullptr || m_wayland.shm() == nullptr)
    throw std::runtime_error("Wayland compositor, shm, and xdg-shell are required");
  m_sharedContext.initialize(m_wayland.display());
  m_renderContext.initialize(m_sharedContext);
  m_renderContext.setTextBaseDirection(i18n::Service::instance().rtl());
  TooltipManager::instance().initialize(m_wayland, &m_configService, &m_renderContext);
  TooltipManager::instance().setFreshHoverPolicy([this] {
    return m_menus.hasOpenPopup()
        || m_transfers.isDraggingFiles()
        || m_reorderId.has_value()
        || m_transfers.isDragPending()
        || m_tabBar.gestureActive()
        || m_resizingSidebar
        || m_modalDialog.isOpen()
        || (m_destinationPicker && m_destinationPicker->isOpen())
        || m_dispatcher.pointerCaptured()
        || (m_grid && m_grid->rubberBandActive());
  });
  m_dispatcher.setHoverChangeCallback([this](InputArea*, InputArea* next) {
    if (m_surface)
      TooltipManager::instance().onHoverChange(
          next, m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
      );
  });
  m_surface = std::make_unique<ToplevelSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);
  m_surface->setAnimationManager(&m_menuAnimations);
  m_surface->setClosedCallback([this] {
    m_windowOpen = false;
    m_tabs.shutdownAll();
    if (m_destinationPicker) {
      m_destinationPicker->closeFileDialogWithoutResult();
      FileDialog::cancelIfPending();
    }
    if (m_propertiesWindow)
      m_propertiesWindow->close();
    cancelFileGesture();
    TooltipManager::instance().forceDestroy();
    m_places.cancelCapacityQueries();
    m_menus.closeAll();
    struct MainCloseRequest {
      FileManager* self;
      std::weak_ptr<bool> alive;
    };
    g_idle_add(
        [](gpointer context) -> gboolean {
          std::unique_ptr<MainCloseRequest> request(static_cast<MainCloseRequest*>(context));
          const auto locked = request->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          auto* self = request->self;
          self->m_dispatcher.setSceneRoot(nullptr);
          if (self->m_surface) {
            self->m_surface->setSceneRoot(nullptr);
            self->m_surface.reset();
          }
          self->m_listHeader.detachUi();
          self->m_places.detachUi();
          self->m_menus.detachUi();
          self->m_tabBar.detachUi();
          self->m_transfers.detachUi();
          self->m_root.reset();
          self->m_appBar = nullptr;
          self->m_appBrandContainer = nullptr;
          self->m_adminHeaderContainer = nullptr;
          self->m_adminHeaderSeparator = nullptr;
          self->m_adminHeaderShield = nullptr;
          self->m_adminHeaderLabel = nullptr;
          self->m_adminFinishButton = nullptr;
          self->m_grid = nullptr;
          self->m_locationInput = nullptr;
          self->m_statusLabel = nullptr;
          if (!self->m_operations.active() && self->m_transfers.pendingClipboardReads() == 0)
            self->m_running = false;
          return G_SOURCE_REMOVE;
        },
        new MainCloseRequest{this, m_alive}
    );
  });
  m_surface->setConfigureCallback([this](std::uint32_t width, std::uint32_t) {
    updateAdminHeaderUi(static_cast<int>(width));
    m_surface->requestLayout();
  });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  m_windowTitle =
      filemanager::windowTitleForPath(activeSession().path(), g_get_home_dir(), activeTab().hasActiveAdminSession());
  ToplevelSurfaceConfig config{
      .width = 1120,
      .height = 760,
      .minWidth = 560,
      .minHeight = 380,
      .maxWidth = 0,
      .maxHeight = 0,
      .title = m_windowTitle,
      .appId = "dev.nythera.FileManager"
  };
  wl_output* output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output;
  if (!m_surface->initialize(output, config))
    throw std::runtime_error("could not create xdg_toplevel");

  m_thumbnailPendingSub = m_pickerThumbnails.subscribePendingUpload([this] {
    if (m_surface)
      m_surface->requestUpdate();
  });

  m_pickerHosts.registerHost(
      [this](wl_surface* surface) -> std::optional<LayerPopupParentContext> {
        auto* parent = m_surface.get();
        if (m_checksumDialog.isOpen() && m_modalDialog.ownsSurface(surface))
          parent = m_modalDialog.surface();
        if (!parent || surface != parent->wlSurface())
          return std::nullopt;
        return LayerPopupParentContext{
            .surface = surface,
            .xdgSurface = parent->xdgSurface(),
            .output = m_wayland.outputForSurface(surface),
            .width = parent->width(),
            .height = parent->height()
        };
      },
      {}, {},
      [this]() -> std::optional<LayerPopupParentContext> {
        return m_surface ? m_pickerHosts.contextForSurface(m_surface->wlSurface()) : std::nullopt;
      }
  );
  m_destinationPicker = std::make_unique<FileDialogPopup>();
  m_destinationPicker->initialize(m_wayland, m_configService, m_renderContext, m_pickerHosts, m_pickerThumbnails);
  FileDialog::setPresenter(m_destinationPicker.get());
  m_menus.initializePopups(m_configService, m_pickerHosts, [this](const ContextMenuControlEntry& entry) {
    activateContextMenuEntry(entry);
  });

  m_wayland.setPointerEventCallback([this](const PointerEvent& event) {
    if (m_menus.handleAppPickerPointer(event))
      return;
    if (m_destinationPicker && m_destinationPicker->onPointerEvent(event))
      return;
    if (m_propertiesWindow && m_propertiesWindow->isOpen() && m_propertiesWindow->ownsSurface(event.surface))
      m_propertiesWindow->handlePointer(event);
    else
      handlePointer(event);
  });
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland.setCursorShape(serial, shape);
  });
  m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) {
    if (m_menus.isApplicationPickerOpen() && m_menus.handleKeyboard(event))
      return;
    else if (m_destinationPicker && m_destinationPicker->isOpen())
      m_destinationPicker->onKeyboardEvent(event);
    else if (m_menus.isContextMenuOpen() && m_menus.handleKeyboard(event))
      return;
    else if (m_modalDialog.ownsSurface(m_wayland.lastKeyboardSurface()))
      m_modalDialog.handleKeyboard(event);
    else if (
        m_propertiesWindow
        && m_propertiesWindow->isOpen()
        && m_propertiesWindow->ownsSurface(m_wayland.lastKeyboardSurface())
    )
      m_propertiesWindow->handleKeyboard(event);
    else if (m_modalDialog.isOpen())
      m_modalDialog.handleKeyboard(event);
    else if (m_progressWindow.ownsSurface(m_wayland.lastKeyboardSurface()))
      m_progressWindow.handleKeyboard(event);
    else
      handleKeyboard(event);
  });
  m_wayland.setKeyboardModifiersCallback([this](std::uint32_t modifiers) {
    m_keyboardModifiers = modifiers;
    m_transfer.updateDragTarget();
    if (m_transfers.isDraggingFiles() && m_surface)
      m_surface->requestUpdate();
  });
  navigate(activeSession().path(), false);
  TimerPollSource timers;
  MainLoop loop(
      m_wayland,
      [this, &timers] {
        return std::vector<PollSource*>{
            &m_gioPollSource, &m_configPollSource, &m_pickerThumbnails, &timers, &m_transfer
        };
      },
      [this] { return !m_running && !m_transfer.ownsSelection() && !m_transfer.busy(); },
      [this] {
        if (m_destinationPicker) {
          m_destinationPicker->closeFileDialogWithoutResult();
          FileDialog::cancelIfPending();
        }
        m_menus.closeApplicationPicker();
        m_tabBar.detachUi();
        cancelFileGesture();
        TooltipManager::instance().forceDestroy();
        m_dispatcher.setSceneRoot(nullptr);
        m_modalDialog.forceCloseImmediate();
        m_progressWindow.close();
        if (m_surface) {
          m_surface->setSceneRoot(nullptr);
          m_surface.reset();
        }
      }
  );
  loop.run();
  return 0;
}

void FileManager::restoreWastebinSelection(bool chooseDestination) {
  const auto& session = activeSession();
  if (!session.browsingTrash() || !m_windowOpen)
    return;
  if (m_places.hasActiveDeviceOperations()) {
    setStatus("Wait for the device operation to finish");
    return;
  }
  if (std::ranges::any_of(m_operations.operations(), [](const auto& operation) {
        return operation.kind == FileOperationManager::Kind::EmptyWastebin && !operation.finished;
      })) {
    setStatus("Wait for Wastebin emptying to finish");
    return;
  }
  std::vector<std::string> uris;
  for (const auto& path : selectedPaths()) {
    const auto found = session.trashUris().find(path);
    if (found == session.trashUris().end()) {
      setStatus("A selected Wastebin item is no longer available; refresh the view");
      return;
    }
    uris.push_back(found->second);
  }
  if (uris.empty())
    return;
  if (!chooseDestination) {
    m_operations.restore(std::move(uris));
    return;
  }
  FileDialogOptions options{
      .mode = FileDialogMode::SelectFolder, .startDirectory = g_get_home_dir(), .title = "Restore to…"
  };
  const std::weak_ptr<bool> alive = m_alive;
  if (!FileDialog::open(
          std::move(options), [this, alive, tabId = activeTab().id, uris = std::move(uris)](auto destination) mutable {
            const auto locked = alive.lock();
            if (!locked || !*locked || !m_windowOpen || !destination)
              return;
            if (const auto* tab = m_tabs.findTab(tabId); tab && !tab->hasActiveAdminSession())
              m_operations.restore(std::move(uris), destination->string());
          }
      ))
    setStatus("Could not open the destination picker");
}

void FileManager::confirmEmptyWastebin() {
  if (activeTab().hasActiveAdminSession()) {
    setStatus("Wastebin actions are unavailable in Administrator Mode");
    return;
  }
  if (!m_windowOpen)
    return;
  if (m_operations.active() || m_places.hasActiveDeviceOperations()) {
    setStatus("Wait for the current operation to finish before emptying Wastebin");
    return;
  }
  showDialog(
      "Empty Wastebin?",
      "Permanently delete all items in Wastebin, including Wastebins on mounted filesystems?\n\n"
      "This cannot be undone. These items cannot be restored from Wastebin afterwards.",
      {{"Cancel", 0}, {"Empty Wastebin", 1}}, [this, tabId = activeTab().id](int response) {
        const auto* tab = m_tabs.findTab(tabId);
        if (response == 1 && m_windowOpen && tab && !tab->hasActiveAdminSession()) {
          if (m_operations.active() || m_places.hasActiveDeviceOperations())
            setStatus("Wait for the current operation to finish before emptying Wastebin");
          else
            m_operations.emptyWastebin(true);
        }
      }
  );
}

void FileManager::activateContextMenuEntry(const ContextMenuControlEntry& entry) {
  auto& tab = activeTab();
  auto& session = tab.session;
  const auto caps = tab.capabilities();
  if (entry.id == kActAsAdministrator) {
    (void)enterAdministratorMode(tab.id);
    return;
  }
  if (entry.id == kOpenFolderAsAdministrator) {
    if (m_menus.contextMenuTarget())
      (void)enterAdministratorMode(tab.id, *m_menus.contextMenuTarget());
    return;
  }
  if (entry.id == kCalculateChecksum || entry.id == kVerifyChecksum) {
    if (entry.enabled && caps.canChecksum)
      showChecksumDialog(entry.id == kVerifyChecksum);
    return;
  }
  if (m_helpDialog.handleMenuAction(entry.id))
    return;
  if (entry.id >= kFirstExternalToolApp) {
    if (!caps.canRunExternalTools) {
      setStatus("External tools are disabled in Administrator Mode");
      return;
    }
    m_menus.launchExternalTool(static_cast<std::size_t>(entry.id - kFirstExternalToolApp));
    return;
  }
  if (entry.id >= 30001 && entry.id <= kShowMenuBar) {
    switch (entry.id) {
    case 30001:
      if (!session.browsingTrash())
        launchNewWindow(session.path());
      break;
    case 30002:
      activateSelected();
      break;
    case 30003:
      if (!caps.canLaunchApps) {
        setStatus("Opening files with external applications is disabled in Administrator Mode");
        return;
      }
      if (tab.selectedPath) {
        m_menus.setContextMenuTarget(*tab.selectedPath);
        m_menus.openApplicationChooser();
      }
      break;
    case 30004:
      if (tab.selectedPath)
        launchNewWindow(*tab.selectedPath);
      break;
    case 30005:
      if (!caps.canPinToPlaces) {
        setStatus("Pinning locations to Places is disabled in Administrator Mode");
        return;
      }
      if (tab.selectedPath)
        m_places.savePlace(*tab.selectedPath);
      break;
    case 30006:
      if (!caps.canOpenProperties) {
        setStatus("Properties is unavailable in Administrator Mode");
        return;
      }
      if (tab.selectedPath && tab.selectedPaths.size() == 1)
        openProperties(*tab.selectedPath);
      break;
    case 30007:
      requestQuit(m_surface.get());
      break;
    case kShowMenuBar:
      m_menus.toggleMenuBarVisibility();
      break;
    }
    return;
  }
  switch (entry.id) {
  case kNewTab:
  case kTabContextNew:
    openNewTab();
    return;
  case kReopenClosedTab:
    (void)reopenClosedTab();
    return;
  case kCloseTab:
    (void)closeTab(tab.id);
    return;
  case kOpenFolderInNewTab:
    if (m_menus.contextMenuTarget() && !session.browsingTrash())
      openFolderInNewTab(*m_menus.contextMenuTarget(), !m_openTabsInBackground);
    return;
  case kFileOpenInNewTab:
    if (!session.browsingTrash() && tab.selectedPath) {
      const auto found = std::ranges::find(session.entries(), *tab.selectedPath, &FileEntry::absPath);
      if (found != session.entries().end() && found->isDir)
        openFolderInNewTab(*tab.selectedPath, !m_openTabsInBackground);
    }
    return;
  case kAlwaysShowTabBar:
    setAlwaysShowTabBar(!m_alwaysShowTabBar);
    return;
  case kOpenTabsInBackground:
    setOpenTabsInBackground(!m_openTabsInBackground);
    return;
  case kTabContextDuplicate:
    (void)duplicateTab(m_tabBar.contextMenuTabId());
    return;
  case kTabContextClose:
    (void)closeTab(m_tabBar.contextMenuTabId());
    return;
  case kTabContextCloseOthers:
    (void)closeOtherTabs(m_tabBar.contextMenuTabId());
    return;
  case kTabContextCloseRight:
    (void)closeTabsToRight(m_tabBar.contextMenuTabId());
    return;
  default:
    break;
  }
  if (entry.id == kPlaceOpenInNewTab && m_places.placeMenuTarget()) {
    const auto place = *m_places.placeMenuTarget();
    const bool activate = !m_openTabsInBackground;
    if (place.identity == "personal:trash")
      openFolderInNewTab(std::filesystem::path("trash:/"), activate);
    else if (place.mounted && !place.path.empty())
      openFolderInNewTab(place.path, activate);
    return;
  }
  if (entry.id >= 20000 && m_menus.contextMenuTarget()) {
    if (!caps.canLaunchApps) {
      setStatus("Opening files with external applications is disabled in Administrator Mode");
      return;
    }
    const auto index = static_cast<std::size_t>(entry.id - 20000);
    if (!m_menus.launchOpenWithApp(index))
      setStatus("That application is no longer available");
    return;
  }
  if (entry.id == 81) {
    if (!caps.canLaunchApps) {
      setStatus("Opening files with external applications is disabled in Administrator Mode");
      return;
    }
    m_menus.openApplicationChooser();
    return;
  }
  if (entry.id >= 90 && entry.id <= 93 && m_locationInput != nullptr) {
    if ((entry.id == 90 || entry.id == 91) && tab.hasActiveAdminSession())
      return;
    switch (entry.id) {
    case 90: // Cut
      m_dispatcher.keyEvent('x', 0, KeyMod::Ctrl, true, false);
      break;
    case 91: // Copy
      (void)m_locationInput->copySelectedText();
      break;
    case 92: // Paste
      pasteLocationFromClipboard();
      break;
    case 93: // Select all
      m_locationInput->selectAll();
      break;
    }
    return;
  }
  const bool createsItem = entry.id >= 111 && entry.id <= kSelectionSymlink;
  if (m_places.hasActiveDeviceOperations()
      && ((entry.id >= 1 && entry.id <= 6)
          || createsItem
          || entry.id == kDuplicate
          || entry.id == kUndo
          || entry.id == kRedo)) {
    setStatus("Wait for the device operation to finish");
    return;
  }
  if (m_operations.replaying()
      && ((entry.id >= 3 && entry.id <= 6)
          || (entry.id >= 70 && entry.id <= 72)
          || createsItem
          || entry.id == kDuplicate)) {
    setStatus("Wait for Undo or Redo to finish");
    return;
  }
  switch (entry.id) {
  case kSelectAll:
    recordAdminTabUserActivity(tab);
    changeSelection(SelectionChange::All);
    return;
  case kDeselectAll:
    recordAdminTabUserActivity(tab);
    changeSelection(SelectionChange::None);
    return;
  case kInvertSelection:
    recordAdminTabUserActivity(tab);
    changeSelection(SelectionChange::Invert);
    return;
  case kDuplicate:
    if (!caps.canCreate || !caps.canMutate) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    duplicateSelection();
    return;
  case kSelectionSymlink:
    if (!caps.canCreate || !caps.canMutate) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    createSelectionSymlink();
    return;
  case kManualSymlink:
    if (!caps.canCreate || !caps.canMutate) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    showSymlinkDialog();
    return;
  case kUndo:
  case kRedo:
    if (!caps.canUndo) {
      setStatus("Undo and Redo are unavailable in Administrator Mode");
      return;
    }
    if (!m_modalDialog.isOpen() && !m_renamePath && !m_transfer.busy() && m_transfers.pendingClipboardReads() == 0) {
      if (entry.id == kUndo && m_operations.canUndo())
        m_operations.undo();
      else if (entry.id == kRedo && m_operations.canRedo())
        m_operations.redo();
    }
    return;
  default:
    break;
  }
  if (entry.id == 70 || entry.id == 71) {
    restoreWastebinSelection(entry.id == 71);
    return;
  }
  if (entry.id == 72) {
    confirmEmptyWastebin();
    return;
  }
  if (entry.id == 20) {
    if (!caps.canPinToPlaces) {
      setStatus("Pinning locations to Places is disabled in Administrator Mode");
      return;
    }
    if (m_menus.contextMenuTarget())
      m_places.savePlace(*m_menus.contextMenuTarget());
    return;
  }
  if (entry.id == 60 && m_menus.contextMenuTarget()) {
    launchNewWindow(*m_menus.contextMenuTarget());
    return;
  }
  if (entry.id == 59) {
    if (tab.hasActiveAdminSession()) {
      setStatus("Copying locations is unavailable in Administrator Mode");
      return;
    }
    if (m_menus.contextMenuTarget()) {
      writeDesktopClipboard(m_menus.contextMenuTarget()->string(), "text/plain;charset=utf-8");
    } else {
      const auto paths = selectedPaths();
      if (paths.size() == 1)
        writeDesktopClipboard(paths.front(), "text/plain;charset=utf-8");
    }
    return;
  }
  if (entry.id == 21) {
    if (!caps.canPinToPlaces) {
      setStatus("Pinning locations to Places is disabled in Administrator Mode");
      return;
    }
    m_places.savePlace(session.path());
    return;
  }
  if (entry.id == 38) {
    m_places.toggleShowHiddenMounts();
    return;
  }
  if ((entry.id == 36 || entry.id == 37) && m_places.placeMenuTarget()) {
    m_places.setPlaceHidden(*m_places.placeMenuTarget(), entry.id == 36);
    return;
  }
  if ((entry.id == 39 || entry.id == 40) && m_places.placeMenuTarget()) {
    const auto place = *m_places.placeMenuTarget();
    if (entry.id == 40) {
      if (place.mounted && !place.path.empty())
        launchNewWindow(place.path);
    } else if (!caps.canOpenProperties) {
      setStatus("Properties is unavailable in Administrator Mode");
    } else if (place.mounted && !place.path.empty()) {
      openProperties(place.path);
    } else {
      if (!m_propertiesWindow)
        m_propertiesWindow = std::make_unique<PropertiesWindow>(
            m_wayland, m_renderContext, m_configService, m_operations,
            [this](std::string text) { writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8"); }
        );
      if (!m_propertiesWindow->openVolume(place, m_surface.get()))
        setStatus("Could not open Properties window");
    }
    return;
  }
  if (entry.id >= 30 && entry.id <= 35 && m_places.placeMenuTarget()) {
    auto place = *m_places.placeMenuTarget();
    if (entry.id == 30) {
      if (place.identity == "personal:trash")
        navigateTrash();
      else if (place.mounted && !place.path.empty())
        navigate(place.path);
    } else if (entry.id == 31) {
      m_places.removeSavedPlace(place.identity);
    } else if (entry.id == 32) {
      if (!caps.canPinToPlaces) {
        setStatus("Pinning locations to Places is disabled in Administrator Mode");
        return;
      }
      m_places.savePlace(place.path);
    } else {
      m_places.operatePlace(
          place,
          entry.id == 33       ? filemanager::PlaceOperation::Mount
              : entry.id == 34 ? filemanager::PlaceOperation::Unmount
                               : filemanager::PlaceOperation::Eject
      );
    }
    return;
  }
  if (entry.id == 10) {
    if (m_menus.contextMenuTarget())
      activatePath(*m_menus.contextMenuTarget());
  } else if (entry.id == 22) {
    if (!caps.canOpenProperties) {
      setStatus("Properties is unavailable in Administrator Mode");
      return;
    }
    if (m_menus.contextMenuTarget() && tab.selectedPaths.size() == 1)
      openProperties(*m_menus.contextMenuTarget());
  } else if (entry.id == 1)
    copyToDesktopClipboard(false);
  else if (entry.id == 2)
    copyToDesktopClipboard(true);
  else if (entry.id == 3)
    pasteClipboard();
  else if (entry.id == 4)
    beginRename();
  else if (entry.id == 5 && !session.browsingTrash()) {
    if (!caps.canMutate || !caps.canTrash) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    auto targets = selectedPaths();
    if (!targets.empty())
      m_operations.trash(std::move(targets));
  } else if (entry.id == 6 && !session.browsingTrash()) {
    if (!caps.canMutate) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    auto targets = selectedPaths();
    if (!targets.empty()) {
      const auto prompt = filemanager::permanentDeletePrompt(targets, targets.size());
      showDialog(
          "Permanently delete?", prompt, {{"Cancel", 0}, {"Delete", 1}},
          [this, tabId = tab.id, targets = std::move(targets)](int response) mutable {
            const auto* origin = m_tabs.findTab(tabId);
            if (response == 1 && origin && origin->capabilities().canMutate)
              m_operations.removePermanently(std::move(targets));
          }
      );
    }
  } else if (entry.id >= 114 && entry.id <= 116) {
    if (!caps.canCreate) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    if (session.browsingTrash() || !currentDirectoryWritable()) {
      setStatus("This directory does not support file creation");
      return;
    }
    const int id = entry.id;
    const char* extension = id == 114 ? ".odt" : id == 115 ? ".ods" : ".odp";
    const char* asset = id == 114 ? "templates/filemanager/office/blank.odt"
        : id == 115               ? "templates/filemanager/office/blank.ods"
                                  : "templates/filemanager/office/blank.odp";
    const std::string defaultName = id == 114 ? "New Document" : id == 115 ? "New Spreadsheet" : "New Presentation";
    std::error_code destinationError;
    auto destination =
        filemanager::creationDestinationWithExtension(session.path(), defaultName, extension, destinationError);
    if (!destination) {
      setStatus(destinationError ? destinationError.message() : "Could not choose a filename");
      return;
    }
    if (const auto opId = m_operations.createFileFromTemplate(paths::assetPath(asset).string(), destination->string());
        opId != 0) {
      m_createRequests[opId] = CreateRequest{tab.id, session.navigationGeneration(), *destination};
    }
  } else if (entry.id >= 111 && entry.id <= 113) {
    if (tab.hasActiveAdminSession()) {
      if (entry.id == 113) {
        setStatus("This action is unavailable in Administrator Mode");
        return;
      }
      if (!caps.canCreateBasic || session.browsingTrash() || session.loading() || !currentDirectoryWritable()) {
        setStatus(
            session.loading() ? "Wait for the folder listing to finish"
                              : "This directory does not support file creation"
        );
        return;
      }
      if (m_adminMutationsInFlight.contains(tab.id) || m_adminInlineRenameTarget.has_value()) {
        setStatus("Wait for the current Administrator operation to finish");
        return;
      }
      const bool createFolder = (entry.id == 111);
      const filemanager::TabId tabId = tab.id;
      const auto sessionGen = tab.adminSession.sessionGeneration();
      const auto navGen = session.navigationGeneration();
      const auto dirPath = session.path();
      const std::string baseName = createFolder ? "New Folder" : "New File";
      const std::weak_ptr<bool> alive = m_alive;
      auto onDone = [this, alive, tabId, sessionGen, navGen, dirPath, createFolder](
                        bool ok, const filemanager::AdminMutationRecord& rec, filemanager::AdminStatusCode code,
                        const std::string& detail
                    ) {
        const auto locked = alive.lock();
        if (!locked || !*locked)
          return;
        if (const auto it = m_adminMutationsInFlight.find(tabId);
            it != m_adminMutationsInFlight.end() && it->second.sessionGeneration == sessionGen) {
          m_adminMutationsInFlight.erase(it);
        }
        auto* originTab = m_tabs.findTab(tabId);
        if (!originTab || !originTab->hasActiveAdminSession()
            || originTab->adminSession.sessionGeneration() != sessionGen) {
          return;
        }
        if (!ok) {
          const std::string errorMsg = adminMutationErrorMessage(code, detail);
          if (tabId == m_tabs.activeTabId())
            setStatus(errorMsg);
          else
            originTab->lastStatusMessage = errorMsg;
          return;
        }
        const std::filesystem::path createdPath = dirPath / rec.name;
        const std::string statusMsg = (createFolder ? "Created folder \"" : "Created file \"")
            + filemanager::sanitizeFilenameForDisplay(rec.name) + "\"";
        if (tabId == m_tabs.activeTabId()) {
          setStatus(statusMsg);
          originTab->operationResultAfterRefresh = statusMsg;
        } else {
          originTab->lastStatusMessage = statusMsg;
          originTab->operationResultAfterRefresh = statusMsg;
        }
        if (originTab->session.navigationGeneration() == navGen && originTab->session.path() == dirPath) {
          originTab->selectedPath = createdPath;
          originTab->selectedPaths.clear();
          originTab->selectedPaths.insert(createdPath.string());
          originTab->selectionAnchor = createdPath;
          if (tabId == m_tabs.activeTabId()) {
            m_pendingInlineRenamePath = createdPath;
            originTab->session.setNextEnumerateUserInitiated(false);
            originTab->session.refreshPreservingSelection();
          } else {
            originTab->needsRefresh = true;
          }
        }
        markOtherTabsDirtyForDirectory(tabId, dirPath);
      };
      const auto reqId = createFolder
          ? tab.adminSession.createDirectoryAsync(dirPath, baseName, true, std::move(onDone))
          : tab.adminSession.createFileAsync(dirPath, baseName, true, std::move(onDone));
      if (reqId != filemanager::kInvalidAdminRequestId)
        m_adminMutationsInFlight[tabId] = AdminMutationRequest{reqId, sessionGen};
      return;
    }
    if (!caps.canCreate) {
      setStatus("This action is unavailable in Administrator Mode");
      return;
    }
    if (session.browsingTrash() || !currentDirectoryWritable()) {
      setStatus("This directory does not support file creation");
      return;
    }
    const int id = entry.id;
    const std::string defaultName = id == 111 ? "New Folder" : "New File";
    std::error_code error;
    auto destination = filemanager::creationDestination(session.path(), defaultName, id == 113, error);
    if (!destination) {
      setStatus(error ? error.message() : "Could not choose a filename");
      return;
    }
    const auto opId =
        id == 111 ? m_operations.createFolder(destination->string()) : m_operations.createFile(destination->string());
    if (opId != 0)
      m_createRequests[opId] = CreateRequest{tab.id, session.navigationGeneration(), *destination};
  } else if (entry.id == 13)
    enumerate(session.path());
  else if (entry.id == 14 || entry.id == kListView || entry.id == kGridView) {
    recordAdminTabUserActivity(tab);
    setGridMode(entry.id == 14 ? !m_listHeader.gridMode() : entry.id == kGridView);
  } else if (entry.id >= kZoomIn && entry.id <= kResetZoom) {
    recordAdminTabUserActivity(tab);
    adjustViewScale(
        entry.id == kResetZoom ? filemanager::kFileViewDefaultScale
                               : m_listHeader.viewScale()
                + (entry.id == kZoomIn ? filemanager::kFileViewZoomStep : -filemanager::kFileViewZoomStep)
    );
  } else if (
      entry.id >= kFirstColumnAction
      && entry.id < kFirstColumnAction + static_cast<int>(filemanager::kFileColumns.size())
  ) {
    recordAdminTabUserActivity(tab);
    const auto column = static_cast<FileColumn>(entry.id - kFirstColumnAction);
    if (column != FileColumn::Name) {
      auto& visible = m_listHeader.columnLayout().visible[columnIndex(column)];
      visible = !visible;
      if (column == FileColumn::Size && !visible) {
        for (const auto& tabPtr : m_tabs.tabs())
          tabPtr->session.clearFolderCounts();
      }
      if (!m_listHeader.gridMode())
        m_listHeader.updateColumnHeaders();
      m_listHeader.persistColumnLayout();
    }
  } else if (entry.id >= 53 && entry.id <= 57) {
    recordAdminTabUserActivity(tab);
    const auto field = static_cast<FileDialogSortField>(entry.id - 53);
    if (field != m_listHeader.sortField())
      setSortField(field);
  } else if (entry.id == 61) {
    recordAdminTabUserActivity(tab);
    if (m_listHeader.sortField() != FileDialogSortField::Type)
      setSortField(FileDialogSortField::Type);
  } else if (entry.id == 58 || entry.id == 62) {
    recordAdminTabUserActivity(tab);
    const auto requested = entry.id == 58 ? FileDialogSortOrder::Ascending : FileDialogSortOrder::Descending;
    if (m_listHeader.sortOrder() != requested)
      toggleSortOrder();
  } else if (entry.id == 50) {
    recordAdminTabUserActivity(tab);
    session.setShowHidden(!session.showHidden());
    persistFileViewState();
    enumerate(session.path());
  } else if (entry.id == 51 || entry.id == 52) {
    recordAdminTabUserActivity(tab);
    if (entry.id == 51)
      session.setFoldersFirst(!session.foldersFirst());
    else
      session.setHiddenLast(!session.hiddenLast());
    persistFileViewState();
    sortEntries();
    if (m_grid) {
      m_grid->notifyDataChanged();
      restoreSelection();
    }
  }
}

void FileManager::openProperties(const std::filesystem::path& path) {
  if (!activeTab().capabilities().canOpenProperties) {
    setStatus("Properties is unavailable in Administrator Mode");
    return;
  }
  if (!m_propertiesWindow)
    m_propertiesWindow = std::make_unique<PropertiesWindow>(
        m_wayland, m_renderContext, m_configService, m_operations,
        [this](std::string text) { writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8"); }
    );
  if (!m_propertiesWindow->open(path, m_surface.get()))
    setStatus("Could not open Properties window");
}

void FileManager::persistFileViewState() {
  auto& tab = activeTab();
  tab.viewState.gridMode = m_listHeader.gridMode();
  tab.viewState.showHidden = tab.session.showHidden();
  tab.viewState.foldersFirst = tab.session.foldersFirst();
  tab.viewState.hiddenLast = tab.session.hiddenLast();
  tab.viewState.sortField = m_listHeader.sortField();
  tab.viewState.sortOrder = m_listHeader.sortOrder();
  tab.viewState.viewScale = m_listHeader.viewScale();
  if (!m_settings.saveFileView({
          .gridMode = tab.viewState.gridMode,
          .showHidden = tab.viewState.showHidden,
          .foldersFirst = tab.viewState.foldersFirst,
          .hiddenLast = tab.viewState.hiddenLast,
          .sortField = tab.viewState.sortField,
          .sortOrder = tab.viewState.sortOrder,
      }))
    setStatus("Unable to save file view preferences");
}

void FileManager::launchNewWindow(const std::filesystem::path& path) {
  std::error_code pathError;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", pathError);
  if (pathError) {
    setStatus("Could not locate the Files executable");
    return;
  }
  const std::string executableText = executable.string();
  const std::string pathText = path.string();
  gchar* argv[] = {const_cast<gchar*>(executableText.c_str()), const_cast<gchar*>(pathText.c_str()), nullptr};
  GError* error = nullptr;
  if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
    setStatus(error ? error->message : "Could not open a new window");
    g_clear_error(&error);
  }
}

void FileManager::restoreSelection(bool chooseFirst) {
  if (m_grid == nullptr)
    return;
  auto& tab = activeTab();
  auto& session = tab.session;
  const auto setGridSelection = [this, &tab](std::optional<std::size_t> index) {
    m_restoringSelection = true;
    // Metadata/monitor refreshes before the next layout must not cancel the
    // paste's pending reveal in VirtualGridView.
    const bool reveal = index
        && tab.pendingPasteRevealPath
        && tab.selectedPath == tab.pendingPasteRevealPath
        && !tab.pendingDropScrollGeneration;
    m_grid->setSelectedIndex(index, reveal);
    m_restoringSelection = false;
  };
  if (tab.selectedPath.has_value()) {
    const auto selected = std::ranges::find(session.entries(), *tab.selectedPath, &FileEntry::absPath);
    if (selected != session.entries().end()) {
      setGridSelection(static_cast<std::size_t>(selected - session.entries().begin()));
      return;
    }
    if (session.loading())
      return;
    tab.selectedPath.reset();
  }
  if (chooseFirst && !session.entries().empty()) {
    setGridSelection(0);
    return;
  }
  setGridSelection(std::nullopt);
}

void FileManager::activateSelected() {
  if (m_grid == nullptr)
    return;
  const auto& entries = activeSession().entries();
  const auto selected = m_grid->selectedIndex();
  if (!selected || *selected >= entries.size())
    return;
  activatePath(entries[*selected].absPath);
}

void FileManager::activatePath(const std::filesystem::path& path) {
  auto& tab = activeTab();
  const auto& session = tab.session;
  const auto found = std::ranges::find(session.entries(), path, &FileEntry::absPath);
  if (found == session.entries().end())
    return;
  const auto entry = *found;
  if (entry.isDir) {
    navigate(entry.absPath);
    return;
  }
  if (tab.hasActiveAdminSession() && !tab.capabilities().canLaunchApps) {
    setStatus("Opening files with external applications is disabled in Administrator Mode");
    return;
  }
  if (session.browsingTrash() && !session.trashUris().contains(entry.absPath.string())) {
    setStatus("This Wastebin item is no longer available; refresh the view");
    return;
  }
  GFile* file = session.browsingTrash() ? g_file_new_for_uri(session.trashUris().at(entry.absPath.string()).c_str())
                                        : g_file_new_for_path(entry.absPath.c_str());
  char* uri = g_file_get_uri(file);
  struct LaunchRequest {
    FileManager* manager;
    std::weak_ptr<bool> alive;
    filemanager::TabId tabId;
  };
  g_app_info_launch_default_for_uri_async(
      uri, nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<LaunchRequest> request(static_cast<LaunchRequest*>(data));
        GError* error = nullptr;
        const bool launched = g_app_info_launch_default_for_uri_finish(result, &error);
        if (const auto locked = request->alive.lock(); locked && *locked) {
          std::string message = !launched ? (error == nullptr ? "Could not open this file" : error->message) : "";
          if (request->tabId == request->manager->m_tabs.activeTabId())
            request->manager->setStatus(std::move(message));
          else if (auto* originTab = request->manager->m_tabs.findTab(request->tabId))
            originTab->lastStatusMessage = std::move(message);
        }
        g_clear_error(&error);
      },
      new LaunchRequest{this, m_alive, tab.id}
  );
  g_free(uri);
  g_object_unref(file);
}

void FileManager::activatePointerItem(std::size_t index) {
  if (!m_grid || index >= activeSession().entries().size())
    return;
  // Administrator clicks arrive directly from the grid, without the drag handler's modifier check.
  if (activeTab().hasActiveAdminSession() && (m_keyboardModifiers & (KeyMod::Ctrl | KeyMod::Shift))) {
    m_lastPointerIndex.reset();
    return;
  }
  m_grid->setSelectedIndex(index, false);
  const std::uint32_t elapsed = m_pointerClickTime - m_lastPointerClickTime;
  if (m_lastPointerIndex == index && elapsed <= 500U) {
    m_lastPointerIndex.reset();
    activateSelected();
    return;
  }
  m_lastPointerIndex = index;
  m_lastPointerClickTime = m_pointerClickTime;
}

void FileManager::setStatus(std::string text) {
  m_statusMessage = std::move(text);
  m_statusMessageUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  updateStatusBar();
  m_statusMessageTimer.stop();
  if (!m_statusMessage.empty())
    m_statusMessageTimer.start(std::chrono::seconds(5), [this] {
      m_statusMessage.clear();
      updateStatusBar();
      if (m_surface)
        m_surface->requestRedraw();
    });
}

void FileManager::updateSelectionSummary() { updateStatusBar(); }

void FileManager::updateStatusBar() {
  if (!m_statusLabel)
    return;
  auto& tab = activeTab();
  const auto& session = tab.session;
  std::string primary;
  std::string details;
  std::string freeSpace;
  if (!m_statusMessage.empty() && std::chrono::steady_clock::now() < m_statusMessageUntil) {
    primary = m_statusMessage;
  } else if (tab.selectedPaths.size() == 1) {
    const std::filesystem::path path(*tab.selectedPaths.begin());
    const auto entry = std::ranges::find(session.entries(), path, &FileEntry::absPath);
    const std::string name = entry == session.entries().end() ? path.filename().string() : entry->name;
    primary = filemanager::sanitizeFilenameForDisplay(name);
    if (const auto& info = tab.selectionInfo(); info && info->path == path) {
      details = info->type;
      if (!info->target.empty())
        details += "  |  Target: " + filemanager::sanitizeFilenameForDisplay(info->target);
      if (info->hasSize) {
        const auto formatted = compactSize(info->size);
        std::ostringstream out;
        out
            << std::fixed
            << std::setprecision(formatted.value == std::round(formatted.value) ? 0 : 1)
            << formatted.value;
        details += "  |  " + out.str() + " " + formatted.unit;
      }
    } else {
      const bool isDir = entry != session.entries().end() && entry->isDir;
      const std::string entryType = entry != session.entries().end() ? entry->type : std::string{};
      details = guessFileTypeDescription(path, isDir, entryType);
      if (entry != session.entries().end() && !entry->isDir) {
        const auto formatted = compactSize(entry->size);
        std::ostringstream out;
        out
            << std::fixed
            << std::setprecision(formatted.value == std::round(formatted.value) ? 0 : 1)
            << formatted.value;
        details += "  |  " + out.str() + " " + formatted.unit;
      }
      if (tab.selectionInfoPendingPath() != path)
        querySelectionInfoAsync(path);
    }
  } else if (!tab.selectedPaths.empty()) {
    cancelSelectionInfoQuery();
    std::size_t folders = 0, files = 0;
    std::uint64_t bytes = 0;
    bool hasSize = false;
    for (const auto& selected : tab.selectedPaths) {
      const auto found = std::ranges::find(session.entries(), std::filesystem::path(selected), &FileEntry::absPath);
      if (found == session.entries().end())
        continue;
      if (found->isDir)
        ++folders;
      else {
        ++files;
        bytes += found->size;
        hasSize = true;
      }
    }
    primary = std::to_string(tab.selectedPaths.size()) + " selected";
    details = std::to_string(folders) + " folders, " + std::to_string(files) + " files";
    if (hasSize) {
      const auto size = compactSize(bytes);
      std::ostringstream out;
      out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
      details += "  |  " + out.str() + " " + size.unit;
    }
  } else {
    cancelSelectionInfoQuery();
    std::size_t folders = 0, files = 0;
    std::uint64_t bytes = 0;
    for (const auto& entry : session.entries()) {
      if (entry.isDir)
        ++folders;
      else {
        ++files;
        bytes += entry.size;
      }
    }
    primary = std::to_string(folders) + " folders";
    details = std::to_string(files) + " files";
    if (files) {
      const auto size = compactSize(bytes);
      std::ostringstream out;
      out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
      details += " · " + out.str() + " " + size.unit;
    }
    if (!session.browsingTrash()) {
      if (tab.statusFreeSpacePath() != session.path())
        queryStatusFreeSpaceAsync(session.path());
      if (tab.statusFreeSpacePath() == session.path() && tab.freeSpaceBytes().has_value()) {
        const auto size = compactSize(*tab.freeSpaceBytes());
        std::ostringstream out;
        out << std::fixed << std::setprecision(size.value == std::round(size.value) ? 0 : 1) << size.value;
        freeSpace = out.str() + " " + size.unit + " free";
      }
    }
  }
  m_statusLabel->setText(primary);
  if (m_statusDetailLabel)
    m_statusDetailLabel->setText(details.empty() ? "" : "  |  " + details);
  if (m_statusFreeLabel)
    m_statusFreeLabel->setText(freeSpace);
  m_statusLabel->setTooltip("");
}

void FileManager::updateWindowTitle() {
  m_windowTitle =
      filemanager::windowTitleForPath(activeSession().path(), g_get_home_dir(), activeTab().hasActiveAdminSession());
  if (m_surface)
    m_surface->setTitle(m_windowTitle);
}

std::unique_ptr<Flex> FileManager::buildHeaderAppBar() {
  auto appBar = std::make_unique<Flex>();
  m_appBar = appBar.get();
  appBar->setAlign(FlexAlign::Center);
  appBar->setGap(Style::spaceSm);
  appBar->setMinHeight(34.0F);
  appBar->setPadding(Style::spaceSm, Style::spaceMd);
  appBar->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh, 0.72F));
  appBar->setBorder(colorSpecFromRole(ColorRole::Primary, 0.38F), Style::borderWidth);
  applyNytheraBevel(*appBar, Style::scaledRadiusMd(1.0F));

  auto appName = std::make_unique<Flex>();
  m_appBrandContainer = appName.get();
  appName->setAlign(FlexAlign::Center);
  appName->setGap(9.0F); // The display font's space advance is 360/1000 em at 25 px.
  auto appNameNythera = std::make_unique<Label>();
  appNameNythera->setText("NYTHERA");
  appNameNythera->setFontSize(25.0F);
  appNameNythera->setDirectDisplayFont(true);
  appNameNythera->setColor(colorSpecFromRole(ColorRole::OnSurface));
  appName->addChild(std::move(appNameNythera));
  auto appNameFiles = std::make_unique<Label>();
  appNameFiles->setText("FILES");
  appNameFiles->setFontSize(25.0F);
  appNameFiles->setDirectDisplayFont(true);
  appNameFiles->setColor(colorSpecFromRole(ColorRole::Primary));
  appName->addChild(std::move(appNameFiles));
  appBar->addChild(std::move(appName));

  auto adminHeader = std::make_unique<Flex>();
  m_adminHeaderContainer = adminHeader.get();
  adminHeader->setAlign(FlexAlign::Center);
  adminHeader->setGap(Style::spaceSm);

  auto separator = std::make_unique<Box>();
  m_adminHeaderSeparator = separator.get();
  separator->setFrameSize(1.0F, 18.0F);
  separator->setFill(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  adminHeader->addChild(std::move(separator));

  auto shield = std::make_unique<Glyph>();
  m_adminHeaderShield = shield.get();
  shield->setGlyph("shield");
  shield->setGlyphSize(15.0F);
  shield->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  adminHeader->addChild(std::move(shield));

  auto adminLabel = std::make_unique<Label>();
  m_adminHeaderLabel = adminLabel.get();
  adminLabel->setText("Administrator Mode");
  adminLabel->setFontSize(Style::fontSizeBody);
  adminLabel->setFontWeight(FontWeight::SemiBold);
  adminLabel->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  adminLabel->setMaxLines(1);
  adminHeader->addChild(std::move(adminLabel));

  auto finishButton = std::make_unique<Button>();
  m_adminFinishButton = finishButton.get();
  finishButton->setText("Finish");
  finishButton->setVariant(ButtonVariant::Outline);
  finishButton->setControlHeight(24.0F);
  finishButton->setPadding(3.0F, 8.0F);
  finishButton->setRadius(Style::scaledRadiusSm(1.0F));
  finishButton->setFocusHighlightEnabled(false);
  finishButton->setTooltip("Exit Administrator Mode for this tab");
  finishButton->setOnClick([this] { finishAdministratorMode(activeTab().id, "Exited Administrator Mode."); });
  adminHeader->addChild(std::move(finishButton));

  appBar->addChild(std::move(adminHeader));

  auto appSpacer = std::make_unique<Flex>();
  appSpacer->setFlexGrow(1.0F);
  appBar->addChild(std::move(appSpacer));
  m_menus.attachMenuBarUi(*appBar, kToolbarHeight);

  updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
  return appBar;
}

void FileManager::updateAdminHeaderUi(int surfaceWidth) {
  if (surfaceWidth > 0)
    m_lastHeaderWidth = surfaceWidth;
  const int effectiveWidth = surfaceWidth > 0 ? surfaceWidth : m_lastHeaderWidth;
  const bool adminActive = activeTab().hasActiveAdminSession();
  // At narrow window widths, prioritise the administrator status and Finish control over branding.
  const bool compactNarrowAdmin = adminActive && effectiveWidth > 0 && effectiveWidth < 760;

  if (m_appBrandContainer != nullptr) {
    m_appBrandContainer->setVisible(!compactNarrowAdmin);
    m_appBrandContainer->setParticipatesInLayout(!compactNarrowAdmin);
  }
  if (m_adminHeaderContainer != nullptr) {
    m_adminHeaderContainer->setVisible(adminActive);
    m_adminHeaderContainer->setParticipatesInLayout(adminActive);
  }
  if (m_adminHeaderSeparator != nullptr) {
    m_adminHeaderSeparator->setVisible(adminActive && !compactNarrowAdmin);
    m_adminHeaderSeparator->setParticipatesInLayout(adminActive && !compactNarrowAdmin);
    m_adminHeaderSeparator->setFill(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  }
  if (m_adminHeaderShield != nullptr)
    m_adminHeaderShield->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  if (m_adminHeaderLabel != nullptr)
    m_adminHeaderLabel->setColor(colorSpecFromRole(filemanager::kAdminWarningColorRole));
  if (m_adminFinishButton != nullptr) {
    auto btnPalette = Button::defaultPalette(ButtonVariant::Outline);
    btnPalette.normal = Button::ButtonStateColors{
        .bg = colorSpecFromRole(ColorRole::ErrorContainer),
        .border = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .label = colorSpecFromRole(filemanager::kAdminWarningColorRole),
    };
    btnPalette.hover = Button::ButtonStateColors{
        .bg = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .border = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .label = colorSpecFromRole(ColorRole::OnError),
    };
    btnPalette.pressed = Button::ButtonStateColors{
        .bg = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .border = colorSpecFromRole(filemanager::kAdminWarningColorRole),
        .label = colorSpecFromRole(ColorRole::OnError),
    };
    m_adminFinishButton->setCustomPalette(std::move(btnPalette));
  }
}

bool FileManager::enterAdministratorMode(
    filemanager::TabId tabId, std::optional<std::filesystem::path> targetDirectory
) {
  auto* tab = m_tabs.findTab(tabId);
  if (tab == nullptr)
    return false;
  if (tab->session.browsingTrash() || (targetDirectory && filemanager::isTrashTabPath(*targetDirectory))) {
    if (tabId == m_tabs.activeTabId())
      setStatus("Administrator Mode is unavailable in Wastebin");
    return false;
  }
  if (tab->hasActiveAdminSession()) {
    if (targetDirectory && !targetDirectory->empty()) {
      tab->session.navigate(*targetDirectory, true);
      if (tabId == m_tabs.activeTabId()) {
        rebuildBreadcrumbs();
        updateNavigationControls();
        updateWindowTitle();
      }
      return true;
    }
    return false;
  }
  if (tab->adminSession.isAuthenticating()) {
    if (tabId == m_tabs.activeTabId())
      setStatus("Administrator authentication is already in progress for this tab");
    return false;
  }
  if (!tab->adminSession.hasCustomTimeoutForTesting()) {
    const int timeoutSeconds = m_settings.adminInactivityTimeoutSeconds();
    tab->adminSession.setInactivityTimeoutMs(static_cast<std::uint32_t>(timeoutSeconds) * 1000U);
  }
  const std::weak_ptr<bool> alive = m_alive;
  const bool started = tab->adminSession.startSessionAsync(
      [this, alive, tabId, targetDirectory = std::move(targetDirectory)](
          bool, const filemanager::AdminSessionStatus& status
      ) {
        const auto locked = alive.lock();
        if (!locked || !*locked)
          return;
        const filemanager::TabId completedTabId = status.tabId != filemanager::kInvalidTabId ? status.tabId : tabId;
        auto* currentTab = m_tabs.findTab(completedTabId);
        if (currentTab == nullptr)
          return;
        if (status.state == filemanager::AdminSessionState::Active) {
          currentTab->administratorBrowsing = true;
          if (m_renamePath && completedTabId == m_tabs.activeTabId())
            cancelRename();
          if (completedTabId == m_tabs.activeTabId()) {
            cancelFileGesture();
            m_menus.closeAll();
            m_menus.setContextMenuTarget(std::nullopt);
          }
          currentTab->cancelStatusFreeSpaceQuery();
          currentTab->cancelSelectionInfoQuery();
          if (targetDirectory && !targetDirectory->empty()) {
            currentTab->session.navigate(*targetDirectory, true);
          } else {
            currentTab->session.enumerate(currentTab->session.path());
          }
          m_tabBar.markDirty();
          if (completedTabId == m_tabs.activeTabId()) {
            rebuildBreadcrumbs();
            if (m_locationInput != nullptr)
              m_locationInput->setValue(currentTab->session.path().string());
            updateNavigationControls();
            updateWindowTitle();
            updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
            updateStatusBar();
            setStatus("Administrator Mode active.");
          } else {
            currentTab->lastStatusMessage = "Administrator Mode active.";
          }
          if (m_surface)
            m_surface->requestLayout();
          return;
        }
        if (status.state == filemanager::AdminSessionState::Inactive) {
          // Cancelled by user or closed tab.
          if (completedTabId == m_tabs.activeTabId())
            setStatus("Administrator authentication cancelled.");
          else
            currentTab->lastStatusMessage = "Administrator authentication cancelled.";
          return;
        }
        if (status.state == filemanager::AdminSessionState::Failed) {
          const std::string msg =
              status.message.empty() ? "Administrator authentication failed." : status.message;
          if (completedTabId == m_tabs.activeTabId())
            setStatus(msg);
          else
            currentTab->lastStatusMessage = msg;
        }
      }
  );
  if (started && tabId == m_tabs.activeTabId())
    setStatus("Authenticating Administrator Mode…");
  return started;
}

void FileManager::finishAdministratorMode(filemanager::TabId tabId, std::string statusMessage) {
  auto* tab = m_tabs.findTab(tabId);
  if (tab == nullptr)
    return;
  m_adminMutationsInFlight.erase(tabId);
  if (m_adminInlineRenameTarget && m_adminInlineRenameTarget->tabId == tabId)
    m_adminInlineRenameTarget.reset();
  if (tabId == m_tabs.activeTabId()) {
    m_pendingInlineRenamePath.reset();
    if (m_renamePath) {
      m_refreshAfterRename = false;
      cancelRename();
    }
    cancelFileGesture();
    m_menus.closeAll();
    m_menus.setContextMenuTarget(std::nullopt);
    if (m_grid)
      m_grid->resetTransientStateForDataSwap();
  }
  tab->leaveAdministratorMode();
  tab->needsRefresh = false;
  struct Recovery {
    FileManager* manager;
    std::weak_ptr<bool> alive;
    filemanager::TabId tabId;
    std::uint64_t enumerationGeneration;
    std::filesystem::path currentPath;
    decltype(m_unprivilegedDirectoryAccessibleHook) accessHook;
    std::string statusMessage;
  };
  GTask* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        auto* completed = G_TASK(result);
        const auto& recovery = *static_cast<Recovery*>(g_task_get_task_data(completed));
        const std::unique_ptr<std::filesystem::path> path(
            static_cast<std::filesystem::path*>(g_task_propagate_pointer(completed, nullptr))
        );
        const auto alive = recovery.alive.lock();
        if (!alive || !*alive || !path)
          return;
        auto& manager = *recovery.manager;
        auto* origin = manager.m_tabs.findTab(recovery.tabId);
        if (!origin || origin->session.enumerationGeneration() != recovery.enumerationGeneration
            || origin->hasActiveAdminSession())
          return;
        if (*path == origin->session.path())
          origin->session.enumerate(*path);
        else
          origin->session.navigate(*path, false);
        if (origin->active && !recovery.statusMessage.empty())
          manager.setStatus(recovery.statusMessage);
        else
          origin->lastStatusMessage = recovery.statusMessage;
      },
      nullptr
  );
  g_task_set_task_data(
      task, new Recovery{this, m_alive, tabId, tab->session.enumerationGeneration(), tab->session.path(),
                         m_unprivilegedDirectoryAccessibleHook, statusMessage},
      [](gpointer data) { delete static_cast<Recovery*>(data); }
  );
  g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
    const auto& recovery = *static_cast<Recovery*>(data);
    const auto accessible = [&](const std::filesystem::path& path) {
      if (path.empty() || filemanager::isTrashTabPath(path))
        return false;
      if (recovery.accessHook) {
        if (const auto overridden = recovery.accessHook(path); overridden.has_value())
          return *overridden;
      }
      std::error_code ec;
      return std::filesystem::is_directory(path, ec) && ::access(path.c_str(), R_OK | X_OK) == 0;
    };
    auto path = recovery.currentPath;
    if (!accessible(path)) {
      path = defaultTabPath();
      if (!accessible(path)) {
        path = g_get_home_dir();
        if (!accessible(path))
          path = "/";
      }
    }
    g_task_return_pointer(
        worker, new std::filesystem::path(std::move(path)),
        [](gpointer value) { delete static_cast<std::filesystem::path*>(value); }
    );
  });
  g_object_unref(task);
  m_tabBar.markDirty();
  if (tabId == m_tabs.activeTabId()) {
    if (m_grid)
      m_grid->notifyDataChanged();
    rebuildBreadcrumbs();
    if (m_locationInput != nullptr)
      m_locationInput->setValue(tab->session.path().string());
    updateNavigationControls();
    updateWindowTitle();
    updateAdminHeaderUi(m_surface ? m_surface->width() : m_lastHeaderWidth);
    updateStatusBar();
    if (!statusMessage.empty())
      setStatus(std::move(statusMessage));
  } else if (!statusMessage.empty()) {
    tab->lastStatusMessage = std::move(statusMessage);
  }
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::recordAdminTabUserActivity(filemanager::BrowserTab& tab) {
  if (tab.hasActiveAdminSession())
    tab.adminSession.renewActivityAsync();
}

std::string FileManager::adminMutationErrorMessage(filemanager::AdminStatusCode code, const std::string& detail) {
  switch (code) {
  case filemanager::AdminStatusCode::AlreadyExists:
    return "An item with that name already exists";
  case filemanager::AdminStatusCode::ReadOnlyFilesystem:
    return "This filesystem is mounted read-only";
  case filemanager::AdminStatusCode::IdentityMismatch:
    return detail.empty() ? "The target changed before the operation completed; refresh and try again" : detail;
  case filemanager::AdminStatusCode::NotSupported:
    return detail.empty() ? "This filesystem does not support safe non-overwriting renames" : detail;
  case filemanager::AdminStatusCode::Unauthorized:
    return "This directory is not authorised in the current Administrator session";
  case filemanager::AdminStatusCode::AccessDenied:
    return detail.empty() ? "Administrator permission denied for this operation" : detail;
  case filemanager::AdminStatusCode::NotFound:
    return detail.empty() ? "The item or directory is no longer available" : detail;
  case filemanager::AdminStatusCode::NotDirectory:
    return "The target path is not a directory";
  case filemanager::AdminStatusCode::BadRequest:
    return detail.empty() ? "Choose a valid filename" : detail;
  default:
    return detail.empty() ? "The Administrator operation failed" : detail;
  }
}

void FileManager::markOtherTabsDirtyForDirectory(
    filemanager::TabId originTabId, const std::filesystem::path& dirPath
) {
  const auto normalizedDir = filemanager::normalizeTabPath(dirPath);
  for (const auto& tabPtr : m_tabs.tabs()) {
    if (tabPtr->id == originTabId || tabPtr->session.browsingTrash())
      continue;
    if (filemanager::normalizeTabPath(tabPtr->session.path()) == normalizedDir) {
      tabPtr->needsRefresh = true;
      continue;
    }
    const auto same = m_places.sameDirectoryPathCached(tabPtr->session.path(), dirPath);
    if (same.value_or(false))
      tabPtr->needsRefresh = true;
  }
}

void FileManager::updateNavigationControls() {
  const auto& session = activeSession();
  if (m_backButton != nullptr)
    m_backButton->setEnabled(!session.backHistory().empty());
  if (m_forwardButton != nullptr)
    m_forwardButton->setEnabled(!session.forwardHistory().empty());
  if (m_upButton != nullptr)
    m_upButton->setEnabled(
        session.path().has_parent_path()
        && session.path().parent_path() != session.path()
        && (!session.browsingTrash() || session.path() != std::filesystem::path("trash:/"))
    );
  m_places.updateActiveHighlight(session.path(), session.browsingTrash());
  syncSortControls();
}

void FileManager::textPrompt(
    std::string title, std::string message, bool password, std::function<void(std::optional<std::string>)> response
) {
  m_modalDialog.textPrompt(std::move(title), std::move(message), password, std::move(response));
}

void FileManager::setGridMode(bool gridMode) { m_listHeader.setGridMode(gridMode); }

void FileManager::adjustViewScale(float value) { m_listHeader.adjustViewScale(value); }

void FileManager::applyViewScale() {
  if (!m_grid)
    return;
  cancelFileGesture();
  const auto metrics = filemanager::fileViewMetrics(m_listHeader.viewScale());
  m_grid->setAdapter(nullptr);
  m_grid->setAdapter(m_adapter.get());
  m_grid->setColumns(m_listHeader.gridMode() ? 0 : 1);
  m_grid->setMinCellWidth(metrics.gridCellWidth);
  m_grid->setCellHeight(m_listHeader.gridMode() ? metrics.gridCellHeight : metrics.listRowHeight);
  m_grid->setRowGap(m_listHeader.gridMode() ? 3.0F : 1.0F);
  restoreSelection();
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::rebuildBreadcrumbs() {
  if (m_breadcrumbBar == nullptr)
    return;
  while (!m_breadcrumbBar->children().empty())
    m_breadcrumbBar->removeChild(m_breadcrumbBar->children().front().get());
  auto addCrumb = [this](std::string label, std::filesystem::path path, bool last) {
    if (!m_breadcrumbBar->children().empty()) {
      auto separator = std::make_unique<Label>();
      separator->setText("›");
      separator->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      separator->setFontSize(Style::fontSizeCaption);
      m_breadcrumbBar->addChild(std::move(separator));
    }
    auto button = std::make_unique<Button>();
    button->setText(filemanager::sanitizeFilenameForDisplay(label));
    button->setVariant(ButtonVariant::Ghost);
    button->setFocusHighlightEnabled(false);
    useSubtleSelection(*button);
    button->setSelected(false);
    if (last && button->label())
      button->label()->setFontWeight(FontWeight::SemiBold);
    button->setControlHeight(kToolbarHeight);
    button->setPadding(5.0F);
    button->setOnClick([this, path = std::move(path)] { navigate(path); });
    m_breadcrumbBar->addChild(std::move(button));
  };
  const auto crumbs = filemanager::fileBreadcrumbs(activeSession().path());
  for (std::size_t i = 0; i < crumbs.size(); ++i)
    addCrumb(crumbs[i].label, crumbs[i].path, i + 1 == crumbs.size());
  auto emptyField = std::make_unique<Button>();
  emptyField->setVariant(ButtonVariant::Ghost);
  emptyField->setFocusHighlightEnabled(false);
  emptyField->setControlHeight(kToolbarHeight);
  emptyField->setFlexGrow(1.0F);
  emptyField->setOnClick([this] { setLocationEditMode(true); });
  m_breadcrumbBar->addChild(std::move(emptyField));
}

void FileManager::setLocationEditMode(bool editing, bool preserveOutsideClickFocus) {
  ++m_locationEditGeneration;
  m_editingLocation = editing;
  if (m_locationInput != nullptr) {
    m_locationInput->setVisible(editing);
    m_locationInput->setParticipatesInLayout(editing);
    if (!editing)
      m_locationInput->setValue(activeSession().path().string());
  }
  if (m_breadcrumbBar != nullptr) {
    m_breadcrumbBar->setVisible(!editing);
    m_breadcrumbBar->setParticipatesInLayout(!editing);
  }
  if (editing && m_locationInput != nullptr) {
    m_locationInput->setValue(activeSession().path().string());
    m_dispatcher.setFocus(m_locationInput->inputArea());
    m_locationInput->selectAll();
  } else if (m_grid != nullptr) {
    if (!preserveOutsideClickFocus
        || m_dispatcher.focusedArea() == (m_locationInput ? m_locationInput->inputArea() : nullptr))
      m_dispatcher.setFocus(m_grid->focusArea());
  }
  if (m_surface != nullptr)
    m_surface->requestLayout();
}

void FileManager::deferCancelLocationEdit() {
  if (!m_editingLocation)
    return;
  const auto generation = m_locationEditGeneration;
  const std::weak_ptr<bool> alive = m_alive;
  DeferredCall::callLater([this, alive, generation] {
    const auto locked = alive.lock();
    if (locked
        && *locked
        && filemanager::locationEditCancelIsCurrent(m_editingLocation, m_locationEditGeneration, generation))
      setLocationEditMode(false, true);
  });
}

void FileManager::pasteLocationFromClipboard() {
  if (!m_windowOpen || !m_editingLocation || !m_locationInput
      || m_dispatcher.focusedArea() != m_locationInput->inputArea())
    return;

  const auto generation = m_locationEditGeneration;
  const auto original = m_locationInput->textInputState();
  auto onText = [this, alive = std::weak_ptr<bool>(m_alive), generation, original](std::optional<std::string> text) {
    const auto locked = alive.lock();
    if (!locked || !*locked || !m_windowOpen || !m_editingLocation || generation != m_locationEditGeneration
        || !m_locationInput || m_dispatcher.focusedArea() != m_locationInput->inputArea())
      return;
    if (!text || text->empty() || text->find('\0') != std::string::npos
        || !g_utf8_validate(text->data(), static_cast<gssize>(text->size()), nullptr))
      return;

    const auto current = m_locationInput->textInputState();
    if (current.surroundingText != original.surroundingText
        || current.cursor != original.cursor || current.anchor != original.anchor)
      return;
    m_locationInput->textInputApplyEdit({.commitText = std::move(*text), .hasCommitText = true});
  };
  if (m_readLocationClipboardForTesting)
    m_readLocationClipboardForTesting(std::move(onText));
  else
    m_transfer.readTextSelection(std::move(onText));
}

void FileManager::setSortField(FileDialogSortField field) { m_listHeader.setSortField(field); }

void FileManager::toggleSortOrder() { m_listHeader.toggleSortOrder(); }

void FileManager::syncSortControls() { m_listHeader.syncSortControls(); }

void FileManager::prepareFrame(bool needsUpdate, bool needsLayout) {
  (void)needsUpdate;
  if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0)
    return;
  m_renderContext.makeCurrent(m_surface->renderTarget());
  if (m_places.isDirty())
    m_places.rebuild();
  Renderer& renderer = m_surface->renderTarget().renderer();
  if (m_pickerThumbnails.uploadPending(renderer.textureManager()) && m_grid)
    m_grid->notifyDataChanged();
  m_adapter->setRenderer(&renderer);
  if (!m_root) {
    constexpr float pad = 8.0F;
    m_root = std::make_unique<Flex>();
    m_root->setDirection(FlexDirection::Vertical);
    m_root->setAlign(FlexAlign::Stretch);
    m_root->setGap(Style::spaceSm);
    m_root->setPadding(pad);
    m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.88F));
    m_root->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.72F), Style::borderWidth);
    applyNytheraBevel(*m_root, Style::scaledRadiusLg(1.0F));

    m_root->addChild(buildHeaderAppBar());

    auto content = std::make_unique<Flex>();
    content->setAlign(FlexAlign::Stretch);
    content->setGap(2.0F);
    content->setFlexGrow(1.0F);

    auto sidebar = std::make_unique<Flex>();
    sidebar->setDirection(FlexDirection::Vertical);
    sidebar->setAlign(FlexAlign::Stretch);
    sidebar->setGap(3.0F);
    sidebar->setPadding(7.0F);
    m_sidebarWidth = m_settings.loadSidebarWidth(m_sidebarWidth, kSidebarMinWidth, kSidebarMaxWidth);
    sidebar->setMinWidth(m_sidebarWidth);
    sidebar->setMaxWidth(m_sidebarWidth);
    sidebar->setFill(colorSpecFromRole(ColorRole::SurfaceContainer, 0.62F));
    sidebar->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.42F), Style::borderWidth);
    applyNytheraBevel(*sidebar, Style::scaledRadiusMd(1.0F));
    auto placesScroll = std::make_unique<ScrollView>();
    placesScroll->setFlexGrow(1.0F);
    placesScroll->setScrollbarVisible(true);
    auto* placesScrollPtr = placesScroll.get();
    auto* placesContent = placesScroll->content();
    placesContent->setAlign(FlexAlign::Stretch);
    sidebar->addChild(std::move(placesScroll));
    m_places.attachUi(placesScrollPtr, placesContent);
    m_places.rebuild();
    m_sidebar = static_cast<Flex*>(content->addChild(std::move(sidebar)));

    auto divider = std::make_unique<Flex>();
    divider->setMinWidth(4.0F);
    divider->setMaxWidth(4.0F);
    divider->setFill(clearColorSpec());
    divider->setHitTestVisible(true);
    m_sidebarDivider = static_cast<Flex*>(content->addChild(std::move(divider)));

    auto mainPane = std::make_unique<Flex>();
    mainPane->setDirection(FlexDirection::Vertical);
    mainPane->setAlign(FlexAlign::Stretch);
    mainPane->setGap(2.0F);
    mainPane->setFlexGrow(1.0F);
    m_statusHeight = m_settings.loadStatusHeight(m_statusHeight, kStatusMinHeight, kStatusMaxHeight);

    auto navigation = std::make_unique<Flex>();
    navigation->setAlign(FlexAlign::Center);
    navigation->setGap(2.0F);
    navigation->addChild(makeButton(
        "‹",
        [this] {
          auto& session = activeSession();
          if (session.backHistory().empty())
            return;
          auto path = session.backHistory().back();
          session.backHistory().pop_back();
          session.forwardHistory().push_back(session.path());
          navigate(path, false);
        },
        &m_backButton
    ));
    navigation->addChild(makeButton(
        "›",
        [this] {
          auto& session = activeSession();
          if (session.forwardHistory().empty())
            return;
          auto path = session.forwardHistory().back();
          session.forwardHistory().pop_back();
          session.backHistory().push_back(session.path());
          navigate(path, false);
        },
        &m_forwardButton
    ));
    navigation->addChild(makeButton(
        "↑",
        [this] {
          const auto& session = activeSession();
          if (session.path().has_parent_path())
            navigate(session.path().parent_path());
        },
        &m_upButton
    ));

    auto locationBar = std::make_unique<Flex>();
    locationBar->setAlign(FlexAlign::Center);
    locationBar->setGap(2.0F);
    locationBar->setPadding(2.0F, 4.0F);
    locationBar->setFlexGrow(1.0F);
    locationBar->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.78F));
    locationBar->setBorder(colorSpecFromRole(ColorRole::Primary, 0.28F), Style::borderWidth);
    applyNytheraBevel(*locationBar, Style::scaledRadiusSm(1.0F));
    auto location = std::make_unique<Input>();
    m_locationInput = location.get();
    m_locationInput->setValue(activeSession().path().string());
    m_locationInput->setPlaceholder("Enter a folder path");
    m_locationInput->setFlexGrow(1.0F);
    // This standalone file-manager process does not share the shell's global Input clipboard.
    // Route text copies through the same Wayland clipboard writer used by its other copy actions.
    m_locationInput->setOnCopyText([this](std::string text) {
      if (!activeTab().hasActiveAdminSession())
        writeDesktopClipboard(std::move(text), "text/plain;charset=utf-8");
    });
    m_locationInput->setVisible(false);
    m_locationInput->setParticipatesInLayout(false);
    m_locationInput->setOnSubmit([this](const std::string& value) {
      navigate(value);
      setLocationEditMode(false);
    });
    m_locationInput->setOnFocusLoss([this] { deferCancelLocationEdit(); });
    locationBar->addChild(std::move(location));
    auto crumb = std::make_unique<Flex>();
    crumb->setAlign(FlexAlign::Center);
    crumb->setGap(1.0F);
    crumb->setFlexGrow(1.0F);
    crumb->setClipChildren(true);
    crumb->setMinHeight(kToolbarHeight);
    m_breadcrumbBar = static_cast<Flex*>(locationBar->addChild(std::move(crumb)));
    navigation->addChild(std::move(locationBar));
    navigation->addChild(makeButton("↻", [this] { activateContextMenuEntry({.id = 13}); }, &m_refreshButton));
    mainPane->addChild(std::move(navigation));

    mainPane->addChild(m_tabBar.buildUi());

    mainPane->addChild(m_listHeader.buildHeader());
    mainPane->addChild(
        ui::separator({
            .color = colorSpecFromRole(ColorRole::OutlineVariant, 0.45F),
            .gradientEdges = false,
        })
    );

    auto grid = std::make_unique<VirtualGridView>();
    m_grid = grid.get();
    m_listHeader.attachGrid(m_grid);
    m_grid->setFlexGrow(1.0F);
    m_grid->setFill(colorSpecFromRole(ColorRole::Surface, 0.38F));
    m_grid->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.26F), Style::borderWidth);
    applyNytheraBevel(*m_grid, Style::scaledRadiusMd(1.0F));
    m_grid->setAdapter(m_adapter.get());
    m_grid->setSquareCells(false);
    m_listHeader.ensureViewScaleLoaded();
    const auto metrics = filemanager::fileViewMetrics(m_listHeader.viewScale());
    m_grid->setColumns(m_listHeader.gridMode() ? 0 : 1);
    m_grid->setMinCellWidth(metrics.gridCellWidth);
    m_grid->setCellHeight(m_listHeader.gridMode() ? metrics.gridCellHeight : metrics.listRowHeight);
    m_grid->setRowGap(m_listHeader.gridMode() ? 3.0F : 1.0F);
    m_grid->bindScrollState(&activeTab().scrollState);
    m_listHeader.updateColumnHeaders();
    m_grid->setOnRubberBandSelection([this](const std::vector<std::size_t>& hits, bool finished, bool cancelled) {
      auto& tab = activeTab();
      const auto& entries = tab.session.entries();
      if (cancelled) {
        tab.selectedPaths = m_rubberBaseline;
        tab.selectedPath = m_rubberSelectedPath;
        tab.selectionAnchor = m_rubberAnchor;
      } else {
        const bool ctrl = m_rubberModifiers & KeyMod::Ctrl;
        const bool shift = m_rubberModifiers & KeyMod::Shift;
        tab.selectedPaths = (ctrl || shift) ? m_rubberBaseline : std::unordered_set<std::string>{};
        for (const auto index : hits) {
          if (index >= entries.size())
            continue;
          const auto path = entries[index].absPath.string();
          if (ctrl && m_rubberBaseline.contains(path))
            tab.selectedPaths.erase(path);
          else
            tab.selectedPaths.insert(path);
        }
        if (shift && !hits.empty() && m_rubberAnchor) {
          const auto anchor = std::ranges::find(entries, *m_rubberAnchor, &FileEntry::absPath);
          if (anchor != entries.end()) {
            const auto index = static_cast<std::size_t>(anchor - entries.begin());
            const auto endpoint = hits.back();
            for (auto i = std::min(index, endpoint); i <= std::max(index, endpoint) && i < entries.size(); ++i)
              tab.selectedPaths.insert(entries[i].absPath.string());
          }
        }
        if (!hits.empty() && hits.back() < entries.size())
          tab.selectedPath = entries[hits.back()].absPath;
        else if (tab.selectedPaths.empty())
          tab.selectedPath.reset();
        if (finished && !ctrl && !shift)
          tab.selectionAnchor = tab.selectedPath;
      }
      m_restoringSelection = true;
      const auto selected =
          tab.selectedPath ? std::ranges::find(entries, *tab.selectedPath, &FileEntry::absPath) : entries.end();
      m_grid->setSelectedIndex(
          selected == entries.end() ? std::nullopt : std::optional<std::size_t>(selected - entries.begin()), false
      );
      m_restoringSelection = false;
      m_grid->notifyDataChanged();
      updateSelectionSummary();
      if (finished) {
        m_rubberBaseline.clear();
        m_selectionTimer.stop();
      }
      if (m_surface)
        m_surface->requestRedraw();
    });
    m_grid->setOnSelectionChanged([this](std::optional<std::size_t> index) {
      if (m_restoringSelection)
        return;
      auto& tab = activeTab();
      recordAdminTabUserActivity(tab);
      const auto& entries = tab.session.entries();
      if (index.has_value() && *index < entries.size()) {
        tab.selectedPath = entries[*index].absPath;
        const auto path = entries[*index].absPath.string();
        if ((m_keyboardModifiers & KeyMod::Ctrl) != 0U) {
          if (!tab.selectedPaths.erase(path))
            tab.selectedPaths.insert(path);
          if (!tab.selectionAnchor)
            tab.selectionAnchor = entries[*index].absPath;
        } else if ((m_keyboardModifiers & KeyMod::Shift) != 0U && tab.selectionAnchor) {
          const auto anchor = std::ranges::find(entries, *tab.selectionAnchor, &FileEntry::absPath);
          if (anchor != entries.end()) {
            const auto anchorIndex = static_cast<std::size_t>(anchor - entries.begin());
            const auto first = std::min(anchorIndex, *index);
            const auto last = std::max(anchorIndex, *index);
            tab.selectedPaths.clear();
            for (auto selected = first; selected <= last; ++selected)
              tab.selectedPaths.insert(entries[selected].absPath.string());
          }
        } else {
          tab.selectedPaths.clear();
          tab.selectedPaths.insert(path);
          tab.selectionAnchor = entries[*index].absPath;
        }
        m_grid->notifyDataChanged();
      } else {
        tab.selectedPaths.clear();
        tab.selectionAnchor.reset();
        tab.selectedPath.reset();
      }
      updateSelectionSummary();
    });
    mainPane->addChild(std::move(grid));

    auto status = std::make_unique<Label>();
    m_statusLabel = status.get();
    m_statusLabel->setText("Loading…");
    const float statusFontSize =
        12.0F + (m_statusHeight - kStatusMinHeight) * (10.0F / (kStatusMaxHeight - kStatusMinHeight));
    m_statusLabel->setFontSize(statusFontSize);
    m_statusLabel->setMaxLines(1);
    m_statusLabel->setEllipsize(TextEllipsize::Middle);
    m_statusLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
    auto statusDetails = std::make_unique<Label>();
    m_statusDetailLabel = statusDetails.get();
    m_statusDetailLabel->setFontSize(statusFontSize);
    m_statusDetailLabel->setMaxLines(1);
    m_statusDetailLabel->setEllipsize(TextEllipsize::Middle);
    m_statusDetailLabel->setFlexGrow(1.0F);
    m_statusDetailLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    auto statusFree = std::make_unique<Label>();
    m_statusFreeLabel = statusFree.get();
    m_statusFreeLabel->setFontSize(statusFontSize);
    m_statusFreeLabel->setMaxLines(1);
    m_statusFreeLabel->setEllipsize(TextEllipsize::End);
    m_statusFreeLabel->setTextAlign(TextAlign::End);
    m_statusFreeLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    auto statusRow = std::make_unique<Flex>();
    statusRow->setAlign(FlexAlign::Center);
    statusRow->setGap(Style::spaceSm);
    statusRow->setFlexGrow(1.0F);
    statusRow->addChild(std::move(status));
    statusRow->addChild(std::move(statusDetails));
    statusRow->addChild(std::move(statusFree));
    auto statusDivider = std::make_unique<Flex>();
    statusDivider->setMinHeight(4.0F);
    statusDivider->setMaxHeight(4.0F);
    statusDivider->setHitTestVisible(true);
    statusDivider->setFill(clearColorSpec());
    m_statusDivider = static_cast<Flex*>(mainPane->addChild(std::move(statusDivider)));
    auto statusStrip = std::make_unique<Flex>();
    statusStrip->setAlign(FlexAlign::Center);
    statusStrip->setPadding(2.0F, 10.0F);
    statusStrip->setFill(colorSpecFromRole(ColorRole::SurfaceContainer, 0.82F));
    statusStrip->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.55F), Style::borderWidth);
    statusStrip->setRadius(Style::scaledRadiusSm(1.0F));
    statusStrip->setMinHeight(m_statusHeight);
    statusStrip->setMaxHeight(m_statusHeight);
    statusStrip->setHitTestVisible(true);
    m_statusStrip = static_cast<Flex*>(mainPane->addChild(std::move(statusStrip)));
    m_statusStrip->addChild(std::move(statusRow));
    m_statusStrip->setPadding(std::clamp((m_statusHeight - statusFontSize) * 0.5F, 2.0F, 7.0F), 10.0F);
    content->addChild(std::move(mainPane));
    m_root->addChild(std::move(content));
    m_dispatcher.setSceneRoot(m_root.get());
    m_dispatcher.setFocus(m_grid->focusArea());
    m_surface->setSceneRoot(m_root.get());
    rebuildBreadcrumbs();
    updateNavigationControls();
    setLocationEditMode(false);
    needsLayout = true;
  }
  m_menus.syncResponsiveMenuBar(m_surface->width());
  updateAdminHeaderUi(m_surface->width());
  if (m_tabBar.rebuildIfDirty())
    needsLayout = true;
  if (m_transfers.isDraggingFiles() || m_transfers.hasDragPreview()) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_transfers.updateDragPreview(renderer);
  }
  if (needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    const float maxSidebar =
        std::min(kSidebarMaxWidth, std::max(kSidebarMinWidth, static_cast<float>(m_surface->width()) - 300.0F));
    const float effectiveSidebar = std::clamp(m_sidebarWidth, kSidebarMinWidth, maxSidebar);
    if (m_sidebar) {
      m_sidebar->setMinWidth(effectiveSidebar);
      m_sidebar->setMaxWidth(effectiveSidebar);
    }
    m_places.updateCapacityBarWidths(effectiveSidebar);
    const bool showSidebar = m_surface->width() >= 760;
    m_sidebar->setVisible(showSidebar);
    m_sidebar->setParticipatesInLayout(showSidebar);
    m_sidebarDivider->setVisible(showSidebar);
    m_sidebarDivider->setParticipatesInLayout(showSidebar);
    m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
    m_root->layout(renderer);
    m_tabBar.syncLayout(renderer);
    if (m_listHeader.applyColumnWidths()) {
      m_listHeader.arrangeAfterWidthChange(renderer);
      m_surface->requestLayout();
    }
    m_listHeader.positionColumnMarkers();
    finishPendingTransferReveal(renderer);
    m_listHeader.layoutVisibleMarkers(renderer);
    syncTooltipHover();
  }
  if (m_pendingInlineRenamePath && !activeSession().loading()) {
    const auto pending = *m_pendingInlineRenamePath;
    m_pendingInlineRenamePath.reset();
    beginRenamePath(pending);
  }
}

void FileManager::syncProgressWindow() {
  m_progressWindow.sync(m_surface.get(), m_modalDialog.backgroundConflictPending());
}

void FileManager::requestQuit(ToplevelSurface* parent) {
  if (!m_operations.active()) {
    m_quitting = true;
    if (m_surface && m_windowOpen)
      ToplevelSurface::handleToplevelClose(m_surface.get(), nullptr);
    if (m_transfers.pendingClipboardReads() == 0)
      m_running = false;
    return;
  }
  const auto activeCount =
      std::ranges::count_if(m_operations.operations(), [](const auto& operation) { return !operation.finished; });
  showDialog(
      "Quit Nythera Files?",
      "Quitting will stop "
          + std::to_string(activeCount)
          + (activeCount == 1 ? " active file operation. Incomplete staged copies will be removed."
                              : " active file operations. Incomplete staged copies will be removed."),
      {{"Cancel", 0}, {"Quit", 1}},
      [this](int choice) {
        if (choice != 1)
          return;
        for (const auto& operation : m_operations.operations())
          if (!operation.finished)
            m_operations.cancel(operation.id);
        m_quitting = true;
        if (m_surface && m_windowOpen)
          ToplevelSurface::handleToplevelClose(m_surface.get(), nullptr);
        if (!m_operations.active() && m_transfers.pendingClipboardReads() == 0)
          m_running = false;
      },
      parent
  );
}

std::optional<std::filesystem::path> FileManager::checksumTarget() const {
  const auto& tab = activeTab();
  if (!tab.capabilities().canChecksum
      || !tab.selectedPath
      || !tab.selectedPaths.contains(tab.selectedPath->string())
      || !filemanager::checksum::eligibleSelection(
          *tab.selectedPath, tab.selectedPaths.size(), !tab.session.loading() && !tab.session.browsingTrash()
      ))
    return std::nullopt;
  return tab.selectedPath;
}

std::array<ContextMenuControlEntry, 2> FileManager::checksumMenuEntries() const {
  return filemanager::ChecksumDialogController::menuEntries(checksumTarget().has_value());
}

void FileManager::showChecksumDialog(bool verify) {
  if (const auto target = checksumTarget())
    m_checksumDialog.show(*target, verify);
}

void FileManager::showDialog(
    std::string title, std::string message, std::vector<std::pair<std::string, int>> choices,
    std::function<void(int)> response, ToplevelSurface* parent, bool applyToAllCheckbox, bool alternativeName
) {
  m_modalDialog.showDialog(
      std::move(title), std::move(message), std::move(choices), std::move(response), parent, applyToAllCheckbox,
      alternativeName
  );
}

void FileManager::closeDialog(int response) { m_modalDialog.closeDialog(response); }

void FileManager::syncTooltipHover() {
  m_dispatcher.syncPointerHover();
  if (m_surface)
    TooltipManager::instance().onHoverChange(
        m_dispatcher.hoveredArea(), m_surface->xdgSurface(), m_wayland.outputForSurface(m_surface->wlSurface())
    );
}

void FileManager::handlePointer(const PointerEvent& event) {
  if (m_surface && event.surface == m_surface->wlSurface()) {
    if (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)
      TooltipManager::instance().setPointerPosition(static_cast<float>(event.sx), static_cast<float>(event.sy));
    // Menu routing below can consume the leave before the main dispatcher sees it.
    if (event.type == PointerEvent::Type::Leave)
      m_dispatcher.pointerLeave();
    if (event.type == PointerEvent::Type::Axis || (event.type == PointerEvent::Type::Button && event.pressed))
      TooltipManager::instance().forceDestroy();
  } else if (event.type == PointerEvent::Type::Enter) {
    m_dispatcher.pointerLeave();
  }
  dispatchPointer(event);
  syncTooltipHover();
}

void FileManager::dispatchPointer(const PointerEvent& event) {
  if (m_editingLocation
      && m_locationInput != nullptr
      && m_surface
      && event.surface == m_surface->wlSurface()
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && (event.button == BTN_LEFT || event.button == BTN_RIGHT)) {
    float inputX = 0.0F;
    float inputY = 0.0F;
    Node::absolutePosition(m_locationInput, inputX, inputY);
    if (filemanager::pointerOutsideRect(
            static_cast<float>(event.sx), static_cast<float>(event.sy), inputX, inputY, m_locationInput->width(),
            m_locationInput->height()
        ))
      deferCancelLocationEdit();
    else if (event.button == BTN_RIGHT && m_surface) {
      const bool selected = m_locationInput->hasTextSelection() && !activeTab().hasActiveAdminSession();
      const bool hasClipboardText = m_transfer.selectionOffersText();
      std::vector<ContextMenuControlEntry> entries{
          {.id = 90, .label = "Cut", .enabled = selected},
          {.id = 91, .label = "Copy", .enabled = selected},
          {.id = 92,
           .label = "Paste",
           .enabled = hasClipboardText,
           .actionIconNames = {"edit-paste-symbolic", "edit-paste"},
           .actionGlyph = "U+EA6F"},
          {.id = 93, .label = "Select All", .enabled = !m_locationInput->value().empty()},
      };
      m_lastInputSerial = event.serial;
      m_menus.openColumnHeaderMenu(std::move(entries), static_cast<float>(event.sx), static_cast<float>(event.sy));
      return;
    }
  }
  if (event.type == PointerEvent::Type::Button && event.pressed && event.button == BTN_RIGHT)
    TooltipManager::instance().forceDestroy();
  if (m_menus.handlePointer(event))
    return;
  if (event.type == PointerEvent::Type::Button && event.pressed)
    m_lastInputSerial = event.serial;
  if (m_modalDialog.isOpen()) {
    m_modalDialog.handlePointer(event);
    return;
  }
  if (m_progressWindow.ownsSurface(event.surface)) {
    m_progressWindow.handlePointer(event);
    return;
  }
  if (!m_surface || event.surface != m_surface->wlSurface())
    return;
  if ((event.type == PointerEvent::Type::Button && event.pressed) || event.type == PointerEvent::Type::Axis)
    recordAdminTabUserActivity(activeTab());
  const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  if (m_renamePath
      && m_inlineEditArea
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && (event.button == BTN_LEFT || event.button == BTN_RIGHT)) {
    float editorX = 0.0F;
    float editorY = 0.0F;
    Node::absolutePosition(m_inlineEditArea, editorX, editorY);
    if (filemanager::pointerOutsideRect(
            x, y, editorX, editorY, m_inlineEditArea->width(), m_inlineEditArea->height()
        )) {
      m_dispatcher.setFocus(m_inlineEditArea);
      m_dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true, false);
      if (m_renamePath)
        cancelRename();
    }
  }
  if (m_tabBar.handlePointer(event, x, y))
    return;
  if (m_listHeader.handlePointer(event, x, y))
    return;
  if (m_statusDivider && m_statusStrip) {
    float dx = 0.0F, dy = 0.0F;
    Node::absolutePosition(m_statusDivider, dx, dy);
    const bool over =
        x >= dx && x <= dx + m_statusDivider->width() && y >= dy - 2.0F && y <= dy + m_statusDivider->height() + 2.0F;
    if ((over || m_resizingStatus) && m_pointerSerial)
      m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE);
    if (over && (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      m_statusDivider->setFill(colorSpecFromRole(ColorRole::Primary, m_resizingStatus ? 1.0F : 0.62F));
      m_surface->requestRedraw();
    } else if (!m_resizingStatus) {
      m_statusDivider->setFill(clearColorSpec());
    }
    if (m_resizingStatus)
      m_statusDivider->setFill(colorSpecFromRole(ColorRole::Primary));
    if (over && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed) {
      m_dispatcher.cancelPointerCapture();
      m_resizingStatus = true;
      m_statusDragY = y;
      m_statusDragHeight = m_statusHeight;
      return;
    }
    if (m_resizingStatus && event.type == PointerEvent::Type::Motion) {
      m_statusHeight = std::clamp(m_statusDragHeight + m_statusDragY - y, kStatusMinHeight, kStatusMaxHeight);
      const float fontSize =
          12.0F + (m_statusHeight - kStatusMinHeight) * (10.0F / (kStatusMaxHeight - kStatusMinHeight));
      m_statusStrip->setMinHeight(m_statusHeight);
      m_statusStrip->setMaxHeight(m_statusHeight);
      m_statusStrip->setPadding(std::clamp((m_statusHeight - fontSize) * 0.5F, 2.0F, 7.0F), 10.0F);
      m_statusLabel->setFontSize(fontSize);
      m_statusDetailLabel->setFontSize(fontSize);
      m_statusFreeLabel->setFontSize(fontSize);
      m_surface->requestLayout();
      return;
    }
    if (m_resizingStatus && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      m_resizingStatus = false;
      (void)m_settings.saveStatusHeight(m_statusHeight);
      m_dispatcher.cancelPointerCapture();
      return;
    }
  }
  if (event.type == PointerEvent::Type::Leave && (m_transfers.isDraggingFiles() || m_transfers.isDragPending())) {
    m_transfers.handlePointerLeaveDuringDrag();
    return;
  }
  if (event.type == PointerEvent::Type::Enter && event.serial)
    m_pointerSerial = event.serial;
  if (m_transfer.dragging() && !m_transfer.dragDropped()) {
    if (event.type == PointerEvent::Type::Axis)
      m_transfers.handleAxisDuringProtocolDrag(event);
    return;
  }
  if (m_transfer.dragDropped() && (m_transfers.isDragPending() || m_transfers.isDraggingFiles()))
    cancelFileGesture();
  if (event.type == PointerEvent::Type::Axis && (m_keyboardModifiers & KeyMod::Ctrl) && m_grid && !m_renamePath) {
    float gx = 0, gy = 0;
    Node::absolutePosition(m_grid, gx, gy);
    if (x >= gx && x < gx + m_grid->width() && y >= gy && y < gy + m_grid->height()) {
      const double delta = event.axisValue120 != 0 ? event.axisValue120
          : event.axisDiscrete != 0                ? event.axisDiscrete
                                                   : event.axisValue;
      if (delta != 0 && event.axis == 0) {
        adjustViewScale(
            m_listHeader.viewScale() + (delta < 0 ? filemanager::kFileViewZoomStep : -filemanager::kFileViewZoomStep)
        );
      }
      return;
    }
  }
  auto rowAt = [&]() -> std::optional<std::size_t> { return m_places.rowIndexAt(x, y); };
  if (m_reorderId) {
    const auto& placeRows = m_places.placeRows();
    const auto source =
        std::ranges::find_if(placeRows, [this](const auto& pair) { return pair.second.identity == *m_reorderId; });
    if (event.type == PointerEvent::Type::Motion) {
      if (std::hypot(x - m_dragStartX, y - m_dragStartY) > 5.0F)
        m_reordering = true;
      m_reorderBefore.reset();
      if (m_reordering && source != placeRows.end()) {
        const auto& place = source->second;
        m_reorderBefore = m_places.placeInsertionAt(
            x, y,
            place.personal      ? "Personal"
                : place.network ? "Network"
                                : "Devices",
            false
        );
      }
      m_places.showPlaceInsertion(m_reorderBefore);
      if (m_pointerSerial && m_reordering)
        m_wayland.setCursorShape(
            m_pointerSerial,
            m_reorderBefore ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
        );
      m_surface->requestRedraw();
      return;
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      if (m_reordering && m_reorderBefore)
        m_places.reorderPlace(*m_reorderId, *m_reorderBefore);
      else if (!m_reordering && source != placeRows.end()) {
        const auto place = source->second;
        if (place.mounted)
          navigate(place.path);
      }
      m_reorderId.reset();
      m_reorderBefore.reset();
      m_reordering = false;
      m_places.showPlaceInsertion(std::nullopt);
      m_dispatcher.cancelPointerCapture();
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      m_surface->requestRedraw();
      return;
    }
    return;
  }
  if (m_transfers.handleFileDragPointer(event, x, y))
    return;
  if (m_sidebarDivider != nullptr && m_sidebar != nullptr && m_sidebarDivider->visible()) {
    float dividerX = 0, dividerY = 0;
    Node::absolutePosition(m_sidebarDivider, dividerX, dividerY);
    const bool overDivider = x >= dividerX - 2.0F
        && x <= dividerX + m_sidebarDivider->width() + 2.0F
        && y >= dividerY
        && y <= dividerY + m_sidebarDivider->height();
    const bool feedback = overDivider || m_resizingSidebar;
    if (overDivider
        && !m_resizingSidebar
        && (event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter)) {
      m_dispatcher.pointerLeave();
      m_sidebarDivider->setFill(colorSpecFromRole(ColorRole::Primary, 0.62F));
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
      m_surface->requestRedraw();
      return;
    }
    m_sidebarDivider->setFill(
        feedback ? colorSpecFromRole(ColorRole::Primary, m_resizingSidebar ? 1.0F : 0.62F) : clearColorSpec()
    );
    if (m_resizingSidebar || overDivider) {
      m_sidebarDivider->setFill(colorSpecFromRole(ColorRole::Primary, m_resizingSidebar ? 1.0F : 0.62F));
      if (m_pointerSerial)
        m_wayland.setCursorShape(m_pointerSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed && overDivider) {
      m_dispatcher.cancelPointerCapture();
      m_resizingSidebar = true;
      m_sidebarDragOffset = x;
      m_sidebarDragWidth = m_sidebar->width();
      return;
    }
    if (m_resizingSidebar && event.type == PointerEvent::Type::Motion) {
      const float maxWidth =
          std::min(kSidebarMaxWidth, std::max(kSidebarMinWidth, static_cast<float>(m_surface->width()) - 300));
      // Both coordinates are surface-local logical pixels. Apply one delta from
      // the press, independent of layout origin, render scale and intermediate clamps.
      m_sidebarWidth = std::clamp(m_sidebarDragWidth + x - m_sidebarDragOffset, kSidebarMinWidth, maxWidth);
      m_sidebar->setMinWidth(m_sidebarWidth);
      m_sidebar->setMaxWidth(m_sidebarWidth);
      m_places.updateCapacityBarWidths(m_sidebarWidth);
      m_surface->requestLayout();
      return;
    }
    if (m_resizingSidebar && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      m_resizingSidebar = false;
      (void)m_settings.saveSidebarWidth(m_sidebarWidth);
      m_dispatcher.cancelPointerCapture();
      return;
    }
  }
  if (m_grid && m_grid->rubberBandActive()) {
    if (event.type == PointerEvent::Type::Motion)
      m_grid->updateRubberBand(x, y);
    else if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed)
      m_grid->endRubberBand();
    else if (event.type == PointerEvent::Type::Axis)
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
    return;
  }
  if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && event.pressed) {
    if (auto row = rowAt()) {
      const auto& place = m_places.placeRows()[*row].second;
      {
        m_reorderId = place.identity;
        m_dragStartX = x;
        m_dragStartY = y;
        m_reordering = false;
        m_dispatcher.cancelPointerCapture();
        return;
      }
    }
    auto& tab = activeTab();
    const auto& session = tab.session;
    // Leave clicks on tabs without drag support to the normal grid dispatcher.
    if (tab.capabilities().canDragSource && m_grid && !m_renamePath && !m_places.hasActiveDeviceOperations()) {
      if (auto index = m_grid->indexAtSurface(x, y)) {
        m_transfers.beginFileDragPress(*index, x, y, event.serial);
        const bool preserve = tab.selectedPaths.contains(session.entries()[*index].absPath.string())
            && !(m_keyboardModifiers & (KeyMod::Ctrl | KeyMod::Shift));
        m_restoringSelection = true;
        if (!preserve)
          m_grid->setSelectedIndex(std::nullopt, false);
        m_restoringSelection = preserve;
        m_grid->setSelectedIndex(index, false);
        m_restoringSelection = false;
        m_dispatcher.setFocus(m_grid->focusArea());
        return;
      }
    }
  }
  if (event.type == PointerEvent::Type::Button
      && event.button == BTN_LEFT
      && event.pressed
      && m_grid
      && !m_renamePath) {
    const auto& tab = activeTab();
    m_rubberBaseline = tab.selectedPaths;
    m_rubberSelectedPath = tab.selectedPath;
    m_rubberAnchor = tab.selectionAnchor;
    m_rubberModifiers = m_keyboardModifiers;
    if (m_grid->beginRubberBand(x, y)) {
      m_dispatcher.pointerLeave();
      TooltipManager::instance().forceDestroy();
      m_selectionTick = std::chrono::steady_clock::now();
      m_selectionTimer.startRepeating(std::chrono::milliseconds(16), [this] {
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - m_selectionTick).count();
        m_selectionTick = now;
        if (m_grid)
          m_grid->advanceRubberBandScroll(std::min(seconds, 0.05F));
        if (m_surface)
          m_surface->requestLayout();
      });
      m_lastPointerIndex.reset();
      m_dispatcher.cancelPointerCapture();
      m_dispatcher.setFocus(m_grid->focusArea());
      return;
    }
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_dispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_dispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_dispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    if (event.button == BTN_LEFT || event.button == BTN_RIGHT || event.button == BTN_MIDDLE) {
      if (event.pressed && event.button == BTN_LEFT)
        m_pointerClickTime = event.time;
      m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      // The virtual canvas only covers populated rows. The remainder of the
      // grid viewport still belongs to the directory background.
      if (event.button == BTN_RIGHT && event.pressed && m_grid && !m_menus.isContextMenuOpen()) {
        float gridX = 0.0F;
        float gridY = 0.0F;
        Node::absolutePosition(m_grid, gridX, gridY);
        if (x >= gridX && x < gridX + m_grid->width() && y >= gridY && y < gridY + m_grid->height())
          m_menus.openEmptyContextMenu(x, y);
      }
    }
    break;
  case PointerEvent::Type::Axis:
    m_dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
}

void FileManager::handleKeyboard(const KeyboardEvent& event) {
  if (!event.pressed || event.preedit || !m_surface || m_wayland.lastKeyboardSurface() != m_surface->wlSurface())
    return;
  handleWindowKeyEvent(event);
}

void FileManager::handleWindowKeyEvent(const KeyboardEvent& event) {
  if (event.pressed && !event.repeat)
    recordAdminTabUserActivity(activeTab());
  if (event.sym == XKB_KEY_Escape) {
    m_quitConfirmation = false;
    if (m_menus.isContextMenuOpen()) {
      m_menus.closeContextMenu();
      return;
    }
    if (m_renamePath) {
      cancelRename();
      return;
    }
    if (m_inlineEditArea) {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
      return;
    }
    if (m_editingLocation) {
      setLocationEditMode(false);
      return;
    }
    if (m_transfer.dragging() && !m_transfer.dragDropped()) {
      m_transfer.cancelDrag();
      return;
    }
    if (m_transfers.isDragPending()
        || m_transfers.isDraggingFiles()
        || m_reorderId
        || m_listHeader.gestureActive()
        || m_tabBar.gestureActive()
        || m_resizingSidebar
        || m_resizingStatus
        || (m_grid && m_grid->rubberBandActive())) {
      if (m_grid)
        m_grid->endRubberBand(true);
      if (m_listHeader.gestureActive()) {
        m_dispatcher.cancelPointerCapture();
        m_listHeader.cancelGesture();
      }
      if (m_tabBar.gestureActive()) {
        m_dispatcher.cancelPointerCapture();
        m_tabBar.cancelGesture();
      }
      m_reordering = m_resizingSidebar = m_resizingStatus = false;
      m_reorderId.reset();
      m_reorderBefore.reset();
      m_transfers.cancelLocalDragState();
      return;
    }
    if (!activeTab().selectedPaths.empty())
      changeSelection(SelectionChange::None);
    m_transfers.cancelCutSelection();
    return;
  }
  if (m_transfers.isDragPending()
      || m_transfers.isDraggingFiles()
      || m_reorderId
      || m_listHeader.gestureActive()
      || m_tabBar.gestureActive()
      || m_resizingSidebar
      || (m_grid && m_grid->rubberBandActive()))
    return;
  const std::uint32_t tabModifiers = event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super);
  if (tabModifiers == KeyMod::Ctrl && (event.sym == XKB_KEY_t || event.sym == XKB_KEY_T)) {
    openNewTab();
    return;
  }
  if (tabModifiers == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_t || event.sym == XKB_KEY_T)) {
    (void)reopenClosedTab();
    return;
  }
  if (tabModifiers == KeyMod::Ctrl && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_W)) {
    (void)closeTab(activeTab().id);
    return;
  }
  if (tabModifiers == KeyMod::Ctrl && event.sym == XKB_KEY_Tab) {
    (void)activateRelativeTab(1);
    return;
  }
  if ((tabModifiers == (KeyMod::Ctrl | KeyMod::Shift)
       && (event.sym == XKB_KEY_Tab || event.sym == XKB_KEY_ISO_Left_Tab))
      || (tabModifiers == KeyMod::Ctrl && event.sym == XKB_KEY_ISO_Left_Tab)) {
    (void)activateRelativeTab(-1);
    return;
  }
  if (m_locationInput != nullptr && m_editingLocation && m_dispatcher.focusedArea() == m_locationInput->inputArea()) {
    if (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) {
      navigate(filemanager::expandHomePath(m_locationInput->value(), g_get_home_dir()));
      setLocationEditMode(false);
    } else if (((event.modifiers & KeyMod::Ctrl) != 0U && (event.sym == XKB_KEY_v || event.sym == XKB_KEY_V))
               || (tabModifiers == KeyMod::Shift && event.sym == XKB_KEY_Insert)) {
      pasteLocationFromClipboard();
    } else if (!(activeTab().hasActiveAdminSession() && (event.modifiers & KeyMod::Ctrl) != 0U
                 && (event.sym == XKB_KEY_c || event.sym == XKB_KEY_C || event.sym == XKB_KEY_x
                     || event.sym == XKB_KEY_X || event.sym == XKB_KEY_Insert))) {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    }
    return;
  }
  if (m_places.hasActiveDeviceOperations()
      && (event.sym == XKB_KEY_Delete
          || event.sym == XKB_KEY_F2
          || ((event.modifiers & KeyMod::Ctrl)
              && (event.sym == XKB_KEY_v || event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)))) {
    setStatus("Wait for the device operation to finish");
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_q) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = 30007});
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Shift)) == KeyMod::Ctrl && event.sym == XKB_KEY_n) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = 30001});
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_l) {
    setLocationEditMode(true);
    return;
  }
  if (m_inlineEditArea != nullptr && m_dispatcher.focusedArea() == m_inlineEditArea) {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super)) == KeyMod::Ctrl
      && (event.sym == XKB_KEY_a || event.sym == XKB_KEY_A)) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = kSelectAll});
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == KeyMod::Ctrl
      && (event.sym == XKB_KEY_z || event.sym == XKB_KEY_Z)) {
    if (!event.repeat)
      activateContextMenuEntry(ContextMenuControlEntry{.id = (event.modifiers & KeyMod::Shift) ? kRedo : kUndo});
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_c) {
    copyToDesktopClipboard(false);
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_x) {
    copyToDesktopClipboard(true);
    return;
  }
  if ((event.modifiers & KeyMod::Ctrl) != 0U && event.sym == XKB_KEY_v) {
    pasteClipboard();
    return;
  }
  if ((event.modifiers & KeyMod::Alt) && event.sym == XKB_KEY_Return && activeTab().selectedPath) {
    if (activeTab().selectedPaths.size() != 1) {
      setStatus("Properties supports one selected item at a time");
      return;
    }
    openProperties(*activeTab().selectedPath);
    return;
  }
  if (event.sym == XKB_KEY_F2) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = 4});
    return;
  }
  if ((event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)
      && (event.modifiers & (KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt | KeyMod::Super))
          == (KeyMod::Ctrl | KeyMod::Shift)) {
    if (activeSession().browsingTrash())
      return;
    activateContextMenuEntry(ContextMenuControlEntry{.id = 111});
    return;
  }
  if (event.sym == XKB_KEY_Delete) {
    activateContextMenuEntry(ContextMenuControlEntry{.id = (event.modifiers & KeyMod::Shift) ? 6 : 5});
    return;
  }
  if (event.sym == XKB_KEY_BackSpace) {
    const auto& session = activeSession();
    if (session.browsingTrash() && session.path() == std::filesystem::path("trash:/"))
      return;
    if (session.path().has_parent_path() && session.path().parent_path() != session.path())
      navigate(session.path().parent_path());
    return;
  }
  if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == KeyMod::Ctrl) {
    switch (event.sym) {
    case XKB_KEY_plus:
    case XKB_KEY_KP_Add:
      activateContextMenuEntry({.id = kZoomIn});
      return;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
      activateContextMenuEntry({.id = kZoomOut});
      return;
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
      activateContextMenuEntry({.id = kResetZoom});
      return;
    default:
      break;
    }
  }
  if (event.sym == XKB_KEY_r && (event.modifiers & KeyMod::Ctrl) != 0U) {
    activateContextMenuEntry({.id = 13});
    return;
  }
  if (m_grid == nullptr)
    return;
  if (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) {
    activateSelected();
    return;
  }
  const auto current = m_grid->selectedIndex();
  const auto selected = current.value_or(0);
  const auto& entries = activeSession().entries();
  if (event.sym == XKB_KEY_Down && !current && !entries.empty())
    m_grid->setSelectedIndex(0);
  else if (event.sym == XKB_KEY_Down && selected + 1 < entries.size())
    m_grid->setSelectedIndex(selected + 1);
  else if (event.sym == XKB_KEY_Up && selected > 0)
    m_grid->setSelectedIndex(selected - 1);
  else
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
}

std::filesystem::path FileManager::defaultTabPath() {
  try {
    return std::filesystem::path(filemanager::startupPath(""));
  } catch (const std::exception&) {
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    return error ? std::filesystem::path("/") : cwd;
  }
}

filemanager::TabId FileManager::openNewTab() { return m_tabs.openTab(defaultTabPath(), activeTab().viewState, true); }

filemanager::TabId FileManager::openFolderInNewTab(const std::filesystem::path& path, bool activate) {
  if (path.empty())
    return filemanager::kInvalidTabId;
  return m_tabs.openTab(path, activeTab().viewState, activate);
}

filemanager::TabId FileManager::duplicateTab(filemanager::TabId tabId) { return m_tabs.duplicateTab(tabId); }

bool FileManager::activateTab(filemanager::TabId tabId) { return m_tabs.activateTab(tabId); }

bool FileManager::activateRelativeTab(int delta) { return m_tabs.activateRelative(delta); }

bool FileManager::closeTab(filemanager::TabId tabId) { return m_tabs.closeTab(tabId); }

bool FileManager::closeOtherTabs(filemanager::TabId keepTabId) { return m_tabs.closeOtherTabs(keepTabId); }

bool FileManager::closeTabsToRight(filemanager::TabId pivotTabId) { return m_tabs.closeTabsToRight(pivotTabId); }

filemanager::TabId FileManager::reopenClosedTab() { return m_tabs.reopenClosedTab(); }

void FileManager::setAlwaysShowTabBar(bool show) {
  m_alwaysShowTabBar = show;
  if (!m_settings.setAlwaysShowTabBar(show))
    setStatus("Unable to save tab preferences");
  m_tabBar.markDirty();
  if (m_surface)
    m_surface->requestLayout();
}

void FileManager::setOpenTabsInBackground(bool background) {
  m_openTabsInBackground = background;
  if (!m_settings.setOpenTabsInBackground(background))
    setStatus("Unable to save tab preferences");
}

void FileManager::activatePointerMiddleItem(std::size_t index) {
  const auto& session = activeSession();
  const auto& entries = session.entries();
  if (index >= entries.size())
    return;
  if (m_renamePath || m_places.hasActiveDeviceOperations())
    return;
  const auto& entry = entries[index];
  if (!entry.isDir || session.browsingTrash())
    return;
  openFolderInNewTab(entry.absPath, false);
}
