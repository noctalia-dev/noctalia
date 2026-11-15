#pragma once

#include <gio/gio.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Local file operations run on GTask workers; callbacks return on the caller's
// GLib context. The manager deliberately owns operation state independently of
// the file browser's transient row indices.
class FileOperationManager {
public:
  enum class Kind { Copy, Move, Rename, CreateFolder, CreateFile, Trash, Delete };
  enum class ConflictAction { Replace, Skip, Rename, Cancel };
  struct ConflictDecision { ConflictAction action = ConflictAction::Cancel; bool applyToAll = false; };
  struct Operation {
    std::uint64_t id = 0;
    Kind kind = Kind::Copy;
    std::string source;
    std::string destination;
    std::string status;
    std::string resultTitle;
    std::string resultDescription;
    std::size_t requestedItemCount = 0;
    double fraction = 0.0;
    std::uint64_t processedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t processedFiles = 0;
    std::uint64_t totalFiles = 0;
    std::uint64_t speedBytesPerSecond = 0;
    bool finished = false;
    bool succeeded = false;
    bool cancelled = false;
    bool progressRevealed = false;
  };
  using Changed = std::function<void(const Operation&)>;
  using ConflictResolver = std::function<ConflictDecision(const std::string&, const std::string&, GCancellable*)>;

  explicit FileOperationManager(Changed changed = {});
  ~FileOperationManager();
  FileOperationManager(const FileOperationManager&) = delete;
  FileOperationManager& operator=(const FileOperationManager&) = delete;

  std::uint64_t copy(std::vector<std::string> sources, std::string destinationDirectory);
  std::uint64_t move(std::vector<std::string> sources, std::string destinationDirectory);
  std::uint64_t rename(std::string source, std::string destination);
  std::uint64_t createFolder(std::string destination);
  std::uint64_t createFile(std::string destination);
  std::uint64_t trash(std::vector<std::string> sources);
  std::uint64_t removePermanently(std::vector<std::string> sources);
  void cancel(std::uint64_t id);
  void setConflictResolver(ConflictResolver resolver) { m_conflictResolver = std::move(resolver); }
  [[nodiscard]] const std::vector<Operation>& operations() const noexcept { return m_operations; }
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool hasJobService() const noexcept;
  void updateProgress(std::uint64_t id, std::uint64_t bytes, std::uint64_t totalBytes,
      std::uint64_t files, std::uint64_t totalFiles, std::uint64_t speed);

private:
  struct Task;
  std::uint64_t start(Kind kind, std::vector<std::string> sources, std::string destination);
  static void worker(GTask* task, gpointer source, gpointer taskData, GCancellable* cancellable);
  static void completed(GObject* source, GAsyncResult* result, gpointer userData);
  static void serviceAppeared(GDBusConnection* connection, const gchar* name, const gchar* owner, gpointer userData);
  static void serviceVanished(GDBusConnection* connection, const gchar* name, gpointer userData);
  static void jobSignal(GDBusConnection* connection, const gchar* sender, const gchar* path,
      const gchar* interfaceName, const gchar* signalName, GVariant* parameters, gpointer userData);
  void registerJob(const Operation& operation);
  void publishJob(const Operation& operation);
  void finishJob(const Operation& operation);
  void update(std::uint64_t id, double fraction, std::string status);
  Operation* find(std::uint64_t id);

  Changed m_changed;
  ConflictResolver m_conflictResolver;
  std::vector<Operation> m_operations;
  std::vector<std::pair<std::uint64_t, GCancellable*>> m_cancellables;
  std::uint64_t m_nextId = 1;
  GDBusConnection* m_bus = nullptr;
  guint m_serviceWatch = 0;
  bool m_serviceAvailable = false;
  std::unordered_map<std::uint64_t, std::string> m_jobPaths;
  std::unordered_map<std::uint64_t, guint> m_signalSubscriptions;
  std::unordered_map<std::uint64_t, guint> m_jobRevealTimers;
};
