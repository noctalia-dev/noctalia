#include "filemanager/directory_session.h"

#include "filemanager/file_columns.h"
#include "filemanager/file_sort.h"
#include "filemanager/folder_size.h"
#include "ui/dialogs/file_icon.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

namespace filemanager {

  namespace {

    FileEntry makeFileEntry(GFileInfo* info, const std::filesystem::path& path) {
      FileEntry entry;
      entry.name = g_file_info_get_name(info);
      entry.absPath = path;
      entry.isDir = gFileInfoTypeOrUnknown(info) == G_FILE_TYPE_DIRECTORY;
      entry.size = g_file_info_get_size(info);
      if (const auto* type = g_file_info_get_content_type(info))
        entry.type = type;
      entry.mtime = std::filesystem::file_time_type::clock::from_sys(
          std::chrono::system_clock::from_time_t(
              static_cast<std::time_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
          )
      );
      entry.hasCreated = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_CREATED);
      entry.created = entry.hasCreated ? g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CREATED) : 0;
      entry.hasAccessed = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_ACCESS);
      entry.accessed = entry.hasAccessed ? g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_ACCESS) : 0;
      entry.isSymlink =
          g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) && g_file_info_get_is_symlink(info);
      if (!entry.isSymlink
          && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE)
          && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_INODE)) {
        entry.device = static_cast<std::uint64_t>(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE));
        entry.inode = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE);
        entry.hasIdentity = true;
      }
      return entry;
    }

    bool sameEntry(const FileEntry& lhs, const FileEntry& rhs) {
      return lhs.name == rhs.name
          && lhs.absPath == rhs.absPath
          && lhs.isDir == rhs.isDir
          && lhs.size == rhs.size
          && lhs.mtime == rhs.mtime
          && lhs.created == rhs.created
          && lhs.accessed == rhs.accessed
          && lhs.hasCreated == rhs.hasCreated
          && lhs.hasAccessed == rhs.hasAccessed
          && lhs.device == rhs.device
          && lhs.inode == rhs.inode
          && lhs.hasIdentity == rhs.hasIdentity
          && lhs.isSymlink == rhs.isSymlink
          && lhs.type == rhs.type;
    }

    struct MonitoredFileQuery {
      DirectorySession* session;
      std::weak_ptr<bool> alive;
      std::uint64_t generation;
      std::filesystem::path path;
      std::filesystem::path replacedPath;
    };

  } // namespace

  DirectorySession::DirectorySession(std::filesystem::path initialPath, Callbacks callbacks)
      : m_callbacks(std::move(callbacks)),
        m_path(std::move(initialPath)) {}

  DirectorySession::~DirectorySession() { shutdown(); }

  void DirectorySession::shutdown() {
    if (m_alive)
      *m_alive = false;
    clearFolderCounts();
    cancelDirectoryWritableQuery();
    cancelEnumeration();
    closeMonitor();
  }

  void DirectorySession::navigate(const std::filesystem::path& requestedPath, bool recordHistory) {
    if (m_callbacks.onCancelFileGesture)
      m_callbacks.onCancelFileGesture();
    std::error_code ec;
    const bool trash = requestedPath == "trash:" || requestedPath.string().starts_with("trash:/");
    auto path =
        trash ? requestedPath.lexically_normal() : std::filesystem::absolute(requestedPath, ec).lexically_normal();
    if (ec)
      path = requestedPath.lexically_normal();
    if (trash && !path.string().starts_with("trash:/"))
      path = "trash:/";
    const bool pathChanged = path != m_path;
    if (m_callbacks.onBeforeNavigate)
      m_callbacks.onBeforeNavigate(path, pathChanged);
    if (recordHistory && pathChanged) {
      m_backHistory.push_back(m_path);
      m_forwardHistory.clear();
    }
    if (pathChanged) {
      ++m_navigationGeneration;
      m_chooseFirstAfterLoad = true;
      m_entries.clear();
      if (m_callbacks.onBeforePathChange)
        m_callbacks.onBeforePathChange(path);
    }
    m_path = std::move(path);
    m_browsingTrash = trash;
    m_trashUris.clear();
    m_trashOriginalPaths.clear();
    if (m_callbacks.onAfterNavigatePathSet)
      m_callbacks.onAfterNavigatePathSet(m_path, m_browsingTrash);
    enumerate(m_path);
  }

  void DirectorySession::navigateTrash() { navigate(std::filesystem::path("trash:/")); }

  std::optional<std::uint64_t> DirectorySession::folderItemCount(const FileEntry& entry) {
    if (!entry.isDir || m_loading || !(m_callbacks.isSizeColumnVisible && m_callbacks.isSizeColumnVisible()))
      return std::nullopt;
    const auto path = entry.absPath.string();
    if (const auto found = m_folderCounts.find(path); found != m_folderCounts.end())
      return found->second.items;
    const auto id = ++m_nextFolderCountId;
    m_folderCounts.emplace(path, FolderCount{id, std::nullopt});
    m_pendingFolderCounts.emplace_back(path, id);
    startNextFolderCount();
    return std::nullopt;
  }

  void DirectorySession::startNextFolderCount() {
    if (m_folderCountRunning)
      return;
    // Only one worker per window. Recent rows take priority after a scroll.
    while (!m_pendingFolderCounts.empty()) {
      auto [path, id] = std::move(m_pendingFolderCounts.back());
      m_pendingFolderCounts.pop_back();
      const auto cached = m_folderCounts.find(path);
      if (cached == m_folderCounts.end() || cached->second.id != id)
        continue;
      g_autoptr(GFile) directory = nullptr;
      if (m_browsingTrash) {
        const auto uri = m_trashUris.find(path);
        if (uri == m_trashUris.end())
          continue;
        directory = g_file_new_for_uri(uri->second.c_str());
      } else {
        directory = g_file_new_for_path(path.c_str());
      }

      struct Request {
        DirectorySession* session;
        std::weak_ptr<bool> alive;
        std::filesystem::path path;
        std::uint64_t id;
      };
      m_folderCountRunning = true;
      m_folderCountCancellable = g_cancellable_new();
      GTask* task = g_task_new(
          directory, m_folderCountCancellable,
          [](GObject*, GAsyncResult* result, gpointer) {
            auto* completed = G_TASK(result);
            const auto* request = static_cast<const Request*>(g_task_get_task_data(completed));
            const std::unique_ptr<std::uint64_t> count(
                static_cast<std::uint64_t*>(g_task_propagate_pointer(completed, nullptr))
            );
            const auto alive = request->alive.lock();
            if (!alive || !*alive)
              return;
            auto* self = request->session;
            g_clear_object(&self->m_folderCountCancellable);
            self->m_folderCountRunning = false;
            const auto found = self->m_folderCounts.find(request->path.string());
            if (count && found != self->m_folderCounts.end() && found->second.id == request->id) {
              found->second.items = *count;
              const auto row = std::ranges::find(self->m_entries, request->path, &FileEntry::absPath);
              if (row != self->m_entries.end() && self->m_callbacks.onEntryChangedAt)
                self->m_callbacks.onEntryChangedAt(static_cast<std::size_t>(row - self->m_entries.begin()));
            }
            self->startNextFolderCount();
          },
          nullptr
      );
      g_task_set_task_data(
          task, new Request{this, m_alive, std::filesystem::path(std::move(path)), id},
          [](gpointer data) { delete static_cast<Request*>(data); }
      );
      g_task_run_in_thread(task, [](GTask* worker, gpointer source, gpointer, GCancellable* cancellable) {
        const auto count = filemanager::countImmediateFolderItems(G_FILE(source), cancellable);
        g_task_return_pointer(worker, count ? new std::uint64_t(*count) : nullptr, [](gpointer data) {
          delete static_cast<std::uint64_t*>(data);
        });
      });
      g_object_unref(task);
      return;
    }
  }

  void DirectorySession::clearFolderCounts() {
    if (m_folderCountCancellable) {
      g_cancellable_cancel(m_folderCountCancellable);
      g_clear_object(&m_folderCountCancellable);
    }
    m_pendingFolderCounts.clear();
    m_folderCounts.clear();
  }

  void DirectorySession::cancelEnumeration() {
    if (m_enumerationCancellable != nullptr) {
      g_cancellable_cancel(m_enumerationCancellable);
      g_clear_object(&m_enumerationCancellable);
    }
  }

  void DirectorySession::enumerate(const std::filesystem::path& path) {
    FileIcon::invalidateDirectoryMetadata();
    if (m_callbacks.onCancelFileGesture)
      m_callbacks.onCancelFileGesture();
    if (m_callbacks.isRenaming && m_callbacks.isRenaming()) {
      if (m_callbacks.onDeferRefreshDuringRename)
        m_callbacks.onDeferRefreshDuringRename();
      return;
    }
    closeMonitor();
    cancelEnumeration();
    m_path = path;
    if (!m_browsingTrash) {
      queryDirectoryWritableAsync(path);
    } else {
      cancelDirectoryWritableQuery();
    }
    clearFolderCounts();
    m_enumerationCancellable = g_cancellable_new();
    const auto generation = ++m_enumerationGeneration;
    m_pendingMonitorQueries = 0;
    m_loading = true;
    m_loadingEntries.clear();
    m_pendingMonitorChanges.clear();
    if (m_callbacks.onEnumerateStarted)
      m_callbacks.onEnumerateStarted(m_path, m_browsingTrash);
    monitorDirectory();

    struct EnumerateRequest {
      DirectorySession* session;
      std::weak_ptr<bool> alive;
      std::uint64_t generation;
    };
    GFile* file = browsingFile();
    g_file_enumerate_children_async(
        file,
        "standard::name,standard::display-name,standard::type,standard::size,standard::content-type,standard::is-symlink,"
        "time::modified,time::created,time::access,trash::orig-path,unix::device,unix::inode",
        G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, m_enumerationCancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<EnumerateRequest> request(static_cast<EnumerateRequest*>(data));
          GError* error = nullptr;
          GFileEnumerator* enumerator = g_file_enumerate_children_finish(G_FILE(source), result, &error);
          const auto locked = request->alive.lock();
          if (!locked || !*locked || request->generation != request->session->m_enumerationGeneration) {
            if (enumerator != nullptr)
              g_object_unref(enumerator);
            g_clear_error(&error);
            return;
          }
          auto* self = request->session;
          const auto requestGeneration = request->generation;
          if (enumerator == nullptr) {
            self->m_loading = false;
            self->m_loadingEntries.clear();
            const std::string detail = error == nullptr ? "Unable to open this folder" : error->message;
            g_clear_error(&error);
            if (self->m_callbacks.onEnumerateFailed)
              self->m_callbacks.onEnumerateFailed(detail);
            self->applyPendingMonitorChanges();
            return;
          }
          g_clear_error(&error);
          self->fetchNext(enumerator, requestGeneration);
          g_object_unref(enumerator);
        },
        new EnumerateRequest{this, m_alive, generation}
    );
    g_object_unref(file);
  }

  void DirectorySession::fetchNext(GFileEnumerator* enumerator, std::uint64_t generation) {
    struct FetchNextRequest {
      DirectorySession* session;
      std::weak_ptr<bool> alive;
      GFileEnumerator* enumerator;
      std::uint64_t generation;
    };
    g_file_enumerator_next_files_async(
        enumerator, 128, G_PRIORITY_DEFAULT, m_enumerationCancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<FetchNextRequest> state(static_cast<FetchNextRequest*>(data));
          GError* error = nullptr;
          GList* infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), result, &error);
          const auto locked = state->alive.lock();
          if (!locked || !*locked || state->generation != state->session->m_enumerationGeneration) {
            g_list_free_full(infos, g_object_unref);
            g_clear_error(&error);
            g_object_unref(state->enumerator);
            return;
          }
          auto* self = state->session;
          auto* activeEnumerator = state->enumerator;
          const auto requestGeneration = state->generation;
          if (error != nullptr) {
            self->m_loading = false;
            self->m_loadingEntries.clear();
            const std::string detail = error->message;
            g_clear_error(&error);
            g_list_free_full(infos, g_object_unref);
            g_object_unref(activeEnumerator);
            if (self->m_callbacks.onEnumerateFailed)
              self->m_callbacks.onEnumerateFailed(detail);
            self->applyPendingMonitorChanges();
            return;
          }
          for (GList* node = infos; node != nullptr; node = node->next) {
            auto* info = G_FILE_INFO(node->data);
            const char* name = g_file_info_get_name(info);
            if (name == nullptr || (!self->m_showHidden && name[0] == '.'))
              continue;
            auto entry = makeFileEntry(info, self->m_path / name);
            if (self->m_browsingTrash) {
              GFile* child = g_file_enumerator_get_child(activeEnumerator, info);
              char* uri = g_file_get_uri(child);
              if (uri != nullptr)
                self->m_trashUris[entry.absPath.string()] = uri;
              const char* original = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
              if (original != nullptr)
                self->m_trashOriginalPaths[entry.absPath.string()] = original;
              const char* displayName = g_file_info_get_display_name(info);
              if (displayName != nullptr)
                entry.name = displayName;
              g_free(uri);
              g_object_unref(child);
            }
            self->m_loadingEntries.push_back(std::move(entry));
          }
          g_list_free_full(infos, g_object_unref);
          if (infos == nullptr) {
            self->m_loading = false;
            self->m_entries = std::move(self->m_loadingEntries);
            self->sortEntries();
            const bool chooseFirst = self->m_chooseFirstAfterLoad;
            self->m_chooseFirstAfterLoad = false;
            if (self->m_callbacks.onEnumerateCompletedStatus)
              self->m_callbacks.onEnumerateCompletedStatus(self->m_entries.empty());
            if (self->m_callbacks.onEntriesLoaded)
              self->m_callbacks.onEntriesLoaded(chooseFirst);
            g_object_unref(activeEnumerator);
            self->applyPendingMonitorChanges();
            if (self->m_callbacks.onAfterEntriesLoaded)
              self->m_callbacks.onAfterEntriesLoaded();
            return;
          }
          self->fetchNext(activeEnumerator, requestGeneration);
          g_object_unref(activeEnumerator);
        },
        new FetchNextRequest{this, m_alive, G_FILE_ENUMERATOR(g_object_ref(enumerator)), generation}
    );
  }

  void DirectorySession::sortEntries() {
    if (m_callbacks.onCancelFileGesture)
      m_callbacks.onCancelFileGesture();
    const auto field = m_callbacks.sortField ? m_callbacks.sortField() : FileDialogSortField::Name;
    const auto order = m_callbacks.sortOrder ? m_callbacks.sortOrder() : FileDialogSortOrder::Ascending;
    std::ranges::stable_sort(m_entries, [this, field, order](const FileEntry& lhs, const FileEntry& rhs) {
      return filemanager::fileEntryLess(lhs, rhs, field, order, m_foldersFirst, m_hiddenLast);
    });
  }

  void DirectorySession::cancelDirectoryWritableQuery() {
    ++m_directoryWritableGeneration;
    if (m_directoryWritableCancellable != nullptr) {
      g_cancellable_cancel(m_directoryWritableCancellable);
      g_clear_object(&m_directoryWritableCancellable);
    }
    m_directoryWritablePath.clear();
    m_directoryWritable.reset();
  }

  void DirectorySession::queryDirectoryWritableAsync(const std::filesystem::path& path) {
    cancelDirectoryWritableQuery();
    if (path.empty() || path.string().starts_with("trash:"))
      return;
    const auto generation = m_directoryWritableGeneration;
    m_directoryWritablePath = path;
    std::weak_ptr<bool> alive = m_alive;
    if (m_directoryWritableQueryHook) {
      auto complete = [this, alive, generation, path](bool writable) {
        const auto locked = alive.lock();
        if (!locked || !*locked || m_directoryWritableGeneration != generation || m_path != path)
          return;
        m_directoryWritable = writable;
      };
      if (m_directoryWritableQueryHook(path, std::move(complete)))
        return;
    }

    struct WritableRequest {
      DirectorySession* session;
      std::weak_ptr<bool> alive;
      std::uint64_t generation;
      std::filesystem::path path;
    };
    m_directoryWritableCancellable = g_cancellable_new();
    GFile* file = g_file_new_for_path(path.c_str());
    g_file_query_info_async(
        file, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
        m_directoryWritableCancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<WritableRequest> req(static_cast<WritableRequest*>(data));
          GError* error = nullptr;
          GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
          const bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) != FALSE;
          g_clear_error(&error);
          const auto locked = req->alive.lock();
          if (cancelled
              || !locked
              || !*locked
              || req->session->m_directoryWritableGeneration != req->generation
              || req->session->m_path != req->path) {
            g_clear_object(&info);
            return;
          }
          const bool writable =
              info != nullptr && g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) != FALSE;
          g_clear_object(&info);
          req->session->m_directoryWritable = writable;
        },
        new WritableRequest{this, alive, generation, path}
    );
    g_object_unref(file);
  }

  bool DirectorySession::currentDirectoryWritable() const {
    if (m_browsingTrash)
      return false;
    return m_directoryWritable.has_value() && m_directoryWritablePath == m_path && *m_directoryWritable;
  }

  GFile* DirectorySession::browsingFile() const {
    if (!m_browsingTrash)
      return g_file_new_for_path(m_path.c_str());
    GFile* file = g_file_new_for_uri("trash:///");
    for (const auto& part : m_path) {
      if (part == "trash:" || part == "/" || part == ".")
        continue;
      GFile* child = g_file_get_child(file, part.c_str());
      g_object_unref(file);
      file = child;
    }
    return file;
  }

  void DirectorySession::monitorDirectory() {
    closeMonitor();
    // Trash is a GIO virtual namespace. Never monitor a filesystem path bearing
    // its URI token; explicit refresh enumerates the backend again.
    if (m_browsingTrash)
      return;
    GFile* file = g_file_new_for_path(m_path.c_str());
    GError* error = nullptr;
    m_monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
    g_object_unref(file);
    if (m_monitor != nullptr) {
      g_signal_connect(
          m_monitor, "changed",
          G_CALLBACK(+[](GFileMonitor* monitor, GFile* changedFile, GFile* otherFile, GFileMonitorEvent event,
                         gpointer data) {
            auto* self = static_cast<DirectorySession*>(data);
            if (monitor == self->m_monitor)
              self->handleMonitorEvent(changedFile, otherFile, event);
          }),
          this
      );
    }
    g_clear_error(&error);
  }

  void DirectorySession::handleMonitorEvent(GFile* file, GFile* otherFile, GFileMonitorEvent event) {
    if (file == nullptr)
      return;
    char* rawPath = g_file_get_path(file);
    char* rawOtherPath = otherFile == nullptr ? nullptr : g_file_get_path(otherFile);
    if (rawPath == nullptr) {
      g_free(rawOtherPath);
      return;
    }
    PendingMonitorChange change{
        .event = event,
        .path = std::filesystem::path(rawPath).lexically_normal(),
        .otherPath =
            rawOtherPath == nullptr ? std::filesystem::path{} : std::filesystem::path(rawOtherPath).lexically_normal(),
    };
    g_free(rawPath);
    g_free(rawOtherPath);
    if (change.path == m_path || change.path.parent_path() != m_path)
      return;
    if (m_loading || (m_callbacks.isRenaming && m_callbacks.isRenaming())) {
      m_pendingMonitorChanges.push_back(std::move(change));
      return;
    }

    switch (change.event) {
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
      removeMonitoredFile(change.path);
      break;
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED:
      if (!change.otherPath.empty() && change.otherPath.parent_path() == m_path)
        queryMonitoredFile(change.otherPath, change.path);
      else
        removeMonitoredFile(change.path);
      break;
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_CHANGED:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
      queryMonitoredFile(change.path);
      break;
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
      if (m_callbacks.onStatus)
        m_callbacks.onStatus("This folder is no longer available");
      break;
    }
  }

  void DirectorySession::applyPendingMonitorChanges() {
    auto changes = std::move(m_pendingMonitorChanges);
    m_pendingMonitorChanges.clear();
    for (const auto& change : changes) {
      GFile* file = g_file_new_for_path(change.path.c_str());
      GFile* otherFile = change.otherPath.empty() ? nullptr : g_file_new_for_path(change.otherPath.c_str());
      handleMonitorEvent(file, otherFile, change.event);
      g_object_unref(file);
      if (otherFile != nullptr)
        g_object_unref(otherFile);
    }
  }

  void DirectorySession::queryMonitoredFile(
      const std::filesystem::path& path, const std::filesystem::path& replacedPath
  ) {
    ++m_pendingMonitorQueries;
    if (m_enumerationCancellable == nullptr)
      m_enumerationCancellable = g_cancellable_new();
    GFile* file = g_file_new_for_path(path.c_str());
    g_file_query_info_async(
        file,
        "standard::name,standard::type,standard::size,standard::content-type,standard::is-symlink,time::modified,"
        "time::created,time::access,unix::device,unix::inode",
        G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, m_enumerationCancellable,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<MonitoredFileQuery> query(static_cast<MonitoredFileQuery*>(data));
          GError* error = nullptr;
          GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
          const auto locked = query->alive.lock();
          if (!locked || !*locked) {
            if (info != nullptr)
              g_object_unref(info);
            g_clear_error(&error);
            return;
          }
          auto* self = query->session;
          if (query->generation == self->m_enumerationGeneration) {
            --self->m_pendingMonitorQueries;
            if (self->m_callbacks.onMonitorQueryCompleted)
              self->m_callbacks.onMonitorQueryCompleted();
          }
          if (query->generation != self->m_enumerationGeneration || query->path.parent_path() != self->m_path) {
            if (info != nullptr)
              g_object_unref(info);
            g_clear_error(&error);
            return;
          }
          if (info == nullptr) {
            g_clear_error(&error);
            self->removeMonitoredFile(query->replacedPath.empty() ? query->path : query->replacedPath);
            return;
          }
          g_clear_error(&error);
          const char* name = g_file_info_get_name(info);
          if (name == nullptr || (!self->m_showHidden && name[0] == '.')) {
            g_object_unref(info);
            self->removeMonitoredFile(query->replacedPath.empty() ? query->path : query->replacedPath);
            return;
          }

          FileEntry entry = makeFileEntry(info, query->path);
          g_object_unref(info);
          auto oldEntry = std::ranges::find(self->m_entries, query->path, &FileEntry::absPath);
          const auto replacedEntry = query->replacedPath.empty()
              ? self->m_entries.end()
              : std::ranges::find(self->m_entries, query->replacedPath, &FileEntry::absPath);
          if (query->replacedPath.empty() && oldEntry != self->m_entries.end() && sameEntry(*oldEntry, entry))
            return;
          self->m_folderCounts.erase(query->path.string());
          if (!query->replacedPath.empty()) {
            self->m_folderCounts.erase(query->replacedPath.string());
            if (self->m_callbacks.onEntryMonitoredRenamed)
              self->m_callbacks.onEntryMonitoredRenamed(query->replacedPath, query->path);
          }
          if (replacedEntry != self->m_entries.end())
            self->m_entries.erase(replacedEntry);
          oldEntry = std::ranges::find(self->m_entries, query->path, &FileEntry::absPath);
          if (oldEntry != self->m_entries.end())
            *oldEntry = std::move(entry);
          else
            self->m_entries.push_back(std::move(entry));

          self->sortEntries();
          if (self->m_callbacks.onEntriesChanged)
            self->m_callbacks.onEntriesChanged();
          if (self->m_callbacks.onStatus)
            self->m_callbacks.onStatus("");
        },
        new MonitoredFileQuery{this, m_alive, m_enumerationGeneration, path, replacedPath}
    );
    g_object_unref(file);
  }

  void DirectorySession::removeMonitoredFile(const std::filesystem::path& path) {
    if (path.empty())
      return;
    const auto oldSize = m_entries.size();
    std::erase_if(m_entries, [&path](const FileEntry& entry) { return entry.absPath == path; });
    if (oldSize == m_entries.size())
      return;
    m_folderCounts.erase(path.string());
    if (m_callbacks.onEntryMonitoredRemoved)
      m_callbacks.onEntryMonitoredRemoved(path);
    if (m_callbacks.onEntriesChanged)
      m_callbacks.onEntriesChanged();
    if (m_callbacks.onStatus)
      m_callbacks.onStatus(m_entries.empty() ? "This folder is empty" : "");
  }

  void DirectorySession::closeMonitor() {
    if (m_monitor != nullptr) {
      g_signal_handlers_disconnect_by_data(m_monitor, this);
      g_file_monitor_cancel(m_monitor);
      g_object_unref(m_monitor);
      m_monitor = nullptr;
    }
  }

} // namespace filemanager
