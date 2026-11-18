#include "filemanager/file_operation_manager.h"

#include "filemanager/file_interaction.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <condition_variable>
#include <linux/fs.h>
#include <mutex>
#include <ranges>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <tuple>

// Cooperative suspension at GIO I/O and finalization boundaries. The worker
// acknowledges suspension only after it has actually stopped at this gate.
class FileTransferPauseState {
public:
  void pause() { std::lock_guard lock(m_mutex); m_requested = true; }
  void resume() { std::lock_guard lock(m_mutex); m_requested = false; m_changed.notify_all(); }
  void cancel() { std::lock_guard lock(m_mutex); m_cancelled = true; m_changed.notify_all(); }
  bool requested() const { std::lock_guard lock(m_mutex); return m_requested; }
  std::chrono::steady_clock::duration pausedTime() const { std::lock_guard lock(m_mutex); return m_pausedTime; }
  bool checkpoint(GCancellable* cancellable, const std::function<void(bool)>& acknowledge) {
    std::unique_lock lock(m_mutex);
    if (m_requested && !m_cancelled && !g_cancellable_is_cancelled(cancellable)) {
      const auto started = std::chrono::steady_clock::now();
      lock.unlock();
      acknowledge(true);
      lock.lock();
      m_changed.wait(lock, [&] { return !m_requested || m_cancelled || g_cancellable_is_cancelled(cancellable); });
      m_pausedTime += std::chrono::steady_clock::now() - started;
      lock.unlock();
      acknowledge(false);
      lock.lock();
    }
    return !m_cancelled && !g_cancellable_is_cancelled(cancellable);
  }
private:
  mutable std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_requested = false;
  bool m_cancelled = false;
  std::chrono::steady_clock::duration m_pausedTime{};
};

namespace {
  using Kind = FileOperationManager::Kind;

  bool isCopy(Kind kind) { return kind == Kind::Copy || kind == Kind::Duplicate; }
  bool isTransfer(Kind kind) { return isCopy(kind) || kind == Kind::Move; }
  bool isReplay(Kind kind) { return kind == Kind::Undo || kind == Kind::Redo; }
  bool reportsProgress(Kind kind) {
    return isTransfer(kind) || isReplay(kind) || kind == Kind::Trash || kind == Kind::Delete || kind == Kind::Restore
        || kind == Kind::EmptyWastebin || kind == Kind::Properties;
  }

  std::string numberedName(const std::string& stem, const std::string& extension, unsigned suffix, bool copyStyle) {
    if (!copyStyle)
      return stem + (suffix == 1 ? std::string{} : "-" + std::to_string(suffix)) + extension;
    return stem + " (copy" + (suffix == 1 ? std::string{} : " " + std::to_string(suffix)) + ")" + extension;
  }

  struct TransferMeasure { std::uint64_t bytes = 0; std::uint64_t items = 0; };

  bool measureTree(GFile* file, GCancellable* cancellable, TransferMeasure& measure, GError** error) {
    GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (info == nullptr) return false;
    const auto type = g_file_info_get_file_type(info);
    if (type != G_FILE_TYPE_DIRECTORY) {
      measure.bytes += g_file_info_get_size(info);
      ++measure.items;
      g_object_unref(info);
      return true;
    }
    g_object_unref(info);
    GFileEnumerator* entries = g_file_enumerate_children(file,
        G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (entries == nullptr) return false;
    while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
      GFile* next = g_file_get_child(file, g_file_info_get_name(child));
      const bool ok = measureTree(next, cancellable, measure, error);
      g_object_unref(next);
      g_object_unref(child);
      if (!ok) { g_object_unref(entries); return false; }
    }
    const bool ok = error == nullptr || *error == nullptr;
    g_object_unref(entries);
    return ok;
  }

  struct TransferProgress {
    FileOperationManager* owner;
    std::uint64_t id;
    std::uint64_t baseBytes;
    std::uint64_t progressedBytes = 0;
    std::uint64_t totalBytes;
    std::uint64_t completedItems;
    std::uint64_t totalItems;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastReport = started;
    std::uint64_t lastBytes = 0;
    std::shared_ptr<FileTransferPauseState> pause;
    GCancellable* cancellable = nullptr;
    std::function<void(bool)> acknowledgePause;
    std::function<void(std::uint64_t, std::uint64_t)> boundary;
    std::chrono::steady_clock::duration reportedPause{};
  };

  void publishProgress(TransferProgress& context, std::uint64_t currentBytes, bool force = false) {
    const auto now = std::chrono::steady_clock::now();
    const auto paused = context.pause ? context.pause->pausedTime() : std::chrono::steady_clock::duration{};
    const auto activeElapsed = now - context.lastReport - (paused - context.reportedPause);
    if (!force && activeElapsed < std::chrono::milliseconds(100)) return;
    const std::uint64_t total = std::max(context.progressedBytes, context.baseBytes + currentBytes);
    const double seconds = std::chrono::duration<double>(activeElapsed).count();
    const std::uint64_t speed = seconds > 0.0 ? static_cast<std::uint64_t>(
        static_cast<double>(total - context.lastBytes) / seconds) : 0;
    context.lastReport = now;
    context.reportedPause = paused;
    context.lastBytes = total;
    auto* owner = context.owner;
    g_idle_add([](gpointer data) -> gboolean {
      auto* report = static_cast<std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>*>(data);
      const auto& values = report->second;
      report->first->updateProgress(values[0], values[1], values[2], values[3], values[4], values[5]);
      delete report;
      return G_SOURCE_REMOVE;
    }, new std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>{owner,
        {context.id, total, context.totalBytes, context.completedItems, context.totalItems, speed}});
    context.progressedBytes = total;
  }

  bool transferCheckpoint(TransferProgress* progress, std::uint64_t currentBytes = 0) {
    if (!progress || !progress->pause) return true;
    if (progress->boundary) progress->boundary(progress->id, progress->baseBytes + currentBytes);
    if (progress->pause->requested()) publishProgress(*progress, currentBytes, true);
    return progress->pause->checkpoint(progress->cancellable, progress->acknowledgePause);
  }

  std::string basename(const std::string& path) { return std::filesystem::path(path).filename().string(); }

  const char* operationName(FileOperationManager::Kind kind) {
    switch (kind) {
    case FileOperationManager::Kind::Copy: return "Copy";
    case FileOperationManager::Kind::Duplicate: return "Duplicate";
    case FileOperationManager::Kind::Move: return "Move";
    case FileOperationManager::Kind::Rename: return "Rename";
    case FileOperationManager::Kind::CreateFolder:
    case FileOperationManager::Kind::CreateFile: return "Create";
    case FileOperationManager::Kind::CreateSymlink: return "Create symlink";
    case FileOperationManager::Kind::Trash: return "Trash";
    case FileOperationManager::Kind::Delete: return "Permanent delete";
    case FileOperationManager::Kind::Restore:
      return "Restore";
    case FileOperationManager::Kind::EmptyWastebin:
      return "Empty Wastebin";
    case FileOperationManager::Kind::Properties:
      return "Properties";
    case Kind::Undo: return "Undo";
    case Kind::Redo: return "Redo";
    }
    return "File operation";
  }

  std::string quotedBasename(const std::string& path) {
    const std::string name = basename(path);
    return name.empty() ? "item" : "\"" + name + "\"";
  }

  std::string displayDestination(const std::string& path) {
    std::filesystem::path destination(path);
    if (const char* home = g_get_home_dir(); home != nullptr) {
      const std::filesystem::path homePath(home);
      std::error_code ec;
      const auto relative = std::filesystem::relative(destination, homePath, ec);
      if (!ec && !relative.empty() && *relative.begin() != "..") destination = relative;
      else if (!ec && relative == ".") return "Home";
    }
    std::string label = destination.string();
    constexpr std::size_t kMaxDestinationLabel = 72;
    if (label.size() > kMaxDestinationLabel)
      label = "…" + label.substr(label.size() - (kMaxDestinationLabel - 1));
    return label;
  }

  std::string completedDescription(const FileOperationManager::Operation& operation) {
    const auto kind = operation.kind;
    const auto count = isTransfer(kind)
        ? (operation.totalFiles > 0 ? operation.totalFiles : operation.requestedItemCount)
        : operation.requestedItemCount;
    const char* unit = isTransfer(kind)
        ? (operation.totalFiles > 0 ? "file" : "item") : "item";
    const std::string quantity = std::to_string(count) + " " + unit + (count == 1 ? "" : "s");
    switch (kind) {
    case FileOperationManager::Kind::Copy:
      return quantity + " copied to " + displayDestination(operation.destination);
    case Kind::Duplicate:
      return quantity + " duplicated in " + displayDestination(operation.destination);
    case FileOperationManager::Kind::Move:
      return quantity + " moved to " + displayDestination(operation.destination);
    case FileOperationManager::Kind::Rename:
      return "Renamed " + quotedBasename(operation.source) + " to " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::CreateFolder:
      return "Created folder " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::CreateFile:
      return "Created file " + quotedBasename(operation.destination);
    case Kind::CreateSymlink:
      return "Created symlink " + quotedBasename(operation.destination);
    case Kind::Undo:
    case Kind::Redo:
      return std::string(operationName(kind)) + " completed for " + quotedBasename(operation.destination);
    case FileOperationManager::Kind::Trash:
      return count == 1 ? "Moved " + quotedBasename(operation.source) + " to Trash"
                        : "Moved " + quantity + " to Trash";
    case FileOperationManager::Kind::Restore:
      return count == 1 ? "Restored " + quotedBasename(operation.source) : "Restored " + quantity;
    case FileOperationManager::Kind::EmptyWastebin:
      return "Wastebin emptied";
    case FileOperationManager::Kind::Properties:
      return operation.status;
    case FileOperationManager::Kind::Delete:
      return count == 1 ? "Permanently deleted " + quotedBasename(operation.source)
                        : "Permanently deleted " + quantity;
    }
    return "File operation completed";
  }

  std::string failedDescription(const FileOperationManager::Operation& operation, std::string detail) {
    if (detail.find("Cannot copy a directory into itself") != std::string::npos)
      return "Cannot copy " + quotedBasename(operation.source) + " into itself";
    const auto replacePath = [&detail](const std::string& path) {
      if (path.empty() || path.size() < 2) return;
      std::size_t offset = 0;
      const std::string replacement = quotedBasename(path);
      while ((offset = detail.find(path, offset)) != std::string::npos) {
        detail.replace(offset, path.size(), replacement);
        offset += replacement.size();
      }
    };
    replacePath(operation.source);
    replacePath(operation.destination);
    constexpr std::size_t kMaxErrorDescription = 240;
    if (detail.size() > kMaxErrorDescription) detail = detail.substr(0, kMaxErrorDescription - 1) + "…";
    return detail;
  }

  bool isSameOrDescendant(const std::filesystem::path& parent, const std::filesystem::path& child) {
    std::error_code ec;
    auto p = std::filesystem::weakly_canonical(parent, ec);
    if (ec) return true;
    auto c = std::filesystem::weakly_canonical(child, ec);
    if (ec) return true;
    auto pi = p.begin();
    auto ci = c.begin();
    for (; pi != p.end(); ++pi, ++ci)
      if (ci == c.end() || *pi != *ci) return false;
    return true;
  }

  bool removeTree(GFile* file, GCancellable* cancellable, GError** error, bool* mutated = nullptr) {
    if (cancellable != nullptr && g_cancellable_set_error_if_cancelled(cancellable, error)) return false;
    GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (info == nullptr) return false;
    const auto type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if (type == G_FILE_TYPE_DIRECTORY) {
      GFileEnumerator* entries = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME,
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
      if (entries == nullptr) return false;
      while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
        const char* name = g_file_info_get_name(child);
        GFile* childFile = g_file_get_child(file, name);
        const bool ok = removeTree(childFile, cancellable, error, mutated);
        g_object_unref(childFile);
        g_object_unref(child);
        if (!ok) { g_object_unref(entries); return false; }
      }
      if (error != nullptr && *error != nullptr) { g_object_unref(entries); return false; }
      g_object_unref(entries);
    }
    const bool removed = g_file_delete(file, cancellable, error);
    if (removed && mutated)
      *mutated = true;
    return removed;
  }

  bool queryTrashOriginalPath(GFile* file, GCancellable* cancellable, std::string& path, std::string& queryError) {
    GError* error = nullptr;
    GFileInfo* info =
        g_file_query_info(file, "trash::orig-path", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
    if (info == nullptr && error != nullptr)
      queryError = error->message;
    g_clear_error(&error);
    if (info == nullptr)
      return false;
    const char* original = g_file_info_get_attribute_byte_string(info, "trash::orig-path");
    if (original != nullptr && original[0] == '/')
      path = original;
    g_object_unref(info);
    return !path.empty();
  }

  bool gioOriginalPath(const std::string& uri, GCancellable* cancellable, std::string& path, std::string& message) {
    GFile* current = g_file_new_for_uri(uri.c_str());
    std::vector<std::string> suffix;
    std::string lastQueryError;
    GFile* trashRoot = g_file_new_for_uri("trash:///");
    while (current != nullptr) {
      path.clear();
      if (queryTrashOriginalPath(current, cancellable, path, lastQueryError)) {
        for (auto it = suffix.rbegin(); it != suffix.rend(); ++it)
          path = (std::filesystem::path(path) / *it).string();
        g_object_unref(current);
        g_object_unref(trashRoot);
        return true;
      }
      GFile* parent = g_file_get_parent(current);
      if (parent == nullptr)
        break;
      char* name = g_file_get_basename(current);
      if (name != nullptr)
        suffix.emplace_back(name);
      g_free(name);
      g_object_unref(current);
      current = parent;
      if (g_file_equal(current, trashRoot)) {
        g_object_unref(current);
        current = nullptr;
        break;
      }
    }
    if (current != nullptr)
      g_object_unref(current);
    g_object_unref(trashRoot);
    message =
        lastQueryError.empty() ? "Original location metadata is unavailable for this Wastebin item." : lastQueryError;
    return false;
  }

  bool gioEnumerateTrash(GCancellable* cancellable, std::vector<std::string>& uris, std::string& message) {
    GFile* root = g_file_new_for_uri("trash:///");
    GError* error = nullptr;
    GFileEnumerator* entries = g_file_enumerate_children(
        root, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error
    );
    if (entries == nullptr) {
      message = error == nullptr ? "Could not enumerate the Wastebin." : error->message;
      g_clear_error(&error);
      g_object_unref(root);
      return false;
    }
    while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, &error)) {
      GFile* item = g_file_get_child(root, g_file_info_get_name(child));
      char* uri = g_file_get_uri(item);
      if (uri != nullptr)
        uris.emplace_back(uri);
      g_free(uri);
      g_object_unref(item);
      g_object_unref(child);
    }
    const bool ok = error == nullptr;
    if (!ok)
      message = error->message;
    g_clear_error(&error);
    g_object_unref(entries);
    g_object_unref(root);
    return ok;
  }

  bool isTrashUri(const std::string& uri) {
    GFile* item = g_file_new_for_uri(uri.c_str());
    char* resolvedUri = g_file_get_uri(item);
    const bool valid = resolvedUri != nullptr && g_str_has_prefix(resolvedUri, "trash:");
    g_free(resolvedUri);
    g_object_unref(item);
    return valid;
  }

  bool isDirectTrashChild(const std::string& uri) {
    GFile* item = g_file_new_for_uri(uri.c_str());
    GFile* parent = g_file_get_parent(item);
    GFile* root = g_file_new_for_uri("trash:///");
    const bool direct = parent != nullptr && g_file_equal(parent, root);
    if (parent != nullptr)
      g_object_unref(parent);
    g_object_unref(root);
    g_object_unref(item);
    return direct;
  }

  bool copyTree(GFile* source, GFile* destination, GCancellable* cancellable, GError** error, TransferProgress* progress,
      bool preserveDirectoryMetadata = false
  );

  bool gioMoveWithBackup(
      GFile* source, GFile* target, bool replace, GCancellable* cancellable, std::string& message, bool& mutated
  ) {
    GError* error = nullptr;
    GFile* backup = nullptr;
    bool backupMoved = false;
    bool preserveBackup = false;
    bool ok = true;
    const GFileCopyFlags moveFlags =
        static_cast<GFileCopyFlags>(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE);
    if (replace) {
      GFile* parent = g_file_get_parent(target);
      for (unsigned attempt = 0; attempt < 100; ++attempt) {
        char* name = g_strdup_printf(".nythera-backup-%u", static_cast<unsigned>(g_random_int()));
        backup = g_file_get_child(parent, name);
        g_free(name);
        if (!g_file_query_exists(backup, cancellable))
          break;
        g_object_unref(backup);
        backup = nullptr;
      }
      g_object_unref(parent);
      if (backup == nullptr) {
        g_set_error_literal(
            &error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a safe replacement backup name."
        );
        ok = false;
      } else {
        ok = g_file_move(target, backup, moveFlags, cancellable, nullptr, nullptr, &error);
        backupMoved = ok;
        mutated |= ok;
      }
    }
    if (ok) {
      ok = g_file_move(source, target, moveFlags, cancellable, nullptr, nullptr, &error);
      mutated |= ok;
      if (!ok
          && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
              || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE)
              || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE))) {
        g_clear_error(&error);
        GFile* parent = g_file_get_parent(target);
        char* uuid = g_uuid_string_random();
        char* leaf = g_strdup_printf(".nythera-restore-%s", uuid);
        GFile* staged = g_file_get_child(parent, leaf);
        g_free(leaf);
        g_free(uuid);
        g_object_unref(parent);
        const bool absent = !g_file_query_exists(staged, cancellable);
        if (!absent) {
          g_set_error_literal(
              &error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary restore destination."
          );
        } else if (!copyTree(source, staged, cancellable, &error, nullptr, true)) {
          // Preserve the source when staging fails.
        } else if (!g_file_move(staged, target, moveFlags, cancellable, nullptr, nullptr, &error)) {
          // Target remains absent (or is protected by the backup) on failure.
        } else {
          mutated = true;
          // Production sources are trash:// GFiles, so recursive semantics
          // remain owned by that backend. Local recursion is limited to the
          // explicit isolated provider seam used by tests.
          ok = g_file_has_uri_scheme(source, "trash") ? g_file_delete(source, nullptr, &error)
                                                      : removeTree(source, nullptr, &error);
          if (!ok && error == nullptr)
            g_set_error_literal(
                &error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Restored a copy but could not remove the original Wastebin item."
            );
          if (!ok) {
            preserveBackup = backupMoved;
            char* targetPath = g_file_get_path(target);
            char* backupPath = backupMoved ? g_file_get_path(backup) : nullptr;
            message = std::string("A restored copy remains at ")
                + (targetPath == nullptr ? "the destination" : targetPath)
                + "; the original could not be removed from Wastebin"
                + (backupPath == nullptr ? std::string{}
                                         : "; previous destination backup remains at " + std::string(backupPath));
            g_free(targetPath);
            g_free(backupPath);
          }
        }
        if (!ok && absent && g_file_query_exists(staged, nullptr)) {
          GError* cleanupError = nullptr;
          removeTree(staged, nullptr, &cleanupError);
          g_clear_error(&cleanupError);
        }
        g_object_unref(staged);
      }
    }
    if (!ok && backupMoved && !preserveBackup) {
      GError* restoreError = nullptr;
      if (!g_file_move(backup, target, moveFlags, nullptr, nullptr, nullptr, &restoreError)) {
        char* backupPath = g_file_get_path(backup);
        message = std::string(error == nullptr ? "Restore failed" : error->message)
            + "; previous destination remains at "
            + (backupPath == nullptr ? "the backup file" : backupPath);
        g_free(backupPath);
        g_clear_error(&restoreError);
      }
    }
    if (!ok && message.empty())
      message = error == nullptr ? "Could not restore Wastebin item." : error->message;
    g_clear_error(&error);
    if (ok && backup != nullptr) {
      GError* cleanupError = nullptr;
      if (!removeTree(backup, nullptr, &cleanupError)) {
        message = std::string("Restored item, but could not remove the replaced destination backup: ")
            + (cleanupError == nullptr ? "unknown error" : cleanupError->message);
        ok = false;
      }
      g_clear_error(&cleanupError);
    }
    if (backup != nullptr)
      g_object_unref(backup);
    return ok;
  }

  bool copyTree(
      GFile* source, GFile* destination, GCancellable* cancellable, GError** error, TransferProgress* progress,
      bool preserveDirectoryMetadata
  ) {
    if (!transferCheckpoint(progress) || (cancellable != nullptr && g_cancellable_is_cancelled(cancellable))) {
      g_cancellable_set_error_if_cancelled(cancellable, error);
      return false;
    }
    GFileInfo* info = g_file_query_info(source, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (info == nullptr) return false;
    const auto type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if (type != G_FILE_TYPE_DIRECTORY) {
      if (progress != nullptr) progress->baseBytes = progress->progressedBytes;
      const bool copied = g_file_copy(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable,
          progress == nullptr ? nullptr : [](goffset current, goffset, gpointer data) {
            auto* context = static_cast<TransferProgress*>(data);
            const auto bytes = static_cast<std::uint64_t>(std::max<goffset>(0, current));
            if (transferCheckpoint(context, bytes)) publishProgress(*context, bytes);
          }, progress, error);
      return copied;
    }
    if (!g_file_make_directory(destination, cancellable, error)) return false;
    GFileEnumerator* entries = g_file_enumerate_children(source,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error);
    if (entries == nullptr) return false;
    while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, error)) {
      const char* name = g_file_info_get_name(child);
      GFile* from = g_file_get_child(source, name);
      GFile* to = g_file_get_child(destination, name);
      const bool ok = copyTree(from, to, cancellable, error, progress, preserveDirectoryMetadata);
      g_object_unref(from); g_object_unref(to); g_object_unref(child);
      if (!ok) { g_object_unref(entries); return false; }
    }
    if (error != nullptr && *error != nullptr) { g_object_unref(entries); return false; }
    g_object_unref(entries);
    if (preserveDirectoryMetadata) {
      // Directory size is not a writable attribute. Copy only the relevant
      // mode and timestamps after children have finished changing the directory.
      GFileInfo* metadata = g_file_query_info(
          source, "unix::mode,time::modified,time::modified-usec,time::access,time::access-usec",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
      );
      if (metadata == nullptr)
        return false;
      const bool copied = g_file_set_attributes_from_info(
          destination, metadata, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, error
      );
      g_object_unref(metadata);
      return copied;
    }
    return true;
  }

  using FileIdentity = std::pair<std::uint64_t, std::uint64_t>;

  FileIdentity parentIdentity(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    struct stat state{};
    if (parent.empty() || stat(parent.c_str(), &state) != 0 || !S_ISDIR(state.st_mode))
      return {};
    return {state.st_dev, state.st_ino};
  }

  // Replay is keyed to the recorded directory as well as the entry. Pin it
  // before validation so a concurrent rename or retargeted parent symlink
  // cannot redirect a later filesystem call to a different directory.
  struct ReplayParent {
    int fd = -1;
    std::filesystem::path path;
    explicit ReplayParent(const std::string& entry, FileIdentity expected) {
      if (expected == FileIdentity{})
        return;
      const auto directory = std::filesystem::path(entry).parent_path();
      fd = open(directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
      if (fd < 0)
        return;
      struct stat state{};
      if (fstat(fd, &state) == 0 && FileIdentity{state.st_dev, state.st_ino} == expected)
        path = "/proc/self/fd/" + std::to_string(fd);
    }
    ReplayParent(const ReplayParent&) = delete;
    ReplayParent& operator=(const ReplayParent&) = delete;
    ~ReplayParent() {
      if (fd >= 0)
        close(fd);
    }
  };

  bool renameWithoutReplace(const std::string& source, const std::string& destination, GError** error) {
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0)
      return true;
    const int savedError = errno;
    g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(savedError), g_strerror(savedError));
    return false;
  }

  bool reject(GError** error, const char* message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
    return false;
  }

  bool changedEntry(GError** error, const char* message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, message);
    return false;
  }

  bool emptyDirectory(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_empty(path, error) && !error;
  }

  bool createItem(Kind kind, GFile* target, const std::string& link, GCancellable* cancellable,
                  GError** error, bool& created) {
    if (kind == Kind::CreateSymlink)
      return created = g_file_make_symbolic_link(target, link.c_str(), cancellable, error);
    if (kind == Kind::CreateFolder)
      return created = g_file_make_directory(target, cancellable, error);
    GFileOutputStream* stream = g_file_create(target, G_FILE_CREATE_NONE, cancellable, error);
    if (!stream)
      return false;
    created = true;
    const bool closed = g_output_stream_close(G_OUTPUT_STREAM(stream), cancellable, error);
    g_object_unref(stream);
    return closed;
  }

  struct PrivateStaging {
    std::string directory;
    explicit PrivateStaging(const std::string& sibling)
        : directory((std::filesystem::path(sibling).parent_path() / ".nythera-stage-XXXXXX").string()) {
      if (!g_mkdtemp(directory.data()))
        directory.clear();
    }
    ~PrivateStaging() {
      if (!directory.empty())
        rmdir(directory.c_str());
    }
  };
} // namespace

struct FileOperationManager::Result {
  bool ok = false;
  bool cancelled = false;
  bool mutated = false;
  bool historyBlocked = false;
  std::string message;
  std::string customTitle;
  std::string customDescription;
  std::optional<std::size_t> requestedCount;
  filemanager::PropertyResult metadata;
  std::vector<CompletedEntry> completedEntries;
  std::vector<HistoryEntry> history;
};

struct FileOperationManager::Task {
  FileOperationManager* owner;
  std::uint64_t id;
  Kind kind;
  std::vector<std::string> sources;
  std::string destination;
  std::vector<TransferMeasure> measures;
  TransferMeasure totalMeasure;
  TransferMeasure completedMeasure;
  std::optional<filemanager::PropertyRequest> metadata;
  std::uint32_t uid = 0;
  std::function<void(const filemanager::PropertyResult&)> metadataCompleted;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> renameIdentity;
  std::function<void(bool, const std::string&)> renameCompleted;
  std::shared_ptr<FileTransferPauseState> pause;
  std::shared_ptr<bool> alive;
  std::function<void(std::uint64_t, std::uint64_t)> boundary;
  bool keepBoth = false;
  std::optional<HistoryEntry> replayEntry;
};

std::optional<FileOperationManager::Revision> FileOperationManager::revision(const std::string& path) {
  struct stat state{};
  if (lstat(path.c_str(), &state) != 0)
    return std::nullopt;
  return Revision{
      state.st_dev, state.st_ino, state.st_nlink, state.st_mode, state.st_size,
      {state.st_mtim.tv_sec, state.st_mtim.tv_nsec}, {state.st_ctim.tv_sec, state.st_ctim.tv_nsec}
  };
}

bool FileOperationManager::replay(HistoryEntry& entry, bool undoing, GCancellable* cancellable, GError** error) {
  const bool moving = entry.kind == Kind::Move || entry.kind == Kind::Rename;
  const bool copying = isCopy(entry.kind);
  const bool readsSource = moving || copying;
  if (g_cancellable_set_error_if_cancelled(cancellable, error))
    return false;
  const ReplayParent destinationParent(entry.destination, entry.destinationParent);
  const ReplayParent sourceParent(entry.source, readsSource ? entry.sourceParent : FileIdentity{});
  if (destinationParent.path.empty() || (readsSource && sourceParent.path.empty()))
    return reject(error, "An original folder is missing or was replaced.");
  const auto destinationPath = (destinationParent.path / basename(entry.destination)).string();
  const auto sourcePath = readsSource ? (sourceParent.path / basename(entry.source)).string() : std::string{};

  // Moving updates ctime. Require the entire fingerprint before moving and
  // recheck everything except that timestamp after the no-overwrite rename.
  const auto afterMove = [](std::optional<Revision> actual, Revision expected) {
    if (actual)
      expected.changed = actual->changed;
    return actual == expected;
  };
  const auto expectedAt = [](const std::string& path, const Revision& expected) {
    return revision(path) == expected && (!S_ISDIR(expected.mode) || emptyDirectory(path));
  };
  const auto restoreMovedItem = [&](const std::string& from, const std::string& to, const std::string& location) {
    GError* recoveryError = nullptr;
    if (renameWithoutReplace(from, to, &recoveryError))
      return changedEntry(error, "The item changed during the operation and was put back.");
    g_clear_error(&recoveryError);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, "The item could not be restored and remains at %s.", location.c_str());
    return false;
  };
  if (moving) {
    const auto& from = undoing ? destinationPath : sourcePath;
    const auto& to = undoing ? sourcePath : destinationPath;
    const auto& expected = undoing ? entry.resultRevision : *entry.sourceRevision;
    if (!expectedAt(from, expected))
      return changedEntry(error, "The item is missing, replaced, or has been modified.");
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
      return false;
    if (!renameWithoutReplace(from, to, error))
      return false;
    const auto actual = revision(to);
    if (!afterMove(actual, expected) || (S_ISDIR(expected.mode) && !emptyDirectory(to)))
      return restoreMovedItem(to, from, undoing ? entry.source : entry.destination);
    if (undoing)
      entry.sourceRevision = actual;
    else
      entry.resultRevision = *actual;
    return true;
  }

  if (undoing && !expectedAt(destinationPath, entry.resultRevision))
    return changedEntry(error, "The result is missing, replaced, or has been modified.");
  if (copying && !expectedAt(sourcePath, *entry.sourceRevision))
    return changedEntry(error, "The original source is missing, replaced, or has been modified.");

  // Isolate an output before deleting it. If the directory entry changed
  // between validation and rename, recover it without overwriting anything.
  // The private directory also gives redo a no-overwrite publication point.
  PrivateStaging staging(destinationPath);
  if (staging.directory.empty())
    return reject(error, "Could not create a private staging directory beside the destination.");
  const std::string staged = staging.directory + "/item";
  if (undoing) {
    const auto recovery = (std::filesystem::path(entry.destination).parent_path() / basename(staging.directory) / "item").string();
    if (g_cancellable_set_error_if_cancelled(cancellable, error)
        || !renameWithoutReplace(destinationPath, staged, error))
      return false;
    const auto actual = revision(staged);
    if (!afterMove(actual, entry.resultRevision) || (copying && !expectedAt(sourcePath, *entry.sourceRevision)))
      return restoreMovedItem(staged, destinationPath, recovery);
    const int removed = S_ISDIR(entry.resultRevision.mode) ? rmdir(staged.c_str()) : unlink(staged.c_str());
    return removed == 0 || restoreMovedItem(staged, destinationPath, recovery);
  }

  GFile* target = g_file_new_for_path(staged.c_str());
  bool ok = false;
  if (copying) {
    GFile* source = g_file_new_for_path(sourcePath.c_str());
    ok = copyTree(source, target, cancellable, error, nullptr);
    g_object_unref(source);
    if (ok && !expectedAt(sourcePath, *entry.sourceRevision))
      ok = changedEntry(error, "The source changed during copying.");
  } else {
    bool created = false;
    ok = createItem(entry.kind, target, entry.linkTarget, cancellable, error, created);
  }
  if (ok) {
    const auto created = revision(staged);
    if (!created || created->mode != entry.resultRevision.mode)
      ok = reject(error, "The recreated item's permissions differ from the original result.");
    else
      ok = !g_cancellable_set_error_if_cancelled(cancellable, error)
          && renameWithoutReplace(staged, destinationPath, error);
    if (ok) {
      const auto actual = revision(destinationPath);
      entry.resultRevision = afterMove(actual, *created) ? *actual : Revision{};
    }
  }
  if (!ok)
    g_file_delete(target, nullptr, nullptr);
  g_object_unref(target);
  return ok;
}

FileOperationManager::FileOperationManager(Changed changed, WastebinBackend wastebinBackend) : m_changed(std::move(changed)), m_wastebinBackend(std::move(wastebinBackend)) {
  GError* error = nullptr;
  m_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  g_clear_error(&error);
  if (m_bus != nullptr)
    m_serviceWatch = g_bus_watch_name_on_connection(m_bus, "org.kde.JobViewServer", G_BUS_NAME_WATCHER_FLAGS_NONE,
        serviceAppeared, serviceVanished, this, nullptr);
}
FileOperationManager::~FileOperationManager() {
  *m_alive = false;
  if (m_rateTimer) g_source_remove(m_rateTimer);
  for (const auto& [id, cancellable] : m_cancellables) { (void)id; g_cancellable_cancel(cancellable); }
  for (const auto& [id, pause] : m_pauseStates) { (void)id; pause->cancel(); }
  for (const auto& [id, timer] : m_jobRevealTimers) { (void)id; g_source_remove(timer); }
  if (m_serviceWatch != 0) g_bus_unwatch_name(m_serviceWatch);
  for (const auto& [id, subscription] : m_signalSubscriptions) { (void)id; g_dbus_connection_signal_unsubscribe(m_bus, subscription); }
  if (m_bus != nullptr) g_object_unref(m_bus);
  for (auto& [id, cancellable] : m_cancellables) { (void)id; g_cancellable_cancel(cancellable); g_object_unref(cancellable); }
}
bool FileOperationManager::active() const noexcept {
  return std::ranges::any_of(m_operations, [](const Operation& op) { return !op.finished; });
}
bool FileOperationManager::usesMount(const std::filesystem::path& mountPath) const {
  if (mountPath.empty()) return active();
  // Test path components, never string prefixes ("/media/a" is not "/media/ab").
  // Keep both lexical and canonical paths so symlink aliases and paths removed
  // during a move remain protected. Ancestors also cover recursive operations
  // whose directory trees contain another mount.
  auto contains = [](const auto& parent, const auto& child) {
    auto p = parent.begin();
    auto c = child.begin();
    for (; p != parent.end(); ++p, ++c)
      if (c == child.end() || *p != *c) return false;
    return true;
  };
  std::error_code error;
  const auto lexical = std::filesystem::absolute(mountPath, error).lexically_normal();
  if (error) return active();
  const auto canonical = std::filesystem::weakly_canonical(lexical, error);
  for (const auto& [id, paths] : m_activePaths) {
    (void)id;
    for (const auto& path : paths) {
      if (path.empty() || contains(lexical, path) || contains(path, lexical)) return true;
      if (!error && (contains(canonical, path) || contains(path, canonical))) return true;
    }
  }
  return false;
}
bool FileOperationManager::hasJobService() const noexcept {
  if (!m_serviceAvailable) return false;
  return std::ranges::all_of(m_operations, [this](const Operation& operation) {
    return operation.finished || !reportsProgress(operation.kind) || !operation.progressRevealed
        || m_jobPaths.contains(operation.id);
  });
}
FileOperationManager::Operation* FileOperationManager::find(std::uint64_t id) {
  auto it = std::ranges::find(m_operations, id, &Operation::id);
  return it == m_operations.end() ? nullptr : &*it;
}
void FileOperationManager::update(std::uint64_t id, double fraction, std::string status) {
  if (auto* op = find(id); op && !op->finished) {
    op->fraction = fraction;
    op->status = std::move(status);
    publishJob(*op);
    if (m_changed)
      m_changed(*op);
  }
}
bool FileOperationManager::updateTransferMetrics(Operation& operation) {
  const auto found = m_transferRates.find(operation.id);
  if (found == m_transferRates.end()) return false;
  found->second.observe(std::chrono::steady_clock::now(), operation.processedBytes, operation.paused);
  const auto display = found->second.displaySpeed();
  const auto eta = found->second.etaSpeed();
  const bool changed = display != operation.displaySpeedBytesPerSecond || eta != operation.etaBytesPerSecond;
  operation.displaySpeedBytesPerSecond = display;
  operation.etaBytesPerSecond = eta;
  if (operation.totalBytes > 0) {
    char* processed = g_format_size(operation.processedBytes);
    char* total = g_format_size(operation.totalBytes);
    char* speed = g_format_size(display);
    operation.status = std::string(processed) + " / " + total + " · " + std::to_string(operation.processedFiles) + " / "
        + std::to_string(operation.totalFiles) + " files" + (display > 0 ? std::string(" · ") + speed + "/s" : std::string{});
    g_free(processed); g_free(total); g_free(speed);
  }
  return changed;
}

void FileOperationManager::updateProgress(std::uint64_t id, std::uint64_t bytes, std::uint64_t totalBytes,
    std::uint64_t files, std::uint64_t totalFiles, std::uint64_t speed) {
  if (auto* op = find(id); op && !op->finished) {
    op->processedBytes = bytes;
    op->totalBytes = totalBytes;
    op->processedFiles = files;
    op->totalFiles = totalFiles;
    op->speedBytesPerSecond = speed;
    if (totalBytes > 0) op->fraction = std::clamp(static_cast<double>(bytes) / static_cast<double>(totalBytes), 0.0, 1.0);
    else if (totalFiles > 0) op->fraction = std::clamp(static_cast<double>(files) / static_cast<double>(totalFiles), 0.0, 1.0);
    if (totalBytes > 0) {
      updateTransferMetrics(*op);
    } else op->status = std::to_string(files) + " / " + std::to_string(totalFiles) + " items";
    publishJob(*op);
    if (m_changed) m_changed(*op);
  }
}
void FileOperationManager::cancel(std::uint64_t id) {
  auto it = std::ranges::find(m_cancellables, id, &std::pair<std::uint64_t, GCancellable*>::first);
  if (it != m_cancellables.end()) g_cancellable_cancel(it->second);
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end()) state->second->cancel();
}
void FileOperationManager::pause(std::uint64_t id) {
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end()) state->second->pause();
}
void FileOperationManager::resume(std::uint64_t id) {
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end()) state->second->resume();
}
void FileOperationManager::updatePaused(std::uint64_t id, bool paused) {
  if (auto* operation = find(id); operation && !operation->finished) {
    operation->paused = paused;
    updateTransferMetrics(*operation);
    publishJob(*operation, true, true);
    if (m_changed) m_changed(*operation);
  }
}

std::uint64_t FileOperationManager::copy(std::vector<std::string> sources, std::string destination) {
  return start(Kind::Copy, std::move(sources), std::move(destination));
}
std::uint64_t FileOperationManager::duplicate(std::vector<std::string> sources, std::string destination) {
  return start(Kind::Duplicate, std::move(sources), std::move(destination));
}
std::uint64_t FileOperationManager::move(std::vector<std::string> sources, std::string destination) {
  return start(Kind::Move, std::move(sources), std::move(destination));
}
std::uint64_t FileOperationManager::rename(std::string source, std::string destination) {
  return start(Kind::Rename, {std::move(source)}, std::move(destination));
}
std::uint64_t FileOperationManager::renameChecked(
    std::string source, std::string destination, std::uint64_t device, std::uint64_t inode,
    std::function<void(bool, const std::string&)> completed
) {
  return start(
      Kind::Rename, {std::move(source)}, std::move(destination), {}, 0, {}, std::pair{device, inode},
      std::move(completed)
  );
}

std::uint64_t FileOperationManager::createFolder(std::string destination) { return start(Kind::CreateFolder, {}, std::move(destination)); }
std::uint64_t FileOperationManager::createFile(std::string destination) { return start(Kind::CreateFile, {}, std::move(destination)); }
std::uint64_t FileOperationManager::createSymlink(std::string target, std::string destination, bool keepBoth) {
  return start(Kind::CreateSymlink, {std::move(target)}, std::move(destination), {}, 0, {}, {}, {}, keepBoth);
}

bool FileOperationManager::canUndo() const noexcept {
  return !m_replaying && !active() && !m_undo.empty() && !m_undo.back().blocked;
}
bool FileOperationManager::canRedo() const noexcept {
  return !m_replaying && !active() && !m_redo.empty() && !m_redo.back().blocked;
}

std::string FileOperationManager::undoLabel() const {
  if (m_undo.empty() || m_undo.back().blocked)
    return "Undo";
  const auto& entry = m_undo.back();
  return std::string("Undo ") + operationName(entry.kind) + " " + quotedBasename(entry.destination);
}

std::string FileOperationManager::redoLabel() const {
  if (m_redo.empty() || m_redo.back().blocked)
    return "Redo";
  const auto& entry = m_redo.back();
  return std::string("Redo ") + operationName(entry.kind) + " " + quotedBasename(entry.destination);
}

std::uint64_t FileOperationManager::undo() {
  if (!canUndo())
    return 0;
  return start(Kind::Undo, {m_undo.back().source}, m_undo.back().destination);
}

std::uint64_t FileOperationManager::redo() {
  if (!canRedo())
    return 0;
  return start(Kind::Redo, {m_redo.back().source}, m_redo.back().destination);
}
std::uint64_t FileOperationManager::trash(std::vector<std::string> sources) { return start(Kind::Trash, std::move(sources), {}); }
std::uint64_t FileOperationManager::restore(std::vector<std::string> trashUris, std::string destinationDirectory) {
  return start(Kind::Restore, std::move(trashUris), std::move(destinationDirectory));
}
std::uint64_t FileOperationManager::emptyWastebin(bool confirmed) {
  if (!confirmed)
    return 0;
  return start(Kind::EmptyWastebin, {}, {});
}
std::uint64_t FileOperationManager::removePermanently(std::vector<std::string> sources) { return start(Kind::Delete, std::move(sources), {}); }

std::uint64_t FileOperationManager::modifyProperties(
    filemanager::PropertyRequest request, std::uint32_t uid,
    std::function<void(const filemanager::PropertyResult&)> completed
) {
  const auto identity = std::pair{request.device, request.inode};
  for (const auto& [id, target] : m_metadataTargets) {
    (void)id;
    if (target == identity)
      return 0;
  }
  const auto path = request.path.string();
  const auto id = start(Kind::Properties, {path}, {}, request, uid, std::move(completed));
  if (id)
    m_metadataTargets[id] = identity;
  return id;
}

std::uint64_t FileOperationManager::start(
    Kind kind, std::vector<std::string> sources, std::string destination,
    std::optional<filemanager::PropertyRequest> metadata, std::uint32_t uid,
    std::function<void(const filemanager::PropertyResult&)> completedCallback,
    std::optional<std::pair<std::uint64_t, std::uint64_t>> renameIdentity,
    std::function<void(bool, const std::string&)> renameCompleted, bool keepBoth
) {
  if (m_replaying)
    return 0;
  m_replaying = isReplay(kind);
  std::optional<HistoryEntry> replayEntry;
  if (m_replaying)
    replayEntry = (kind == Kind::Undo ? m_undo : m_redo).back();
  const auto id = m_nextId++;
  Operation operation{.id=id, .kind=kind, .source=sources.empty() ? std::string{} : sources.front(), .destination=destination,
      .status="Queued", .requestedItemCount=std::max<std::size_t>(1, sources.size())};
  if (kind == Kind::Properties)
    operation.status = "Preparing property changes / authorisation…";
  m_operations.push_back(operation);
  auto& activePaths = m_activePaths[id];
  auto trackPath = [&](const std::string& rawPath) {
    if (rawPath.empty()) return;
    std::error_code error;
    const auto lexical = std::filesystem::absolute(rawPath, error).lexically_normal();
    if (error) { activePaths.emplace_back(); return; }
    activePaths.push_back(lexical);
    const auto canonical = std::filesystem::weakly_canonical(lexical, error);
    if (!error && canonical != lexical) activePaths.push_back(canonical);
  };
  if (kind != Kind::Restore && kind != Kind::CreateSymlink)
    for (const auto& source : sources) trackPath(source);
  trackPath(destination);
  if (kind == Kind::Restore || kind == Kind::EmptyWastebin)
    activePaths.emplace_back(); // Trash locations are provider-owned; protect conservatively during the worker.
  if (reportsProgress(kind)) {
    const auto timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 450, [](gpointer data) -> gboolean {
      auto* request = static_cast<std::pair<FileOperationManager*, std::uint64_t>*>(data);
      auto* self = request->first;
      const auto operationId = request->second;
      self->m_jobRevealTimers.erase(operationId);
      if (auto* activeOperation = self->find(operationId); activeOperation != nullptr && !activeOperation->finished) {
        activeOperation->progressRevealed = true;
        self->registerJob(*activeOperation);
        self->publishJob(*activeOperation);
        if (self->m_changed) self->m_changed(*activeOperation);
      }
      delete request;
      return G_SOURCE_REMOVE;
    }, new std::pair<FileOperationManager*, std::uint64_t>{this, id}, nullptr);
    m_jobRevealTimers.emplace(id, timer);
  }
  if (m_changed) m_changed(m_operations.back());
  auto* taskData = new Task{this, id, kind, std::move(sources), std::move(destination)};
  taskData->keepBoth = keepBoth;
  taskData->replayEntry = std::move(replayEntry);
  GCancellable* cancellable = g_cancellable_new();
  if (isTransfer(kind)) {
    taskData->pause = std::make_shared<FileTransferPauseState>();
    taskData->alive = m_alive;
    taskData->boundary = m_transferBoundary;
    m_pauseStates.emplace(id, taskData->pause);
    m_transferRates.emplace(id, filemanager::TransferRates{});
    if (!m_rateTimer) m_rateTimer = g_timeout_add(100, [](gpointer value) -> gboolean {
      auto* self = static_cast<FileOperationManager*>(value);
      bool active = false;
      for (auto& activeTransfer : self->m_operations) {
        if (activeTransfer.finished || !self->m_transferRates.contains(activeTransfer.id)) continue;
        active = true;
        if (activeTransfer.totalBytes > 0 && self->updateTransferMetrics(activeTransfer)) {
          self->publishJob(activeTransfer, true);
          if (self->m_changed) self->m_changed(activeTransfer);
        }
      }
      if (!active) self->m_rateTimer = 0;
      return active ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }, this);
  }
  m_cancellables.emplace_back(id, G_CANCELLABLE(g_object_ref(cancellable)));
  taskData->renameIdentity = renameIdentity;
  taskData->renameCompleted = std::move(renameCompleted);
  taskData->metadata = std::move(metadata);
  taskData->uid = uid;
  taskData->metadataCompleted = std::move(completedCallback);
  GTask* task = g_task_new(nullptr, cancellable, completed, this);
  // Keep commit records when cancellation arrives after a filesystem effect.
  g_task_set_check_cancellable(task, false);
  // Completion releases mount protection. Cancellation must not dispatch it
  // early while the worker is still unwinding or cleaning staged copies.
  g_task_set_return_on_cancel(task, false);
  g_task_set_task_data(task, taskData, [](gpointer data) { delete static_cast<Task*>(data); });
  g_task_run_in_thread(task, worker);
  g_object_unref(task); g_object_unref(cancellable);
  return id;
}

void FileOperationManager::serviceAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  self->m_serviceAvailable = true;
  for (const auto& operation : self->m_operations)
    if (!operation.finished && reportsProgress(operation.kind)
        && !self->m_jobRevealTimers.contains(operation.id)) {
      self->registerJob(operation);
      self->publishJob(operation);
      if (self->m_changed)
        self->m_changed(operation);
    }
}
void FileOperationManager::serviceVanished(GDBusConnection*, const gchar*, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  self->m_serviceAvailable = false;
  for (const auto& [id, subscription] : self->m_signalSubscriptions) {
    (void)id;
    g_dbus_connection_signal_unsubscribe(self->m_bus, subscription);
  }
  self->m_signalSubscriptions.clear();
  self->m_jobPaths.clear();
  for (const auto& operation : self->m_operations)
    if (!operation.finished && self->m_changed) self->m_changed(operation);
}
void FileOperationManager::jobSignal(GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar* signal, GVariant*, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  for (const auto& [id, jobPath] : self->m_jobPaths)
    if (jobPath == path) {
      if (g_str_equal(signal, "cancelRequested")) self->cancel(id);
      else if (g_str_equal(signal, "suspendRequested")) self->pause(id);
      else if (g_str_equal(signal, "resumeRequested")) self->resume(id);
      return;
    }
}
void FileOperationManager::registerJob(const Operation& operation) {
  if (m_bus == nullptr || !m_serviceAvailable || m_jobPaths.contains(operation.id)) return;
  GVariantBuilder hints;
  g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&hints, "{sv}", "title", g_variant_new_string("File operation"));
  GError* error = nullptr;
  GVariant* response = g_dbus_connection_call_sync(m_bus, "org.kde.JobViewServer", "/JobViewServer",
      "org.kde.JobViewServerV2", "requestView",
      g_variant_new("(si@a{sv})", "org.nythera.FileManager",
          isTransfer(operation.kind) ? 3 : 1, g_variant_builder_end(&hints)),
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error);
  if (response == nullptr) { g_clear_error(&error); return; }
  const gchar* objectPath = nullptr;
  g_variant_get(response, "(&o)", &objectPath);
  const std::string ownedPath(objectPath);
  g_variant_unref(response);
  m_jobPaths.emplace(operation.id, ownedPath);
  const guint sub = g_dbus_connection_signal_subscribe(m_bus, "org.kde.JobViewServer", "org.kde.JobViewV3",
      nullptr, ownedPath.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, jobSignal, this, nullptr);
  m_signalSubscriptions.emplace(operation.id, sub);
  if (operation.paused) publishJob(operation, true, true);
}
void FileOperationManager::publishJob(const Operation& operation, bool metricsOnly, bool suspensionAcknowledgement) {
  const auto path = m_jobPaths.find(operation.id);
  if (m_bus == nullptr || !m_serviceAvailable || path == m_jobPaths.end()) return;
  GVariantBuilder properties;
  g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
  if (m_transferRates.contains(operation.id)) {
    g_variant_builder_add(&properties, "{sv}", "displaySpeed", g_variant_new_uint64(operation.displaySpeedBytesPerSecond));
    g_variant_builder_add(&properties, "{sv}", "etaSpeed", g_variant_new_uint64(operation.etaBytesPerSecond));
  }
  if (suspensionAcknowledgement)
    g_variant_builder_add(&properties, "{sv}", "suspended", g_variant_new_boolean(operation.paused));
  if (!metricsOnly) {
  const char* title = operation.finished   ? operation.resultTitle.c_str()
      : operation.kind == Kind::Duplicate  ? "Duplicating items"
      : operation.kind == Kind::Copy       ? "Copying files"
      : operation.kind == Kind::Move       ? "Moving files"
      : operation.kind == Kind::Rename     ? "Renaming item"
      : operation.kind == Kind::Trash      ? "Moving to Trash"
      : operation.kind == Kind::Delete     ? "Deleting items"
      : operation.kind == Kind::Restore       ? "Restoring items"
      : operation.kind == Kind::EmptyWastebin ? "Emptying Wastebin"
      : operation.kind == Kind::Properties ? "Changing properties"
      : operation.kind == Kind::Undo       ? "Undoing file operation"
      : operation.kind == Kind::Redo       ? "Redoing file operation"
                                           : "Creating item";
  g_variant_builder_add(&properties, "{sv}", "title", g_variant_new_string(title));
  g_variant_builder_add(&properties, "{sv}", "infoMessage", g_variant_new_string(operation.finished
      ? operation.resultDescription.c_str() : operation.status.c_str()));
  g_variant_builder_add(&properties, "{sv}", "percent", g_variant_new_uint32(static_cast<guint32>(operation.fraction * 100.0)));
  if (operation.totalBytes > 0) {
    g_variant_builder_add(&properties, "{sv}", "processedBytes", g_variant_new_uint64(operation.processedBytes));
    g_variant_builder_add(&properties, "{sv}", "totalBytes", g_variant_new_uint64(operation.totalBytes));
    g_variant_builder_add(&properties, "{sv}", "speed", g_variant_new_uint64(operation.speedBytesPerSecond));
  }
  if (operation.totalFiles > 0) {
    g_variant_builder_add(&properties, "{sv}", "processedFiles", g_variant_new_uint64(operation.processedFiles));
    g_variant_builder_add(&properties, "{sv}", "totalFiles", g_variant_new_uint64(operation.totalFiles));
    g_variant_builder_add(&properties, "{sv}", "processedItems", g_variant_new_uint64(operation.processedFiles));
    g_variant_builder_add(&properties, "{sv}", "totalItems", g_variant_new_uint64(operation.totalFiles));
  }
  if (!operation.destination.empty()) {
    char* uri = g_filename_to_uri(operation.destination.c_str(), nullptr, nullptr);
    if (uri != nullptr) { g_variant_builder_add(&properties, "{sv}", "destUrl", g_variant_new_string(uri)); g_free(uri); }
  }
  } else {
    g_variant_builder_add(&properties, "{sv}", "infoMessage", g_variant_new_string(operation.status.c_str()));
  }
  g_dbus_connection_call(m_bus, "org.kde.JobViewServer", path->second.c_str(),
      "org.kde.JobViewV3", "update", g_variant_new("(@a{sv})", g_variant_builder_end(&properties)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer) {
        GError* callError = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &callError);
        if (reply != nullptr) g_variant_unref(reply);
        g_clear_error(&callError);
      }, nullptr);
}
void FileOperationManager::finishJob(const Operation& operation) {
  if (const auto timer = m_jobRevealTimers.find(operation.id); timer != m_jobRevealTimers.end()) {
    g_source_remove(timer->second);
    m_jobRevealTimers.erase(timer);
  }
  const auto path = m_jobPaths.find(operation.id);
  if (m_bus != nullptr && m_serviceAvailable && path != m_jobPaths.end()) {
    const guint subscription = m_signalSubscriptions[operation.id];
    if (subscription != 0) g_dbus_connection_signal_unsubscribe(m_bus, subscription);
    m_signalSubscriptions.erase(operation.id);
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(m_bus, "org.kde.JobViewServer", path->second.c_str(),
        "org.kde.JobViewV3", "terminate",
        g_variant_new("(us@a{sv})", operation.succeeded ? 0U : operation.cancelled ? 1U : 2U,
            operation.status.c_str(), g_variant_builder_end(&hints)),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error);
    if (reply != nullptr) g_variant_unref(reply);
    g_clear_error(&error);
    m_jobPaths.erase(path);
  }
}

void FileOperationManager::worker(GTask* task, gpointer, gpointer taskData, GCancellable* cancellable) {
  auto* data = static_cast<Task*>(taskData);
  auto* result = new Result{};
  if (data->kind == Kind::Properties && data->metadata) {
    auto last = std::chrono::steady_clock::now();
    result->metadata =
        filemanager::runPropertyRequest(*data->metadata, data->uid, cancellable, [&](const auto& progress) {
          const auto now = std::chrono::steady_clock::now();
          if (now - last < std::chrono::milliseconds(100))
            return;
          last = now;
          struct Report {
            FileOperationManager* owner;
            std::uint64_t id;
            filemanager::PropertyResult progress;
            std::weak_ptr<bool> alive;
          };
          g_idle_add(
              [](gpointer value) -> gboolean {
                std::unique_ptr<Report> report(static_cast<Report*>(value));
                const auto alive = report->alive.lock();
                if (!alive || !*alive)
                  return G_SOURCE_REMOVE;
                report->owner->update(
                    report->id, 0.0,
                    report->progress.inspected
                        ? "Checking authorisation: " + std::to_string(report->progress.inspected) + " entries inspected"
                        : std::to_string(report->progress.succeeded)
                            + " succeeded, "
                            + std::to_string(report->progress.failed)
                            + " failed, "
                            + std::to_string(report->progress.skipped)
                            + " skipped"
                );
                return G_SOURCE_REMOVE;
              },
              new Report{data->owner, data->id, progress, data->owner->m_alive}
          );
        });
    result->ok = result->metadata.ok();
    result->cancelled = result->metadata.cancelled;
    result->mutated = result->metadata.succeeded > 0 || result->metadata.ownershipEffects;
    result->message = result->metadata.summary();
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }
  if (data->renameIdentity) {
    struct stat state{};
    const auto& path = data->sources.front();
    const auto destination = std::filesystem::path(data->destination);
    if (!filemanager::validFileName(destination.filename().string())
        || destination.parent_path() != std::filesystem::path(path).parent_path()
        || lstat(path.c_str(), &state) != 0
        || static_cast<std::uint64_t>(state.st_dev) != data->renameIdentity->first
        || state.st_ino != data->renameIdentity->second) {
      result->message = "The item changed or the new filename is invalid. Reopen Properties if the item was replaced.";
      g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
      return;
    }
  }
  GError* error = nullptr;
  auto fail = [&](const char* fallback) {
    result->ok = false;
    result->message = error == nullptr ? fallback : error->message;
    result->cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
  };
  if (data->replayEntry) {
    result->ok = replay(*data->replayEntry, data->kind == Kind::Undo, cancellable, &error);
    if (!result->ok) {
      result->historyBlocked = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
      fail("Could not replay this operation.");
    }
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }

  const auto remember = [&](const std::string& source, const std::string& destination,
                            const std::optional<Revision>& sourceRevision, const std::optional<Revision>& resultRevision,
                            FileIdentity sourceParent, FileIdentity destinationParent, std::string linkTarget = {}) {
    if (!resultRevision || destinationParent == FileIdentity{} || parentIdentity(destination) != destinationParent
        || (!source.empty() && (sourceParent == FileIdentity{} || !sourceRevision
                                || parentIdentity(source) != sourceParent)))
      return;
    const auto& state = *resultRevision;
    if ((!S_ISREG(state.mode) && !S_ISLNK(state.mode) && !S_ISDIR(state.mode)) || state.links > (S_ISDIR(state.mode) ? 2U : 1U))
      return;
    if (S_ISDIR(state.mode) && (isTransfer(data->kind) || !emptyDirectory(destination)))
      return;
    if ((data->kind == Kind::Rename || data->kind == Kind::Move) && state.device != sourceParent.first)
      return;
    result->history.push_back({
        data->kind, source, destination, std::move(linkTarget), sourceRevision, state, sourceParent, destinationParent
    });
  };
  TransferProgress progress{.owner=data->owner, .id=data->id, .baseBytes=0, .progressedBytes=0,
        .totalBytes=0, .completedItems=0, .totalItems=0};
  progress.pause = data->pause;
  progress.cancellable = cancellable;
  progress.boundary = data->boundary;
  progress.acknowledgePause = [owner = data->owner, id = data->id, alive = data->alive](bool paused) {
    using Ack = std::tuple<FileOperationManager*, std::uint64_t, std::shared_ptr<bool>, bool>;
    g_idle_add([](gpointer value) -> gboolean {
      std::unique_ptr<Ack> ack(static_cast<Ack*>(value));
      if (*std::get<2>(*ack)) std::get<0>(*ack)->updatePaused(std::get<1>(*ack), std::get<3>(*ack));
      return G_SOURCE_REMOVE;
    }, new Ack{owner, id, alive, paused});
  };
  if (data->kind == Kind::Restore || data->kind == Kind::EmptyWastebin) {
    std::vector<std::string> uris = data->sources;
    if (data->kind == Kind::EmptyWastebin) {
      const bool enumerated = data->owner->m_wastebinBackend.enumerateTopLevel
          ? data->owner->m_wastebinBackend.enumerateTopLevel(cancellable, uris, result->message)
          : gioEnumerateTrash(cancellable, uris, result->message);
      if (!enumerated) {
        result->ok = false;
        result->cancelled = cancellable != nullptr && g_cancellable_is_cancelled(cancellable);
        result->customTitle = result->cancelled ? "Empty Wastebin cancelled" : "Empty Wastebin failed";
        result->customDescription = result->message;
        g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
        return;
      }
      if (!data->owner->m_wastebinBackend.enumerateTopLevel) {
        const auto invalid =
            std::ranges::find_if(uris, [](const std::string& uri) { return !isDirectTrashChild(uri); });
        if (invalid != uris.end()) {
          result->ok = false;
          result->message = "The Wastebin provider returned an item outside the trash root.";
          result->customTitle = "Empty Wastebin failed";
          result->customDescription = result->message;
          g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
          return;
        }
      }
    }
    const std::size_t requested = uris.size();
    result->requestedCount = requested;
    std::size_t restored = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::vector<std::string> failures;
    std::optional<ConflictAction> applyConflictToAll;
    std::optional<std::pair<GFileType, GFileType>> applyConflictTypes;
    auto noteFailure = [&](const std::string& uri, const std::string& detail) {
      ++failed;
      if (failures.size() < 4) {
        GFile* file = g_file_new_for_uri(uri.c_str());
        char* name = g_file_get_basename(file);
        failures.emplace_back(std::string(name == nullptr ? "item" : name) + ": " + detail);
        g_free(name);
        g_object_unref(file);
      }
    };
    for (std::size_t i = 0; i < requested; ++i) {
      if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        result->cancelled = true;
        g_clear_error(&error);
        break;
      }
      std::string itemError;
      bool succeeded = false;
      if (data->kind == Kind::EmptyWastebin) {
        // The enumerator supplies only the trash backend's direct children.
        // Deletion stays within GIO so the backend owns recursive trash removal.
        GFile* item = g_file_new_for_uri(uris[i].c_str());
        GError* deleteError = nullptr;
        succeeded = g_file_delete(item, cancellable, &deleteError);
        result->mutated |= succeeded;
        if (!succeeded)
          itemError = deleteError == nullptr ? "Could not delete Wastebin entry." : deleteError->message;
        g_clear_error(&deleteError);
        g_object_unref(item);
      } else {
        if (!data->owner->m_wastebinBackend.originalPath && !isTrashUri(uris[i])) {
          noteFailure(uris[i], "Restore source is not a GIO trash URI.");
          continue;
        }
        std::string originalPath;
        const bool resolved = data->owner->m_wastebinBackend.originalPath
            ? data->owner->m_wastebinBackend.originalPath(uris[i], cancellable, originalPath, itemError)
            : gioOriginalPath(uris[i], cancellable, originalPath, itemError);
        if (resolved && !originalPath.empty() && originalPath.front() == '/') {
          const auto original = std::filesystem::path(originalPath);
          const std::string filename = original.filename().string();
          std::filesystem::path destination =
              data->destination.empty() ? original : std::filesystem::path(data->destination) / filename;
          if (!filemanager::validFileName(filename) || originalPath.find('\0') != std::string::npos)
            itemError = "Original location metadata has no valid filename.";
          else {
            GFile* source = g_file_new_for_uri(uris[i].c_str());
            GFile* target = g_file_new_for_path(destination.string().c_str());
            GError* sourceQueryError = nullptr;
            GFileInfo* incoming = g_file_query_info(
                source, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable,
                &sourceQueryError
            );
            GFileInfo* existing = g_file_query_info(
                target, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
            );
            bool replace = false;
            bool proceed = true;
            if (incoming == nullptr) {
              itemError = sourceQueryError == nullptr ? "Could not inspect the Wastebin source item."
                                                      : sourceQueryError->message;
              proceed = false;
            } else if (existing != nullptr) {
              const auto incomingType = incoming == nullptr ? G_FILE_TYPE_UNKNOWN : g_file_info_get_file_type(incoming);
              const auto existingType = g_file_info_get_file_type(existing);
              ConflictDecision decision{.action = ConflictAction::Cancel};
              if (applyConflictToAll && applyConflictTypes == std::pair{incomingType, existingType})
                decision.action = *applyConflictToAll;
              else if (data->owner->m_conflictResolver)
                decision =
                    data->owner->m_conflictResolver(uris[i], destination.string(), i + 1 < requested, cancellable);
              if (decision.applyToAll) {
                applyConflictToAll = decision.action;
                applyConflictTypes = std::pair{incomingType, existingType};
              }
              if (decision.action == ConflictAction::Skip) {
                ++skipped;
                proceed = false;
              } else if (
                  decision.action == ConflictAction::Replace
                  && incomingType != G_FILE_TYPE_UNKNOWN
                  && incomingType == existingType
                  && incomingType != G_FILE_TYPE_DIRECTORY
              ) {
                replace = true;
              } else if (decision.action == ConflictAction::Rename) {
                std::string renamed = decision.alternativeName;
                if (renamed.empty()) {
                  const auto stem =
                      incomingType == G_FILE_TYPE_DIRECTORY ? original.filename().string() : original.stem().string();
                  const auto extension =
                      incomingType == G_FILE_TYPE_DIRECTORY ? std::string{} : original.extension().string();
                  for (unsigned suffix = 1; suffix < 10000; ++suffix) {
                    const std::string candidate = stem
                        + " (restored"
                        + (suffix == 1 ? std::string{} : " " + std::to_string(suffix))
                        + ")"
                        + extension;
                    GFile* parent = g_file_get_parent(target);
                    GFile* next = g_file_get_child(parent, candidate.c_str());
                    g_object_unref(parent);
                    const bool exists = g_file_query_exists(next, cancellable);
                    if (!exists) {
                      g_object_unref(target);
                      target = next;
                      destination = std::filesystem::path(destination).parent_path() / candidate;
                      renamed = candidate;
                      break;
                    }
                    g_object_unref(next);
                  }
                  if (renamed.empty())
                    itemError = "Could not find an available restore name.";
                } else if (renamed == "." || renamed == ".." || renamed.find('/') != std::string::npos) {
                  itemError = "The alternative name must be a single filename.";
                } else {
                  GFile* parent = g_file_get_parent(target);
                  GFile* next = g_file_get_child(parent, renamed.c_str());
                  g_object_unref(parent);
                  if (g_file_query_exists(next, cancellable))
                    itemError = "The alternative name already exists.";
                  else {
                    g_object_unref(target);
                    target = next;
                    destination = std::filesystem::path(destination).parent_path() / renamed;
                  }
                  if (target != next)
                    g_object_unref(next);
                }
              } else {
                proceed = false;
                result->cancelled = true;
                itemError = "Restore cancelled at the name conflict.";
              }
            }
            if (proceed && itemError.empty())
              succeeded = gioMoveWithBackup(source, target, replace, cancellable, itemError, result->mutated);
            g_clear_error(&sourceQueryError);
            if (incoming != nullptr)
              g_object_unref(incoming);
            if (existing != nullptr)
              g_object_unref(existing);
            g_object_unref(source);
            g_object_unref(target);
          }
        } else if (itemError.empty()) {
          itemError = "Original location metadata is unavailable or invalid for this Wastebin item.";
        }
      }
      if (succeeded)
        ++restored;
      else if (
          !itemError.empty() && !(data->kind == Kind::Restore && itemError == "Restore cancelled at the name conflict.")
      ) {
        noteFailure(uris[i], itemError);
      }
      if (result->cancelled)
        break;
    }
    result->cancelled |= g_cancellable_is_cancelled(cancellable) && restored + skipped < requested;
    result->ok = failed == 0 && !result->cancelled;
    result->customTitle = result->cancelled ? operationName(data->kind) + std::string(" cancelled")
        : failed == 0                       ? operationName(data->kind) + std::string(" completed")
        : restored + skipped > 0            ? operationName(data->kind) + std::string(" partially completed")
                                            : operationName(data->kind) + std::string(" failed");
    std::string label = data->kind == Kind::Restore ? "restored" : "deleted";
    result->customDescription = std::to_string(restored) + " of " + std::to_string(requested) + " items " + label;
    if (skipped > 0)
      result->customDescription += ", " + std::to_string(skipped) + " skipped";
    if (failed > 0)
      result->customDescription += ", " + std::to_string(failed) + " failed";
    if (result->cancelled)
      result->customDescription += "; operation cancelled";
    for (const auto& failure : failures)
      result->customDescription += "; " + failure;
    if (failed > failures.size())
      result->customDescription += "; additional failures omitted";
    if (requested == 0) {
      result->ok = true;
      result->customTitle = operationName(data->kind) + std::string(" completed");
      result->customDescription =
          data->kind == Kind::EmptyWastebin ? "Wastebin is already empty" : "No items to restore";
    }
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }
  if (data->kind == Kind::CreateSymlink || data->kind == Kind::CreateFolder || data->kind == Kind::CreateFile) {
    const bool symlink = data->kind == Kind::CreateSymlink;
    auto destination = std::filesystem::path(data->destination);
    const auto link = symlink ? data->sources.front() : std::string{};
    const auto parent = parentIdentity(data->destination);
    const auto source = data->keepBoth && std::filesystem::path(link).is_absolute() ? revision(link) : std::nullopt;
    if (symlink && (parent == FileIdentity{} || !filemanager::validFileName(destination.filename().string())
                    || link.empty() || link.find('\0') != std::string::npos)) {
      reject(&error, "Enter a valid name and target path.");
    } else if (data->keepBoth && (!source || (!S_ISREG(source->mode) && !S_ISDIR(source->mode)))) {
      reject(&error, "The selected file or folder is no longer available.");
    } else {
      const auto filename = destination.filename().string();
      const auto extension = source && S_ISREG(source->mode) ? filename.find('.', 1) : std::string::npos;
      const auto stem = filename.substr(0, extension);
      const auto suffix = extension == std::string::npos ? std::string{} : filename.substr(extension);
      const auto directory = destination.parent_path();
      PrivateStaging staging(data->destination);
      const std::string staged = staging.directory + "/item";
      std::optional<Revision> stagedState;
      bool useDirectCreate = staging.directory.empty();
      if (!useDirectCreate) {
        GFile* target = g_file_new_for_path(staged.c_str());
        bool created = false;
        if (createItem(data->kind, target, link, cancellable, &error, created))
          stagedState = revision(staged);
        g_object_unref(target);
      }
      for (unsigned number = 1; (stagedState || useDirectCreate) && number < 10000; ++number) {
        destination = directory / numberedName(stem, suffix, number, false);
        if (g_cancellable_set_error_if_cancelled(cancellable, &error))
          break;
        if (data->keepBoth) {
          const auto current = revision(link);
          if (!current || current->device != source->device || current->inode != source->inode
              || (current->mode & S_IFMT) != (source->mode & S_IFMT)) {
            reject(&error, "The selected item was replaced before the link could be created.");
            break;
          }
        }
        if (stagedState && !useDirectCreate) {
          result->ok = renameWithoutReplace(staged, destination.string(), &error);
          useDirectCreate = !result->ok && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)
                                           || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT));
          if (useDirectCreate)
            g_clear_error(&error);
        }
        if (useDirectCreate) {
          // Preserve creation on providers without native no-replace rename.
          // Only a staged, identifiable result can enter undo history.
          GFile* target = g_file_new_for_path(destination.c_str());
          result->ok = createItem(data->kind, target, link, cancellable, &error, result->mutated);
          g_object_unref(target);
        }
        if (result->ok || !data->keepBoth || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS))
          break;
        g_clear_error(&error);
      }
      if (!staging.directory.empty()) {
        if (data->kind == Kind::CreateFolder)
          rmdir(staged.c_str());
        else
          unlink(staged.c_str());
      }
      if (result->ok) {
        result->mutated = true;
        data->destination = destination.string();
        const auto created = revision(data->destination);
        if (created && stagedState && !useDirectCreate) {
          stagedState->changed = created->changed;
          if (created == stagedState) {
            remember({}, data->destination, {}, created, {}, parent, link);
            if (symlink)
              result->completedEntries.push_back({data->destination, created->device, created->inode});
          }
        } else if (symlink && created && S_ISLNK(created->mode)) {
          std::error_code linkError;
          if (std::filesystem::read_symlink(destination, linkError).native() == link && !linkError
              && revision(data->destination) == created)
            result->completedEntries.push_back({data->destination, created->device, created->inode});
        }
      }
    }
    if (!result->ok)
      fail(symlink ? "Could not create symlink or find an available name." : "Could not create item.");
  } else {
    auto count = data->sources.size();
    std::vector<std::size_t> reportRoots(count);
    for (std::size_t i = 0; i < count; ++i)
      reportRoots[i] = i;
    std::vector<std::string> reportPaths(count);
    std::vector<bool> reportPathSet(count, false);
    std::vector<bool> reportTransferred(count, false);
    std::vector<bool> reportFailed(count, false);
    std::vector<std::optional<CompletedEntry>> reportEntries(count);
    std::vector<std::string> destinationParents(count, data->destination);
    std::vector<std::string> mergedMoveDirectories;
    std::vector<std::size_t> mergedMoveRoots;
    std::optional<ConflictAction> applyConflictToAll;
    std::optional<GFileType> applyConflictItemType;
    std::optional<GFileType> applyConflictDestinationType;
    if (isTransfer(data->kind)) {
      data->measures.resize(count);
      for (std::size_t i = 0; i < count; ++i) {
        if (!transferCheckpoint(&progress)) { g_cancellable_set_error_if_cancelled(cancellable, &error); fail("Cancelled"); break; }
        GFile* source = g_file_new_for_path(data->sources[i].c_str());
        const bool measured = measureTree(source, cancellable, data->measures[i], &error);
        g_object_unref(source);
        if (!measured) { fail("Could not calculate transfer size"); break; }
        data->totalMeasure.bytes += data->measures[i].bytes;
        data->totalMeasure.items += data->measures[i].items;
      }
      if (result->message.empty()) {
        auto* report = new std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>{data->owner,
            {data->id, 0, data->totalMeasure.bytes, 0, data->totalMeasure.items, 0}};
        g_idle_add([](gpointer value) -> gboolean {
          auto* event = static_cast<std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>*>(value);
          auto& v = event->second;
          event->first->updateProgress(v[0], v[1], v[2], v[3], v[4], v[5]);
          delete event; return G_SOURCE_REMOVE;
        }, report);
      }
    }
    std::uint64_t completedBytes = 0;
    std::uint64_t completedItems = 0;
    progress.totalBytes = data->totalMeasure.bytes;
    progress.totalItems = data->totalMeasure.items;
    progress.lastReport = std::chrono::steady_clock::now();
    if (progress.pause) progress.reportedPause = progress.pause->pausedTime();
    for (std::size_t i = 0; i < count && result->message.empty(); ++i) {
      transferCheckpoint(&progress);
      if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        if (isTransfer(data->kind) && i < reportRoots.size())
          reportFailed[reportRoots[i]] = true;
        fail("Cancelled");
        break;
      }
      const std::size_t reportRoot = reportRoots[i];
      const auto& sourcePath = data->sources[i];
      const auto sourceBefore = isTransfer(data->kind) || data->kind == Kind::Rename
          ? revision(sourcePath) : std::nullopt;
      const auto sourceParent = sourceBefore ? parentIdentity(sourcePath) : FileIdentity{};
      GFile* source = g_file_new_for_path(data->sources[i].c_str());
      GFile* target = nullptr;
      if (isTransfer(data->kind))
      {
        GFile* parent = g_file_new_for_path(destinationParents[i].c_str());
        target = g_file_get_child(parent, basename(data->sources[i]).c_str());
        g_object_unref(parent);
      }
      else if (data->kind == Kind::Rename) target = g_file_new_for_path(data->destination.c_str());
      if (isTransfer(data->kind) && target != nullptr) {
        const bool sameItem = g_file_equal(source, target);
        if (sameItem && data->kind == Kind::Move) result->message = "Cannot move an item onto itself";
        if (data->kind == Kind::Duplicate && !sameItem)
          result->message = "Duplicate requires a source in the displayed folder";
        GFileInfo* sourceInfo = g_file_query_info(source, G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
        if (sourceInfo == nullptr) { fail("Could not inspect source"); }
        else {
          const bool directory = g_file_info_get_file_type(sourceInfo) == G_FILE_TYPE_DIRECTORY;
          g_object_unref(sourceInfo);
          char* targetPath = g_file_get_path(target);
          if (directory && !sameItem && targetPath != nullptr && isSameOrDescendant(data->sources[i], targetPath))
            result->message = "Cannot copy a directory into itself or one of its descendants";
          g_free(targetPath);
        }
      }
      if (!result->message.empty()) {
        result->ok = false;
        if (isTransfer(data->kind))
          reportFailed[reportRoot] = true;
        if (target != nullptr) g_object_unref(target);
        g_object_unref(source);
        break;
      }
      if (data->renameIdentity && target) {
        GFileInfo* existing = g_file_query_info(
            target, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
        );
        if (existing) {
          g_object_unref(existing);
          result->message = "An item with that name already exists.";
          g_object_unref(target);
          g_object_unref(source);
          break;
        }
      }
      bool replaceExisting = false;
      bool skipped = false;
      bool merge = false;
      if (data->kind != Kind::Duplicate && (isTransfer(data->kind) || data->kind == Kind::Rename)
          && target != nullptr && g_file_query_exists(target, cancellable)) {
        const bool sameItem = data->kind != Kind::Rename && g_file_equal(source, target);
        GFileInfo* incomingInfo = g_file_query_info(source, G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr);
        const auto incomingType = incomingInfo == nullptr ? G_FILE_TYPE_UNKNOWN : g_file_info_get_file_type(incomingInfo);
        if (incomingInfo != nullptr) g_object_unref(incomingInfo);
        const auto existingType = g_file_query_file_type(target, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable);
        bool compatibleBatchConflict = true;
        if (applyConflictToAll) {
          GFileInfo* existingInfo = g_file_query_info(target, G_FILE_ATTRIBUTE_STANDARD_TYPE,
              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr);
          compatibleBatchConflict = !sameItem && incomingType != G_FILE_TYPE_UNKNOWN && existingInfo != nullptr
              && incomingType == g_file_info_get_file_type(existingInfo) && applyConflictItemType == incomingType && applyConflictDestinationType == existingType;
          if (existingInfo != nullptr) g_object_unref(existingInfo);
        }
        ConflictDecision decision{.action=ConflictAction::Cancel};
        if (applyConflictToAll && compatibleBatchConflict) decision.action = *applyConflictToAll;
        else if (data->owner->m_conflictResolver) {
          char* destinationPath = g_file_get_path(target);
          decision = data->owner->m_conflictResolver(data->sources[i],
              destinationPath == nullptr ? data->destination : destinationPath, i + 1 < count || incomingType == G_FILE_TYPE_DIRECTORY, cancellable);
          g_free(destinationPath);
        }
        if (decision.applyToAll) {
          applyConflictToAll = decision.action;
          applyConflictItemType = incomingType;
          applyConflictDestinationType = existingType;
        }
        if (decision.action == ConflictAction::Skip) skipped = true;
        else if (decision.action == ConflictAction::Merge && !sameItem
            && incomingType == G_FILE_TYPE_DIRECTORY && existingType == G_FILE_TYPE_DIRECTORY
            && isTransfer(data->kind)) merge = true;
        else if (decision.action == ConflictAction::Replace && !sameItem
            && incomingType == existingType && incomingType != G_FILE_TYPE_DIRECTORY
            && incomingType != G_FILE_TYPE_UNKNOWN) replaceExisting = true;
        else if (decision.action == ConflictAction::Rename) {
          GFile* parent = g_file_get_parent(target);
          const auto original = std::filesystem::path(data->sources[i]).filename();
          const auto stem = incomingType == G_FILE_TYPE_DIRECTORY ? original.string() : original.stem().string();
          const auto extension = incomingType == G_FILE_TYPE_DIRECTORY ? std::string{} : original.extension().string();
          std::string candidate;
          if (!decision.alternativeName.empty()) {
            const auto& name = decision.alternativeName;
            if (name == "." || name == ".." || name.find('/') != std::string::npos) {
              result->message = "The alternative name must be a single valid filename";
            } else {
              candidate = name;
              GFile* next = g_file_get_child(parent, candidate.c_str());
              if (g_file_query_exists(next, cancellable)) {
                result->message = "The alternative name already exists in the destination";
                g_object_unref(next);
              } else {
                g_object_unref(target);
                target = next;
              }
            }
          } else {
            for (unsigned suffix = 1; suffix < 10000; ++suffix) {
              candidate = numberedName(stem, extension, suffix, true);
              GFile* next = g_file_get_child(parent, candidate.c_str());
              const bool exists = g_file_query_exists(next, cancellable);
              if (!exists) { g_object_unref(target); target = next; break; }
              g_object_unref(next);
            }
            if (target == nullptr || g_file_query_exists(target, cancellable))
              result->message = "Could not find a free name for the incoming item";
          }
          g_object_unref(parent);
        } else {
          result->message = "Operation cancelled";
          result->cancelled = true;
        }
      }
      if (!result->message.empty()) {
        result->ok = false;
        if (isTransfer(data->kind))
          reportFailed[reportRoot] = true;
        if (target != nullptr) g_object_unref(target);
        g_object_unref(source);
        break;
      }
      if (isTransfer(data->kind) && target != nullptr && !reportPathSet[reportRoot]) {
        char* path = g_file_get_path(target);
        if (path != nullptr) {
          reportPaths[reportRoot] = path;
          reportPathSet[reportRoot] = true;
        }
        g_free(path);
      }
      if (merge) {
        // Expand only a pair explicitly approved for merging. Every child still
        // passes through the normal conflict and staged-finalisation path.
        GFileEnumerator* entries = g_file_enumerate_children(source, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
        if (entries == nullptr) fail("Could not enumerate directory for merging");
        else {
          char* destinationPath = g_file_get_path(target);
          while (GFileInfo* child = g_file_enumerator_next_file(entries, cancellable, &error)) {
            GFile* childFile = g_file_get_child(source, g_file_info_get_name(child));
            char* childPath = g_file_get_path(childFile);
            TransferMeasure measure;
            const bool measured = childPath != nullptr && measureTree(childFile, cancellable, measure, &error);
            if (measured) {
              data->sources.emplace_back(childPath);
              destinationParents.emplace_back(destinationPath);
              data->measures.push_back(measure);
              reportRoots.push_back(reportRoot);
            }
            g_free(childPath);
            g_object_unref(childFile);
            g_object_unref(child);
            if (!measured) break;
          }
          g_free(destinationPath);
          g_object_unref(entries);
          if (error != nullptr) fail("Could not enumerate directory for merging");
          else if (data->kind == Kind::Move) {
            mergedMoveDirectories.push_back(data->sources[i]);
            mergedMoveRoots.push_back(reportRoot);
          }
          count = data->sources.size();
        }
        if (!result->message.empty())
          reportFailed[reportRoot] = true;
        g_object_unref(target);
        g_object_unref(source);
        continue;
      }
      if (skipped) {
        result->ok = true;
        if (isTransfer(data->kind)) {
          data->totalMeasure.bytes -= data->measures[i].bytes;
          data->totalMeasure.items -= data->measures[i].items;
          progress.totalBytes = data->totalMeasure.bytes;
          progress.totalItems = data->totalMeasure.items;
          progress.baseBytes = completedBytes;
          progress.progressedBytes = completedBytes;
          progress.completedItems = completedItems;
          publishProgress(progress, 0, true);
        }
        if (target != nullptr) g_object_unref(target);
        g_object_unref(source);
        continue;
      }
      if (data->kind == Kind::Trash) {
        result->ok = g_file_trash(source, cancellable, &error);
        result->mutated |= result->ok;
      }
      else if (data->kind == Kind::Delete) result->ok = removeTree(source, cancellable, &error, &result->mutated);
      else if (data->kind == Kind::Rename) {
        g_autofree char* targetPath = g_file_get_path(target);
        const auto targetParent = targetPath ? parentIdentity(targetPath) : FileIdentity{};
        GFile* backup = nullptr;
        if (replaceExisting) {
          char* backupName = g_strdup_printf(".nythera-backup-%u", static_cast<unsigned>(g_random_int()));
          GFile* parent = g_file_get_parent(target);
          backup = g_file_get_child(parent, backupName);
          g_object_unref(parent);
          g_free(backupName);
          result->ok = g_file_move(target, backup, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
          result->mutated |= result->ok;
        } else result->ok = true;
        if (result->ok && data->renameIdentity) {
          struct stat state{};
          if (lstat(data->sources.front().c_str(), &state) != 0
              || static_cast<std::uint64_t>(state.st_dev) != data->renameIdentity->first
              || state.st_ino != data->renameIdentity->second) {
            result->ok = false;
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "The source item was replaced before rename.");
          }
        }
        if (result->ok) result->ok = g_file_move(source, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
        result->mutated |= result->ok;
        if (result->ok && !replaceExisting && sourceBefore && targetPath) {
          const auto moved = revision(targetPath);
          if (moved) {
            auto expected = *sourceBefore;
            expected.changed = moved->changed;
            if (moved == expected)
              remember(sourcePath, targetPath, sourceBefore, moved, sourceParent, targetParent);
          }
        }
        if (!result->ok && backup != nullptr) {
          GError* restoreError = nullptr;
          g_file_move(backup, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, &restoreError);
          g_clear_error(&restoreError);
        } else if (result->ok && backup != nullptr) {
          GError* cleanupError = nullptr;
          removeTree(backup, nullptr, &cleanupError);
          g_clear_error(&cleanupError);
        }
        if (backup != nullptr) g_object_unref(backup);
      }
      else {
        // Stage beside the destination so finalisation is a single no-overwrite rename.
        char* uuid = g_uuid_string_random();
        char* leaf = g_strdup_printf(".nythera-partial-%s-%zu", uuid, i);
        g_free(uuid);
        GFile* targetParent = g_file_get_parent(target);
        GFile* staged = g_file_get_child(targetParent, leaf);
        g_object_unref(targetParent);
        g_free(leaf);
        const bool stagedWasAbsent = !g_file_query_exists(staged, cancellable);
        progress.baseBytes = completedBytes;
        progress.progressedBytes = completedBytes;
        progress.completedItems = completedItems;
        if (stagedWasAbsent) result->ok = copyTree(source, staged, cancellable, &error, &progress);
        else {
          g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary destination");
          result->ok = false;
        }
        if (result->ok && !transferCheckpoint(&progress)) {
          g_cancellable_set_error_if_cancelled(cancellable, &error);
          result->ok = false;
        }
        g_autofree char* stagePath = g_file_get_path(staged);
        g_autofree char* plannedPath = g_file_get_path(target);
        const auto stagedRevision = result->ok && stagePath ? revision(stagePath) : std::nullopt;
        const auto destinationParent = plannedPath ? parentIdentity(plannedPath) : FileIdentity{};
        GFile* backup = nullptr;
        if (result->ok && replaceExisting) {
          char* backupName = g_strdup_printf(".nythera-backup-%u", static_cast<unsigned>(g_random_int()));
          GFile* parent = g_file_get_parent(target);
          backup = g_file_get_child(parent, backupName);
          g_object_unref(parent);
          g_free(backupName);
          result->ok = g_file_move(target, backup, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
          result->mutated |= result->ok;
        }
        if (result->ok && data->kind == Kind::Duplicate) {
          const auto name = std::filesystem::path(sourcePath).filename();
          const bool directory = sourceBefore && S_ISDIR(sourceBefore->mode);
          const auto stem = directory ? name.string() : name.stem().string();
          const auto extension = directory ? std::string{} : name.extension().string();
          const auto parent = std::filesystem::path(plannedPath).parent_path();
          result->ok = false;
          for (unsigned suffix = 1; suffix < 10000; ++suffix) {
            const auto candidate = (parent / numberedName(stem, extension, suffix, true)).string();
            if (!transferCheckpoint(&progress)) {
              g_cancellable_set_error_if_cancelled(cancellable, &error);
              break;
            }
            if (renameWithoutReplace(stagePath, candidate, &error)) {
              reportPaths[reportRoot] = candidate;
              result->ok = true;
              break;
            }
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS))
              break;
            g_clear_error(&error);
          }
          if (!result->ok && !error)
            reject(&error, "Could not find a free name for the duplicate.");
        } else if (result->ok) {
          result->ok = g_file_move(staged, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, cancellable, nullptr, nullptr, &error);
        }
        result->mutated |= result->ok;
        const bool stableSource = sourceBefore && revision(sourcePath) == sourceBefore;
        if (!result->ok && backup != nullptr) {
          GError* restoreError = nullptr;
          g_file_move(backup, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, &restoreError);
          g_clear_error(&restoreError);
        } else if (result->ok && backup != nullptr) {
          GError* cleanupError = nullptr;
          removeTree(backup, nullptr, &cleanupError);
          g_clear_error(&cleanupError);
        }
        if (backup != nullptr) g_object_unref(backup);
        if (!result->ok && stagedWasAbsent && g_file_query_exists(staged, nullptr)) {
          GError* cleanupError = nullptr; removeTree(staged, nullptr, &cleanupError); g_clear_error(&cleanupError);
        }
        if (result->ok && data->kind == Kind::Move) {
          if (g_cancellable_set_error_if_cancelled(cancellable, &error)) result->ok = false;
          else result->ok = removeTree(source, nullptr, &error);
        }
        if (result->ok) {
          if (!replaceExisting && i == reportRoot && stagedRevision) {
            const auto& committedPath = reportPaths[reportRoot];
            const auto current = revision(committedPath);
            auto expected = *stagedRevision;
            if (current) {
              expected.changed = current->changed;
              if (current == expected) {
                if (stableSource)
                  remember(sourcePath, committedPath, sourceBefore, current, sourceParent, destinationParent);
                if (data->kind == Kind::Duplicate)
                  reportEntries[reportRoot] = CompletedEntry{committedPath, current->device, current->inode};
              }
            }
          }
          completedBytes += data->measures[i].bytes;
          completedItems += data->measures[i].items;
          data->completedMeasure.bytes = completedBytes;
          data->completedMeasure.items = completedItems;
          progress.baseBytes = completedBytes;
          progress.progressedBytes = completedBytes;
          progress.completedItems = completedItems;
          publishProgress(progress, 0, true);
        }
        g_object_unref(staged);
      }
      if ((data->kind == Kind::Trash || data->kind == Kind::Delete) && result->ok) {
        const auto done = static_cast<std::uint64_t>(i + 1);
        auto* event = new std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>{data->owner,
            {data->id, 0, 0, done, static_cast<std::uint64_t>(count), 0}};
        g_idle_add([](gpointer value) -> gboolean {
          auto* report = static_cast<std::pair<FileOperationManager*, std::array<std::uint64_t, 6>>*>(value);
          auto& v = report->second;
          report->first->updateProgress(v[0], v[1], v[2], v[3], v[4], v[5]);
          delete report; return G_SOURCE_REMOVE;
        }, event);
      }
      if (!result->ok) fail("Operation failed");
      if (isTransfer(data->kind)) {
        if (result->ok) {
          reportTransferred[reportRoot] = true;
          if (data->kind != Kind::Duplicate && !reportEntries[reportRoot] && reportPathSet[reportRoot]) {
            struct stat state {};
            if (lstat(reportPaths[reportRoot].c_str(), &state) == 0)
              reportEntries[reportRoot] = CompletedEntry{
                  reportPaths[reportRoot], static_cast<std::uint64_t>(state.st_dev),
                  static_cast<std::uint64_t>(state.st_ino)
              };
          }
        } else
          reportFailed[reportRoot] = true;
      }
      if (target != nullptr) g_object_unref(target);
      g_object_unref(source);
    }
    if (result->message.empty() && data->kind == Kind::Move) {
      // Remove only empty merged source directories. Skipped children remain
      // at the source, and cancellation/failure never triggers recursive cleanup.
      for (std::size_t index = mergedMoveDirectories.size(); index > 0; --index) {
        const auto root = mergedMoveRoots[index - 1];
        GFile* directory = g_file_new_for_path(mergedMoveDirectories[index - 1].c_str());
        GError* cleanupError = nullptr;
        const bool removed = g_file_delete(directory, cancellable, &cleanupError);
        result->mutated |= removed;
        if (!removed
            && !g_error_matches(cleanupError, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY)) {
          reportFailed[root] = true;
          error = cleanupError;
          cleanupError = nullptr;
          fail("Could not remove merged source directory");
        }
        g_clear_error(&cleanupError);
        g_object_unref(directory);
        if (!result->message.empty()) break;
      }
    } else if (data->kind == Kind::Move && !mergedMoveRoots.empty()) {
      // A later top-level error/cancellation prevents merged source-directory
      // cleanup, so those merge roots are not fully completed move entries.
      for (const auto root : mergedMoveRoots)
        reportFailed[root] = true;
    }
    if (isTransfer(data->kind)) {
      for (std::size_t root = 0; root < reportPaths.size(); ++root) {
        if (!reportTransferred[root] || reportFailed[root] || !reportEntries[root])
          continue;
        struct stat state{};
        const auto& entry = *reportEntries[root];
        if (lstat(entry.path.c_str(), &state) == 0 && static_cast<std::uint64_t>(state.st_dev) == entry.device
            && static_cast<std::uint64_t>(state.st_ino) == entry.inode)
          result->completedEntries.push_back(entry);
      }
    }
    if (result->message.empty()) result->ok = true;
  }
  g_task_return_pointer(task, result, [](gpointer ptr) { delete static_cast<Result*>(ptr); });
}

void FileOperationManager::completed(GObject*, GAsyncResult* asyncResult, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  GError* error = nullptr;
  std::unique_ptr<Result> result(static_cast<Result*>(g_task_propagate_pointer(G_TASK(asyncResult), &error)));
  auto* data = static_cast<Task*>(g_task_get_task_data(G_TASK(asyncResult)));
  if (data != nullptr) {
    auto* operation = self->find(data->id);
    if (operation != nullptr) {
      operation->finished = true;
      operation->paused = false;
      self->m_pauseStates.erase(data->id);
      self->m_transferRates.erase(data->id);
      self->m_activePaths.erase(data->id);
      operation->succeeded = result && result->ok;
      operation->cancelled = result ? result->cancelled : error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      if (isReplay(data->kind)) {
        self->m_replaying = false;
        const bool undoing = data->kind == Kind::Undo;
        auto& from = undoing ? self->m_undo : self->m_redo;
        auto& to = undoing ? self->m_redo : self->m_undo;
        if (operation->succeeded) {
          auto before = std::move(from.back());
          from.pop_back();
          auto& after = *data->replayEntry;
          // Rebase only exact matches to the state this replay replaced.
          // An independently modified item keeps its stale fingerprint.
          const auto rebase = [&](const std::string& path, const Revision& oldState, const Revision& newState) {
            for (auto* stack : {&self->m_undo, &self->m_redo}) {
              for (auto& previous : *stack) {
                if (previous.source == path && previous.sourceRevision == oldState)
                  previous.sourceRevision = newState;
                if (previous.destination == path && previous.resultRevision == oldState)
                  previous.resultRevision = newState;
              }
            }
          };
          if (undoing && (after.kind == Kind::Move || after.kind == Kind::Rename))
            rebase(after.source, *before.sourceRevision, *after.sourceRevision);
          if (!undoing && after.resultRevision.inode)
            rebase(after.destination, before.resultRevision, after.resultRevision);
          if (after.resultRevision.inode)
            to.push_back(std::move(after));
        } else if (result && result->historyBlocked) {
          from.back().blocked = true;
        }
      } else if (result && result->mutated) {
        self->noteExternalMutation();
        operation->undoUnavailable = result->history.empty()
            && data->kind != Kind::Delete && data->kind != Kind::EmptyWastebin && data->kind != Kind::Properties;
        constexpr std::size_t historyLimit = 32;
        for (auto& entry : result->history) {
          if (self->m_undo.size() == historyLimit)
            self->m_undo.erase(self->m_undo.begin());
          self->m_undo.push_back(std::move(entry));
        }
      }
      if (data->kind == Kind::CreateSymlink)
        operation->destination = data->destination;
      if (result && (isTransfer(data->kind) || data->kind == Kind::CreateSymlink))
        operation->completedEntries = std::move(result->completedEntries);
      if (isTransfer(data->kind)) {
        operation->processedBytes = data->completedMeasure.bytes;
        operation->totalBytes = data->totalMeasure.bytes;
        operation->processedFiles = data->completedMeasure.items;
        operation->totalFiles = data->totalMeasure.items;
        if (operation->totalBytes > 0)
          operation->fraction = std::clamp(static_cast<double>(operation->processedBytes) / static_cast<double>(operation->totalBytes), 0.0, 1.0);
      }
      operation->fraction = operation->succeeded ? 1.0 : operation->fraction;
      const std::string verb = operationName(data->kind);
      operation->resultTitle = verb + (operation->succeeded ? " completed" : operation->cancelled ? " cancelled" : " failed");
      if (result && result->requestedCount)
        operation->requestedItemCount = *result->requestedCount;
      if (result && !result->customTitle.empty())
        operation->resultTitle = result->customTitle;
      if (result && !result->customDescription.empty())
        operation->resultDescription = result->customDescription;
      else if (operation->succeeded) operation->resultDescription = completedDescription(*operation);
      else if (operation->cancelled) {
        if (isTransfer(data->kind))
          operation->resultDescription = "Transfer to " + displayDestination(operation->destination) + " was cancelled";
        else operation->resultDescription = verb + " of " + quotedBasename(operation->source.empty() ? operation->destination : operation->source) + " was cancelled";
      } else {
        operation->resultDescription = failedDescription(*operation,
            result ? result->message : error ? error->message : "Operation failed");
      }
      if (data->kind == Kind::Properties) {
        self->m_metadataTargets.erase(data->id);
        operation->resultDescription = result ? result->message : "Properties operation failed.";
        if (data->metadataCompleted && result)
          data->metadataCompleted(result->metadata);
      }
      operation->status = operation->resultDescription;
      if (operation->undoUnavailable)
        operation->status += " · Undo unavailable for this operation";
      if (!operation->succeeded && !operation->cancelled && !self->m_jobPaths.contains(operation->id))
        self->registerJob(*operation);
      self->publishJob(*operation);
      self->finishJob(*operation);
      if (data->renameCompleted)
        data->renameCompleted(operation->succeeded, operation->resultDescription);
      if (self->m_changed) self->m_changed(*operation);
      auto it = std::ranges::find(self->m_cancellables, data->id, &std::pair<std::uint64_t, GCancellable*>::first);
      if (it != self->m_cancellables.end()) { g_object_unref(it->second); self->m_cancellables.erase(it); }
    }
  }
  g_clear_error(&error);
}
