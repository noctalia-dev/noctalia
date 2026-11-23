#pragma once

#include "filemanager/file_operation_engine.h"
#include "filemanager/property_operations.h"
#include "filemanager/transfer_rates.h"
#include "filemanager/tab_presentation.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Local file operations run on GTask workers; callbacks return on the caller's
// GLib context. The manager deliberately owns operation state independently of
// the file browser's transient row indices.
namespace filemanager {
  class BrowserTab;
}

class FileOperationManager {
public:
  enum class Kind {
    Copy,
    Duplicate,
    Move,
    Rename,
    CreateFolder,
    CreateFile,
    CreateSymlink,
    Trash,
    TrashToHome,
    Delete,
    Restore,
    EmptyWastebin,
    Properties,
    Undo,
    Redo
  };
  using ConflictAction = filemanager::file_operation_engine::ConflictAction;
  using ConflictDecision = filemanager::file_operation_engine::ConflictDecision;
  using CompletedEntry = filemanager::file_operation_engine::CompletedEntry;
  struct TrashFallbackOffer {
    std::vector<std::string> sources;
    std::string mountPath;
    std::string trashDirectory;
    std::string diagnostic;
  };
  struct AdminCopyOrigin {
    filemanager::TabId tabId;
    std::uint64_t sessionGeneration;
    std::uint64_t navigation;
  };
  struct Operation {
    std::uint64_t id = 0;
    Kind kind = Kind::Copy;
    std::string source;
    std::string destination;
    std::string status;
    std::string resultTitle;
    std::string resultDescription;
    std::vector<CompletedEntry> completedEntries;
    std::vector<std::string> movedSources;
    std::optional<TrashFallbackOffer> trashFallback;
    std::optional<AdminCopyOrigin> adminCopy;
    std::size_t requestedItemCount = 0;
    double fraction = 0.0;
    std::uint64_t processedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t processedFiles = 0;
    std::uint64_t totalFiles = 0;
    std::uint64_t speedBytesPerSecond = 0;
    std::uint64_t displaySpeedBytesPerSecond = 0;
    std::uint64_t etaBytesPerSecond = 0;
    bool paused = false;
    bool finished = false;
    bool succeeded = false;
    bool cancelled = false;
    bool mutated = false;
    bool progressRevealed = false;
    bool undoUnavailable = false;
  };
  using Changed = std::function<void(const Operation&)>;
  using ConflictResolver = filemanager::file_operation_engine::ConflictResolver;
  // Optional provider seam for isolated tests. Production operations use GIO's
  // trash:// backend; callbacks receive trash URIs and native destination paths.
  struct WastebinBackend {
    std::function<bool(GCancellable*, std::vector<std::string>&, std::string&)> enumerateTopLevel;
    std::function<bool(const std::string&, GCancellable*, std::string&, std::string&)> originalPath;
  };

  explicit FileOperationManager(Changed changed = {}, WastebinBackend wastebinBackend = {});
  ~FileOperationManager();
  void shutdown() noexcept;
  FileOperationManager(const FileOperationManager&) = delete;
  FileOperationManager& operator=(const FileOperationManager&) = delete;

  std::uint64_t copy(std::vector<std::string> sources, std::string destinationDirectory);
  std::uint64_t copyAsAdministrator(filemanager::BrowserTab& tab, std::vector<std::string> sources);
  std::uint64_t duplicate(std::vector<std::string> sources, std::string destinationDirectory);
  std::uint64_t move(std::vector<std::string> sources, std::string destinationDirectory);
  std::uint64_t rename(std::string source, std::string destination);
  std::uint64_t renameChecked(
      std::string source, std::string destination, std::uint64_t device, std::uint64_t inode,
      std::function<void(bool, const std::string&)> completed
  );
  std::uint64_t createFolder(std::string destination);
  std::uint64_t createFile(std::string destination);
  std::uint64_t createFileFromTemplate(std::string templateSource, std::string destination);
  std::uint64_t createSymlink(std::string target, std::string destination, bool keepBoth = false);
  std::uint64_t trash(std::vector<std::string> sources);
  std::uint64_t trashToHomeWastebin(std::vector<std::string> sources);
  std::uint64_t restore(std::vector<std::string> trashUris, std::string destinationDirectory = {});
  std::uint64_t emptyWastebin(bool confirmed);
  std::uint64_t removePermanently(std::vector<std::string> sources);
  std::uint64_t modifyProperties(
      filemanager::PropertyRequest request, std::uint32_t uid,
      std::function<void(const filemanager::PropertyResult&)> completed
  );
  void cancel(std::uint64_t id);
  void pause(std::uint64_t id);
  void resume(std::uint64_t id);
  void updatePaused(std::uint64_t id, bool paused);
  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] bool canRedo() const noexcept;
  [[nodiscard]] bool replaying() const noexcept { return m_replaying; }
  [[nodiscard]] std::string undoLabel() const;
  [[nodiscard]] std::string redoLabel() const;
  std::uint64_t undo();
  std::uint64_t redo();
  void noteExternalMutation() { m_redo.clear(); }
  void setConflictResolver(ConflictResolver resolver) { m_conflictResolver = std::move(resolver); }
  [[nodiscard]] const std::vector<Operation>& operations() const noexcept { return m_operations; }
  [[nodiscard]] bool active() const noexcept;
  // Remains true through cancellation until the worker has actually returned.
  [[nodiscard]] bool usesMount(const std::filesystem::path& mountPath) const;
  [[nodiscard]] bool hasJobService() const noexcept;
  void updateProgress(
      std::uint64_t id, std::uint64_t bytes, std::uint64_t totalBytes, std::uint64_t files, std::uint64_t totalFiles,
      std::uint64_t speed
  );

private:
  friend class FileOperationManagerTestAccess;
  struct WorkerDrain;
  struct Task;
  struct Result;
  using Revision = filemanager::file_operation_engine::Revision;
  struct HistoryEntry {
    Kind kind;
    std::string source;
    std::string destination;
    std::string linkTarget;
    std::optional<std::vector<std::uint8_t>> createdContent;
    std::optional<Revision> sourceRevision;
    Revision resultRevision;
    std::pair<std::uint64_t, std::uint64_t> sourceParent;
    std::pair<std::uint64_t, std::uint64_t> destinationParent;
    bool blocked = false;
  };
  static bool replay(HistoryEntry& entry, bool undo, GCancellable* cancellable, GError** error);
  std::uint64_t start(
      Kind kind, std::vector<std::string> sources, std::string destination,
      std::optional<filemanager::PropertyRequest> metadata = {}, std::uint32_t uid = 0,
      std::function<void(const filemanager::PropertyResult&)> completed = {},
      std::optional<std::pair<std::uint64_t, std::uint64_t>> renameIdentity = {},
      std::function<void(bool, const std::string&)> renameCompleted = {}, bool keepBoth = false,
      filemanager::BrowserTab* adminTab = nullptr
  );
  static void worker(GTask* task, gpointer source, gpointer taskData, GCancellable* cancellable);
  static void completed(GObject* source, GAsyncResult* result, gpointer userData);
  static void serviceAppeared(GDBusConnection* connection, const gchar* name, const gchar* owner, gpointer userData);
  static void serviceVanished(GDBusConnection* connection, const gchar* name, gpointer userData);
  static void jobSignal(
      GDBusConnection* connection, const gchar* sender, const gchar* path, const gchar* interfaceName,
      const gchar* signalName, GVariant* parameters, gpointer userData
  );
  void registerJob(const Operation& operation);
  void publishJob(const Operation& operation, bool metricsOnly = false, bool suspensionAcknowledgement = false);
  bool updateTransferMetrics(Operation& operation);
  void finishJob(const Operation& operation);
  void update(std::uint64_t id, double fraction, std::string status);
  Operation* find(std::uint64_t id);

  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  std::shared_ptr<WorkerDrain> m_workerDrain;
  Changed m_changed;
  ConflictResolver m_conflictResolver;
  WastebinBackend m_wastebinBackend;
  std::vector<Operation> m_operations;
  std::vector<HistoryEntry> m_undo;
  std::vector<HistoryEntry> m_redo;
  bool m_replaying = false;
  std::unordered_map<std::uint64_t, std::vector<std::filesystem::path>> m_activePaths;
  std::vector<std::pair<std::uint64_t, GCancellable*>> m_cancellables;
  std::uint64_t m_nextId = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<filemanager::file_operation_engine::FileTransferPauseState>> m_pauseStates;
  std::unordered_map<std::uint64_t, filemanager::TransferRates> m_transferRates;
  std::unordered_map<std::uint64_t, std::function<void(bool)>> m_adminCopyControls;
  guint m_rateTimer = 0;
  // Isolated tests coordinate a real GIO copy at an I/O boundary.
  std::function<void(std::uint64_t, std::uint64_t)> m_transferBoundary;
  std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> m_metadataTargets;
  GDBusConnection* m_bus = nullptr;
  guint m_serviceWatch = 0;
  bool m_serviceAvailable = false;
  std::unordered_map<std::uint64_t, std::string> m_jobPaths;
  std::unordered_map<std::uint64_t, guint> m_signalSubscriptions;
  std::unordered_map<std::uint64_t, guint> m_jobRevealTimers;
};
