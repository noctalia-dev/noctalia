#include "filemanager/file_operation_manager.h"

#include "filemanager/browser_tab.h"

#include "filemanager/file_interaction.h"
#include "filemanager/fs_compat.h"
#include "filemanager/wastebin_fallback.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <mutex>
#include <ranges>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace {
  namespace engine = filemanager::file_operation_engine;
  namespace fs_compat = filemanager::fs_compat;

  using Kind = FileOperationManager::Kind;
  namespace wastebin_fallback = filemanager::wastebin_fallback;

  bool isCopy(Kind kind) { return kind == Kind::Copy || kind == Kind::Duplicate; }
  bool isTransfer(Kind kind) { return isCopy(kind) || kind == Kind::Move; }
  bool usesTransferControls(Kind kind) { return isTransfer(kind) || kind == Kind::TrashToHome; }
  bool isReplay(Kind kind) { return kind == Kind::Undo || kind == Kind::Redo; }
  bool reportsProgress(Kind kind) {
    return isTransfer(kind)
        || isReplay(kind)
        || kind == Kind::Trash
        || kind == Kind::TrashToHome
        || kind == Kind::Delete
        || kind == Kind::Restore
        || kind == Kind::EmptyWastebin
        || kind == Kind::Properties;
  }

  using engine::cleanupStagedTree;
  using engine::copyTree;
  using engine::entryExistsNoFollow;
  using engine::FileIdentity;
  using engine::FileTransferPauseState;
  using engine::measureTree;
  using engine::parentIdentity;
  using engine::PrivateStaging;
  using engine::publishProgress;
  using engine::reject;
  using engine::removeTree;
  using engine::revision;
  using engine::setErrorFromPublishOutcome;
  using engine::toCompatIdentity;
  using engine::transferCheckpoint;
  using engine::TransferMeasure;
  using engine::TransferProgress;

  std::optional<engine::Kind> localKind(Kind kind) {
    switch (kind) {
    case Kind::Copy:
      return engine::Kind::Copy;
    case Kind::Duplicate:
      return engine::Kind::Duplicate;
    case Kind::Move:
      return engine::Kind::Move;
    case Kind::Rename:
      return engine::Kind::Rename;
    case Kind::CreateFolder:
      return engine::Kind::CreateFolder;
    case Kind::CreateFile:
      return engine::Kind::CreateFile;
    case Kind::CreateSymlink:
      return engine::Kind::CreateSymlink;
    case Kind::Delete:
      return engine::Kind::Delete;
    default:
      return std::nullopt;
    }
  }

  void postProgressIdle(FileOperationManager* owner, std::weak_ptr<bool> alive, std::array<std::uint64_t, 6> values) {
    struct ProgressReport {
      FileOperationManager* owner;
      std::weak_ptr<bool> alive;
      std::array<std::uint64_t, 6> values;
    };
    g_idle_add(
        [](gpointer data) -> gboolean {
          std::unique_ptr<ProgressReport> report(static_cast<ProgressReport*>(data));
          const auto locked = report->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          const auto& v = report->values;
          report->owner->updateProgress(v[0], v[1], v[2], v[3], v[4], v[5]);
          return G_SOURCE_REMOVE;
        },
        new ProgressReport{owner, std::move(alive), values}
    );
  }

  std::string basename(const std::string& path) { return std::filesystem::path(path).filename().string(); }

  const char* operationName(FileOperationManager::Kind kind) {
    switch (kind) {
    case FileOperationManager::Kind::Copy:
      return "Copy";
    case FileOperationManager::Kind::Duplicate:
      return "Duplicate";
    case FileOperationManager::Kind::Move:
      return "Move";
    case FileOperationManager::Kind::Rename:
      return "Rename";
    case FileOperationManager::Kind::CreateFolder:
    case FileOperationManager::Kind::CreateFile:
      return "Create";
    case FileOperationManager::Kind::CreateSymlink:
      return "Create symlink";
    case FileOperationManager::Kind::Trash:
      return "Trash";
    case FileOperationManager::Kind::TrashToHome:
      return "Move to Home Wastebin";
    case FileOperationManager::Kind::Delete:
      return "Permanent delete";
    case FileOperationManager::Kind::Restore:
      return "Restore";
    case FileOperationManager::Kind::EmptyWastebin:
      return "Empty Wastebin";
    case FileOperationManager::Kind::Properties:
      return "Properties";
    case Kind::Undo:
      return "Undo";
    case Kind::Redo:
      return "Redo";
    }
    return "File operation";
  }

  std::string quotedBasename(const std::string& path) {
    const std::string name = basename(path);
    return name.empty() ? "item" : "\"" + name + "\"";
  }

  std::string displayDestination(const std::string& path, bool executorOnly = false) {
    std::filesystem::path destination(path);
    if (const char* home = g_get_home_dir(); home != nullptr) {
      const std::filesystem::path homePath(home);
      std::error_code ec;
      const auto relative = executorOnly ? destination.lexically_relative(homePath)
                                         : std::filesystem::relative(destination, homePath, ec);
      if (!ec && !relative.empty() && *relative.begin() != "..")
        destination = relative;
      else if (!ec && relative == ".")
        return "Home";
    }
    std::string label = destination.string();
    constexpr std::size_t kMaxDestinationLabel = 72;
    if (label.size() > kMaxDestinationLabel)
      label = "…" + label.substr(label.size() - (kMaxDestinationLabel - 1));
    return label;
  }

  std::string completedDescription(const FileOperationManager::Operation& operation) {
    const auto kind = operation.kind;
    const auto count = !isTransfer(kind) ? operation.requestedItemCount
        : operation.adminOrigin ? operation.completedEntries.size()
        : operation.totalFiles > 0 ? operation.totalFiles : operation.requestedItemCount;
    const char* unit = isTransfer(kind) && !operation.adminOrigin && operation.totalFiles > 0 ? "file" : "item";
    const std::string quantity = std::to_string(count) + " " + unit + (count == 1 ? "" : "s");
    switch (kind) {
    case FileOperationManager::Kind::Copy:
      return quantity + " copied to " + displayDestination(operation.destination, operation.adminOrigin.has_value());
    case Kind::Duplicate:
      return quantity + " duplicated in " + displayDestination(operation.destination, operation.adminOrigin.has_value());
    case FileOperationManager::Kind::Move:
      return quantity + " moved to " + displayDestination(operation.destination, operation.adminOrigin.has_value());
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
      return count == 1 ? "Moved " + quotedBasename(operation.source) + " to Trash" : "Moved " + quantity + " to Trash";
    case FileOperationManager::Kind::TrashToHome:
      return count == 1 ? "Moved " + quotedBasename(operation.source) + " to Home Wastebin"
                        : "Moved " + quantity + " to Home Wastebin";
    case FileOperationManager::Kind::Restore:
      return count == 1 ? "Restored " + quotedBasename(operation.source) : "Restored " + quantity;
    case FileOperationManager::Kind::EmptyWastebin:
      return "Wastebin emptied";
    case FileOperationManager::Kind::Properties:
      return operation.status;
    case FileOperationManager::Kind::Delete:
      return count == 1 ? "Permanently deleted " + quotedBasename(operation.source) : "Permanently deleted " + quantity;
    }
    return "File operation completed";
  }

  std::string failedDescription(const FileOperationManager::Operation& operation, std::string detail) {
    if (detail.find("Cannot copy a directory into itself") != std::string::npos)
      return "Cannot copy " + quotedBasename(operation.source) + " into itself";
    const auto replacePath = [&detail](const std::string& path) {
      if (path.empty() || path.size() < 2)
        return;
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
    if (detail.size() > kMaxErrorDescription)
      detail = detail.substr(0, kMaxErrorDescription - 1) + "…";
    return detail;
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
    if (wastebin_fallback::productionWastebinForbidden()) {
      path.clear();
      message = "Production Wastebin metadata access is disabled in test isolation mode.";
      return false;
    }
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
    if (wastebin_fallback::productionWastebinForbidden()) {
      uris.clear();
      message = "Production Wastebin access is disabled in test isolation mode.";
      return false;
    }
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

  bool gioMoveWithBackup(
      GFile* source, GFile* target, GFileType incomingType, bool replace,
      const std::optional<fs_compat::EntryIdentity>& expectedTarget, GCancellable* cancellable, std::string& message,
      bool& mutated
  ) {
    GError* error = nullptr;
    std::optional<fs_compat::ReplacementBackup> backup;
    bool backupMoved = false;
    bool preserveBackup = false;
    bool ok = true;
    g_autofree char* sourcePath = g_file_get_path(source);
    g_autofree char* targetPath = g_file_get_path(target);
    const bool isDirectory = incomingType == G_FILE_TYPE_DIRECTORY;
    const bool isSymlink = incomingType == G_FILE_TYPE_SYMBOLIC_LINK;

    if (sourcePath == nullptr && targetPath != nullptr) {
      ok = fs_compat::preflightTransferDestination(
          std::filesystem::path(targetPath).parent_path(), isDirectory, isSymlink, replace, &error
      );
    }
    if (ok && replace) {
      ok = fs_compat::allocateReplacementBackup(targetPath, expectedTarget, backup, message, &error);
      backupMoved = ok;
      mutated |= ok || !message.empty();
    }
    if (ok) {
      int directErrno = 0;
      if (sourcePath != nullptr && targetPath != nullptr) {
        const auto outcome = fs_compat::moveOrPublishEntryNoReplace(sourcePath, targetPath, false, std::nullopt, true);
        ok = outcome.ok;
        directErrno = outcome.savedErrno;
        mutated |= outcome.destinationCommitted;
        if (outcome.splitState) {
          preserveBackup = backupMoved;
          message = outcome.message
              + (backupMoved ? "; previous destination backup remains at " + backup->backupPath.string()
                             : std::string{});
        } else if (!ok) {
          setErrorFromPublishOutcome(outcome, &error);
        }
      } else {
        ok = false;
      }
      if (!ok && !preserveBackup && (sourcePath == nullptr || directErrno == EXDEV)) {
        g_clear_error(&error);
        GFile* parent = g_file_get_parent(target);
        if (parent == nullptr) {
          g_set_error_literal(
              &error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not resolve the destination parent folder."
          );
        } else {
          g_autofree char* parentPath = g_file_get_path(parent);
          const bool preflightOk = parentPath != nullptr
              && fs_compat::preflightTransferDestination(parentPath, isDirectory, isSymlink, replace, &error);
          if (!preflightOk) {
            g_object_unref(parent);
          } else {
            char* uuid = g_uuid_string_random();
            char* leaf = g_strdup_printf(".nythera-restore-%s", uuid);
            GFile* staged = g_file_get_child(parent, leaf);
            g_free(leaf);
            g_free(uuid);
            g_object_unref(parent);
            const bool absent = !g_file_query_exists(staged, cancellable);
            g_autofree char* stagedPath = g_file_get_path(staged);
            bool destinationCommitted = false;
            if (!absent) {
              g_set_error_literal(
                  &error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary restore destination."
              );
            } else if (!copyTree(source, staged, cancellable, &error, nullptr, engine::CopyMetadata::DirectoryModeAndTimes)) {
              // Preserve the source when staging fails.
            } else {
              const auto outcome =
                  fs_compat::moveOrPublishEntryNoReplace(stagedPath, targetPath, true, std::nullopt, true);
              destinationCommitted = outcome.destinationCommitted;
              mutated |= destinationCommitted;
              if (outcome.splitState) {
                preserveBackup = backupMoved;
                message = outcome.message
                    + "; source remains in Wastebin"
                    + (backupMoved ? "; previous destination backup remains at " + backup->backupPath.string()
                                   : std::string{});
              } else if (!outcome.ok) {
                setErrorFromPublishOutcome(outcome, &error);
              } else {
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
                  message = std::string("A restored copy remains at ")
                      + (targetPath == nullptr ? "the destination" : targetPath)
                      + "; the original could not be removed from Wastebin"
                      + (backupMoved ? "; previous destination backup remains at " + backup->backupPath.string()
                                     : std::string{});
                }
              }
            }
            if (!destinationCommitted && absent && g_file_query_exists(staged, nullptr)) {
              cleanupStagedTree(staged);
            }
            g_object_unref(staged);
          }
        }
      }
    }
    if (!ok && backupMoved && !preserveBackup && backup) {
      fs_compat::restoreOrReportBackup(*backup, "Restore failed", error, message);
    }
    if (!ok && message.empty())
      message = error == nullptr ? "Could not restore Wastebin item." : error->message;
    g_clear_error(&error);
    if (ok && backup) {
      ok = fs_compat::removeOrReportBackup(
          *backup, "Restored item, but could not remove the replaced destination backup: ", message
      );
    }
    return ok;
  }

  // Replay is keyed to the recorded directory as well as the entry. Pin it
  // before validation so a concurrent rename or retargeted parent symlink
  // cannot redirect a later filesystem call to a different directory.
  struct ReplayParent {
    int fd = -1;
    std::filesystem::path path;
    std::filesystem::path logicalDirectory;
    explicit ReplayParent(const std::string& entry, FileIdentity expected) {
      if (expected == FileIdentity{})
        return;
      const auto directory = std::filesystem::path(entry).parent_path();
      fd = open(directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
      if (fd < 0)
        return;
      struct stat state{};
      if (fstat(fd, &state) == 0 && FileIdentity{state.st_dev, state.st_ino} == expected) {
        path = "/proc/self/fd/" + std::to_string(fd);
        logicalDirectory = directory;
      }
    }
    ReplayParent(const ReplayParent&) = delete;
    ReplayParent& operator=(const ReplayParent&) = delete;
    ~ReplayParent() {
      if (fd >= 0)
        close(fd);
    }
  };

  bool changedEntry(GError** error, const char* message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, message);
    return false;
  }

  bool emptyDirectory(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_empty(path, error) && !error;
  }

} // namespace

struct FileOperationManager::WorkerDrain {
  std::mutex mutex;
  std::condition_variable cv;
  std::size_t active = 0;
};

struct FileOperationManager::Result : engine::Result {
  bool historyBlocked = false;
  std::string customTitle;
  bool outcomeUncertain = false;
  std::optional<std::size_t> requestedCount;
  filemanager::PropertyResult metadata;
  std::optional<TrashFallbackOffer> trashFallback;
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
  std::shared_ptr<WorkerDrain> drain;
  ConflictResolver conflictResolver;
  WastebinBackend wastebinBackend;
  std::function<void(std::uint64_t, std::uint64_t)> boundary;
  bool keepBoth = false;
  std::function<std::vector<filemanager::AdminServerFrame>()> waitForAdminFrames;
  std::function<bool(std::uint64_t, const ConflictDecision&)> replyToAdminConflict;
  std::optional<HistoryEntry> replayEntry;
};


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
  const auto destinationLeaf = basename(entry.destination);
  const auto destinationPath = (destinationParent.path / destinationLeaf).string();
  const auto destinationLogicalPath = (destinationParent.logicalDirectory / destinationLeaf).string();
  const auto sourceLeaf = readsSource ? basename(entry.source) : std::string{};
  const auto sourcePath = readsSource ? (sourceParent.path / sourceLeaf).string() : std::string{};
  const auto sourceLogicalPath = readsSource ? (sourceParent.logicalDirectory / sourceLeaf).string() : std::string{};

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
  const auto restoreMovedItem = [&](const std::string& from, const std::string& to, const std::string& location,
                                    const std::string& fromHook, const std::string& toHook, int fromFd, int toFd) {
    const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
        from.c_str(), to.c_str(), false, std::nullopt, true, fromHook.c_str(), toHook.c_str(), fromFd, toFd
    );
    if (outcome.ok)
      return changedEntry(error, "The item changed during the operation and was put back.");
    g_set_error(
        error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, "The item could not be restored and remains at %s.", location.c_str()
    );
    return false;
  };
  if (moving) {
    const auto& from = undoing ? destinationPath : sourcePath;
    const auto& to = undoing ? sourcePath : destinationPath;
    const auto& fromHook = undoing ? destinationLogicalPath : sourceLogicalPath;
    const auto& toHook = undoing ? sourceLogicalPath : destinationLogicalPath;
    const int fromFd = undoing ? destinationParent.fd : sourceParent.fd;
    const int toFd = undoing ? sourceParent.fd : destinationParent.fd;
    const auto& expected = undoing ? entry.resultRevision : *entry.sourceRevision;
    if (!expectedAt(from, expected))
      return changedEntry(error, "The item is missing, replaced, or has been modified.");
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
      return false;
    const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
        from.c_str(), to.c_str(), false, toCompatIdentity(expected), S_ISLNK(expected.mode), fromHook.c_str(),
        toHook.c_str(), fromFd, toFd
    );
    if (outcome.splitState)
      return changedEntry(error, outcome.message.c_str());
    if (!outcome.ok) {
      setErrorFromPublishOutcome(outcome, error);
      return false;
    }
    const auto actual = revision(to);
    if (!afterMove(actual, expected) || (S_ISDIR(expected.mode) && !emptyDirectory(to)))
      return restoreMovedItem(to, from, undoing ? entry.source : entry.destination, toHook, fromHook, toFd, fromFd);
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

  if (!undoing && entry.kind == Kind::CreateFolder) {
    const auto caps = fs_compat::probeCapabilities(destinationParent.logicalDirectory);
    if (caps.renameNoReplace == fs_compat::CapabilitySupport::Unsupported) {
      int mkdirErr = 0;
      if (!fs_compat::createDirectoryExclusiveAt(
              destinationParent.fd, destinationLeaf.c_str(), entry.resultRevision.mode & 07777, mkdirErr, error
          )) {
        return false;
      }
      const auto created = revision(destinationPath);
      if (!created || created->mode != entry.resultRevision.mode) {
        unlinkat(destinationParent.fd, destinationLeaf.c_str(), AT_REMOVEDIR);
        return reject(error, "The recreated item's permissions differ from the original result.");
      }
      entry.resultRevision = *created;
      return true;
    }
  }

  // Isolate an output before deleting it. If the directory entry changed
  // between validation and rename, recover it without overwriting anything.
  // The private directory also gives redo a no-overwrite publication point.
  PrivateStaging staging(destinationPath);
  if (staging.directory.empty())
    return reject(error, "Could not create a private staging directory beside the destination.");
  const std::string stagingFolder = basename(staging.directory);
  const std::string staged = staging.directory + "/item";
  const std::string stagedHook = (destinationParent.logicalDirectory / stagingFolder / "item").string();
  if (undoing) {
    const auto recovery = (std::filesystem::path(entry.destination).parent_path() / stagingFolder / "item").string();
    if (g_cancellable_set_error_if_cancelled(cancellable, error))
      return false;
    bool isolated = false;
    int renameErr = 0;
    GError* noReplaceErr = nullptr;
    if (fs_compat::renameNoReplaceRaw(destinationLogicalPath.c_str(), stagedHook.c_str(), renameErr, &noReplaceErr)) {
      isolated = true;
    } else if (fs_compat::isNoReplaceUnsupportedErrno(renameErr)) {
      g_clear_error(&noReplaceErr);
      // staging.directory was created exclusively with mkdtemp (0700), so "item" is absent.
      const int stagingFd = open(staging.directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
      if (stagingFd >= 0) {
        if (renameat(destinationParent.fd, destinationLeaf.c_str(), stagingFd, "item") == 0)
          isolated = true;
        else
          g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(errno), g_strerror(errno));
        close(stagingFd);
      } else {
        g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(errno), g_strerror(errno));
      }
    } else {
      g_propagate_error(error, noReplaceErr);
    }
    if (!isolated)
      return false;
    const auto actual = revision(staged);
    if (!afterMove(actual, entry.resultRevision) || (copying && !expectedAt(sourcePath, *entry.sourceRevision)))
      return restoreMovedItem(
          staged, destinationPath, recovery, stagedHook, destinationLogicalPath, -1, destinationParent.fd
      );
    const int removed = S_ISDIR(entry.resultRevision.mode) ? rmdir(staged.c_str()) : unlink(staged.c_str());
    return removed == 0
        || restoreMovedItem(
               staged, destinationPath, recovery, stagedHook, destinationLogicalPath, -1, destinationParent.fd
        );
  }

  GFile* target = g_file_new_for_path(staged.c_str());
  bool ok = false;
  bool destinationCommitted = false;
  if (copying) {
    GFile* source = g_file_new_for_path(sourcePath.c_str());
    ok = copyTree(source, target, cancellable, error, nullptr);
    g_object_unref(source);
    if (ok && !expectedAt(sourcePath, *entry.sourceRevision))
      ok = changedEntry(error, "The source changed during copying.");
  } else {
    bool created = false;
    ok = engine::createItem(
        *localKind(entry.kind), target, entry.linkTarget, entry.createdContent, {}, cancellable, error, created
    );
  }
  if (ok) {
    const auto created = revision(staged);
    if (!created || created->mode != entry.resultRevision.mode)
      ok = reject(error, "The recreated item's permissions differ from the original result.");
    else if (!g_cancellable_set_error_if_cancelled(cancellable, error)) {
      const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
          staged.c_str(), destinationPath.c_str(), true, toCompatIdentity(*created), true, stagedHook.c_str(),
          destinationLogicalPath.c_str(), -1, destinationParent.fd
      );
      ok = outcome.ok;
      destinationCommitted = outcome.destinationCommitted;
      if (!ok) {
        if (outcome.splitState)
          reject(error, outcome.message.c_str());
        else
          setErrorFromPublishOutcome(outcome, error);
      }
    } else {
      ok = false;
    }
    if (ok) {
      const auto actual = revision(destinationPath);
      entry.resultRevision = afterMove(actual, *created) ? *actual : Revision{};
    }
  }
  if (!destinationCommitted)
    g_file_delete(target, nullptr, nullptr);
  g_object_unref(target);
  return ok;
}

FileOperationManager::FileOperationManager(Changed changed, WastebinBackend wastebinBackend)
    : m_workerDrain(std::make_shared<WorkerDrain>()), m_changed(std::move(changed)),
      m_wastebinBackend(std::move(wastebinBackend)) {
  GError* error = nullptr;
  m_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  g_clear_error(&error);
  if (m_bus != nullptr)
    m_serviceWatch = g_bus_watch_name_on_connection(
        m_bus, "org.kde.JobViewServer", G_BUS_NAME_WATCHER_FLAGS_NONE, serviceAppeared, serviceVanished, this, nullptr
    );
}
void FileOperationManager::shutdown() noexcept {
  if (m_alive)
    *m_alive = false;
  if (m_rateTimer) {
    g_source_remove(m_rateTimer);
    m_rateTimer = 0;
  }
  for (const auto& [id, cancellable] : m_cancellables) {
    (void)id;
    g_cancellable_cancel(cancellable);
  }
  for (const auto& [id, control] : m_adminControls) {
    (void)id;
    if (control.stop)
      control.stop();
  }
  for (const auto& [id, pause] : m_pauseStates) {
    (void)id;
    pause->cancel();
  }
  for (const auto& [id, timer] : m_jobRevealTimers) {
    (void)id;
    g_source_remove(timer);
  }
  m_jobRevealTimers.clear();
  if (m_serviceWatch != 0) {
    g_bus_unwatch_name(m_serviceWatch);
    m_serviceWatch = 0;
  }
  if (m_bus != nullptr) {
    for (const auto& [id, subscription] : m_signalSubscriptions) {
      (void)id;
      g_dbus_connection_signal_unsubscribe(m_bus, subscription);
    }
  }
  m_signalSubscriptions.clear();
  if (m_bus != nullptr) {
    g_object_unref(m_bus);
    m_bus = nullptr;
  }
  if (m_workerDrain) {
    std::unique_lock lock(m_workerDrain->mutex);
    m_workerDrain->cv.wait(lock, [this] { return m_workerDrain->active == 0; });
  }
  for (auto& [id, cancellable] : m_cancellables) {
    (void)id;
    g_cancellable_cancel(cancellable);
    g_object_unref(cancellable);
  }
  m_cancellables.clear();
  m_adminControls.clear();
  m_pauseStates.clear();
  m_activePaths.clear();
}
FileOperationManager::~FileOperationManager() { shutdown(); }
bool FileOperationManager::active() const noexcept {
  return std::ranges::any_of(m_operations, [](const Operation& op) { return !op.finished; });
}
bool FileOperationManager::usesMount(const std::filesystem::path& mountPath) const {
  if (mountPath.empty())
    return active();
  // Test path components, never string prefixes ("/media/a" is not "/media/ab").
  // Keep both lexical and canonical paths so symlink aliases and paths removed
  // during a move remain protected. Ancestors also cover recursive operations
  // whose directory trees contain another mount.
  auto contains = [](const auto& parent, const auto& child) {
    auto p = parent.begin();
    auto c = child.begin();
    for (; p != parent.end(); ++p, ++c)
      if (c == child.end() || *p != *c)
        return false;
    return true;
  };
  std::error_code error;
  const auto lexical = std::filesystem::absolute(mountPath, error).lexically_normal();
  if (error)
    return active();
  const auto canonical = std::filesystem::weakly_canonical(lexical, error);
  for (const auto& [id, paths] : m_activePaths) {
    (void)id;
    for (const auto& path : paths) {
      if (path.empty() || contains(lexical, path) || contains(path, lexical))
        return true;
      if (!error && (contains(canonical, path) || contains(path, canonical)))
        return true;
    }
  }
  return false;
}
bool FileOperationManager::hasJobService() const noexcept {
  if (!m_serviceAvailable)
    return false;
  return std::ranges::all_of(m_operations, [this](const Operation& operation) {
    return operation.finished
        || !reportsProgress(operation.kind)
        || !operation.progressRevealed
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
  if (found == m_transferRates.end())
    return false;
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
    operation.status = std::string(processed)
        + " / "
        + total
        + " · "
        + std::to_string(operation.processedFiles)
        + " / "
        + std::to_string(operation.totalFiles)
        + (operation.adminOrigin ? " items" : " files")
        + (display > 0 ? std::string(" · ") + speed + "/s" : std::string{});
    g_free(processed);
    g_free(total);
    g_free(speed);
  }
  return changed;
}

void FileOperationManager::updateProgress(
    std::uint64_t id, std::uint64_t bytes, std::uint64_t totalBytes, std::uint64_t files, std::uint64_t totalFiles,
    std::uint64_t speed
) {
  if (auto* op = find(id); op && !op->finished) {
    op->processedBytes = bytes;
    op->totalBytes = totalBytes;
    op->processedFiles = files;
    op->totalFiles = totalFiles;
    op->speedBytesPerSecond = speed;
    if (totalBytes > 0)
      op->fraction = std::clamp(static_cast<double>(bytes) / static_cast<double>(totalBytes), 0.0, 1.0);
    else if (totalFiles > 0)
      op->fraction = std::clamp(static_cast<double>(files) / static_cast<double>(totalFiles), 0.0, 1.0);
    if (totalBytes > 0) {
      updateTransferMetrics(*op);
    } else
      op->status = std::to_string(files) + " / " + std::to_string(totalFiles) + " items";
    publishJob(*op);
    if (m_changed)
      m_changed(*op);
  }
}
void FileOperationManager::cancel(std::uint64_t id) {
  if (const auto control = m_adminControls.find(id); control != m_adminControls.end() && control->second.cancel)
    (void)control->second.cancel();
  auto it = std::ranges::find(m_cancellables, id, &std::pair<std::uint64_t, GCancellable*>::first);
  if (it != m_cancellables.end())
    g_cancellable_cancel(it->second);
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end())
    state->second->cancel();
}
void FileOperationManager::pause(std::uint64_t id) {
  if (const auto control = m_adminControls.find(id); control != m_adminControls.end() && control->second.setPaused)
    (void)control->second.setPaused(true);
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end())
    state->second->pause();
}
void FileOperationManager::resume(std::uint64_t id) {
  if (const auto control = m_adminControls.find(id); control != m_adminControls.end() && control->second.setPaused)
    (void)control->second.setPaused(false);
  if (const auto state = m_pauseStates.find(id); state != m_pauseStates.end())
    state->second->resume();
}
void FileOperationManager::updatePaused(std::uint64_t id, bool paused) {
  if (auto* operation = find(id); operation && !operation->finished) {
    operation->paused = paused;
    updateTransferMetrics(*operation);
    publishJob(*operation, true, true);
    if (m_changed)
      m_changed(*operation);
  }
}

bool FileOperationManager::administratorOperationActive(filemanager::TabId tabId) const {
  return std::ranges::any_of(m_operations, [tabId](const auto& operation) {
    return !operation.finished && operation.adminOrigin && operation.adminOrigin->tabId == tabId;
  });
}

std::uint64_t FileOperationManager::copy(
    std::vector<std::string> sources, std::string destination, filemanager::BrowserTab* adminTab
) {
  return start(Kind::Copy, std::move(sources), std::move(destination), {}, 0, {}, {}, {}, false, adminTab);
}
std::uint64_t FileOperationManager::duplicate(
    std::vector<std::string> sources, std::string destination, filemanager::BrowserTab* adminTab
) {
  return start(Kind::Duplicate, std::move(sources), std::move(destination), {}, 0, {}, {}, {}, false, adminTab);
}
std::uint64_t FileOperationManager::move(
    std::vector<std::string> sources, std::string destination, filemanager::BrowserTab* adminTab
) {
  return start(Kind::Move, std::move(sources), std::move(destination), {}, 0, {}, {}, {}, false, adminTab);
}
std::uint64_t FileOperationManager::rename(std::string source, std::string destination) {
  return start(Kind::Rename, {std::move(source)}, std::move(destination));
}
std::uint64_t FileOperationManager::renameChecked(
    std::string source, std::string destination, std::uint64_t device, std::uint64_t inode,
    std::function<void(bool, const std::string&)> completed, filemanager::BrowserTab* adminTab
) {
  return start(
      Kind::Rename, {std::move(source)}, std::move(destination), {}, 0, {}, std::pair{device, inode},
      std::move(completed), false, adminTab
  );
}

std::uint64_t FileOperationManager::createFolder(std::string destination, filemanager::BrowserTab* adminTab) {
  return start(Kind::CreateFolder, {}, std::move(destination), {}, 0, {}, {}, {}, adminTab != nullptr, adminTab);
}
std::uint64_t FileOperationManager::createFile(std::string destination, filemanager::BrowserTab* adminTab) {
  return start(Kind::CreateFile, {}, std::move(destination), {}, 0, {}, {}, {}, adminTab != nullptr, adminTab);
}

std::uint64_t FileOperationManager::createFileFromTemplate(
    std::string templateSource, std::string destination, filemanager::BrowserTab* adminTab
) {
  return start(Kind::CreateFile, {std::move(templateSource)}, std::move(destination),
               {}, 0, {}, {}, {}, adminTab != nullptr, adminTab);
}
std::uint64_t FileOperationManager::createSymlink(
    std::string target, std::string destination, bool keepBoth, filemanager::BrowserTab* adminTab
) {
  return start(Kind::CreateSymlink, {std::move(target)}, std::move(destination), {}, 0, {}, {}, {}, keepBoth, adminTab);
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
std::uint64_t FileOperationManager::trash(std::vector<std::string> sources) {
  return start(Kind::Trash, std::move(sources), {});
}
std::uint64_t FileOperationManager::trashToHomeWastebin(std::vector<std::string> sources) {
  return start(Kind::TrashToHome, std::move(sources), {});
}
std::uint64_t FileOperationManager::restore(std::vector<std::string> trashUris, std::string destinationDirectory) {
  return start(Kind::Restore, std::move(trashUris), std::move(destinationDirectory));
}
std::uint64_t FileOperationManager::emptyWastebin(bool confirmed) {
  if (!confirmed)
    return 0;
  return start(Kind::EmptyWastebin, {}, {});
}
std::uint64_t FileOperationManager::removePermanently(
    std::vector<std::string> sources, filemanager::BrowserTab* adminTab
) {
  return start(Kind::Delete, std::move(sources), {}, {}, 0, {}, {}, {}, false, adminTab);
}

std::uint64_t FileOperationManager::modifyProperties(
    filemanager::PropertyRequest request, std::uint32_t uid,
    std::function<void(const filemanager::PropertyResult&)> completed, filemanager::BrowserTab* adminTab
) {
  const auto identity = std::pair{request.device, request.inode};
  for (const auto& [id, target] : m_metadataTargets) {
    (void)id;
    if (target == identity)
      return 0;
  }
  const auto path = request.path.string();
  const auto id = start(Kind::Properties, {path}, {}, request, uid, std::move(completed), {}, {}, false, adminTab);
  if (id)
    m_metadataTargets[id] = identity;
  return id;
}

std::uint64_t FileOperationManager::start(
    Kind kind, std::vector<std::string> sources, std::string destination,
    std::optional<filemanager::PropertyRequest> metadata, std::uint32_t uid,
    std::function<void(const filemanager::PropertyResult&)> completedCallback,
    std::optional<std::pair<std::uint64_t, std::uint64_t>> renameIdentity,
    std::function<void(bool, const std::string&)> renameCompleted, bool keepBoth, filemanager::BrowserTab* adminTab
) {
  if (m_replaying || !m_alive || !*m_alive)
    return 0;
  const bool propertyEdit = kind == Kind::Properties && metadata.has_value();
  if (adminTab && (!adminTab->hasActiveAdminSession() || administratorOperationActive(adminTab->id)
                   || (!propertyEdit && (!adminTab->capabilities().canMutate || !localKind(kind)))))
    return 0;
  m_replaying = isReplay(kind);
  std::optional<HistoryEntry> replayEntry;
  if (m_replaying)
    replayEntry = (kind == Kind::Undo ? m_undo : m_redo).back();
  const auto id = m_nextId++;
  Operation operation{
      .id = id,
      .kind = kind,
      .source = sources.empty() ? std::string{} : sources.front(),
      .destination = destination,
      .status = "Queued",
      .requestedItemCount = std::max<std::size_t>(1, sources.size())
  };
  if (kind == Kind::Properties)
    operation.status = "Preparing property changes / authorisation…";
  if (adminTab) {
    operation.status = "Preparing Administrator operation…";
    operation.adminOrigin = AdminOperationOrigin{
        adminTab->id, adminTab->adminSession.sessionGeneration(), adminTab->session.navigationGeneration(),
        adminTab->session.path()
    };
  }
  m_operations.push_back(operation);
  auto& activePaths = m_activePaths[id];
  auto trackPath = [&](const std::string& rawPath) {
    if (rawPath.empty())
      return;
    std::error_code error;
    const auto lexical = std::filesystem::absolute(rawPath, error).lexically_normal();
    if (error) {
      activePaths.emplace_back();
      return;
    }
    activePaths.push_back(lexical);
    if (adminTab)
      return;
    const auto canonical = std::filesystem::weakly_canonical(lexical, error);
    if (!error && canonical != lexical)
      activePaths.push_back(canonical);
  };
  if (kind != Kind::Restore && kind != Kind::CreateSymlink)
    for (const auto& source : sources)
      trackPath(source);
  trackPath(destination);
  if (adminTab || kind == Kind::Restore || kind == Kind::EmptyWastebin || kind == Kind::TrashToHome)
    activePaths.emplace_back(); // Paths that the GUI cannot resolve require conservative mount protection.
  if (reportsProgress(kind)) {
    struct RevealRequest {
      FileOperationManager* self;
      std::weak_ptr<bool> alive;
      std::uint64_t operationId;
    };
    const auto timer = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 450,
        [](gpointer data) -> gboolean {
          auto* request = static_cast<RevealRequest*>(data);
          const auto locked = request->alive.lock();
          if (!locked || !*locked)
            return G_SOURCE_REMOVE;
          auto* self = request->self;
          const auto operationId = request->operationId;
          self->m_jobRevealTimers.erase(operationId);
          if (auto* activeOperation = self->find(operationId);
              activeOperation != nullptr && !activeOperation->finished) {
            activeOperation->progressRevealed = true;
            self->registerJob(*activeOperation);
            self->publishJob(*activeOperation);
            if (self->m_changed)
              self->m_changed(*activeOperation);
          }
          return G_SOURCE_REMOVE;
        },
        new RevealRequest{this, m_alive, id}, [](gpointer data) { delete static_cast<RevealRequest*>(data); }
    );
    m_jobRevealTimers.emplace(id, timer);
  }
  const auto alive = m_alive;
  if (m_changed && !adminTab)
    m_changed(m_operations.back());
  if (!alive || !*alive)
    return 0;
  auto* taskData = new Task{this, id, kind, std::move(sources), std::move(destination)};
  taskData->keepBoth = keepBoth;
  taskData->replayEntry = std::move(replayEntry);
  taskData->alive = m_alive;
  taskData->drain = m_workerDrain;
  taskData->conflictResolver = adminTab ? m_adminConflictResolver : m_conflictResolver;
  taskData->wastebinBackend = m_wastebinBackend;
  GCancellable* cancellable = g_cancellable_new();
  if (usesTransferControls(kind)) {
    if (!adminTab) {
      taskData->pause = std::make_shared<FileTransferPauseState>();
      taskData->boundary = m_transferBoundary;
      m_pauseStates.emplace(id, taskData->pause);
    }
    m_transferRates.emplace(id, filemanager::TransferRates{});
    struct RateTimerData {
      FileOperationManager* self;
      std::weak_ptr<bool> alive;
    };
    if (!m_rateTimer)
      m_rateTimer = g_timeout_add_full(
          G_PRIORITY_DEFAULT, 100,
          [](gpointer value) -> gboolean {
            auto* state = static_cast<RateTimerData*>(value);
            const auto locked = state->alive.lock();
            if (!locked || !*locked)
              return G_SOURCE_REMOVE;
            auto* self = state->self;
            bool active = false;
            for (std::size_t idx = 0; idx < self->m_operations.size(); ++idx) {
              auto& activeTransfer = self->m_operations[idx];
              if (activeTransfer.finished || !self->m_transferRates.contains(activeTransfer.id))
                continue;
              active = true;
              if (activeTransfer.totalBytes > 0 && self->updateTransferMetrics(activeTransfer)) {
                self->publishJob(activeTransfer, true);
                if (self->m_changed)
                  self->m_changed(activeTransfer);
                if (!*locked)
                  return G_SOURCE_REMOVE;
              }
            }
            if (!active)
              self->m_rateTimer = 0;
            return active ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
          },
          new RateTimerData{this, m_alive}, [](gpointer value) { delete static_cast<RateTimerData*>(value); }
      );
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
  {
    std::lock_guard lock(m_workerDrain->mutex);
    ++m_workerDrain->active;
  }
  if (adminTab) {
    const auto request = propertyEdit
        ? engine::Request{.kind = engine::Kind::ModifyProperties, .destination = taskData->metadata->path.string(),
                          .properties = taskData->metadata}
        : engine::Request{*localKind(kind), taskData->sources, taskData->destination, renameIdentity, keepBoth};
    auto transport = adminTab->adminSession.runOperation(
        request, operation.adminOrigin->sessionGeneration, cancellable
    );
    taskData->waitForAdminFrames = std::move(transport.waitForFrames);
    taskData->replyToAdminConflict = [reply = std::move(transport.sendConflictReply), cancel = transport.cancel](
                                         auto conflict, const auto& decision
                                     ) {
      if (!reply(conflict, decision))
        return cancel();
      return true;
    };
    m_adminControls.emplace(id, std::move(transport));
  }
  g_task_run_in_thread(task, worker);
  if (adminTab && m_changed)
    m_changed(m_operations.back());
  g_object_unref(task);
  g_object_unref(cancellable);
  return id;
}

void FileOperationManager::serviceAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  self->m_serviceAvailable = true;
  for (const auto& operation : self->m_operations)
    if (!operation.finished && reportsProgress(operation.kind) && !self->m_jobRevealTimers.contains(operation.id)) {
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
    if (!operation.finished && self->m_changed)
      self->m_changed(operation);
}
void FileOperationManager::jobSignal(
    GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar* signal, GVariant*, gpointer userData
) {
  auto* self = static_cast<FileOperationManager*>(userData);
  for (const auto& [id, jobPath] : self->m_jobPaths)
    if (jobPath == path) {
      if (g_str_equal(signal, "cancelRequested"))
        self->cancel(id);
      else if (g_str_equal(signal, "suspendRequested"))
        self->pause(id);
      else if (g_str_equal(signal, "resumeRequested"))
        self->resume(id);
      return;
    }
}
void FileOperationManager::registerJob(const Operation& operation) {
  if (m_bus == nullptr || !m_serviceAvailable || m_jobPaths.contains(operation.id))
    return;
  GVariantBuilder hints;
  g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&hints, "{sv}", "title", g_variant_new_string("File operation"));
  GError* error = nullptr;
  GVariant* response = g_dbus_connection_call_sync(
      m_bus, "org.kde.JobViewServer", "/JobViewServer", "org.kde.JobViewServerV2", "requestView",
      g_variant_new(
          "(si@a{sv})", "org.nythera.FileManager", usesTransferControls(operation.kind) ? 3 : 1,
          g_variant_builder_end(&hints)
      ),
      G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error
  );
  if (response == nullptr) {
    g_clear_error(&error);
    return;
  }
  const gchar* objectPath = nullptr;
  g_variant_get(response, "(&o)", &objectPath);
  const std::string ownedPath(objectPath);
  g_variant_unref(response);
  m_jobPaths.emplace(operation.id, ownedPath);
  const guint sub = g_dbus_connection_signal_subscribe(
      m_bus, "org.kde.JobViewServer", "org.kde.JobViewV3", nullptr, ownedPath.c_str(), nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, jobSignal, this, nullptr
  );
  m_signalSubscriptions.emplace(operation.id, sub);
  if (operation.paused)
    publishJob(operation, true, true);
}
void FileOperationManager::publishJob(const Operation& operation, bool metricsOnly, bool suspensionAcknowledgement) {
  const auto path = m_jobPaths.find(operation.id);
  if (m_bus == nullptr || !m_serviceAvailable || path == m_jobPaths.end())
    return;
  GVariantBuilder properties;
  g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
  if (m_transferRates.contains(operation.id)) {
    g_variant_builder_add(
        &properties, "{sv}", "displaySpeed", g_variant_new_uint64(operation.displaySpeedBytesPerSecond)
    );
    g_variant_builder_add(&properties, "{sv}", "etaSpeed", g_variant_new_uint64(operation.etaBytesPerSecond));
  }
  if (suspensionAcknowledgement)
    g_variant_builder_add(&properties, "{sv}", "suspended", g_variant_new_boolean(operation.paused));
  if (!metricsOnly) {
    const char* title = operation.finished      ? operation.resultTitle.c_str()
        : operation.kind == Kind::Duplicate     ? "Duplicating items"
        : operation.kind == Kind::Copy          ? "Copying files"
        : operation.kind == Kind::Move          ? "Moving files"
        : operation.kind == Kind::Rename        ? "Renaming item"
        : operation.kind == Kind::Trash         ? "Moving to Trash"
        : operation.kind == Kind::TrashToHome   ? "Moving to Home Wastebin"
        : operation.kind == Kind::Delete        ? "Deleting items"
        : operation.kind == Kind::Restore       ? "Restoring items"
        : operation.kind == Kind::EmptyWastebin ? "Emptying Wastebin"
        : operation.kind == Kind::Properties    ? "Changing properties"
        : operation.kind == Kind::Undo          ? "Undoing file operation"
        : operation.kind == Kind::Redo          ? "Redoing file operation"
                                                : "Creating item";
    g_variant_builder_add(&properties, "{sv}", "title", g_variant_new_string(title));
    g_variant_builder_add(
        &properties, "{sv}", "infoMessage",
        g_variant_new_string(operation.finished ? operation.resultDescription.c_str() : operation.status.c_str())
    );
    g_variant_builder_add(
        &properties, "{sv}", "percent", g_variant_new_uint32(static_cast<guint32>(operation.fraction * 100.0))
    );
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
      if (uri != nullptr) {
        g_variant_builder_add(&properties, "{sv}", "destUrl", g_variant_new_string(uri));
        g_free(uri);
      }
    }
  } else {
    g_variant_builder_add(&properties, "{sv}", "infoMessage", g_variant_new_string(operation.status.c_str()));
  }
  g_dbus_connection_call(
      m_bus, "org.kde.JobViewServer", path->second.c_str(), "org.kde.JobViewV3", "update",
      g_variant_new("(@a{sv})", g_variant_builder_end(&properties)), nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer) {
        GError* callError = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &callError);
        if (reply != nullptr)
          g_variant_unref(reply);
        g_clear_error(&callError);
      },
      nullptr
  );
}
void FileOperationManager::finishJob(const Operation& operation) {
  if (const auto timer = m_jobRevealTimers.find(operation.id); timer != m_jobRevealTimers.end()) {
    g_source_remove(timer->second);
    m_jobRevealTimers.erase(timer);
  }
  const auto path = m_jobPaths.find(operation.id);
  if (m_bus != nullptr && m_serviceAvailable && path != m_jobPaths.end()) {
    const guint subscription = m_signalSubscriptions[operation.id];
    if (subscription != 0)
      g_dbus_connection_signal_unsubscribe(m_bus, subscription);
    m_signalSubscriptions.erase(operation.id);
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        m_bus, "org.kde.JobViewServer", path->second.c_str(), "org.kde.JobViewV3", "terminate",
        g_variant_new(
            "(us@a{sv})",
            operation.succeeded       ? 0U
                : operation.cancelled ? 1U
                                      : 2U,
            operation.status.c_str(), g_variant_builder_end(&hints)
        ),
        nullptr, G_DBUS_CALL_FLAGS_NONE, 1200, nullptr, &error
    );
    if (reply != nullptr)
      g_variant_unref(reply);
    g_clear_error(&error);
    m_jobPaths.erase(path);
  }
}

void FileOperationManager::worker(GTask* task, gpointer, gpointer taskData, GCancellable* cancellable) {
  auto* data = static_cast<Task*>(taskData);
  struct DrainGuard {
    std::shared_ptr<WorkerDrain> drain;
    ~DrainGuard() {
      if (!drain)
        return;
      std::lock_guard lock(drain->mutex);
      if (--drain->active == 0)
        drain->cv.notify_all();
    }
  } drainGuard{data->drain};
  auto* result = new Result{};
  if (data->kind == Kind::Properties && data->metadata && !data->waitForAdminFrames) {
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
              new Report{data->owner, data->id, progress, data->alive}
          );
        });
    result->ok = result->metadata.ok();
    result->cancelled = result->metadata.cancelled;
    result->mutated = result->metadata.succeeded > 0 || result->metadata.ownershipEffects;
    result->message = result->metadata.summary();
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
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

  const auto remember =
      [&](const std::string& source, const std::string& destination, const std::optional<Revision>& sourceRevision,
          const std::optional<Revision>& resultRevision, FileIdentity sourceParent, FileIdentity destinationParent,
          std::string linkTarget, std::optional<std::vector<std::uint8_t>> createdContent) {
        if (!resultRevision
            || destinationParent == FileIdentity{}
            || parentIdentity(destination) != destinationParent
            || (!source.empty()
                && (sourceParent == FileIdentity{} || !sourceRevision || parentIdentity(source) != sourceParent)))
          return;
        const auto& state = *resultRevision;
        if ((!S_ISREG(state.mode) && !S_ISLNK(state.mode) && !S_ISDIR(state.mode))
            || state.links > (S_ISDIR(state.mode) ? 2U : 1U))
          return;
        if (S_ISDIR(state.mode) && (isTransfer(data->kind) || !emptyDirectory(destination)))
          return;
        if ((data->kind == Kind::Rename || data->kind == Kind::Move) && state.device != sourceParent.first)
          return;
        result->history.push_back(
            {data->kind, source, destination, std::move(linkTarget), std::move(createdContent), sourceRevision, state,
             sourceParent, destinationParent}
        );
      };
  engine::TransferCallbacks transferCallbacks;
  transferCallbacks.report = [data](auto bytes, auto totalBytes, auto items, auto totalItems, auto speed) {
    postProgressIdle(data->owner, data->alive, {data->id, bytes, totalBytes, items, totalItems, speed});
  };
  if (data->boundary)
    transferCallbacks.boundary = [data](std::uint64_t bytes) { data->boundary(data->id, bytes); };
  transferCallbacks.acknowledgePause = [owner = data->owner, id = data->id,
                                        alive = std::weak_ptr<bool>(data->alive)](bool paused) {
    using Ack = std::tuple<FileOperationManager*, std::uint64_t, std::weak_ptr<bool>, bool>;
    g_idle_add(
        [](gpointer value) -> gboolean {
          std::unique_ptr<Ack> ack(static_cast<Ack*>(value));
          if (const auto locked = std::get<2>(*ack).lock(); locked && *locked)
            std::get<0>(*ack)->updatePaused(std::get<1>(*ack), std::get<3>(*ack));
          return G_SOURCE_REMOVE;
        },
        new Ack{owner, id, alive, paused}
    );
  };

  if (data->waitForAdminFrames) {
    using namespace filemanager;
    for (;;) {
      auto frames = data->waitForAdminFrames();
      for (auto& frame : frames) {
        switch (frame.kind) {
        case AdminServerFrameKind::OpProgress:
          transferCallbacks.report(
              frame.completedBytes, frame.totalBytes, frame.completedItems, frame.totalItems, frame.speed
          );
          data->completedMeasure = {frame.completedBytes, frame.completedItems};
          data->totalMeasure = {frame.totalBytes, frame.totalItems};
          break;
        case AdminServerFrameKind::OpPaused:
          transferCallbacks.acknowledgePause(frame.paused);
          break;
        case AdminServerFrameKind::OpConflict:
          if (!g_cancellable_is_cancelled(cancellable)) {
            const auto decision = data->conflictResolver
                ? data->conflictResolver(
                      frame.sourcePath, frame.destinationPath, frame.hasRemaining, frame.conflictMetadata, cancellable
                  )
                : ConflictDecision{};
            if (!g_cancellable_is_cancelled(cancellable))
              (void)data->replyToAdminConflict(frame.conflictId, decision);
          }
          break;
        case AdminServerFrameKind::OpCompletedEntry:
          result->completedEntries.push_back(std::move(frame.completedEntry));
          break;
        case AdminServerFrameKind::OpMovedSource:
          result->movedSources.push_back(std::move(frame.sourcePath));
          break;
        case AdminServerFrameKind::OpResult:
          if (data->kind == Kind::Properties)
            result->metadata = frame.properties.value_or(filemanager::PropertyResult{
                .cancelled = frame.status == AdminStatusCode::Cancelled, .error = frame.detail
            });
          result->ok = frame.status == AdminStatusCode::Ok;
          result->cancelled = frame.status == AdminStatusCode::Cancelled;
          result->mutated = frame.mutated;
          result->message = std::move(frame.detail);
          result->customDescription = std::move(frame.customDescription);
          result->outcomeUncertain = frame.outcomeUncertain;
          if (frame.outcomeUncertain) {
            result->customTitle = std::string(operationName(data->kind)) + " outcome uncertain";
            result->customDescription = result->message;
          } else if (data->kind != Kind::Delete) {
            data->completedMeasure = {frame.completedBytes, frame.completedItems};
            data->totalMeasure = {frame.totalBytes, frame.totalItems};
          }
          if (result->customDescription.empty()) {
            const auto completedCount = data->kind == Kind::Move ? result->movedSources.size()
                : data->kind == Kind::Delete ? data->completedMeasure.items : result->completedEntries.size();
            if ((isTransfer(data->kind) || data->kind == Kind::Delete)
                && completedCount != data->sources.size()
                && (result->ok || completedCount > 0)) {
              const auto verb = data->kind == Kind::Copy ? "copied"
                  : data->kind == Kind::Duplicate ? "duplicated"
                  : data->kind == Kind::Move ? "moved"
                                             : "deleted";
              result->customDescription =
                  std::format("{} of {} selected items {}", completedCount, data->sources.size(), verb);
              if (!result->message.empty())
                result->customDescription += ". " + result->message;
            } else if (result->cancelled) {
              result->customDescription = result->message;
            }
          }
          if (result->ok && !frame.destinationPath.empty())
            data->destination = std::move(frame.destinationPath);
          g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
          return;
        default:
          break;
        }
      }
    }
  }

  if (const auto kind = localKind(data->kind)) {
    engine::Request request{
        *kind, std::move(data->sources), std::move(data->destination), data->renameIdentity, data->keepBoth
    };
    const engine::Callbacks callbacks{
        .transfer = std::move(transferCallbacks),
        .resolveConflict = std::move(data->conflictResolver),
        .recordChange = remember,
    };
    static_cast<engine::Result&>(*result) =
        engine::executeLocalOperation(request, cancellable, data->pause.get(), callbacks);
    data->destination = std::move(request.destination);
    data->totalMeasure = result->totalMeasure;
    data->completedMeasure = result->completedMeasure;
    g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
    return;
  }
  TransferProgress progress{.callbacks = transferCallbacks, .pause = data->pause.get(), .cancellable = cancellable};
  if (data->kind == Kind::Restore || data->kind == Kind::EmptyWastebin) {
    std::vector<std::string> uris = data->sources;
    if (data->kind == Kind::EmptyWastebin) {
      const bool enumerated = data->wastebinBackend.enumerateTopLevel
          ? data->wastebinBackend.enumerateTopLevel(cancellable, uris, result->message)
          : gioEnumerateTrash(cancellable, uris, result->message);
      if (!enumerated) {
        result->ok = false;
        result->cancelled = cancellable != nullptr && g_cancellable_is_cancelled(cancellable);
        result->customTitle = result->cancelled ? "Empty Wastebin cancelled" : "Empty Wastebin failed";
        result->customDescription = result->message;
        g_task_return_pointer(task, result, [](gpointer value) { delete static_cast<Result*>(value); });
        return;
      }
      if (!data->wastebinBackend.enumerateTopLevel) {
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
        if (wastebin_fallback::productionWastebinForbidden()
            && (uris[i].starts_with("trash:") || isTrashUri(uris[i]))) {
          succeeded = false;
          itemError = "Production Wastebin deletion is disabled in test isolation mode.";
        } else {
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
        }
      } else {
        if (!data->wastebinBackend.originalPath && !isTrashUri(uris[i])) {
          noteFailure(uris[i], "Restore source is not a GIO trash URI.");
          continue;
        }
        std::string originalPath;
        const bool resolved = data->wastebinBackend.originalPath
            ? data->wastebinBackend.originalPath(uris[i], cancellable, originalPath, itemError)
            : gioOriginalPath(uris[i], cancellable, originalPath, itemError);
        if (resolved && !originalPath.empty() && originalPath.front() == '/') {
          const auto original = std::filesystem::path(originalPath);
          const std::string filename = original.filename().string();
          std::filesystem::path destination =
              data->destination.empty() ? original : std::filesystem::path(data->destination) / filename;
          if (const auto originalNameError = filemanager::invalidFileNameReason(filename); !originalNameError.empty())
            itemError = "Invalid original location metadata. " + std::string(originalNameError);
          else if (originalPath.find('\0') != std::string::npos)
            itemError = "The original location contains a NUL character.";
          else if (
              wastebin_fallback::productionWastebinForbidden() && (uris[i].starts_with("trash:") || isTrashUri(uris[i]))
          ) {
            itemError = "Production Wastebin restore is disabled in test isolation mode.";
          } else {
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
            const auto existingBefore =
                existing != nullptr ? fs_compat::queryEntryIdentity(destination.string().c_str()) : std::nullopt;
            const auto incomingType = incoming == nullptr ? G_FILE_TYPE_UNKNOWN : g_file_info_get_file_type(incoming);
            bool replace = false;
            bool proceed = true;
            if (incoming == nullptr) {
              itemError = sourceQueryError == nullptr ? "Could not inspect the Wastebin source item."
                                                      : sourceQueryError->message;
              proceed = false;
            } else if (existing != nullptr) {
              const auto existingType = g_file_info_get_file_type(existing);
              ConflictDecision decision{.action = ConflictAction::Cancel};
              if (applyConflictToAll && applyConflictTypes == std::pair{incomingType, existingType})
                decision.action = *applyConflictToAll;
              else if (data->conflictResolver && !g_cancellable_is_cancelled(cancellable)) {
                const auto metadata =
                    engine::queryConflictMetadata(source, target, incomingType, existingType, cancellable);
                decision =
                    data->conflictResolver(uris[i], destination.string(), i + 1 < requested, metadata, cancellable);
              }
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
                } else if (const auto reason = filemanager::invalidFileNameReason(renamed); !reason.empty()) {
                  itemError = reason;
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
              succeeded = gioMoveWithBackup(
                  source, target, incomingType, replace, existingBefore, cancellable, itemError, result->mutated
              );
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
  if (data->kind == Kind::TrashToHome) {
    const auto count = data->sources.size();
    data->measures.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
      if (!transferCheckpoint(&progress)) {
        g_cancellable_set_error_if_cancelled(cancellable, &error);
        fail("Cancelled");
        break;
      }
      GFile* source = g_file_new_for_path(data->sources[i].c_str());
      const bool measured = measureTree(source, cancellable, data->measures[i], &error);
      g_object_unref(source);
      if (!measured) {
        fail("Could not calculate transfer size");
        break;
      }
      data->totalMeasure.bytes += data->measures[i].bytes;
      data->totalMeasure.items += data->measures[i].items;
    }
    std::filesystem::path trashRoot;
    std::filesystem::path infoDir;
    std::filesystem::path filesDir;
    if (result->message.empty() && !wastebin_fallback::ensureHomeWastebinDirs(trashRoot, infoDir, filesDir, &error)) {
      fail("Could not prepare Home Wastebin directories.");
    }
    if (result->message.empty()) {
      if (const auto available = wastebin_fallback::queryAvailableBytes(filesDir);
          available.has_value() && *available < data->totalMeasure.bytes) {
        result->ok = false;
        result->message = "Insufficient free space in Home Wastebin ("
            + wastebin_fallback::formatByteSize(data->totalMeasure.bytes)
            + " required, "
            + wastebin_fallback::formatByteSize(*available)
            + " available).";
        result->customDescription = result->message;
      }
    }
    if (result->message.empty()) {
      postProgressIdle(
          data->owner, data->alive, {data->id, 0, data->totalMeasure.bytes, 0, data->totalMeasure.items, 0}
      );
    }
    std::uint64_t completedBytes = 0;
    std::uint64_t completedItems = 0;
    progress.totalBytes = data->totalMeasure.bytes;
    progress.totalItems = data->totalMeasure.items;
    progress.lastReport = std::chrono::steady_clock::now();
    if (progress.pause)
      progress.reportedPause = progress.pause->pausedTime();

    struct stat filesDirStat{};
    const std::uint64_t filesDirDevice =
        lstat(filesDir.c_str(), &filesDirStat) == 0 ? static_cast<std::uint64_t>(filesDirStat.st_dev) : 0;

    for (std::size_t i = 0; i < count && result->message.empty(); ++i) {
      if (!transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_cancellable_set_error_if_cancelled(cancellable, &error);
        fail("Cancelled");
        break;
      }
      const auto& sourcePath = data->sources[i];
      const auto sourceBefore = revision(sourcePath);
      if (!sourceBefore) {
        result->ok = false;
        result->message = "The source item is missing or could not be inspected: " + sourcePath;
        break;
      }
      const bool sourceIsDir = S_ISDIR(sourceBefore->mode);
      const bool sourceIsSymlink = S_ISLNK(sourceBefore->mode);
      if (!fs_compat::preflightTransferDestination(filesDir, sourceIsDir, sourceIsSymlink, false, &error)) {
        fail("Home Wastebin filesystem does not support safe file publication.");
        break;
      }
      std::error_code absEc;
      std::filesystem::path absSource = std::filesystem::absolute(sourcePath, absEc).lexically_normal();
      if (absEc || absSource.empty())
        absSource = sourcePath;

      wastebin_fallback::ReservedTrashEntry reserved;
      if (!wastebin_fallback::reserveHomeTrashEntry(absSource, reserved, &error)) {
        fail("Could not reserve Home Wastebin entry.");
        break;
      }

      bool committedToHomeTrash = false;
      bool removedSourceDirectly = false;
      if (filesDirDevice != 0 && sourceBefore->device == filesDirDevice) {
        const auto directOutcome = fs_compat::moveOrPublishEntryNoReplace(
            sourcePath.c_str(), reserved.filesPath.c_str(), false, toCompatIdentity(*sourceBefore), sourceIsSymlink
        );
        if (directOutcome.ok) {
          committedToHomeTrash = true;
          removedSourceDirectly = true;
          result->ok = true;
        } else if (directOutcome.splitState) {
          committedToHomeTrash = true;
          result->ok = false;
          result->mutated = true;
          result->message = "Trashed copy remains in Home Wastebin at "
              + reserved.filesPath.string()
              + "; source still exists at "
              + sourcePath
              + ": "
              + directOutcome.message;
          result->customDescription = result->message;
        }
      }

      if (!committedToHomeTrash && result->message.empty()) {
        char* uuid = g_uuid_string_random();
        char* leaf = g_strdup_printf(".nythera-trash-partial-%s-%zu", uuid, i);
        g_free(uuid);
        GFile* source = g_file_new_for_path(sourcePath.c_str());
        GFile* filesDirFile = g_file_new_for_path(filesDir.c_str());
        GFile* staged = g_file_get_child(filesDirFile, leaf);
        g_object_unref(filesDirFile);
        g_free(leaf);

        const bool stagedWasAbsent = !entryExistsNoFollow(staged);
        progress.baseBytes = completedBytes;
        progress.progressedBytes = completedBytes;
        progress.completedItems = completedItems;
        if (stagedWasAbsent)
          result->ok = copyTree(source, staged, cancellable, &error, &progress, engine::CopyMetadata::DirectoryModeAndTimes);
        else {
          g_set_error_literal(
              &error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Could not allocate a temporary Home Wastebin staging path"
          );
          result->ok = false;
        }
        if (result->ok
            && (!transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error))) {
          g_cancellable_set_error_if_cancelled(cancellable, &error);
          if (error == nullptr)
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
          result->ok = false;
        }

        g_autofree char* stagePath = g_file_get_path(staged);
        const auto stagedRevision = result->ok && stagePath ? revision(stagePath) : std::nullopt;
        bool publishSplitState = false;
        bool publishMetadataFailed = false;
        if (result->ok) {
          const auto outcome = fs_compat::moveOrPublishEntryNoReplace(
              stagePath == nullptr ? "" : stagePath, reserved.filesPath.c_str(), true,
              stagedRevision ? std::optional<fs_compat::EntryIdentity>(toCompatIdentity(*stagedRevision))
                             : std::nullopt,
              sourceIsSymlink
          );
          result->ok = outcome.ok;
          if (!outcome.ok) {
            publishSplitState = outcome.splitState;
            publishMetadataFailed = outcome.metadataFailed;
            setErrorFromPublishOutcome(outcome, &error);
          }
        }
        committedToHomeTrash = result->ok || publishSplitState;
        if (publishSplitState && stagedWasAbsent && entryExistsNoFollow(staged)) {
          cleanupStagedTree(staged);
          if (!publishMetadataFailed && !entryExistsNoFollow(staged)) {
            result->ok = true;
            publishSplitState = false;
            g_clear_error(&error);
          }
        }
        if (!committedToHomeTrash) {
          if (stagedWasAbsent && entryExistsNoFollow(staged))
            cleanupStagedTree(staged);
          unlink(reserved.infoPath.c_str());
        }
        g_object_unref(staged);
        g_object_unref(source);
      }

      if (committedToHomeTrash) {
        result->mutated = true;
        if (!removedSourceDirectly && result->ok) {
          GFile* source = g_file_new_for_path(sourcePath.c_str());
          const bool cancelledBeforeCleanup =
              !transferCheckpoint(&progress) || g_cancellable_set_error_if_cancelled(cancellable, &error);
          if (cancelledBeforeCleanup) {
            result->ok = false;
            result->cancelled = true;
          } else {
            result->ok = removeTree(source, cancellable, &error, &result->mutated);
            result->cancelled = error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          }
          if (!result->ok) {
            const bool sourceStillExists =
                !sourcePath.empty() ? revision(sourcePath).has_value() : entryExistsNoFollow(source);
            result->message = std::string("Trashed copy remains in Home Wastebin at ")
                + reserved.filesPath.string()
                + (sourceStillExists ? "; source still exists at " + sourcePath
                                     : "; source was already removed from " + sourcePath)
                + (result->cancelled
                       ? " because the operation was cancelled before source cleanup finished"
                       : ": " + std::string(error == nullptr ? "could not remove source" : error->message));
            result->customDescription = result->message;
          }
          g_object_unref(source);
        }
        if (result->ok) {
          result->movedSources.push_back(sourcePath);
          completedBytes += data->measures[i].bytes;
          completedItems += data->measures[i].items;
          data->completedMeasure.bytes = completedBytes;
          data->completedMeasure.items = completedItems;
          progress.baseBytes = completedBytes;
          progress.progressedBytes = completedBytes;
          progress.completedItems = completedItems;
          publishProgress(progress, 0, true);
        }
      }

      if (!result->ok) {
        if (result->message.empty())
          fail("Could not move item to Home Wastebin");
        else {
          result->cancelled |= error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          g_clear_error(&error);
        }
        break;
      }
    }
    if (result->message.empty())
      result->ok = true;
  } else if (data->kind == Kind::Trash) {
    const auto count = data->sources.size();
    for (std::size_t i = 0; i < count && result->message.empty(); ++i) {
      if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        fail("Cancelled");
        break;
      }
      const auto& sourcePath = data->sources[i];
      GFile* source = g_file_new_for_path(sourcePath.c_str());
      if (const int injectedErrno = wastebin_fallback::invokeTrashHook(sourcePath.c_str()); injectedErrno != 0) {
        result->ok = false;
        g_set_error_literal(&error, G_IO_ERROR, g_io_error_from_errno(injectedErrno), g_strerror(injectedErrno));
      } else if (
          std::string isolationError; wastebin_fallback::productionWastebinForbidden()
          && !wastebin_fallback::validateIsolatedTrashTarget(sourcePath, isolationError)
      ) {
        result->ok = false;
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, isolationError.c_str());
      } else {
        result->ok = g_file_trash(source, cancellable, &error);
      }
      result->mutated |= result->ok;
      if (result->ok) {
        result->movedSources.push_back(sourcePath);
      } else if (error == nullptr || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        const auto diag = wastebin_fallback::diagnoseTrashFailure(sourcePath, error);
        if (!diag.summary.empty()) {
          result->message = diag.summary;
          result->customDescription = diag.summary;
        }
        if (diag.eligibleForHomeFallback) {
          result->trashFallback = TrashFallbackOffer{
              .sources = std::vector<std::string>(
                  data->sources.begin() + static_cast<std::ptrdiff_t>(i), data->sources.end()
              ),
              .mountPath = diag.mountRoot.string(),
              .trashDirectory = diag.trashDirPath.string(),
              .diagnostic = diag.summary,
          };
        }
      }
      if (result->ok) {
        const auto done = static_cast<std::uint64_t>(i + 1);
        postProgressIdle(data->owner, data->alive, {data->id, 0, 0, done, static_cast<std::uint64_t>(count), 0});
      }
      if (!result->ok) {
        if (result->message.empty())
          fail("Operation failed");
        else {
          result->cancelled |= error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          g_clear_error(&error);
        }
      }
      g_object_unref(source);
    }
    if (result->message.empty())
      result->ok = true;
  }
  g_task_return_pointer(task, result, [](gpointer ptr) { delete static_cast<Result*>(ptr); });
}

void FileOperationManager::completed(GObject*, GAsyncResult* asyncResult, gpointer userData) {
  auto* self = static_cast<FileOperationManager*>(userData);
  GError* error = nullptr;
  std::unique_ptr<Result> result(static_cast<Result*>(g_task_propagate_pointer(G_TASK(asyncResult), &error)));
  auto* data = static_cast<Task*>(g_task_get_task_data(G_TASK(asyncResult)));
  const auto alive = data != nullptr ? data->alive : std::shared_ptr<bool>{};
  if (data != nullptr && alive && *alive) {
    auto* operation = self->find(data->id);
    if (operation != nullptr) {
      operation->finished = true;
      operation->paused = false;
      self->m_pauseStates.erase(data->id);
      self->m_adminControls.erase(data->id);
      self->m_transferRates.erase(data->id);
      self->m_activePaths.erase(data->id);
      operation->succeeded = result && result->ok;
      operation->cancelled =
          result ? result->cancelled : error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      operation->outcomeUncertain = result && result->outcomeUncertain;
      operation->mutated = result && (result->mutated || (isReplay(data->kind) && result->ok));
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
            && data->kind != Kind::Delete
            && data->kind != Kind::EmptyWastebin
            && data->kind != Kind::Properties;
        constexpr std::size_t historyLimit = 32;
        for (auto& entry : result->history) {
          if (self->m_undo.size() == historyLimit)
            self->m_undo.erase(self->m_undo.begin());
          self->m_undo.push_back(std::move(entry));
        }
      }
      if (data->kind == Kind::CreateSymlink || operation->adminOrigin)
        operation->destination = data->destination;
      if (result) {
        operation->movedSources = std::move(result->movedSources);
        operation->trashFallback = std::move(result->trashFallback);
      }
      if (result && (isTransfer(data->kind) || data->kind == Kind::CreateSymlink))
        operation->completedEntries = std::move(result->completedEntries);
      if (usesTransferControls(data->kind) || (operation->adminOrigin && data->kind == Kind::Delete)) {
        operation->processedBytes = data->completedMeasure.bytes;
        operation->totalBytes = data->totalMeasure.bytes;
        operation->processedFiles = data->completedMeasure.items;
        operation->totalFiles = data->totalMeasure.items;
        if (operation->totalBytes > 0)
          operation->fraction = std::clamp(
              static_cast<double>(operation->processedBytes) / static_cast<double>(operation->totalBytes), 0.0, 1.0
          );
      }
      operation->fraction = operation->succeeded ? 1.0 : operation->fraction;
      const std::string verb = operationName(data->kind);
      operation->resultTitle = verb
          + (operation->succeeded       ? " completed"
                 : operation->cancelled ? " cancelled"
                                        : " failed");
      if (result && result->requestedCount)
        operation->requestedItemCount = *result->requestedCount;
      if (result && !result->customTitle.empty())
        operation->resultTitle = result->customTitle;
      if (result && !result->customDescription.empty())
        operation->resultDescription = result->customDescription;
      else if (operation->succeeded)
        operation->resultDescription = completedDescription(*operation);
      else if (operation->cancelled) {
        if (isTransfer(data->kind))
          operation->resultDescription = "Transfer to " + displayDestination(operation->destination) + " was cancelled";
        else
          operation->resultDescription = verb
              + " of "
              + quotedBasename(operation->source.empty() ? operation->destination : operation->source)
              + " was cancelled";
      } else {
        operation->resultDescription = failedDescription(
            *operation,
            result      ? result->message
                : error ? error->message
                        : "Operation failed"
        );
      }
      if (data->kind == Kind::Properties) {
        self->m_metadataTargets.erase(data->id);
        operation->resultDescription = result ? result->message : "Properties operation failed.";
      }
      operation->status = operation->resultDescription;
      if (operation->undoUnavailable)
        operation->status += " · Undo unavailable for this operation";
      if (!operation->succeeded && !operation->cancelled && !self->m_jobPaths.contains(operation->id))
        self->registerJob(*operation);
      self->publishJob(*operation);
      self->finishJob(*operation);
      auto it = std::ranges::find(self->m_cancellables, data->id, &std::pair<std::uint64_t, GCancellable*>::first);
      if (it != self->m_cancellables.end()) {
        g_object_unref(it->second);
        self->m_cancellables.erase(it);
      }
      const auto operationId = data->id;
      const auto renameCompleted = data->renameCompleted;
      const auto renameSucceeded = operation->succeeded;
      const auto renameDescription = operation->resultDescription;
      if (data->kind == Kind::Properties && data->metadataCompleted && result) {
        data->metadataCompleted(result->metadata);
        if (!*alive) {
          g_clear_error(&error);
          return;
        }
      }
      if (renameCompleted) {
        renameCompleted(renameSucceeded, renameDescription);
        if (!*alive) {
          g_clear_error(&error);
          return;
        }
      }
      if (auto* currentOperation = self->find(operationId); currentOperation != nullptr && self->m_changed)
        self->m_changed(*currentOperation);
    } else {
      auto it = std::ranges::find(self->m_cancellables, data->id, &std::pair<std::uint64_t, GCancellable*>::first);
      if (it != self->m_cancellables.end()) {
        g_object_unref(it->second);
        self->m_cancellables.erase(it);
      }
    }
  }
  g_clear_error(&error);
}
