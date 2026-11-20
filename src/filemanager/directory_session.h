#pragma once

#include "core/files/directory_scanner.h"
#include "ui/dialogs/file_dialog.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class FileManagerTestAccess;

namespace filemanager {

  class DirectorySession {
  public:
    struct PendingMonitorChange {
      GFileMonitorEvent event;
      std::filesystem::path path;
      std::filesystem::path otherPath;
    };

    struct Callbacks {
      std::function<FileDialogSortField()> sortField;
      std::function<FileDialogSortOrder()> sortOrder;
      std::function<bool()> isSizeColumnVisible;
      std::function<bool()> isRenaming;
      std::function<void()> onDeferRefreshDuringRename;
      std::function<void()> onCancelFileGesture;
      std::function<void(const std::filesystem::path& targetPath, bool pathChanged)> onBeforeNavigate;
      std::function<void(const std::filesystem::path& newPath)> onBeforePathChange;
      std::function<void(const std::filesystem::path& path, bool trash)> onAfterNavigatePathSet;
      std::function<void(const std::filesystem::path& path, bool trash)> onEnumerateStarted;
      std::function<void(bool chooseFirst)> onEntriesLoaded;
      std::function<void()> onAfterEntriesLoaded;
      std::function<void(const std::filesystem::path& oldPath, const std::filesystem::path& newPath)>
          onEntryMonitoredRenamed;
      std::function<void(const std::filesystem::path& path)> onEntryMonitoredRemoved;
      std::function<void()> onEntriesChanged;
      std::function<void(std::size_t index)> onEntryChangedAt;
      std::function<void()> onMonitorQueryCompleted;
      std::function<void(std::string)> onStatus;
      std::function<void(std::string detail)> onEnumerateFailed;
      std::function<void(bool empty)> onEnumerateCompletedStatus;
    };

    DirectorySession(std::filesystem::path initialPath, Callbacks callbacks);
    ~DirectorySession();

    void shutdown();

    void navigate(const std::filesystem::path& requestedPath, bool recordHistory = true);
    void navigateTrash();
    void enumerate(const std::filesystem::path& path);
    void cancelEnumeration();
    void bumpEnumerationGeneration() { ++m_enumerationGeneration; }

    void sortEntries();

    [[nodiscard]] std::optional<std::uint64_t> folderItemCount(const FileEntry& entry);
    void clearFolderCounts();

    void monitorDirectory();
    void closeMonitor();
    void applyPendingMonitorChanges();

    void cancelDirectoryWritableQuery();
    void queryDirectoryWritableAsync(const std::filesystem::path& path);
    [[nodiscard]] bool currentDirectoryWritable() const;

    [[nodiscard]] GFile* browsingFile() const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] bool browsingTrash() const noexcept { return m_browsingTrash; }
    [[nodiscard]] const std::vector<FileEntry>& entries() const noexcept { return m_entries; }
    [[nodiscard]] std::vector<FileEntry>& entries() noexcept { return m_entries; }
    [[nodiscard]] const std::vector<std::filesystem::path>& backHistory() const noexcept { return m_backHistory; }
    [[nodiscard]] std::vector<std::filesystem::path>& backHistory() noexcept { return m_backHistory; }
    [[nodiscard]] const std::vector<std::filesystem::path>& forwardHistory() const noexcept { return m_forwardHistory; }
    [[nodiscard]] std::vector<std::filesystem::path>& forwardHistory() noexcept { return m_forwardHistory; }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& trashUris() const noexcept { return m_trashUris; }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& trashOriginalPaths() const noexcept {
      return m_trashOriginalPaths;
    }

    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] std::uint64_t navigationGeneration() const noexcept { return m_navigationGeneration; }
    [[nodiscard]] std::uint64_t enumerationGeneration() const noexcept { return m_enumerationGeneration; }
    [[nodiscard]] GCancellable* ensureEnumerationCancellable() {
      if (m_enumerationCancellable == nullptr)
        m_enumerationCancellable = g_cancellable_new();
      return m_enumerationCancellable;
    }
    void clearChooseFirstAfterLoad() noexcept { m_chooseFirstAfterLoad = false; }
    void clearPendingMonitorChanges() noexcept { m_pendingMonitorChanges.clear(); }
    [[nodiscard]] std::size_t pendingMonitorQueries() const noexcept { return m_pendingMonitorQueries; }
    [[nodiscard]] bool hasPendingMonitorChanges() const noexcept { return !m_pendingMonitorChanges.empty(); }

    [[nodiscard]] bool showHidden() const noexcept { return m_showHidden; }
    void setShowHidden(bool value) noexcept { m_showHidden = value; }
    [[nodiscard]] bool foldersFirst() const noexcept { return m_foldersFirst; }
    void setFoldersFirst(bool value) noexcept { m_foldersFirst = value; }
    [[nodiscard]] bool hiddenLast() const noexcept { return m_hiddenLast; }
    void setHiddenLast(bool value) noexcept { m_hiddenLast = value; }

  private:
    friend class ::FileManagerTestAccess;

    struct FolderCount {
      std::uint64_t id;
      std::optional<std::uint64_t> items;
    };

    void fetchNext(GFileEnumerator* enumerator, std::uint64_t generation);
    void startNextFolderCount();
    void handleMonitorEvent(GFile* file, GFile* otherFile, GFileMonitorEvent event);
    void queryMonitoredFile(
        const std::filesystem::path& path, const std::filesystem::path& replacedPath = std::filesystem::path{}
    );
    void removeMonitoredFile(const std::filesystem::path& path);

    Callbacks m_callbacks;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    std::filesystem::path m_path;
    bool m_browsingTrash = false;
    std::unordered_map<std::string, std::string> m_trashUris;
    std::unordered_map<std::string, std::string> m_trashOriginalPaths;
    std::vector<std::filesystem::path> m_backHistory;
    std::vector<std::filesystem::path> m_forwardHistory;
    std::vector<FileEntry> m_entries;
    std::vector<FileEntry> m_loadingEntries;

    std::unordered_map<std::string, FolderCount> m_folderCounts;
    std::vector<std::pair<std::string, std::uint64_t>> m_pendingFolderCounts;
    std::uint64_t m_nextFolderCountId = 0;
    GCancellable* m_folderCountCancellable = nullptr;
    bool m_folderCountRunning = false;

    std::vector<PendingMonitorChange> m_pendingMonitorChanges;
    std::size_t m_pendingMonitorQueries = 0;
    GFileMonitor* m_monitor = nullptr;

    std::uint64_t m_navigationGeneration = 0;
    GCancellable* m_enumerationCancellable = nullptr;
    std::uint64_t m_enumerationGeneration = 0;

    bool m_showHidden = false;
    bool m_foldersFirst = true;
    bool m_hiddenLast = true;
    bool m_loading = false;
    bool m_chooseFirstAfterLoad = true;

    GCancellable* m_directoryWritableCancellable = nullptr;
    std::uint64_t m_directoryWritableGeneration = 0;
    std::filesystem::path m_directoryWritablePath;
    std::optional<bool> m_directoryWritable;
    std::function<bool(const std::filesystem::path&, std::function<void(bool)>)> m_directoryWritableQueryHook;
  };

} // namespace filemanager
