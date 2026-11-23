#pragma once

#include "filemanager/fs_compat.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <gio/gio.h>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace filemanager::file_operation_engine {

  enum class Kind { Copy, Duplicate, Move, Rename, CreateFolder, CreateFile, CreateSymlink, Delete };
  enum class ConflictAction { Replace, Skip, Rename, Cancel, Merge };

  struct ConflictDecision {
    ConflictAction action = ConflictAction::Cancel;
    bool applyToAll = false;
    std::string alternativeName;
  };

  struct ConflictItemMetadata {
    GFileType type = G_FILE_TYPE_UNKNOWN;
    std::string contentType;
    std::optional<std::int64_t> size;
    std::optional<std::uint64_t> modified;
    bool inspected = false;
  };

  struct ConflictMetadata {
    ConflictItemMetadata incoming;
    ConflictItemMetadata existing;
    bool sameItem = false;
  };

  struct CompletedEntry {
    std::string path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
  };

  using FileIdentity = std::pair<std::uint64_t, std::uint64_t>;

  struct Revision {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t links = 0;
    std::uint32_t mode = 0;
    std::int64_t size = 0;
    std::pair<std::int64_t, std::int64_t> modified;
    std::pair<std::int64_t, std::int64_t> changed;
    bool operator==(const Revision&) const = default;
  };

  // The worker acknowledges suspension only when it reaches a checkpoint.
  class FileTransferPauseState {
  public:
    void pause();
    void resume();
    void cancel();
    bool requested() const;
    std::chrono::steady_clock::duration pausedTime() const;
    bool checkpoint(GCancellable* cancellable, const std::function<void(bool)>& acknowledge);

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_requested = false;
    bool m_cancelled = false;
    std::chrono::steady_clock::duration m_pausedTime{};
  };

  struct TransferMeasure {
    std::uint64_t bytes = 0;
    std::uint64_t items = 0;
  };

  struct TransferCallbacks {
    std::function<void(
        std::uint64_t bytes, std::uint64_t totalBytes, std::uint64_t items, std::uint64_t totalItems,
        std::uint64_t speed
    )>
        report;
    std::function<void(bool)> acknowledgePause;
    std::function<void(std::uint64_t)> boundary;
  };

  using ConflictResolver = std::function<ConflictDecision(
      const std::string& source, const std::string& destination, bool hasRemaining, const ConflictMetadata& metadata,
      GCancellable*
  )>;

  // Keep the types already inspected by the executing operation. Read the
  // comparison details on the same side, without following symbolic links.
  ConflictMetadata queryConflictMetadata(
      GFile* incoming, GFile* existing, GFileType incomingType, GFileType existingType, GCancellable* cancellable
  );

  struct Callbacks {
    TransferCallbacks transfer;
    ConflictResolver resolveConflict;
    // Filesystem observations at the original history-recording boundary.
    std::function<void(
        const std::string& source, const std::string& destination, const std::optional<Revision>& sourceRevision,
        const std::optional<Revision>& resultRevision, FileIdentity sourceParent, FileIdentity destinationParent,
        std::string linkTarget, std::optional<std::vector<std::uint8_t>> createdContent
    )>
        recordChange;
  };

  struct Request {
    Kind kind;
    std::vector<std::string> sources;
    std::string destination;
    std::optional<FileIdentity> renameIdentity;
    bool keepBoth = false;
  };

  struct Result {
    bool ok = false;
    bool cancelled = false;
    bool mutated = false;
    std::string message;
    std::string customDescription;
    std::vector<CompletedEntry> completedEntries;
    std::vector<std::string> movedSources;
    TransferMeasure totalMeasure;
    TransferMeasure completedMeasure;
  };

  // Synchronous execution. Callbacks run on the calling thread. A successful
  // create may update request.destination to the path actually created.
  Result executeLocalOperation(
      Request& request, GCancellable* cancellable, FileTransferPauseState* pause, const Callbacks& callbacks
  );

  // Primitives also used by the manager's replay and Wastebin paths.
  struct TransferProgress {
    const TransferCallbacks& callbacks;
    FileTransferPauseState* pause;
    GCancellable* cancellable;
    std::uint64_t baseBytes = 0;
    std::uint64_t progressedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t completedItems = 0;
    std::uint64_t totalItems = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    std::uint64_t lastBytes = 0;
    std::chrono::steady_clock::duration reportedPause{};
  };

  struct PrivateStaging {
    std::string directory;
    explicit PrivateStaging(const std::string& sibling);
    ~PrivateStaging();
  };

  void publishProgress(TransferProgress& progress, std::uint64_t currentBytes, bool force = false);
  bool transferCheckpoint(TransferProgress* progress, std::uint64_t currentBytes = 0);
  bool measureTree(GFile* file, GCancellable* cancellable, TransferMeasure& measure, GError** error);
  bool copyTree(
      GFile* source, GFile* destination, GCancellable* cancellable, GError** error, TransferProgress* progress,
      bool preserveDirectoryMetadata = false
  );
  bool removeTree(GFile* file, GCancellable* cancellable, GError** error, bool* mutated = nullptr);
  bool entryExistsNoFollow(GFile* file);
  void cleanupStagedTree(GFile* file);
  std::optional<Revision> revision(const std::string& path);
  FileIdentity parentIdentity(const std::string& path);
  fs_compat::EntryIdentity toCompatIdentity(const Revision& revision);
  bool reject(GError** error, const char* message);
  void setErrorFromPublishOutcome(const fs_compat::PublishOutcome& outcome, GError** error);
  bool createItem(
      Kind kind, GFile* target, const std::string& link, const std::optional<std::vector<std::uint8_t>>& content,
      const std::string& pinnedTemplatePath, GCancellable* cancellable, GError** error, bool& created
  );

} // namespace filemanager::file_operation_engine
