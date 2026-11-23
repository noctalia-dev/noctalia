#include "filemanager/browser_tab.h"

#include "ui/dialogs/file_artwork.h"

#include <algorithm>
#include <utility>

namespace filemanager {

  BrowserTab::BrowserTab(
      TabId tabId, std::filesystem::path initialPath, TabViewState initialViewState,
      DirectorySession::Callbacks sessionCallbacks, std::function<void(TabId)> onStatusInfoChanged
  )
      : id(tabId), session(std::move(initialPath), std::move(sessionCallbacks)), adminSession(tabId, alive),
        viewState(initialViewState), m_onStatusInfoChanged(std::move(onStatusInfoChanged)) {
    session.setShowHidden(viewState.showHidden);
    session.setFoldersFirst(viewState.foldersFirst);
    session.setHiddenLast(viewState.hiddenLast);
    session.setPrivilegedBackend(DirectorySession::PrivilegedBackend{
        .isActive = [this] { return adminSession.isActive(); },
        .enumerateDirectory =
            [this](
                const std::filesystem::path& path,
                bool userInitiated,
                std::function<void(const std::vector<AdminDirEntryRecord>&)> onBatch,
                std::function<void(bool ok, bool writable, const std::string& errorDetail)> onComplete
            ) {
              return adminSession.listDirectoryAsync(
                  path, userInitiated, kAdminDefaultDirBatchEntries, std::move(onBatch),
                  [onComplete = std::move(onComplete)](
                      bool ok, bool writable, std::uint64_t, AdminStatusCode, const std::string& detail
                  ) {
                    if (onComplete)
                      onComplete(ok, writable, detail);
                  }
              );
            },
        .queryWritable =
            [this](const std::filesystem::path& path, std::function<void(bool writable)> onComplete) {
              return adminSession.statInfoAsync(
                  path, false,
                  [onComplete = std::move(onComplete)](
                      bool ok, const AdminStatRecord& rec, AdminStatusCode, const std::string&
                  ) {
                    if (onComplete)
                      onComplete(ok && rec.exists && rec.writable);
                  }
              );
            },
        .countFolderItems =
            [this](const std::filesystem::path& path, std::function<void(std::optional<std::uint64_t>)> onComplete) {
              return adminSession.countDirectoryItemsAsync(
                  path,
                  [onComplete = std::move(onComplete)](
                      bool ok, std::uint64_t count, AdminStatusCode, const std::string&
                  ) {
                    if (onComplete)
                      onComplete(ok ? std::optional<std::uint64_t>(count) : std::nullopt);
                  }
              );
            },
        .cancelRequest =
            [this](AdminRequestId reqId) {
              if (adminSession.isActive() && reqId != kInvalidAdminRequestId)
                (void)adminSession.cancelRequestAsync(reqId);
            },
    });
  }

  BrowserTab::~BrowserTab() { shutdown(); }

  void BrowserTab::shutdown() {
    if (alive)
      *alive = false;
    cancelSelectionInfoQuery();
    cancelStatusFreeSpaceQuery();
    session.shutdown();
    if (adminSession.isActive())
      adminSession.stopSession(true);
    adminSession.shutdown();
  }

  void BrowserTab::leaveAdministratorMode() {
    administratorBrowsing = false;
    session.discardPrivilegedListing();
    cancelSelectionInfoQuery();
    cancelStatusFreeSpaceQuery();
    adminSession.stopSession(true);
    selectedPath.reset();
    selectedPaths.clear();
    selectionAnchor.reset();
    pendingPasteSelection.reset();
    identityCheckedPaths.clear();
    pendingPasteRevealPath.reset();
    pendingDropScrollGeneration.reset();
    pendingRevealOnActivate = false;
  }

  TabCapabilities BrowserTab::capabilities() const noexcept {
    if (session.browsingTrash())
      return TabCapabilities::forWastebin();
    if (hasActiveAdminSession() || administratorBrowsing) {
      return TabCapabilities::forAdministrator(
          hasActiveAdminSession() && session.currentDirectoryWritable() && !session.loading()
      );
    }
    return TabCapabilities::forLocal(session.currentDirectoryWritable());
  }

  std::vector<std::string> BrowserTab::selectedPathsOrdered() const {
    std::vector<std::string> paths;
    paths.reserve(selectedPaths.size());
    for (const auto& entry : session.entries()) {
      if (selectedPaths.contains(entry.absPath.string()))
        paths.push_back(entry.absPath.string());
    }
    return paths;
  }

  std::optional<std::pair<std::filesystem::path, bool>> BrowserTab::symlinkSelection() const {
    if (session.browsingTrash() || session.loading() || selectedPaths.size() != 1)
      return std::nullopt;
    const auto selected = std::ranges::find_if(session.entries(), [this](const auto& entry) {
      return selectedPaths.contains(entry.absPath.string());
    });
    if (selected == session.entries().end() || (administratorBrowsing && selected->isSymlink))
      return std::nullopt;
    if (m_selectionInfo && m_selectionInfo->path == selected->absPath && m_selectionInfo->symlink)
      return std::nullopt;
    return std::pair{selected->absPath, selected->isDir};
  }

  void BrowserTab::cancelStatusFreeSpaceQuery() {
    ++m_statusFreeSpaceGeneration;
    if (m_statusFreeSpaceCancellable != nullptr) {
      g_cancellable_cancel(m_statusFreeSpaceCancellable);
      g_clear_object(&m_statusFreeSpaceCancellable);
    }
    if (m_statusFreeSpaceAdminReqId != kInvalidAdminRequestId && adminSession.isActive()) {
      (void)adminSession.cancelRequestAsync(std::exchange(m_statusFreeSpaceAdminReqId, kInvalidAdminRequestId));
    } else {
      m_statusFreeSpaceAdminReqId = kInvalidAdminRequestId;
    }
    m_statusFreeSpacePath.clear();
    m_statusFreeSpaceBytes.reset();
  }

  void BrowserTab::queryStatusFreeSpaceAsync(const std::filesystem::path& path) {
    cancelStatusFreeSpaceQuery();
    if (path.empty() || isTrashTabPath(path))
      return;
    const auto generation = m_statusFreeSpaceGeneration;
    m_statusFreeSpacePath = path;
    std::weak_ptr<bool> weakAlive = alive;
    if (m_freeSpaceQueryHook) {
      auto complete = [this, weakAlive, generation, path](std::optional<std::uint64_t> freeBytes) {
        const auto locked = weakAlive.lock();
        if (!locked || !*locked || m_statusFreeSpaceGeneration != generation || session.path() != path)
          return;
        m_statusFreeSpaceBytes = freeBytes;
        if (m_onStatusInfoChanged)
          m_onStatusInfoChanged(id);
      };
      if (m_freeSpaceQueryHook(path, std::move(complete)))
        return;
    }

    if (hasActiveAdminSession()) {
      m_statusFreeSpaceAdminReqId = adminSession.statVfsAsync(
          path,
          [this, weakAlive, generation, path](
              bool ok, const AdminStatVfsRecord& rec, AdminStatusCode, const std::string&
          ) {
            const auto locked = weakAlive.lock();
            if (!locked || !*locked || m_statusFreeSpaceGeneration != generation || session.path() != path)
              return;
            m_statusFreeSpaceAdminReqId = kInvalidAdminRequestId;
            if (ok)
              m_statusFreeSpaceBytes = rec.freeBytes != 0 ? rec.freeBytes : rec.availableBytes;
            else
              m_statusFreeSpaceBytes.reset();
            if (m_onStatusInfoChanged)
              m_onStatusInfoChanged(id);
          }
      );
      return;
    }

    struct FreeSpaceRequest {
      BrowserTab* tab;
      std::weak_ptr<bool> alive;
      std::uint64_t generation;
      std::filesystem::path path;
    };
    m_statusFreeSpaceCancellable = g_cancellable_new();
    GFile* file = g_file_new_for_path(path.c_str());
    g_file_query_filesystem_info_async(
        file, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, G_PRIORITY_DEFAULT, m_statusFreeSpaceCancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<FreeSpaceRequest> req(static_cast<FreeSpaceRequest*>(data));
          GError* error = nullptr;
          GFileInfo* info = g_file_query_filesystem_info_finish(G_FILE(source), result, &error);
          const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
          g_clear_error(&error);
          const auto locked = req->alive.lock();
          if (cancelled
              || !locked
              || !*locked
              || req->tab->m_statusFreeSpaceGeneration != req->generation
              || req->tab->session.path() != req->path) {
            g_clear_object(&info);
            return;
          }
          std::optional<std::uint64_t> freeBytes;
          if (info != nullptr && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
            freeBytes = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
          g_clear_object(&info);
          req->tab->m_statusFreeSpaceBytes = freeBytes;
          if (req->tab->m_onStatusInfoChanged)
            req->tab->m_onStatusInfoChanged(req->tab->id);
        },
        new FreeSpaceRequest{this, weakAlive, generation, path}
    );
    g_object_unref(file);
  }

  void BrowserTab::cancelSelectionInfoQuery() {
    ++m_selectionInfoGeneration;
    if (m_selectionInfoCancellable != nullptr) {
      g_cancellable_cancel(m_selectionInfoCancellable);
      g_clear_object(&m_selectionInfoCancellable);
    }
    if (m_selectionInfoAdminReqId != kInvalidAdminRequestId && adminSession.isActive()) {
      (void)adminSession.cancelRequestAsync(std::exchange(m_selectionInfoAdminReqId, kInvalidAdminRequestId));
    } else {
      m_selectionInfoAdminReqId = kInvalidAdminRequestId;
    }
    m_selectionInfoPendingPath.clear();
    m_selectionInfo.reset();
  }

  void BrowserTab::querySelectionInfoAsync(const std::filesystem::path& path) {
    cancelSelectionInfoQuery();
    if (path.empty())
      return;
    const auto generation = m_selectionInfoGeneration;
    m_selectionInfoPendingPath = path;
    std::weak_ptr<bool> weakAlive = alive;
    if (m_selectionInfoQueryHook) {
      auto complete = [this, weakAlive, generation, path](std::optional<SelectionInfo> info) {
        const auto locked = weakAlive.lock();
        if (!locked
            || !*locked
            || m_selectionInfoGeneration != generation
            || selectedPaths.size() != 1
            || *selectedPaths.begin() != path.string()) {
          return;
        }
        m_selectionInfo = std::move(info);
        if (m_onStatusInfoChanged)
          m_onStatusInfoChanged(id);
      };
      if (m_selectionInfoQueryHook(path, std::move(complete)))
        return;
    }

    if (hasActiveAdminSession()) {
      m_selectionInfoAdminReqId = adminSession.statInfoAsync(
          path,
          false,
          [this, weakAlive, generation, path](
              bool ok, const AdminStatRecord& rec, AdminStatusCode, const std::string&
          ) {
            const auto locked = weakAlive.lock();
            if (!locked
                || !*locked
                || m_selectionInfoGeneration != generation
                || selectedPaths.size() != 1
                || *selectedPaths.begin() != path.string()) {
              return;
            }
            m_selectionInfoAdminReqId = kInvalidAdminRequestId;
            if (!ok || !rec.exists)
              return;
            std::string mime = rec.contentType;
            if (mime.empty() && !rec.isDir) {
              if (gchar* guessed = g_content_type_guess(path.filename().c_str(), nullptr, 0, nullptr)) {
                mime = guessed;
                g_free(guessed);
              }
            }
            gchar* typeName = mime.empty() ? nullptr : g_content_type_get_description(mime.c_str());
            std::string description;
            if (rec.isSymlink && rec.brokenSymlink) {
              description = "Broken symbolic link";
            } else if (rec.isDir) {
              description = "Folder";
            } else if (typeName != nullptr) {
              description = typeName;
            } else {
              description = "File";
            }
            g_free(typeName);
            if (rec.isSymlink)
              description += " (symbolic link)";
            m_selectionInfo = SelectionInfo{
                .path = path,
                .type = std::move(description),
                .target = rec.symlinkTarget,
                .symlink = rec.isSymlink,
                .hasSize = rec.hasSize,
                .size = rec.size,
            };
            if (m_onStatusInfoChanged)
              m_onStatusInfoChanged(id);
          }
      );
      return;
    }

    struct SelectionInfoRequest {
      BrowserTab* tab;
      std::weak_ptr<bool> alive;
      std::uint64_t generation;
      std::filesystem::path path;
      std::shared_ptr<GCancellable> cancel;
      bool symlink = false;
      std::string target;
      bool hasSize = false;
      std::uint64_t size = 0;
    };
    auto cancel = std::shared_ptr<GCancellable>(g_cancellable_new(), g_object_unref);
    m_selectionInfoCancellable = G_CANCELLABLE(g_object_ref(cancel.get()));
    GFile* file = isTrashTabPath(path) ? g_file_new_for_uri(path.c_str()) : g_file_new_for_path(path.c_str());
    constexpr const char* attributes =
        "standard::type,standard::is-symlink,standard::symlink-target,standard::content-type,standard::size";
    auto* initialReq = new SelectionInfoRequest{this, weakAlive, generation, path, std::move(cancel)};
    g_file_query_info_async(
        file, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, initialReq->cancel.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<SelectionInfoRequest> req(static_cast<SelectionInfoRequest*>(data));
          GError* error = nullptr;
          GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
          const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
          g_clear_error(&error);
          const auto locked = req->alive.lock();
          if (cancelled
              || !locked
              || !*locked
              || req->tab->m_selectionInfoGeneration != req->generation
              || req->tab->selectedPaths.size() != 1
              || *req->tab->selectedPaths.begin() != req->path.string()) {
            g_clear_object(&info);
            return;
          }
          const bool symlink = info != nullptr
              && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK)
              && g_file_info_get_is_symlink(info);
          if (info != nullptr && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
            const auto fileType = gFileInfoTypeOrUnknown(info);
            if (symlink || fileType == G_FILE_TYPE_REGULAR) {
              req->hasSize = true;
              req->size = static_cast<std::uint64_t>(g_file_info_get_size(info));
            }
          }
          if (!symlink) {
            std::string description = "File";
            if (info != nullptr) {
              const char* contentType = g_file_info_get_content_type(info);
              gchar* typeName = contentType == nullptr ? nullptr : g_content_type_get_description(contentType);
              description = gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_DIRECTORY ? "Folder"
                  : typeName != nullptr                                           ? typeName
                                                                                  : "File";
              g_free(typeName);
            }
            g_clear_object(&info);
            req->tab->m_selectionInfo = SelectionInfo{
                .path = req->path,
                .type = std::move(description),
                .target = {},
                .symlink = false,
                .hasSize = req->hasSize,
                .size = req->size,
            };
            if (req->tab->m_onStatusInfoChanged)
              req->tab->m_onStatusInfoChanged(req->tab->id);
            return;
          }

          req->symlink = true;
          if (const char* value = g_file_info_get_symlink_target(info); value != nullptr && *value != '\0') {
            const std::filesystem::path raw(value);
            req->target = (raw.is_absolute() ? raw : req->path.parent_path() / raw).lexically_normal().string();
          }
          g_clear_object(&info);
          auto* followReq = req.release();
          g_file_query_info_async(
              G_FILE(source), "standard::type,standard::content-type", G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
              followReq->cancel.get(),
              [](GObject* followSource, GAsyncResult* followResult, gpointer followData) {
                std::unique_ptr<SelectionInfoRequest> follow(static_cast<SelectionInfoRequest*>(followData));
                GError* followError = nullptr;
                GFileInfo* targetInfo = g_file_query_info_finish(G_FILE(followSource), followResult, &followError);
                const bool followCancelled = g_error_matches(followError, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
                g_clear_error(&followError);
                const auto followLocked = follow->alive.lock();
                if (followCancelled
                    || !followLocked
                    || !*followLocked
                    || follow->tab->m_selectionInfoGeneration != follow->generation
                    || follow->tab->selectedPaths.size() != 1
                    || *follow->tab->selectedPaths.begin() != follow->path.string()) {
                  g_clear_object(&targetInfo);
                  return;
                }
                const bool broken =
                    targetInfo == nullptr || gFileInfoTypeOrUnknown(targetInfo) == G_FILE_TYPE_SYMBOLIC_LINK;
                std::string description = broken ? "Broken symbolic link" : "File";
                if (targetInfo != nullptr && !broken) {
                  const char* contentType = g_file_info_get_content_type(targetInfo);
                  gchar* typeName = contentType == nullptr ? nullptr : g_content_type_get_description(contentType);
                  description = gFileInfoTypeOrUnknown(targetInfo) == G_FILE_TYPE_DIRECTORY ? "Folder"
                      : typeName != nullptr                                                 ? typeName
                                                                                            : "File";
                  g_free(typeName);
                }
                g_clear_object(&targetInfo);
                description += " (symbolic link)";
                follow->tab->m_selectionInfo = SelectionInfo{
                    .path = follow->path,
                    .type = std::move(description),
                    .target = std::move(follow->target),
                    .symlink = true,
                    .hasSize = follow->hasSize,
                    .size = follow->size,
                };
                if (follow->tab->m_onStatusInfoChanged)
                  follow->tab->m_onStatusInfoChanged(follow->tab->id);
              },
              followReq
          );
        },
        initialReq
    );
    g_object_unref(file);
  }

} // namespace filemanager
