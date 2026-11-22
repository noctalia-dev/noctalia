#include "filemanager/file_transfer_controller.h"

#include "core/deferred_call.h"
#include "core/files/display_text.h"
#include "core/input/key_modifiers.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_transfer_payload.h"
#include "render/core/renderer.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <ranges>
#include <utility>
#include <wayland-client-protocol.h>

namespace filemanager {

  namespace {

    std::filesystem::path normalizeDirectoryPath(const std::filesystem::path& path) {
      if (path.empty())
        return {};
      auto normalized = path.lexically_normal();
      if (normalized.has_filename() && normalized.filename() == ".")
        normalized = normalized.parent_path();
      const auto text = normalized.string();
      if (text.size() > 1 && text.ends_with('/'))
        return std::filesystem::path(text.substr(0, text.size() - 1));
      return normalized;
    }

    std::string normalizeCutPathKey(const std::filesystem::path& path) { return normalizeDirectoryPath(path).string(); }

    std::optional<filemanager::FileTransferPayload> decodeClipboardPayload(const NativeDataTransfer::Payload& payload) {
      try {
        if (payload.contains("text/uri-list")) {
          bool cut =
              payload.contains("application/x-kde-cutselection") && payload.at("application/x-kde-cutselection") == "1";
          if (!payload.contains("application/x-kde-cutselection") && payload.contains("x-special/gnome-copied-files")) {
            cut = filemanager::decodeFileTransfer(payload.at("x-special/gnome-copied-files"), true).cut;
          }
          return filemanager::decodeFileTransfer(payload.at("text/uri-list"), false, cut);
        }
        if (payload.contains("x-special/gnome-copied-files"))
          return filemanager::decodeFileTransfer(payload.at("x-special/gnome-copied-files"), true);
      } catch (const std::exception&) {
      }
      return std::nullopt;
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

  } // namespace

  FileTransferController::FileTransferController(
      WaylandConnection& wayland, RenderContext& renderContext, InputDispatcher& dispatcher,
      NativeDataTransfer& transfer, FileOperationManager& operations, TabManager& tabs, PlacesSidebarController& places,
      Callbacks callbacks
  )
      : m_wayland(wayland), m_renderContext(renderContext), m_dispatcher(dispatcher), m_transfer(transfer),
        m_operations(operations), m_tabs(tabs), m_places(places), m_callbacks(std::move(callbacks)) {}

  FileTransferController::~FileTransferController() { shutdown(); }

  void FileTransferController::setupDragHandlers() {
    m_transfer.setSelectionChangedCallback([this] { syncCutStateFromClipboard(); });
    m_transfer.setDragHandlers(
        [this](wl_surface* surface, float x, float y, std::uint32_t actions) {
          return updateProtocolDrag(surface, x, y, actions);
        },
        [this] {
          m_protocolDragInside = false;
          m_protocolDropDestination.reset();
          m_protocolDropSources.clear();
          clearDragFeedback();
          m_pinBefore.reset();
          m_places.showPlaceInsertion(std::nullopt);
          m_dragTargetValid = false;
          if (m_dragPreview)
            m_dragPreview->setVisible(false);
          if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
            surface->requestUpdate();
          scheduleNativeDragPreview();
        },
        [this](std::uint32_t action) { return receiveProtocolDrop(action); },
        [this](bool success) {
          cancelFileGesture();
          if (m_callbacks.isWindowOpen && m_callbacks.isWindowOpen() && m_callbacks.onStatus)
            m_callbacks.onStatus(success ? "Drop completed" : "Drop cancelled");
        },
        [this] {
          if (m_transfer.dragDropped())
            clearNativeDragPreview();
          else
            scheduleNativeDragPreview();
        }
    );
  }

  void FileTransferController::shutdown() {
    if (m_alive)
      *m_alive = false;
    cancelDropValidation();
    m_transfer.cancelDrag();
    clearNativeDragPreview();
  }

  void FileTransferController::detachUi() {
    m_dragPreview = nullptr;
    m_dragPreviewIcon = nullptr;
    m_dragPreviewLabel = nullptr;
    m_dragPreviewAction = nullptr;
    m_dropPlace = nullptr;
  }

  bool FileTransferController::onOperationFinished(const FileOperationManager::Operation& operation) {
    if (auto finish = m_dropFinishes.find(operation.id); finish != m_dropFinishes.end()) {
      auto callback = std::move(finish->second);
      m_dropFinishes.erase(finish);
      if (callback)
        callback(operation.succeeded);
    }
    const bool pastedEntries = recordPasteCompletion(operation);
    if (auto cut = m_cutOperations.find(operation.id); cut != m_cutOperations.end()) {
      const auto cutGeneration = cut->second;
      auto srcIt = m_cutOperationSources.find(operation.id);
      std::vector<std::string> originalSources =
          srcIt != m_cutOperationSources.end() ? std::move(srcIt->second) : m_activeCutOrdered;
      if (srcIt != m_cutOperationSources.end())
        m_cutOperationSources.erase(srcIt);
      m_cutOperations.erase(cut);

      if (cutGeneration == m_transfer.selectionGeneration()) {
        if (operation.succeeded) {
          m_transfer.consumeSelection(cutGeneration);
          m_consumedCutSelection = cutGeneration;
          const bool hadActiveCut = !m_activeCutPaths.empty();
          m_activeCutPaths.clear();
          m_activeCutOrdered.clear();
          if (hadActiveCut && m_callbacks.onCutStateChanged)
            m_callbacks.onCutStateChanged();
        } else if (!operation.movedSources.empty()) {
          std::unordered_set<std::string> movedSet;
          movedSet.reserve(operation.movedSources.size());
          for (const auto& moved : operation.movedSources)
            movedSet.insert(normalizeCutPathKey(moved));

          std::vector<std::string> remainingOrdered;
          std::unordered_set<std::string> remainingSet;
          for (const auto& candidate : originalSources) {
            const auto key = normalizeCutPathKey(candidate);
            if (!movedSet.contains(key)) {
              remainingOrdered.push_back(candidate);
              remainingSet.insert(key);
            }
          }
          m_activeCutOrdered = remainingOrdered;
          m_activeCutPaths = std::move(remainingSet);
          if (m_activeCutOrdered.empty()) {
            m_transfer.consumeSelection(cutGeneration);
            m_consumedCutSelection = cutGeneration;
          } else if (m_transfer.ownsSelection()) {
            try {
              m_transfer.updateOwnedSelection(filemanager::encodeFileTransfer(m_activeCutOrdered, true));
            } catch (const std::exception&) {
            }
          }
          if (m_callbacks.onCutStateChanged)
            m_callbacks.onCutStateChanged();
        }
      }
    } else if (
        (operation.succeeded || operation.mutated)
        && (operation.kind == FileOperationManager::Kind::Rename
            || operation.kind == FileOperationManager::Kind::Delete
            || operation.kind == FileOperationManager::Kind::Trash
            || operation.kind == FileOperationManager::Kind::TrashToHome)
        && !m_activeCutPaths.empty()
    ) {
      bool changed = false;
      const auto removeKey = [&](const std::string& raw) {
        if (raw.empty())
          return;
        const auto key = normalizeCutPathKey(raw);
        if (m_activeCutPaths.erase(key) > 0) {
          std::erase_if(m_activeCutOrdered, [&](const std::string& p) { return normalizeCutPathKey(p) == key; });
          changed = true;
        }
      };
      if (operation.kind == FileOperationManager::Kind::Rename && operation.succeeded)
        removeKey(operation.source);
      else if (operation.kind == FileOperationManager::Kind::Delete && operation.succeeded)
        removeKey(operation.source);
      else
        for (const auto& moved : operation.movedSources)
          removeKey(moved);
      if (changed) {
        if (m_activeCutOrdered.empty() && m_transfer.ownsSelection())
          m_transfer.consumeSelection(m_transfer.selectionGeneration());
        else if (m_transfer.ownsSelection()) {
          try {
            m_transfer.updateOwnedSelection(filemanager::encodeFileTransfer(m_activeCutOrdered, true));
          } catch (const std::exception&) {
          }
        }
        if (m_callbacks.onCutStateChanged)
          m_callbacks.onCutStateChanged();
      }
    }
    return pastedEntries;
  }

  void FileTransferController::onBeforePathChange(BrowserTab& tab) {
    tab.pendingPasteSelection.reset();
    tab.pendingPasteRevealPath.reset();
    tab.pendingDropScrollGeneration.reset();
    tab.pendingRevealOnActivate = false;
  }

  void FileTransferController::onEnumerateStarted(BrowserTab& tab) { tab.identityCheckedPaths.clear(); }

  void FileTransferController::registerPasteRequest(std::uint64_t opId, PasteRequest request) {
    m_pasteRequests[opId] = std::move(request);
  }

  void FileTransferController::pasteClipboard() {
    const auto& active = m_tabs.activeTab();
    const auto caps = active.capabilities();
    const bool adminPaste = caps.canPasteAsAdministrator && active.administratorBrowsing;
    if ((!caps.canMutate && !adminPaste)
        || active.session.browsingTrash()
        || m_places.hasActiveDeviceOperations()
        || m_operations.replaying()) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("This action is currently unavailable");
      return;
    }
    const TabId tabId = active.id;
    const auto destination = active.session.path().string();
    const auto navigation = active.session.navigationGeneration();
    const auto adminGeneration = active.adminSession.sessionGeneration();
    const auto listing = active.session.privilegedListingId();
    const auto generation = m_transfer.selectionGeneration();
    if (m_consumedCutSelection == generation) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("These cut files have already been moved");
      return;
    }
    ++m_pendingClipboardReads;
    auto onSelection = [this, alive = std::weak_ptr<bool>(m_alive), tabId, destination, navigation, generation,
                        adminPaste, adminGeneration, listing](std::optional<NativeDataTransfer::Payload> payload) {
      const auto locked = alive.lock();
      if (!locked || !*locked)
        return;
      --m_pendingClipboardReads;
      const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
      const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
      auto* tab = m_tabs.findTab(tabId);
      const bool tabStillOpen = tab != nullptr;
      const bool canPaste = tabStillOpen
          && (adminPaste ? m_tabs.activeTabId() == tabId
                      && tab->administratorBrowsing
                      && tab->capabilities().canPasteAsAdministrator
                      && tab->adminSession.sessionGeneration() == adminGeneration
                      && tab->session.navigationGeneration() == navigation
                      && tab->session.path() == destination
                      && listing != kInvalidAdminRequestId
                      && tab->session.privilegedListingId() == listing
                      && m_transfer.selectionGeneration() == generation
                         : tab->capabilities().canMutate && !tab->administratorBrowsing);
      const auto reportStatus = [this, tabId](std::string message) {
        if (m_tabs.activeTabId() == tabId && m_callbacks.onStatus)
          m_callbacks.onStatus(std::move(message));
        else if (auto* originTab = m_tabs.findTab(tabId))
          originTab->lastStatusMessage = std::move(message);
      };
      if (!quitting
          && windowOpen
          && canPaste
          && payload
          && !m_places.hasActiveDeviceOperations()
          && !m_operations.replaying()) {
        try {
          filemanager::FileTransferPayload transfer;
          if (payload->contains("text/uri-list")) {
            bool cut = payload->contains("application/x-kde-cutselection")
                && payload->at("application/x-kde-cutselection") == "1";
            if (!payload->contains("application/x-kde-cutselection")
                && payload->contains("x-special/gnome-copied-files"))
              cut = filemanager::decodeFileTransfer(payload->at("x-special/gnome-copied-files"), true).cut;
            transfer = filemanager::decodeFileTransfer(payload->at("text/uri-list"), false, cut);
          } else {
            transfer = filemanager::decodeFileTransfer(payload->at("x-special/gnome-copied-files"), true);
          }
          if (adminPaste) {
            const auto kde = payload->find("application/x-kde-cutselection");
            const auto gnome = payload->find("x-special/gnome-copied-files");
            if (kde != payload->end() && kde->second != "0")
              transfer.cut = true;
            if (gnome != payload->end()) {
              const auto gnomeTransfer = filemanager::decodeFileTransfer(gnome->second, true);
              if (gnomeTransfer.paths != transfer.paths)
                throw std::invalid_argument("Clipboard file lists disagree");
              transfer.cut = transfer.cut || gnomeTransfer.cut;
            }
            if (transfer.cut || transfer.paths.size() != 1) {
              reportStatus("Administrator Paste requires one copied regular file.");
            } else if (const auto id = m_operations.copyAsAdministrator(*tab, std::move(transfer.paths.front())); id) {
              registerPasteRequest(id, PasteRequest{tabId, destination, navigation});
            } else {
              reportStatus("The originating Administrator session is no longer available.");
            }
          } else if (transfer.paths.empty()) {
            reportStatus("Clipboard contains no local files");
          } else if (transfer.cut) {
            if (std::ranges::any_of(m_cutOperations, [generation](const auto& item) {
                  return item.second == generation;
                })) {
              reportStatus("These cut files are already being moved");
              return;
            }
            auto cutSources = transfer.paths;
            const auto id = m_operations.move(std::move(transfer.paths), destination);
            m_cutOperations[id] = generation;
            m_cutOperationSources[id] = std::move(cutSources);
            m_pasteRequests[id] = PasteRequest{tabId, destination, navigation};
          } else {
            const auto id = m_operations.copy(std::move(transfer.paths), destination);
            m_pasteRequests[id] = PasteRequest{tabId, destination, navigation};
          }
        } catch (const std::exception& error) {
          reportStatus(std::string("Cannot paste files: ") + error.what());
        }
      } else if (!quitting && windowOpen && tabStillOpen && (!adminPaste || canPaste)) {
        reportStatus("File clipboard unavailable or changed during paste");
      }
      if (m_callbacks.onMaybeStopMainLoop)
        m_callbacks.onMaybeStopMainLoop();
    };
    if (m_readPasteSelectionForTesting)
      m_readPasteSelectionForTesting(std::move(onSelection));
    else
      m_transfer.readSelection(std::move(onSelection), adminPaste);
  }

  bool FileTransferController::recordPasteCompletion(const FileOperationManager::Operation& operation) {
    const auto found = m_pasteRequests.find(operation.id);
    if (found == m_pasteRequests.end())
      return false;
    const auto request = found->second;
    m_pasteRequests.erase(found);
    const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
    BrowserTab* tab = m_tabs.findTab(request.tabId);
    if (!windowOpen
        || tab == nullptr
        || request.navigation != tab->session.navigationGeneration()
        || operation.completedEntries.empty())
      return false;
    const auto applyMatch = [this](
                                BrowserTab& targetTab, const PasteRequest& matchedRequest,
                                const std::vector<FileOperationManager::CompletedEntry>& completedEntries
                            ) {
      const auto samePending = targetTab.pendingPasteSelection
          ? m_places.sameDirectoryPathCached(
                targetTab.pendingPasteSelection->request.destination, matchedRequest.destination
            )
          : std::optional<bool>(false);
      if (!targetTab.pendingPasteSelection
          || targetTab.pendingPasteSelection->request.tabId != matchedRequest.tabId
          || targetTab.pendingPasteSelection->request.navigation != matchedRequest.navigation
          || !samePending.value_or(false))
        targetTab.pendingPasteSelection = PasteSelection{matchedRequest, {}};
      auto& entries = targetTab.pendingPasteSelection->entries;
      entries.insert(entries.end(), completedEntries.begin(), completedEntries.end());
    };
    if (operation.adminCopy) {
      if (!tab->hasActiveAdminSession()
          || tab->adminSession.sessionGeneration() != operation.adminCopy->sessionGeneration
          || tab->session.path() != request.destination)
        return false;
      applyMatch(*tab, request, operation.completedEntries);
      return true;
    }
    if (const auto sameDest = m_places.sameDirectoryPathCached(request.destination, tab->session.path())) {
      if (!*sameDest)
        return false;
      applyMatch(*tab, request, operation.completedEntries);
      return true;
    }
    const TabId tabId = tab->id;
    const auto expectedPath = tab->session.path();
    auto completedEntries = operation.completedEntries;
    m_places.compareDirectoriesAsync(
        request.destination, expectedPath,
        [this, alive = std::weak_ptr<bool>(m_alive), tabAlive = std::weak_ptr<bool>(tab->alive), tabId, request,
         expectedPath, completedEntries = std::move(completedEntries), applyMatch](bool equivalent) {
          const auto locked = alive.lock();
          const auto tabLocked = tabAlive.lock();
          const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
          const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
          BrowserTab* targetTab = m_tabs.findTab(tabId);
          if (!locked
              || !*locked
              || !tabLocked
              || !*tabLocked
              || quitting
              || !stillOpen
              || targetTab == nullptr
              || request.navigation != targetTab->session.navigationGeneration()
              || targetTab->session.path() != expectedPath
              || !equivalent)
            return;
          applyMatch(*targetTab, request, completedEntries);
          if (!targetTab->session.loading())
            applyPendingPasteSelection(*targetTab);
        }
    );
    return false;
  }

  void FileTransferController::applyPendingPasteSelection(BrowserTab& tab) {
    if (!tab.pendingPasteSelection || tab.session.loading())
      return;
    auto pending = std::move(*tab.pendingPasteSelection);
    tab.pendingPasteSelection.reset();
    const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
    if (!windowOpen
        || pending.request.tabId != tab.id
        || pending.request.navigation != tab.session.navigationGeneration())
      return;
    if (const auto sameDest = m_places.sameDirectoryPathCached(pending.request.destination, tab.session.path())) {
      if (!*sameDest)
        return;
    } else {
      const TabId tabId = tab.id;
      const auto expectedPath = tab.session.path();
      m_places.compareDirectoriesAsync(
          pending.request.destination, expectedPath,
          [this, alive = std::weak_ptr<bool>(m_alive), tabAlive = std::weak_ptr<bool>(tab.alive), tabId,
           pending = std::move(pending), expectedPath](bool equivalent) mutable {
            const auto locked = alive.lock();
            const auto tabLocked = tabAlive.lock();
            const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
            const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
            BrowserTab* targetTab = m_tabs.findTab(tabId);
            if (!locked
                || !*locked
                || !tabLocked
                || !*tabLocked
                || quitting
                || !stillOpen
                || targetTab == nullptr
                || pending.request.navigation != targetTab->session.navigationGeneration()
                || targetTab->session.path() != expectedPath
                || !equivalent)
              return;
            targetTab->pendingPasteSelection = std::move(pending);
            if (!targetTab->session.loading())
              applyPendingPasteSelection(*targetTab);
          }
      );
      return;
    }
    // Resolve NOFOLLOW (unix::device, unix::inode) asynchronously for any candidate
    // entry whose identity is not yet populated (e.g. symlinks, because directory
    // enumeration uses G_FILE_QUERY_INFO_NONE which follows symlinks).
    std::vector<std::filesystem::path> unresolvedCandidates;
    for (const auto& entry : tab.session.entries()) {
      if (entry.hasIdentity || entry.absPath.parent_path() / "" != tab.session.path() / "")
        continue;
      if (tab.identityCheckedPaths.contains(entry.absPath.string()))
        continue;
      const bool nameMatches = std::ranges::any_of(pending.entries, [&](const auto& completed) {
        const std::filesystem::path reported(completed.path);
        return reported == entry.absPath
            || (reported.filename() == entry.absPath.filename()
                && m_places.sameDirectoryPathCached(reported.parent_path(), entry.absPath.parent_path())
                       .value_or(false));
      });
      if (nameMatches)
        unresolvedCandidates.push_back(entry.absPath);
    }
    if (!unresolvedCandidates.empty()) {
      struct IdentityBatch {
        TabId tabId = kInvalidTabId;
        std::weak_ptr<bool> tabAlive;
        PasteSelection pending;
        std::filesystem::path expectedPath;
        std::uint64_t enumGeneration = 0;
        std::size_t remaining = 0;
      };
      auto batch = std::make_shared<IdentityBatch>(IdentityBatch{
          .tabId = tab.id,
          .tabAlive = tab.alive,
          .pending = std::move(pending),
          .expectedPath = tab.session.path(),
          .enumGeneration = tab.session.enumerationGeneration(),
          .remaining = unresolvedCandidates.size(),
      });
      GCancellable* cancellable = tab.session.ensureEnumerationCancellable();
      for (const auto& candidate : unresolvedCandidates) {
        tab.identityCheckedPaths.insert(candidate.string());
        struct IdentityQuery {
          FileTransferController* controller;
          std::weak_ptr<bool> alive;
          std::filesystem::path path;
          std::shared_ptr<IdentityBatch> batch;
        };
        GFile* file = g_file_new_for_path(candidate.c_str());
        g_file_query_info_async(
            file, "unix::device,unix::inode", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, cancellable,
            [](GObject* source, GAsyncResult* result, gpointer userData) {
              std::unique_ptr<IdentityQuery> query(static_cast<IdentityQuery*>(userData));
              GError* error = nullptr;
              GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
              const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
              g_clear_error(&error);
              const auto locked = query->alive.lock();
              const auto tabLocked = query->batch->tabAlive.lock();
              auto* self = query->controller;
              const bool quitting = self && self->m_callbacks.isQuitting && self->m_callbacks.isQuitting();
              const bool stillOpen = self && self->m_callbacks.isWindowOpen && self->m_callbacks.isWindowOpen();
              BrowserTab* targetTab = self ? self->m_tabs.findTab(query->batch->tabId) : nullptr;
              if (cancelled
                  || !locked
                  || !*locked
                  || !tabLocked
                  || !*tabLocked
                  || quitting
                  || !stillOpen
                  || targetTab == nullptr
                  || query->batch->enumGeneration != targetTab->session.enumerationGeneration()
                  || query->batch->pending.request.navigation != targetTab->session.navigationGeneration()
                  || targetTab->session.path() != query->batch->expectedPath) {
                g_clear_object(&info);
                return;
              }
              if (info != nullptr
                  && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE)
                  && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE)) {
                const auto dev =
                    static_cast<std::uint64_t>(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE));
                const auto ino = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE);
                if (auto it = std::ranges::find(targetTab->session.entries(), query->path, &FileEntry::absPath);
                    it != targetTab->session.entries().end()) {
                  it->device = dev;
                  it->inode = ino;
                  it->hasIdentity = true;
                }
              }
              g_clear_object(&info);
              if (--query->batch->remaining == 0) {
                targetTab->pendingPasteSelection = std::move(query->batch->pending);
                if (!targetTab->session.loading())
                  self->applyPendingPasteSelection(*targetTab);
              }
            },
            new IdentityQuery{this, m_alive, candidate, batch}
        );
        g_object_unref(file);
      }
      return;
    }

    std::unordered_set<std::string> paths;
    std::optional<std::size_t> first;
    for (std::size_t index = 0; index < tab.session.entries().size(); ++index) {
      const auto& current = tab.session.entries()[index];
      const auto& path = current.absPath;
      // Selection stays in the displayed namespace, even when a drop resolves
      // a symlinked directory to its physical transfer destination.
      if (path.parent_path() / "" != tab.session.path() / "" || !current.hasIdentity)
        continue;
      if (std::ranges::none_of(pending.entries, [&](const auto& entry) {
            if (entry.device != current.device || entry.inode != current.inode)
              return false;
            const std::filesystem::path reported(entry.path);
            return reported == path
                || (reported.filename() == path.filename()
                    && m_places.sameDirectoryPathCached(reported.parent_path(), path.parent_path()).value_or(false));
          }))
        continue;
      paths.insert(path.string());
      if (!first)
        first = index;
    }
    if (!first)
      return;
    const bool isActive = (tab.id == m_tabs.activeTabId());
    auto* grid = isActive && m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const auto primaryPath = tab.session.entries()[*first].absPath;
    tab.pendingDropScrollGeneration = pending.request.dropScrollGeneration;
    const bool reveal =
        !tab.pendingDropScrollGeneration || (*tab.pendingDropScrollGeneration == tab.scrollState.userScrollGeneration);
    tab.pendingPasteRevealPath = reveal ? std::optional(primaryPath) : std::nullopt;
    tab.session.clearChooseFirstAfterLoad();
    if (m_callbacks.onApplyPasteSelection)
      m_callbacks.onApplyPasteSelection(tab.id, std::move(paths), primaryPath);
    if (isActive) {
      tab.pendingRevealOnActivate = false;
      if (grid) {
        grid->notifyDataChanged();
        if (reveal && !tab.pendingDropScrollGeneration)
          grid->scrollToIndex(*first);
      }
      if (m_callbacks.onUpdateSelectionSummary)
        m_callbacks.onUpdateSelectionSummary();
      if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
        surface->requestLayout();
    } else {
      tab.pendingRevealOnActivate = reveal;
    }
  }

  void FileTransferController::finishPendingTransferReveal(Renderer& renderer) {
    auto& tab = m_tabs.activeTab();
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (tab.pendingDropScrollGeneration) {
      if (!grid || *tab.pendingDropScrollGeneration != tab.scrollState.userScrollGeneration) {
        tab.pendingPasteRevealPath.reset();
        tab.pendingDropScrollGeneration.reset();
        return;
      }
      // Keep the path through layouts of an in-flight refresh. Resolve its index
      // only after enumeration/sorting, with the final viewport geometry available.
      if (tab.session.loading() || tab.session.pendingMonitorQueries() != 0 || tab.session.hasPendingMonitorChanges())
        return;
      if (tab.pendingPasteRevealPath && tab.selectedPath == tab.pendingPasteRevealPath) {
        const auto entry = std::ranges::find(tab.session.entries(), *tab.pendingPasteRevealPath, &FileEntry::absPath);
        if (entry != tab.session.entries().end()) {
          grid->scrollToIndex(static_cast<std::size_t>(entry - tab.session.entries().begin()));
          grid->layout(renderer);
        }
      }
      tab.pendingDropScrollGeneration.reset();
    }
    tab.pendingPasteRevealPath.reset();
  }

  void FileTransferController::copyToDesktopClipboard(bool cut) {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    const auto caps = tab.capabilities();
    if (session.browsingTrash() || !caps.canCopyItems || (cut && !caps.canMutate)) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus(
            session.browsingTrash() ? "This action is unavailable in Wastebin"
                                    : "This action is unavailable in Administrator Mode"
        );
      return;
    }
    const auto paths =
        m_callbacks.selectedPathsOrdered ? m_callbacks.selectedPathsOrdered() : std::vector<std::string>{};
    if (paths.empty())
      return;
    try {
      if (!m_transfer.setSelection(filemanager::encodeFileTransfer(paths, cut), m_wayland.lastInputSerial())) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Could not publish the file clipboard");
        return;
      }
    } catch (const std::exception& error) {
      if (m_callbacks.onStatus)
        m_callbacks.onStatus(std::string("Cannot copy files: ") + error.what());
    }
  }

  void FileTransferController::writeDesktopClipboard(std::string payload, const char* mime) {
    if (!m_transfer.setSelection({{mime, std::move(payload)}}, m_wayland.lastInputSerial()) && m_callbacks.onStatus)
      m_callbacks.onStatus("Could not publish the clipboard");
  }

  void FileTransferController::setCutPaths(std::vector<std::string> paths) {
    std::unordered_set<std::string> nextSet;
    std::vector<std::string> nextOrdered;
    nextSet.reserve(paths.size());
    nextOrdered.reserve(paths.size());
    for (auto& path : paths) {
      const auto key = normalizeCutPathKey(path);
      if (!key.empty() && nextSet.insert(key).second)
        nextOrdered.push_back(std::move(path));
    }
    const bool changed = nextSet != m_activeCutPaths;
    m_activeCutPaths = std::move(nextSet);
    m_activeCutOrdered = std::move(nextOrdered);
    if (!m_activeCutPaths.empty() && m_consumedCutSelection == m_transfer.selectionGeneration())
      m_consumedCutSelection.reset();
    if (changed && m_callbacks.onCutStateChanged)
      m_callbacks.onCutStateChanged();
  }

  bool FileTransferController::cancelCutSelection() {
    const bool hasOwnedCut = m_transfer.ownsSelection()
        && m_transfer.ownedSelectionPayload() != nullptr
        && decodeClipboardPayload(*m_transfer.ownedSelectionPayload())
               .transform([](const auto& p) { return p.cut && !p.paths.empty(); })
               .value_or(false);
    if (m_activeCutPaths.empty() && !hasOwnedCut)
      return false;
    const auto gen = m_transfer.selectionGeneration();
    m_consumedCutSelection = gen;
    const bool hadActive = !m_activeCutPaths.empty();
    m_activeCutPaths.clear();
    m_activeCutOrdered.clear();
    if (m_transfer.ownsSelection())
      m_transfer.consumeSelection(gen);
    if (hadActive && m_callbacks.onCutStateChanged)
      m_callbacks.onCutStateChanged();
    return true;
  }

  void FileTransferController::syncCutStateFromClipboard() {
    ++m_externalCutQueryGeneration;
    const auto generation = m_transfer.selectionGeneration();
    if (m_consumedCutSelection && *m_consumedCutSelection == generation) {
      const bool hadActive = !m_activeCutPaths.empty();
      m_activeCutPaths.clear();
      m_activeCutOrdered.clear();
      if (hadActive && m_callbacks.onCutStateChanged)
        m_callbacks.onCutStateChanged();
      return;
    }

    if (const auto* owned = m_transfer.ownedSelectionPayload()) {
      const auto decoded = decodeClipboardPayload(*owned);
      if (decoded && decoded->cut && !decoded->paths.empty())
        setCutPaths(decoded->paths);
      else
        setCutPaths({});
      return;
    }

    if (!m_transfer.ownsSelection() && m_transfer.selectionOffersFileCut()) {
      const auto queryGen = m_externalCutQueryGeneration;
      ++m_pendingClipboardReads;
      m_transfer.readSelection([this, alive = std::weak_ptr<bool>(m_alive), queryGen,
                                generation](std::optional<NativeDataTransfer::Payload> payload) {
        const auto locked = alive.lock();
        if (!locked || !*locked)
          return;
        --m_pendingClipboardReads;
        if (queryGen == m_externalCutQueryGeneration
            && generation == m_transfer.selectionGeneration()
            && (!m_consumedCutSelection || *m_consumedCutSelection != generation)) {
          const auto decoded = payload ? decodeClipboardPayload(*payload) : std::nullopt;
          if (decoded && decoded->cut && !decoded->paths.empty())
            setCutPaths(decoded->paths);
          else
            setCutPaths({});
        }
        if (m_callbacks.onMaybeStopMainLoop)
          m_callbacks.onMaybeStopMainLoop();
      });
      return;
    }

    setCutPaths({});
  }

  bool FileTransferController::isPathCut(const std::filesystem::path& path) const {
    if (m_activeCutPaths.empty() || path.empty())
      return false;
    const auto normalized = normalizeCutPathKey(path);
    if (m_activeCutPaths.contains(normalized))
      return true;
    const auto filename = path.filename();
    const auto parent = path.parent_path();
    if (filename.empty() || parent.empty())
      return false;
    for (const auto& cutRaw : m_activeCutOrdered) {
      const std::filesystem::path cutPath(cutRaw);
      if (cutPath.filename() == filename
          && m_places.sameDirectoryPathCached(cutPath.parent_path(), parent).value_or(false)) {
        return true;
      }
    }
    return false;
  }

  std::optional<std::filesystem::path> FileTransferController::dropDestination(float x, float y, bool incoming) const {
    const auto& tab = m_tabs.activeTab();
    const auto& session = tab.session;
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (!grid || session.browsingTrash() || !tab.capabilities().canAcceptDrop)
      return std::nullopt;
    if (auto placeDest = m_places.dropDestinationAt(x, y))
      return placeDest;
    float gx = 0, gy = 0;
    Node::absolutePosition(grid, gx, gy);
    if (x < gx || y < gy || x >= gx + grid->width() || y >= gy + grid->height())
      return std::nullopt;
    const auto index = grid->indexAtSurface(x, y);
    if (index && *index < session.entries().size()) {
      if (!session.entries()[*index].isDir)
        return incoming ? std::optional(session.path()) : std::nullopt;
      return session.entries()[*index].absPath;
    }
    return session.path();
  }

  bool FileTransferController::dragSourcesAreFolders() const {
    const auto& session = m_tabs.activeTab().session;
    if (m_dragSources.empty())
      return false;
    return std::ranges::all_of(m_dragSources, [&session](const auto& source) {
      const auto found = std::ranges::find(session.entries(), std::filesystem::path(source), &FileEntry::absPath);
      return found != session.entries().end() && found->isDir;
    });
  }

  void FileTransferController::cancelDropValidation() {
    ++m_dropValidationGeneration;
    if (m_dropValidationCancellable != nullptr) {
      g_cancellable_cancel(m_dropValidationCancellable);
      g_clear_object(&m_dropValidationCancellable);
    }
    m_dropValidationPending.reset();
    m_dropValidation.reset();
  }

  void FileTransferController::validateDropAsync(
      TabId tabId, std::vector<std::string> sources, std::filesystem::path destination, std::uint64_t navigation,
      std::uint64_t dragSession, std::function<void(DropValidation)> onComplete
  ) {
    const auto* activeTab = m_tabs.findTab(tabId);
    if (m_dropValidation
        && m_dropValidation->sources == sources
        && m_dropValidation->rawDestination == destination
        && activeTab != nullptr
        && tabId == m_tabs.activeTabId()
        && activeTab->session.navigationGeneration() == navigation) {
      if (onComplete)
        onComplete(*m_dropValidation);
      return;
    }

    const auto generation = ++m_dropValidationGeneration;
    if (m_dropValidationCancellable != nullptr) {
      g_cancellable_cancel(m_dropValidationCancellable);
      g_clear_object(&m_dropValidationCancellable);
    }
    std::weak_ptr<bool> alive = m_alive;
    auto deliver = [this, alive, tabId, generation, navigation, dragSession,
                    onComplete = std::move(onComplete)](DropValidation result) {
      const auto locked = alive.lock();
      const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
      const auto* tab = m_tabs.findTab(tabId);
      if (!locked
          || !*locked
          || quitting
          || m_dropValidationGeneration != generation
          || tab == nullptr
          || tabId != m_tabs.activeTabId()
          || tab->session.navigationGeneration() != navigation
          || m_dragSessionGeneration != dragSession) {
        return;
      }
      m_dropValidationPending.reset();
      m_dropValidation = result;
      if (result.target) {
        const auto normRaw = normalizeDirectoryPath(result.rawDestination);
        const auto normTarget = normalizeDirectoryPath(*result.target);
        const PlacePathResolution resolved{.exists = true, .isDirectory = true, .canonicalPath = normTarget};
        m_places.cacheResolvedPath(normRaw, resolved);
        m_places.cacheResolvedPath(normTarget, resolved);
      }
      if (onComplete)
        onComplete(std::move(result));
    };

    if (m_dropValidationHook) {
      if (m_dropValidationHook(sources, destination, std::move(deliver)))
        return;
    }

    m_dropValidationCancellable = g_cancellable_new();
    struct DropTaskInput {
      std::vector<std::string> sources;
      std::filesystem::path destination;
    };
    struct DropTaskCallback {
      std::function<void(DropValidation)> deliver;
    };
    GTask* task = g_task_new(
        nullptr, m_dropValidationCancellable,
        [](GObject*, GAsyncResult* result, gpointer userData) {
          std::unique_ptr<DropTaskCallback> cb(static_cast<DropTaskCallback*>(userData));
          GError* error = nullptr;
          auto* validated = static_cast<DropValidation*>(g_task_propagate_pointer(G_TASK(result), &error));
          const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
          g_clear_error(&error);
          if (validated != nullptr) {
            DropValidation value = std::move(*validated);
            delete validated;
            if (!cancelled && cb->deliver)
              cb->deliver(std::move(value));
          }
        },
        new DropTaskCallback{std::move(deliver)}
    );
    auto* input = new DropTaskInput{std::move(sources), std::move(destination)};
    g_task_set_task_data(task, input, [](gpointer p) { delete static_cast<DropTaskInput*>(p); });
    g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer taskData, GCancellable* cancellable) {
      if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(worker, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        return;
      }
      const auto& req = *static_cast<const DropTaskInput*>(taskData);
      DropValidation validated;
      validated.sources = req.sources;
      validated.rawDestination = req.destination;
      validated.target = filemanager::checkedDropTarget(req.sources, req.destination);
      if (validated.target)
        validated.copyByDefault = filemanager::copyDropByDefault(req.sources, *validated.target);
      g_task_return_pointer(worker, new DropValidation(std::move(validated)), [](gpointer p) {
        delete static_cast<DropValidation*>(p);
      });
    });
    g_object_unref(task);
  }

  void FileTransferController::requestHoverDropValidation(
      const std::vector<std::string>& sources, const std::filesystem::path& destination
  ) {
    if (m_dropValidation && m_dropValidation->sources == sources && m_dropValidation->rawDestination == destination)
      return;
    if (m_dropValidationPending
        && m_dropValidationPending->first == sources
        && m_dropValidationPending->second == destination)
      return;
    m_dropValidationPending = std::pair{sources, destination};
    const auto& active = m_tabs.activeTab();
    const TabId tabId = active.id;
    const auto navigation = active.session.navigationGeneration();
    const auto dragSession = m_dragSessionGeneration;
    validateDropAsync(
        tabId, sources, destination, navigation, dragSession,
        [this, alive = std::weak_ptr<bool>(m_alive), tabId, navigation, dragSession](DropValidation result) {
          const auto locked = alive.lock();
          const auto* tab = m_tabs.findTab(tabId);
          if (!locked
              || !*locked
              || tab == nullptr
              || tabId != m_tabs.activeTabId()
              || tab->session.navigationGeneration() != navigation
              || m_dragSessionGeneration != dragSession)
            return;
          if (m_draggingFiles || (m_protocolDragInside && m_transfer.dragging() && !m_transfer.dragDropped())) {
            const auto currentDest = dropDestination(m_dragPointerX, m_dragPointerY);
            if (currentDest && *currentDest == result.rawDestination && m_dragSources == result.sources) {
              m_dragTargetValid = m_pinBefore.has_value() || result.target.has_value();
              const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
              if (result.target) {
                m_protocolDropDestination = result.target;
                m_protocolDropSources = m_dragSources;
                const bool copy = (modifiers & KeyMod::Ctrl) || (!(modifiers & KeyMod::Shift) && result.copyByDefault);
                if (m_callbacks.onStatus)
                  m_callbacks.onStatus(
                      std::string(copy ? "Copy " : "Move ")
                      + std::to_string(m_dragSources.size())
                      + " items to "
                      + result.target->string()
                      + " · Escape cancels"
                  );
              } else {
                m_protocolDropDestination.reset();
                if (m_callbacks.onStatus)
                  m_callbacks.onStatus("Cannot drop items here · Escape cancels");
              }
              const auto serial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
              if (serial)
                m_wayland.setCursorShape(
                    serial,
                    m_dragTargetValid ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                      : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
                );
              if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
                surface->requestUpdate();
              scheduleNativeDragPreview();
            }
          }
        }
    );
  }

  std::uint32_t
  FileTransferController::updateProtocolDrag(wl_surface* surface, float x, float y, std::uint32_t actions) {
    constexpr auto copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    constexpr auto move = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
    auto* mainSurface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
    const bool renaming = m_callbacks.isRenaming && m_callbacks.isRenaming();
    clearDragFeedback();
    m_pinBefore.reset();
    m_places.showPlaceInsertion(std::nullopt);
    m_dragTargetValid = false;
    m_protocolDropDestination.reset();
    m_protocolDropSources.clear();
    m_protocolDragInside = mainSurface && surface == mainSurface->wlSurface();
    const auto& activeTab = m_tabs.activeTab();
    const auto& activeSession = activeTab.session;
    if (!windowOpen
        || !mainSurface
        || surface != mainSurface->wlSurface()
        || !grid
        || activeSession.browsingTrash()
        || !activeTab.capabilities().canAcceptDrop
        || m_places.hasActiveDeviceOperations()
        || m_operations.replaying()
        || renaming
        || activeSession.loading()) {
      if (mainSurface)
        mainSurface->requestUpdate();
      return 0;
    }
    m_dragPointerX = x;
    m_dragPointerY = y;
    const bool own = m_transfer.dragging() && !m_transfer.dragDropped() && !m_dragSources.empty();
    const bool folders = own && dragSourcesAreFolders();
    m_pinBefore = folders ? m_places.placeInsertionAt(x, y, "Personal", true) : std::nullopt;
    m_places.showPlaceInsertion(m_pinBefore);
    auto rawDestination = m_pinBefore ? std::nullopt : dropDestination(x, y, !own);
    std::optional<std::filesystem::path> destination = rawDestination;
    bool copyDefault = true;
    if (own && rawDestination) {
      if (m_dropValidation
          && m_dropValidation->sources == m_dragSources
          && m_dropValidation->rawDestination == *rawDestination) {
        destination = m_dropValidation->target;
        copyDefault = m_dropValidation->copyByDefault;
      } else {
        // Fail closed while asynchronous drop validation is in flight.
        requestHoverDropValidation(m_dragSources, *rawDestination);
        if (m_callbacks.onStatus)
          m_callbacks.onStatus("Checking drop target… · Escape cancels");
        mainSurface->requestUpdate();
        scheduleNativeDragPreview();
        return 0;
      }
    }
    if (!m_pinBefore && !destination) {
      mainSurface->requestUpdate();
      return 0;
    }
    const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    std::uint32_t action = copy;
    if (!m_pinBefore) {
      if (modifiers & KeyMod::Ctrl)
        action = copy;
      else if (modifiers & KeyMod::Shift)
        action = move;
      else {
        action = own && !copyDefault ? move : copy;
        if (!(actions & action))
          action = actions & copy ? copy : move;
      }
    }
    if (!(actions & action)) {
      mainSurface->requestUpdate();
      return 0;
    }
    m_dragTargetValid = true;
    m_protocolDropDestination = destination;
    if (own)
      m_protocolDropSources = m_dragSources;
    if (auto index = grid->indexAtSurface(x, y); destination && index && activeSession.entries()[*index].isDir) {
      m_dropIndex = index;
      grid->notifyItemChanged(*index);
    }
    if (destination) {
      for (const auto& [row, place] : m_places.placeRows()) {
        float rx = 0, ry = 0;
        Node::absolutePosition(row, rx, ry);
        if (x >= rx && x < rx + row->width() && y >= ry && y < ry + row->height()) {
          m_dropPlace = row;
          row->setSelected(true);
          break;
        }
      }
    }
    if (m_callbacks.onStatus)
      m_callbacks.onStatus(
          m_pinBefore ? "Pin folders in Places · Escape cancels"
                      : std::string(action == copy ? "Copy files to " : "Move files to ") + destination->string()
      );
    mainSurface->requestUpdate();
    scheduleNativeDragPreview();
    return action;
  }

  NativeDataTransfer::DropReceiver FileTransferController::receiveProtocolDrop(std::uint32_t action) {
    m_protocolDragInside = false;
    if (auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr)
      surface->requestUpdate();
    const auto pin = m_pinBefore;
    const auto destination = m_protocolDropDestination;
    if ((!pin && !destination) || !m_dragTargetValid)
      return {};
    const auto ownPaths = m_protocolDropSources;
    const auto& active = m_tabs.activeTab();
    const TabId tabId = active.id;
    const auto navigation = active.session.navigationGeneration();
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    const auto scrollGeneration = grid ? grid->scrollView().userScrollGeneration() : 0;
    return [this, alive = std::weak_ptr<bool>(m_alive), tabId, pin, destination, ownPaths, navigation, scrollGeneration,
            action](std::optional<NativeDataTransfer::Payload> payload, NativeDataTransfer::FinishDrop finish) {
      const auto locked = alive.lock();
      const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
      const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
      const auto* currentTab = m_tabs.findTab(tabId);
      if (!locked
          || !*locked
          || !windowOpen
          || quitting
          || currentTab == nullptr
          || tabId != m_tabs.activeTabId()
          || !payload
          || !payload->contains("text/uri-list")
          || m_places.hasActiveDeviceOperations()
          || m_operations.replaying()) {
        finish(false);
        return;
      }
      const auto dropNav = currentTab->session.navigationGeneration();
      const auto dropDragSession = m_dragSessionGeneration;
      try {
        auto paths = filemanager::decodeFileTransfer(payload->at("text/uri-list"), false).paths;
        if (paths.empty() || (!ownPaths.empty() && ownPaths != paths)) {
          finish(false);
          return;
        }
        if (pin) {
          if (action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
            finish(false);
            return;
          }
          struct PinCheck {
            std::vector<std::string> paths;
            std::string before;
            std::size_t remaining = 0;
            bool allFolders = true;
            NativeDataTransfer::FinishDrop finish;
          };
          auto check = std::make_shared<PinCheck>(PinCheck{
              .paths = paths,
              .before = *pin,
              .remaining = paths.size(),
              .allFolders = true,
              .finish = std::move(finish),
          });
          for (const auto& source : paths) {
            m_places.resolvePathAsync(
                std::filesystem::path(source), [this, alive, tabId, navigation, check](const PlacePathResolution& res) {
                  const auto stillAlive = alive.lock();
                  const bool stillQuitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
                  const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
                  const auto* tab = m_tabs.findTab(tabId);
                  if (!stillAlive
                      || !*stillAlive
                      || stillQuitting
                      || !stillOpen
                      || tab == nullptr
                      || tabId != m_tabs.activeTabId()
                      || tab->session.navigationGeneration() != navigation) {
                    if (check->finish) {
                      auto done = std::move(check->finish);
                      done(false);
                    }
                    return;
                  }
                  if (!res.isDirectory)
                    check->allFolders = false;
                  if (--check->remaining > 0)
                    return;
                  auto done = std::move(check->finish);
                  if (!check->allFolders) {
                    if (done)
                      done(false);
                    return;
                  }
                  m_places.pinFolders(check->paths, check->before);
                  if (done)
                    done(true);
                }
            );
          }
        } else {
          struct DropFinishGuard {
            NativeDataTransfer::FinishDrop finish;
            explicit DropFinishGuard(NativeDataTransfer::FinishDrop fn) : finish(std::move(fn)) {}
            DropFinishGuard(const DropFinishGuard&) = delete;
            DropFinishGuard& operator=(const DropFinishGuard&) = delete;
            void complete(bool ok) {
              if (finish) {
                auto fn = std::move(finish);
                fn(ok);
              }
            }
            ~DropFinishGuard() { complete(false); }
          };
          auto finishGuard = std::make_shared<DropFinishGuard>(std::move(finish));
          validateDropAsync(
              tabId, std::move(paths), *destination, dropNav, dropDragSession,
              [this, alive, tabId, navigation, dropNav, scrollGeneration, action,
               finishGuard](DropValidation validated) {
                const auto stillAlive = alive.lock();
                const bool stillOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
                const bool stillQuitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
                const auto* tab = m_tabs.findTab(tabId);
                if (!stillAlive
                    || !*stillAlive
                    || !stillOpen
                    || stillQuitting
                    || tab == nullptr
                    || tabId != m_tabs.activeTabId()
                    || tab->session.navigationGeneration() != dropNav
                    || m_places.hasActiveDeviceOperations()
                    || m_operations.replaying()) {
                  finishGuard->complete(false);
                  return;
                }
                if (validated.target) {
                  const auto id = action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
                      ? m_operations.move(std::move(validated.sources), validated.target->string())
                      : m_operations.copy(std::move(validated.sources), validated.target->string());
                  m_pasteRequests[id] = PasteRequest{tabId, validated.target->string(), navigation, scrollGeneration};
                  m_dropFinishes.emplace(id, std::move(finishGuard->finish));
                } else {
                  if (m_callbacks.onStatus)
                    m_callbacks.onStatus("Cannot drop unavailable files or copy a folder into itself");
                  finishGuard->complete(false);
                }
              }
          );
        }
      } catch (const std::exception& error) {
        if (m_callbacks.onStatus)
          m_callbacks.onStatus(std::string("Cannot drop files: ") + error.what());
        finish(false);
      }
    };
  }

  void FileTransferController::cancelFileGesture() {
    ++m_dragSessionGeneration;
    cancelDropValidation();
    if (!m_transfer.dragDropped())
      m_transfer.cancelDrag();
    clearNativeDragPreview();
    if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
      grid->endRubberBand(true);
    m_dragPending = false;
    m_draggingFiles = false;
    m_dragPressSerial = 0;
    m_protocolDragInside = false;
    m_protocolDropDestination.reset();
    m_protocolDropSources.clear();
    m_dragIndex.reset();
    m_dragSources.clear();
    m_pinBefore.reset();
    if (m_callbacks.onCancelPlaceReorder)
      m_callbacks.onCancelPlaceReorder();
    if (m_dragPreview)
      m_dragPreview->setVisible(false);
    m_places.showPlaceInsertion(std::nullopt);
    clearDragFeedback();
    m_dispatcher.cancelPointerCapture();
  }

  void FileTransferController::clearNativeDragPreview() {
    ++m_nativePreviewGeneration;
    m_nativePreviewQueued = false;
    m_nativePreviewTarget.reset();
    if (m_nativePreviewSurface)
      wl_surface_destroy(m_nativePreviewSurface);
    m_nativePreviewSurface = nullptr;
  }

  void FileTransferController::scheduleNativeDragPreview() {
    if (!m_nativePreviewSurface || m_nativePreviewQueued || !m_transfer.dragging() || m_transfer.dragDropped())
      return;
    m_nativePreviewQueued = true;
    const auto generation = m_nativePreviewGeneration;
    std::weak_ptr<bool> alive = m_alive;
    DeferredCall::callLater([this, alive, generation] {
      const auto locked = alive.lock();
      if (!locked || !*locked || generation != m_nativePreviewGeneration)
        return;
      m_nativePreviewQueued = false;
      auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
      if (!surface || !m_nativePreviewSurface || !m_transfer.dragging() || m_transfer.dragDropped())
        return;
      try {
        if (!m_renderContext.makeCurrent(surface->renderTarget()))
          return;
        const bool canScale =
            wl_surface_get_version(m_nativePreviewSurface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;
        const int scale = canScale
            ? std::clamp(static_cast<int>(std::ceil(surface->renderTarget().renderer().renderScale())), 1, 4)
            : 1;
        ScaledRenderer renderer(m_renderContext, static_cast<float>(scale));
        {
          UiPhaseScope layoutPhase(UiPhase::Layout);
          updateDragPreview(renderer, true);
        }
        if (!m_dragPreview)
          return;
        const auto width = static_cast<std::uint32_t>(std::ceil(m_dragPreview->width()));
        const auto height = static_cast<std::uint32_t>(std::ceil(m_dragPreview->height()));
        if (!width || !height)
          return;
        if (!m_nativePreviewTarget) {
          m_nativePreviewTarget = std::make_unique<RenderTarget>();
          m_nativePreviewTarget->create(m_nativePreviewSurface, m_renderContext);
        }
        if (canScale)
          wl_surface_set_buffer_scale(m_nativePreviewSurface, scale);
        m_nativePreviewTarget->setContentScale(static_cast<float>(scale));
        m_nativePreviewTarget->setLogicalSize(width, height);
        m_nativePreviewTarget->resize(
            width * static_cast<std::uint32_t>(scale), height * static_cast<std::uint32_t>(scale)
        );
        const auto previousX = m_dragPreview->x(), previousY = m_dragPreview->y();
        m_dragPreview->setPosition(0, 0);
        m_renderContext.renderScene(*m_nativePreviewTarget, m_dragPreview);
        m_dragPreview->setPosition(previousX, previousY);
        m_dragPreview->setVisible(false);
        m_renderContext.makeCurrent(surface->renderTarget());
      } catch (const std::exception& error) {
        cancelFileGesture();
        if (m_callbacks.onStatus)
          m_callbacks.onStatus(std::string("Could not render drag preview: ") + error.what());
      }
    });
  }

  void FileTransferController::updateDragPreview(Renderer& renderer, bool nativeIcon) {
    auto* root = m_callbacks.sceneRoot ? m_callbacks.sceneRoot() : nullptr;
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    if (!root || !surface)
      return;
    if (!m_draggingFiles
        || m_dragSources.empty()
        || (!nativeIcon
            && (m_transfer.dragIconVisible()
                || (m_transfer.dragging() && (!m_protocolDragInside || m_transfer.dragDropped()))))) {
      if (m_dragPreview)
        m_dragPreview->setVisible(false);
      return;
    }
    if (!m_dragPreview) {
      auto preview = std::make_unique<Flex>();
      preview->setParticipatesInLayout(false);
      preview->setHitTestVisible(false);
      preview->setExcludeSubtreeFromTabOrder(true);
      preview->setZIndex(1000);
      preview->setPadding(8.0F);
      preview->setGap(8.0F);
      preview->setAlign(FlexAlign::Center);
      constexpr float previewOpacity = 0.94F;
      preview->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh, previewOpacity));
      applyNytheraBevel(*preview, Style::scaledRadiusMd(1.0F));
      auto icon = std::make_unique<FileIcon>();
      icon->setHitTestVisible(false);
      icon->setGlyphSize(36.0F);
      m_dragPreviewIcon = static_cast<FileIcon*>(preview->addChild(std::move(icon)));
      auto text = std::make_unique<Flex>();
      text->setDirection(FlexDirection::Vertical);
      text->setHitTestVisible(false);
      auto name = std::make_unique<Label>();
      name->setFontSize(Style::fontSizeBody);
      name->setMaxWidth(220.0F);
      name->setMaxLines(1);
      name->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_dragPreviewLabel = static_cast<Label*>(text->addChild(std::move(name)));
      auto action = std::make_unique<Label>();
      action->setFontSize(Style::fontSizeCaption);
      action->setColor(colorSpecFromRole(ColorRole::Primary));
      m_dragPreviewAction = static_cast<Label*>(text->addChild(std::move(action)));
      preview->addChild(std::move(text));
      m_dragPreview = static_cast<Flex*>(root->addChild(std::move(preview)));
    }
    const auto& activeSession = m_tabs.activeTab().session;
    const std::filesystem::path path(m_dragSources.front());
    const auto previewEntry = std::ranges::find(activeSession.entries(), path, &FileEntry::absPath);
    m_dragPreviewIcon->setFile(path, previewEntry != activeSession.entries().end() && previewEntry->isDir);
    m_dragPreviewLabel->setText(
        m_dragSources.size() == 1 ? filemanager::sanitizeFilenameForDisplay(path.filename().string())
                                  : std::to_string(m_dragSources.size()) + " selected items"
    );
    const auto destination = dropDestination(m_dragPointerX, m_dragPointerY);
    const bool externalTarget = nativeIcon && m_transfer.dragging() && !m_protocolDragInside;
    const bool targetValid = externalTarget ? m_transfer.dragAction() != 0 : m_dragTargetValid;
    const bool cachedCopyDefault = destination
            && m_dropValidation
            && m_dropValidation->sources == m_dragSources
            && m_dropValidation->rawDestination == *destination
        ? m_dropValidation->copyByDefault
        : true;
    const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
    const bool copy = externalTarget
        ? m_transfer.dragAction() == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
        : (modifiers & KeyMod::Ctrl) || (destination && !(modifiers & KeyMod::Shift) && cachedCopyDefault);
    m_dragPreviewAction->setText(
        m_pinBefore        ? "Pin in Places"
            : !targetValid ? "Cannot drop here"
            : copy         ? "Copy here"
                           : "Move here"
    );
    constexpr float previewBorderOpacity = 0.85F;
    m_dragPreview->setBorder(
        colorSpecFromRole(targetValid ? ColorRole::Primary : ColorRole::OutlineVariant, previewBorderOpacity), 1.0F
    );
    m_dragPreview->setVisible(true);
    m_dragPreview->layout(renderer);
    float px = m_dragPointerX + 18.0F;
    float py = m_dragPointerY + 20.0F;
    if (px + m_dragPreview->width() > static_cast<float>(surface->width()))
      px = m_dragPointerX - m_dragPreview->width() - 18.0F;
    if (py + m_dragPreview->height() > static_cast<float>(surface->height()))
      py = m_dragPointerY - m_dragPreview->height() - 20.0F;
    m_dragPreview->setPosition(std::max(0.0F, px), std::max(0.0F, py));
  }

  void FileTransferController::clearDragFeedback() {
    if (m_dropPlace)
      m_dropPlace->setSelected(false);
    m_dropPlace = nullptr;
    auto old = m_dropIndex;
    m_dropIndex.reset();
    if (old) {
      if (auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr)
        grid->notifyItemChanged(*old);
    }
    if (m_callbacks.onUpdateNavigationControls)
      m_callbacks.onUpdateNavigationControls();
  }

  void FileTransferController::beginFileDragPress(std::size_t index, float x, float y, std::uint32_t serial) {
    if (!m_tabs.activeTab().capabilities().canDragSource)
      return;
    m_dragIndex = index;
    m_dragStartX = x;
    m_dragStartY = y;
    m_dragPending = true;
    m_dragPressSerial = serial;
  }

  void FileTransferController::handlePointerLeaveDuringDrag() {
    if (!m_transfer.dragging())
      cancelFileGesture();
    else {
      clearDragFeedback();
      if (m_dragPreview)
        m_dragPreview->setVisible(false);
    }
    m_dispatcher.pointerLeave();
  }

  void FileTransferController::handleAxisDuringProtocolDrag(const PointerEvent& event) {
    m_dispatcher.pointerAxis(
        m_dragPointerX, m_dragPointerY, event.axis, event.axisSource, event.axisValue, event.axisDiscrete,
        event.axisValue120, event.axisLines, event.axisGestureSerial
    );
    m_transfer.updateDragTarget();
  }

  void FileTransferController::cancelLocalDragState() {
    m_dragPending = false;
    m_draggingFiles = false;
    m_dragIndex.reset();
    m_dragSources.clear();
    m_pinBefore.reset();
    if (m_dragPreview)
      m_dragPreview->setVisible(false);
    clearDragFeedback();
  }

  bool FileTransferController::handleFileDragPointer(const PointerEvent& event, float x, float y) {
    if (!m_dragPending && !m_draggingFiles)
      return false;
    if (!m_tabs.activeTab().capabilities().canDragSource) {
      cancelFileGesture();
      return false;
    }
    auto* surface = m_callbacks.mainSurface ? m_callbacks.mainSurface() : nullptr;
    auto* grid = m_callbacks.grid ? m_callbacks.grid() : nullptr;
    if (event.type == PointerEvent::Type::Axis) {
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      auto motion = event;
      motion.type = PointerEvent::Type::Motion;
      if (m_callbacks.onSynthesizeMotion)
        m_callbacks.onSynthesizeMotion(motion);
      return true;
    }
    if (event.type == PointerEvent::Type::Motion) {
      if (!m_draggingFiles && std::hypot(x - m_dragStartX, y - m_dragStartY) > 5.0F) {
        m_draggingFiles = true;
        m_dragSources =
            m_callbacks.selectedPathsOrdered ? m_callbacks.selectedPathsOrdered() : std::vector<std::string>{};
        if (m_dragSources.empty()) {
          cancelFileGesture();
          return true;
        }
        if (m_callbacks.onClearLastPointerIndex)
          m_callbacks.onClearLastPointerIndex();
        m_dispatcher.pointerLeave();
        TooltipManager::instance().forceDestroy();
        try {
          auto payload = filemanager::encodeFileTransfer(m_dragSources, false);
          // Clipboard cut metadata is not drag action negotiation.
          payload.erase("application/x-kde-cutselection");
          payload.erase("x-special/gnome-copied-files");
          m_dragPointerX = x;
          m_dragPointerY = y;
          clearNativeDragPreview();
          m_nativePreviewSurface = wl_compositor_create_surface(m_wayland.compositor());
          if (surface
              && m_transfer.startDrag(
                  std::move(payload), surface->wlSurface(), m_dragPressSerial, nullptr, m_nativePreviewSurface
              )) {
            scheduleNativeDragPreview();
            return true;
          }
          clearNativeDragPreview();
        } catch (const std::exception& error) {
          if (m_callbacks.onStatus)
            m_callbacks.onStatus(std::string("Cannot start file drag: ") + error.what());
          cancelFileGesture();
          return true;
        }
      }
      if (m_draggingFiles) {
        clearDragFeedback();
        m_dragPointerX = x;
        m_dragPointerY = y;
        const bool folders = dragSourcesAreFolders();
        m_pinBefore = folders ? m_places.placeInsertionAt(x, y, "Personal", true) : std::nullopt;
        m_places.showPlaceInsertion(m_pinBefore);
        const auto rawDestination = m_pinBefore ? std::nullopt : dropDestination(x, y);
        std::optional<std::filesystem::path> destination;
        bool checking = false;
        bool copyDefault = true;
        if (rawDestination) {
          if (m_dropValidation
              && m_dropValidation->sources == m_dragSources
              && m_dropValidation->rawDestination == *rawDestination) {
            destination = m_dropValidation->target;
            copyDefault = m_dropValidation->copyByDefault;
          } else {
            checking = true;
            requestHoverDropValidation(m_dragSources, *rawDestination);
          }
        }
        if (destination) {
          if (auto row = m_places.rowIndexAt(x, y)) {
            m_dropPlace = m_places.placeRows()[*row].first;
            m_dropPlace->setSelected(true);
          } else if (grid) {
            if (auto index = grid->indexAtSurface(x, y)) {
              m_dropIndex = index;
              grid->notifyItemChanged(*index);
            }
          }
        }
        m_dragTargetValid = m_pinBefore.has_value() || destination.has_value();
        const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
        if (m_callbacks.onStatus)
          m_callbacks.onStatus(
              m_pinBefore ? "Pin folders in Places · Escape cancels"
                  : checking
                  ? "Checking drop target… · Escape cancels"
                  : std::string(
                        ((modifiers & KeyMod::Ctrl) || (destination && !(modifiers & KeyMod::Shift) && copyDefault))
                            ? "Copy "
                            : "Move "
                    )
                      + std::to_string(m_dragSources.size())
                      + " items"
                      + (destination ? " to " + destination->string() : " — no drop target")
                      + " · Escape cancels"
          );
        const auto serial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
        if (serial)
          m_wayland.setCursorShape(
              serial,
              m_dragTargetValid ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED
          );
        if (surface)
          surface->requestUpdate();
      }
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      if (m_draggingFiles) {
        const bool folders = dragSourcesAreFolders();
        const auto pin = folders ? m_places.placeInsertionAt(x, y, "Personal", true) : std::nullopt;
        const auto destination = pin ? std::nullopt : dropDestination(x, y);
        if (pin)
          m_places.pinFolders(m_dragSources, *pin);
        else if (
            destination && !m_dragSources.empty() && !m_places.hasActiveDeviceOperations() && !m_operations.replaying()
        ) {
          const auto& active = m_tabs.activeTab();
          const TabId tabId = active.id;
          const auto navigation = active.session.navigationGeneration();
          const auto dragSession = m_dragSessionGeneration;
          const auto modifiers = m_callbacks.keyboardModifiers ? m_callbacks.keyboardModifiers() : 0U;
          validateDropAsync(
              tabId, m_dragSources, *destination, navigation, dragSession,
              [this, alive = std::weak_ptr<bool>(m_alive), tabId, navigation, modifiers](DropValidation validated) {
                const auto locked = alive.lock();
                const bool quitting = m_callbacks.isQuitting && m_callbacks.isQuitting();
                const bool windowOpen = m_callbacks.isWindowOpen && m_callbacks.isWindowOpen();
                const auto* tab = m_tabs.findTab(tabId);
                if (!locked
                    || !*locked
                    || quitting
                    || !windowOpen
                    || tab == nullptr
                    || tabId != m_tabs.activeTabId()
                    || tab->session.navigationGeneration() != navigation
                    || m_places.hasActiveDeviceOperations()
                    || m_operations.replaying())
                  return;
                if (validated.target) {
                  const bool copy =
                      (modifiers & KeyMod::Ctrl) || (!(modifiers & KeyMod::Shift) && validated.copyByDefault);
                  const auto id = copy ? m_operations.copy(std::move(validated.sources), validated.target->string())
                                       : m_operations.move(std::move(validated.sources), validated.target->string());
                  m_pasteRequests[id] = PasteRequest{tabId, validated.target->string(), navigation};
                } else if (m_callbacks.onStatus) {
                  m_callbacks.onStatus("Cannot drop items into themselves or their own subfolders");
                }
              }
          );
        } else if (m_callbacks.onStatus) {
          m_callbacks.onStatus("Drop cancelled");
        }
      } else if (m_dragIndex && *m_dragIndex < m_tabs.activeTab().session.entries().size()) {
        if (m_callbacks.onClickUnmovedDragItem)
          m_callbacks.onClickUnmovedDragItem(*m_dragIndex, event.time);
      }
      m_dragPending = false;
      m_draggingFiles = false;
      m_dragIndex.reset();
      m_dragSources.clear();
      m_pinBefore.reset();
      m_places.showPlaceInsertion(std::nullopt);
      if (m_dragPreview)
        m_dragPreview->setVisible(false);
      clearDragFeedback();
      m_dispatcher.cancelPointerCapture();
      const auto serial = m_callbacks.pointerSerial ? m_callbacks.pointerSerial() : 0U;
      if (serial)
        m_wayland.setCursorShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      return true;
    }
    return false;
  }

} // namespace filemanager
