#include "filemanager/file_operation_manager.h"
#include "test_isolation.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <linux/fs.h>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

void setFileOperationRenameNoReplaceHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationReplaceBackupHookForTesting(std::function<void(const char*, const char*, bool)> hook);
void setFileOperationRemoveEntryHookForTesting(std::function<int(const char*)> hook);
void setFileOperationLinkAtHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationSymlinkAtHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationDirectoryRenameHookForTesting(std::function<int(const char*, const char*)> hook);
void setWastebinAvailableSpaceHookForTesting(std::function<std::optional<std::uint64_t>(const char*)> hook);
void setFileOperationTrashHookForTesting(std::function<int(const char*)> hook);

class FileOperationManagerTestAccess {
public:
  static void boundary(FileOperationManager& manager, std::function<void(std::uint64_t, std::uint64_t)> observer) {
    manager.m_transferBoundary = std::move(observer);
  }
  static void setWastebinBackend(FileOperationManager& manager, FileOperationManager::WastebinBackend backend) {
    manager.m_wastebinBackend = std::move(backend);
  }
};

namespace {
  struct Wait {
    GMainLoop* loop = nullptr;
    FileOperationManager* manager = nullptr;
    std::uint64_t id = 0;
    bool done = false;
    bool ok = false;
  };
  gboolean poll(gpointer data) {
    auto* wait = static_cast<Wait*>(data);
    for (const auto& operation : wait->manager->operations()) {
      if (operation.id == wait->id && operation.finished) {
        wait->done = true;
        wait->ok = operation.succeeded;
        g_main_loop_quit(wait->loop);
        return G_SOURCE_REMOVE;
      }
    }
    return G_SOURCE_CONTINUE;
  }
  bool waitFor(FileOperationManager& manager, std::uint64_t id) {
    TEST_CHECK(id != 0);
    Wait state{.loop = g_main_loop_new(nullptr, false), .manager = &manager, .id = id};
    const guint timer = g_timeout_add(10, poll, &state);
    (void)timer;
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return state.ok;
  }

  void assertCompletedEntry(
      const FileOperationManager::Operation& operation, std::size_t index, const std::filesystem::path& expectedPath
  ) {
    TEST_CHECK(index < operation.completedEntries.size());
    const auto& entry = operation.completedEntries[index];
    TEST_CHECK(entry.path == expectedPath.string());
    struct stat state{};
    TEST_CHECK(lstat(expectedPath.c_str(), &state) == 0);
    TEST_CHECK(entry.device == static_cast<std::uint64_t>(state.st_dev));
    TEST_CHECK(entry.inode == static_cast<std::uint64_t>(state.st_ino));
  }

  void runTests(const nythera::test::ScopedTestEnv& env) {
    const auto resetHooksGuard = nythera::test::makeScopeExit([] {
      setFileOperationRenameNoReplaceHookForTesting({});
      setFileOperationReplaceBackupHookForTesting({});
      setFileOperationRemoveEntryHookForTesting({});
      setFileOperationLinkAtHookForTesting({});
      setFileOperationSymlinkAtHookForTesting({});
      setFileOperationDirectoryRenameHookForTesting({});
      setWastebinAvailableSpaceHookForTesting({});
      setFileOperationTrashHookForTesting({});
    });
    {
      GError* busError = nullptr;
      GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &busError);
      TEST_CHECK(bus == nullptr);
      g_clear_error(&busError);
    }

    const auto& root = env.work();
    const auto from = root / "from";
    const auto to = root / "to";
    std::filesystem::create_directories(from / "nested");
    std::filesystem::create_directory(to);
    {
      std::ofstream file(from / "nested" / "payload.txt");
      file << "synthetic test payload";
    }

    FileOperationManager manager;
    const auto nestedCopyId = manager.copy({(from / "nested").string()}, to.string());
    TEST_CHECK(waitFor(manager, nestedCopyId));
    TEST_CHECK(std::filesystem::exists(to / "nested" / "payload.txt"));
    const auto copied = std::ranges::find(manager.operations(), nestedCopyId, &FileOperationManager::Operation::id);
    TEST_CHECK(copied != manager.operations().end());
    TEST_CHECK(copied->totalBytes == std::filesystem::file_size(from / "nested" / "payload.txt"));
    TEST_CHECK(copied->processedBytes == copied->totalBytes && copied->totalFiles == 1 && copied->processedFiles == 1);
    TEST_CHECK(copied->resultTitle == "Copy completed");
    TEST_CHECK(copied->resultDescription.starts_with("1 file copied to "));
    TEST_CHECK(copied->completedEntries.size() == 1);
    assertCompletedEntry(*copied, 0, to / "nested");
    TEST_CHECK(!waitFor(manager, manager.copy({(from / "nested").string()}, to.string())));
    {
      std::ifstream existing(to / "nested" / "payload.txt");
      std::string contents;
      std::getline(existing, contents);
      TEST_CHECK(contents == "synthetic test payload");
    }
    TEST_CHECK(!waitFor(manager, manager.copy({from.string()}, (from / "nested").string())));
    const auto invalidCopy = std::ranges::find_if(manager.operations(), [](const auto& operation) {
      return operation.finished && !operation.succeeded && operation.status.find("into itself") != std::string::npos;
    });
    TEST_CHECK(invalidCopy != manager.operations().end());
    TEST_CHECK(invalidCopy->resultTitle == "Copy failed");
    TEST_CHECK(invalidCopy->resultDescription == "Cannot copy \"from\" into itself");
    const auto batchSourceA = root / "batch-a.txt";
    const auto batchSourceB = root / "batch-b.txt";
    {
      std::ofstream file(batchSourceA);
      file << "batch a";
    }
    {
      std::ofstream file(batchSourceB);
      file << "batch b";
    }
    const auto batchCopyId = manager.copy({batchSourceA.string(), batchSourceB.string()}, to.string());
    TEST_CHECK(waitFor(manager, batchCopyId));
    TEST_CHECK(std::filesystem::exists(to / "batch-a.txt") && std::filesystem::exists(to / "batch-b.txt"));
    const auto batchCopy = std::ranges::find(manager.operations(), batchCopyId, &FileOperationManager::Operation::id);
    TEST_CHECK(batchCopy->completedEntries.size() == 2);
    assertCompletedEntry(*batchCopy, 0, to / "batch-a.txt");
    assertCompletedEntry(*batchCopy, 1, to / "batch-b.txt");
    // Every batch source and its destination must stay protected through
    // cancellation, until the worker has left its conflict/cleanup path.
    {
      const auto mountA = root / "mount-a";
      const auto mountB = root / "mount-b";
      const auto mountDestination = root / "mount-destination";
      const auto unrelatedMount = root / "mount-a-other";
      for (const auto& directory : {mountA, mountB, mountDestination, unrelatedMount})
        std::filesystem::create_directory(directory);
      {
        std::ofstream file(mountA / "first.txt");
        file << "first";
      }
      {
        std::ofstream file(mountB / "second.txt");
        file << "second";
      }
      {
        std::ofstream file(mountDestination / "first.txt");
        file << "existing";
      }
      std::atomic_bool safetyConflictReached{false};
      std::atomic_bool releaseSafetyWorker{false};
      FileOperationManager mountManager;
      const auto releaseGuard =
          nythera::test::makeScopeExit([&] { releaseSafetyWorker.store(true, std::memory_order_release); });
      mountManager.setConflictResolver([&](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        safetyConflictReached.store(true, std::memory_order_release);
        while (!releaseSafetyWorker.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
      });
      const auto protectedId = mountManager.copy(
          {(mountA / "first.txt").string(), (mountB / "second.txt").string()}, mountDestination.string()
      );
      while (!safetyConflictReached.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      TEST_CHECK(mountManager.usesMount(mountA));
      TEST_CHECK(mountManager.usesMount(mountB));
      TEST_CHECK(mountManager.usesMount(mountDestination));
      TEST_CHECK(!mountManager.usesMount(unrelatedMount));
      mountManager.cancel(protectedId);
      // Drain the caller context while the cancelled worker deliberately remains
      // blocked: no early GTask completion may release these mounts.
      for (int iteration = 0; iteration < 10; ++iteration) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(mountManager.active());
      TEST_CHECK(mountManager.usesMount(mountB));
      TEST_CHECK(mountManager.usesMount(mountDestination));
      releaseSafetyWorker.store(true, std::memory_order_release);
      TEST_CHECK(!waitFor(mountManager, protectedId));
      TEST_CHECK(!mountManager.usesMount(mountA));
      TEST_CHECK(!mountManager.usesMount(mountB));
      TEST_CHECK(!mountManager.usesMount(mountDestination));
      TEST_CHECK(std::filesystem::exists(mountA / "first.txt"));
      TEST_CHECK(std::filesystem::exists(mountB / "second.txt"));
      mountManager.setConflictResolver({});
    }
    TEST_CHECK(waitFor(manager, manager.createFolder((root / "created").string())));
    TEST_CHECK(waitFor(manager, manager.createFile((root / "created" / "empty").string())));
    TEST_CHECK(
        waitFor(manager, manager.rename((root / "created" / "empty").string(), (root / "created" / "renamed").string()))
    );
    TEST_CHECK(std::filesystem::exists(root / "created" / "renamed"));
    const auto incoming = root / "incoming.txt";
    const auto existing = to / "incoming.txt";
    {
      std::ofstream file(incoming);
      file << "replacement";
    }
    {
      std::ofstream file(existing);
      file << "preserve";
    }
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    TEST_CHECK(waitFor(manager, manager.copy({incoming.string()}, to.string())));
    {
      std::ifstream file(existing);
      std::string value;
      file >> value;
      TEST_CHECK(value == "replacement");
    }
    {
      std::ofstream file(existing);
      file << "keep";
    }
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, false};
    });
    TEST_CHECK(waitFor(manager, manager.copy({incoming.string()}, to.string())));
    {
      std::ifstream file(existing);
      std::string value;
      file >> value;
      TEST_CHECK(value == "keep");
    }
    std::atomic_int selfConflictCount{0};
    std::atomic_bool selfConflictMatched{true};
    manager.setConflictResolver([&selfConflictCount, &selfConflictMatched](
                                    const std::string& source, const std::string& destination, bool, const auto&,
                                    GCancellable*
                                ) {
      if (source != destination)
        selfConflictMatched.store(false, std::memory_order_relaxed);
      ++selfConflictCount;
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
    });
    TEST_CHECK(waitFor(manager, manager.copy({incoming.string()}, incoming.parent_path().string())));
    TEST_CHECK(selfConflictMatched.load(std::memory_order_relaxed));
    TEST_CHECK(selfConflictCount.load() == 1);
    TEST_CHECK(std::filesystem::exists(root / "incoming (copy).txt"));
    TEST_CHECK(!waitFor(manager, manager.move({incoming.string()}, incoming.parent_path().string())));
    TEST_CHECK(std::filesystem::exists(incoming));
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
    });
    const auto keepBothCopyId = manager.copy({incoming.string()}, to.string());
    TEST_CHECK(waitFor(manager, keepBothCopyId));
    TEST_CHECK(std::filesystem::exists(to / "incoming (copy).txt"));
    const auto keepBothCopy =
        std::ranges::find(manager.operations(), keepBothCopyId, &FileOperationManager::Operation::id);
    TEST_CHECK(keepBothCopy->completedEntries.size() == 1);
    assertCompletedEntry(*keepBothCopy, 0, to / "incoming (copy).txt");
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{
          FileOperationManager::ConflictAction::Rename, false, "chosen-name.txt"
      };
    });
    TEST_CHECK(waitFor(manager, manager.copy({incoming.string()}, to.string())));
    TEST_CHECK(std::filesystem::exists(to / "chosen-name.txt"));
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{
          FileOperationManager::ConflictAction::Rename, false, "../escape.txt"
      };
    });
    TEST_CHECK(!waitFor(manager, manager.copy({incoming.string()}, to.string())));
    TEST_CHECK(!std::filesystem::exists(root / "escape.txt"));
    {
      std::ofstream file(to / "occupied-name.txt");
      file << "must stay";
    }
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{
          FileOperationManager::ConflictAction::Rename, false, "occupied-name.txt"
      };
    });
    TEST_CHECK(!waitFor(manager, manager.copy({incoming.string()}, to.string())));
    {
      std::ifstream file(to / "occupied-name.txt");
      std::string value;
      file >> value;
      TEST_CHECK(value == "must");
    }
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
    });
    const auto cancelledConflictId = manager.copy({incoming.string()}, to.string());
    TEST_CHECK(!waitFor(manager, cancelledConflictId));
    const auto cancelledConflict =
        std::ranges::find(manager.operations(), cancelledConflictId, &FileOperationManager::Operation::id);
    TEST_CHECK(cancelledConflict->cancelled && cancelledConflict->resultTitle == "Copy cancelled");
    TEST_CHECK(cancelledConflict->completedEntries.empty());
    {
      std::ifstream file(existing);
      std::string value;
      file >> value;
      TEST_CHECK(value == "keep");
    }

    const auto completedBeforeCancel = root / "completed-before-cancel.txt";
    const auto conflictBeforeCancel = root / "conflict-before-cancel.txt";
    {
      std::ofstream file(completedBeforeCancel);
      file << "will transfer";
    }
    {
      std::ofstream file(conflictBeforeCancel);
      file << "incoming conflict";
    }
    {
      std::ofstream file(to / conflictBeforeCancel.filename());
      file << "preserve existing";
    }
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
    });
    const auto partialBatchId =
        manager.copy({completedBeforeCancel.string(), conflictBeforeCancel.string()}, to.string());
    TEST_CHECK(!waitFor(manager, partialBatchId));
    const auto partialBatch =
        std::ranges::find(manager.operations(), partialBatchId, &FileOperationManager::Operation::id);
    TEST_CHECK(partialBatch->cancelled && !partialBatch->succeeded);
    TEST_CHECK(partialBatch->completedEntries.size() == 1);
    assertCompletedEntry(*partialBatch, 0, to / completedBeforeCancel.filename());

    const auto identityFirstSource = root / "identity-first.txt";
    const auto identityConflictSource = root / "identity-conflict.txt";
    const auto identityFirstDestination = to / identityFirstSource.filename();
    const auto heldOriginalDestination = root / "held-original.txt";
    {
      std::ofstream file(identityFirstSource);
      file << "first copy";
    }
    {
      std::ofstream file(identityConflictSource);
      file << "cancel after replacement";
    }
    {
      std::ofstream file(to / identityConflictSource.filename());
      file << "existing conflict";
    }
    bool changedEarlierDestination = false;
    manager.setConflictResolver([&](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      std::filesystem::rename(identityFirstDestination, heldOriginalDestination);
      {
        std::ofstream file(identityFirstDestination);
        file << "unrelated replacement";
      }
      changedEarlierDestination = true;
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
    });
    const auto replacedPriorId =
        manager.copy({identityFirstSource.string(), identityConflictSource.string()}, to.string());
    TEST_CHECK(!waitFor(manager, replacedPriorId));
    TEST_CHECK(changedEarlierDestination);
    const auto replacedPrior =
        std::ranges::find(manager.operations(), replacedPriorId, &FileOperationManager::Operation::id);
    TEST_CHECK(replacedPrior->cancelled && replacedPrior->completedEntries.empty());
    {
      std::ifstream file(heldOriginalDestination);
      std::string value;
      file >> value;
      TEST_CHECK(value == "first");
    }
    {
      std::ifstream file(identityFirstDestination);
      std::string value;
      file >> value;
      TEST_CHECK(value == "unrelated");
    }
    {
      std::atomic_bool conflictReached{false};
      FileOperationManager cancelConflictMgr;
      cancelConflictMgr.setConflictResolver(
          [&conflictReached](const std::string&, const std::string&, bool, const auto&, GCancellable* cancellable) {
            conflictReached.store(true, std::memory_order_release);
            while (!g_cancellable_is_cancelled(cancellable))
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
          }
      );
      const auto cancelledReplaceId = cancelConflictMgr.copy({incoming.string()}, to.string());
      while (!conflictReached.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      cancelConflictMgr.cancel(cancelledReplaceId);
      TEST_CHECK(!waitFor(cancelConflictMgr, cancelledReplaceId));
      const auto cancelledReplace =
          std::ranges::find(cancelConflictMgr.operations(), cancelledReplaceId, &FileOperationManager::Operation::id);
      TEST_CHECK(cancelledReplace->cancelled && cancelledReplace->resultTitle == "Copy cancelled");
      TEST_CHECK(cancelledReplace->resultDescription.starts_with("Transfer to "));
      TEST_CHECK(std::filesystem::exists(incoming));
    }
    {
      std::ifstream file(existing);
      std::string value;
      file >> value;
      TEST_CHECK(value == "keep");
    }
    for (const auto& entry : std::filesystem::directory_iterator(to))
      TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-partial-"));
    const auto allA = root / "all-a.txt";
    const auto allB = root / "all-b.txt";
    {
      std::ofstream file(allA);
      file << "new a";
    }
    {
      std::ofstream file(allB);
      file << "new b";
    }
    {
      std::ofstream file(to / "all-a.txt");
      file << "old a";
    }
    {
      std::ofstream file(to / "all-b.txt");
      file << "old b";
    }
    int conflictCount = 0;
    manager.setConflictResolver(
        [&conflictCount](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
          ++conflictCount;
          return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, true};
        }
    );
    const auto skippedBatchId = manager.copy({allA.string(), allB.string()}, to.string());
    TEST_CHECK(waitFor(manager, skippedBatchId));
    TEST_CHECK(conflictCount == 1);
    const auto skippedBatch =
        std::ranges::find(manager.operations(), skippedBatchId, &FileOperationManager::Operation::id);
    TEST_CHECK(skippedBatch->completedEntries.empty());
    {
      std::ifstream file(to / "all-a.txt");
      std::string value;
      file >> value;
      TEST_CHECK(value == "old");
    }
    {
      std::ifstream file(to / "all-b.txt");
      std::string value;
      file >> value;
      TEST_CHECK(value == "old");
    }
    const auto mixedFile = root / "mixed-conflict.txt";
    const auto mixedFileAfterFolder = root / "mixed-after-folder.txt";
    const auto mixedFolder = root / "mixed-conflict-folder";
    {
      std::ofstream file(mixedFile);
      file << "incoming";
    }
    {
      std::ofstream file(mixedFileAfterFolder);
      file << "incoming later";
    }
    {
      std::ofstream file(to / mixedFile.filename());
      file << "existing";
    }
    {
      std::ofstream file(to / mixedFileAfterFolder.filename());
      file << "existing later";
    }
    std::filesystem::create_directory(mixedFolder);
    {
      std::ofstream file(mixedFolder / "payload.txt");
      file << "incoming payload";
    }
    std::filesystem::create_directory(to / mixedFolder.filename());
    {
      std::ofstream file(to / mixedFolder.filename() / "payload.txt");
      file << "existing payload";
    }
    int mixedConflictCount = 0;
    manager.setConflictResolver([&mixedConflictCount](
                                    const std::string&, const std::string&, bool, const auto&, GCancellable*
                                ) {
      ++mixedConflictCount;
      return FileOperationManager::ConflictDecision{
          FileOperationManager::ConflictAction::Skip, mixedConflictCount == 1
      };
    });
    TEST_CHECK(waitFor(
        manager, manager.copy({mixedFile.string(), mixedFolder.string(), mixedFileAfterFolder.string()}, to.string())
    ));
    TEST_CHECK(mixedConflictCount == 2);
    {
      std::ifstream file(to / mixedFile.filename());
      std::string value;
      file >> value;
      TEST_CHECK(value == "existing");
    }
    {
      std::ifstream file(to / mixedFolder.filename() / "payload.txt");
      std::string value;
      file >> value;
      TEST_CHECK(value == "existing");
    }
    {
      std::ifstream file(to / mixedFileAfterFolder.filename());
      std::string value;
      file >> value;
      TEST_CHECK(value == "existing");
    }
    const auto collidingDirectory = root / "colliding-folder";
    std::filesystem::create_directory(collidingDirectory);
    {
      std::ofstream file(collidingDirectory / "incoming.txt");
      file << "incoming folder payload";
    }
    std::filesystem::create_directory(to / collidingDirectory.filename());
    {
      std::ofstream file(to / collidingDirectory.filename() / "existing.txt");
      file << "existing folder payload";
    }
    int directoryConflictCount = 0;
    manager.setConflictResolver(
        [&directoryConflictCount](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
          ++directoryConflictCount;
          return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
        }
    );
    TEST_CHECK(waitFor(manager, manager.copy({collidingDirectory.string()}, to.string())));
    TEST_CHECK(directoryConflictCount == 1);
    TEST_CHECK(std::filesystem::exists(to / "colliding-folder" / "existing.txt"));
    TEST_CHECK(std::filesystem::exists(to / "colliding-folder (copy)" / "incoming.txt"));
    // Merge retains destination-only items and routes nested collisions through
    // the same resolver, including incompatible types and Keep Both names.
    const auto mergeSource = root / "merge-source" / "tree";
    const auto mergeParent = root / "merge-destination";
    const auto mergeTarget = mergeParent / "tree";
    std::filesystem::create_directories(mergeSource / "nested");
    std::filesystem::create_directories(mergeTarget / "nested");
    auto write = [](const auto& path, const std::string& text) {
      std::ofstream file(path);
      file << text;
    };
    auto read = [](const auto& path) {
      std::ifstream file(path);
      return std::string(std::istreambuf_iterator<char>(file), {});
    };
    write(mergeSource / "only-source.txt", "source");
    write(mergeTarget / "only-destination.txt", "destination");
    write(mergeSource / "nested" / "replace.txt", "new");
    write(mergeTarget / "nested" / "replace.txt", "old");
    write(mergeSource / "nested" / "skip.txt", "incoming");
    write(mergeTarget / "nested" / "skip.txt", "keep");
    write(mergeSource / "nested" / "both.txt", "incoming both");
    write(mergeTarget / "nested" / "both.txt", "existing both");
    write(mergeSource / "file-v-folder", "incoming file");
    std::filesystem::create_directory(mergeTarget / "file-v-folder");
    std::filesystem::create_directory(mergeSource / "folder-v-file");
    write(mergeTarget / "folder-v-file", "existing file");
    std::filesystem::create_directory_symlink(mergeSource, mergeSource / "loop");
    int merges = 0;
    manager.setConflictResolver([&](const std::string& source, const std::string&, bool, const auto&, GCancellable*) {
      using Action = FileOperationManager::ConflictAction;
      const auto name = std::filesystem::path(source).filename();
      if (name == "tree") {
        ++merges;
        return FileOperationManager::ConflictDecision{Action::Merge, true};
      }
      if (name == "replace.txt")
        return FileOperationManager::ConflictDecision{Action::Replace, false};
      if (name == "both.txt")
        return FileOperationManager::ConflictDecision{Action::Rename, false, "chosen.txt"};
      return FileOperationManager::ConflictDecision{Action::Skip, false};
    });
    TEST_CHECK(waitFor(manager, manager.copy({mergeSource.string()}, mergeParent.string())));
    const auto mergedCopy = std::ranges::find_if(manager.operations(), [&](const auto& operation) {
      return operation.kind == FileOperationManager::Kind::Copy
          && operation.source == mergeSource.string()
          && operation.destination == mergeParent.string()
          && operation.finished;
    });
    TEST_CHECK(mergedCopy != manager.operations().end());
    TEST_CHECK(mergedCopy->completedEntries.size() == 1);
    assertCompletedEntry(*mergedCopy, 0, mergeTarget);
    TEST_CHECK(merges == 1); // Explicit directory batch policy covers nested directories.
    TEST_CHECK(read(mergeTarget / "only-destination.txt") == "destination");
    TEST_CHECK(read(mergeTarget / "only-source.txt") == "source");
    TEST_CHECK(read(mergeTarget / "nested" / "replace.txt") == "new");
    TEST_CHECK(read(mergeTarget / "nested" / "skip.txt") == "keep");
    TEST_CHECK(read(mergeTarget / "nested" / "both.txt") == "existing both");
    TEST_CHECK(read(mergeTarget / "nested" / "chosen.txt") == "incoming both");
    TEST_CHECK(std::filesystem::is_directory(mergeTarget / "file-v-folder"));
    TEST_CHECK(read(mergeTarget / "folder-v-file") == "existing file");
    TEST_CHECK(std::filesystem::is_symlink(mergeTarget / "loop"));

    const auto allSkippedSource = root / "all-skipped-merge" / "tree";
    const auto allSkippedParent = root / "all-skipped-target";
    std::filesystem::create_directories(allSkippedSource);
    std::filesystem::create_directories(allSkippedParent / "tree");
    write(allSkippedSource / "same.txt", "incoming");
    write(allSkippedParent / "tree" / "same.txt", "existing");
    manager.setConflictResolver([](const std::string& source, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{
          std::filesystem::path(source).filename() == "tree" ? FileOperationManager::ConflictAction::Merge
                                                             : FileOperationManager::ConflictAction::Skip,
          false
      };
    });
    const auto allSkippedMergeId = manager.copy({allSkippedSource.string()}, allSkippedParent.string());
    TEST_CHECK(waitFor(manager, allSkippedMergeId));
    const auto allSkippedMerge =
        std::ranges::find(manager.operations(), allSkippedMergeId, &FileOperationManager::Operation::id);
    TEST_CHECK(allSkippedMerge->completedEntries.empty());
    TEST_CHECK(!waitFor(manager, manager.copy({mergeSource.string()}, (mergeSource / "nested").string())));
    TEST_CHECK(!waitFor(manager, manager.move({mergeSource.string()}, mergeSource.parent_path().string())));

    const auto explicitSource = root / "explicit-source" / "tree";
    const auto explicitParent = root / "explicit-target";
    std::filesystem::create_directories(explicitSource / "nested");
    std::filesystem::create_directories(explicitParent / "tree" / "nested");
    write(explicitSource / "nested" / "payload", "payload");
    std::atomic_int explicitMergeCount{0};
    std::atomic_bool explicitSawRemaining{true};
    manager.setConflictResolver([&](const std::string&, const std::string&, bool hasRemaining, const auto&,
                                    GCancellable*) {
      if (!hasRemaining) // A directory may contain further conflicts even in a single-item operation.
        explicitSawRemaining.store(false, std::memory_order_relaxed);
      ++explicitMergeCount;
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Merge, false};
    });
    TEST_CHECK(waitFor(manager, manager.move({explicitSource.string()}, explicitParent.string())));
    TEST_CHECK(explicitSawRemaining.load(std::memory_order_relaxed));
    TEST_CHECK(explicitMergeCount.load() == 2);
    TEST_CHECK(!std::filesystem::exists(explicitSource));
    TEST_CHECK(read(explicitParent / "tree" / "nested" / "payload") == "payload");
    const auto explicitMove = std::ranges::find_if(manager.operations(), [&](const auto& operation) {
      return operation.kind == FileOperationManager::Kind::Move
          && operation.source == explicitSource.string()
          && operation.destination == explicitParent.string()
          && operation.finished;
    });
    TEST_CHECK(explicitMove != manager.operations().end());
    TEST_CHECK(explicitMove->completedEntries.size() == 1);
    assertCompletedEntry(*explicitMove, 0, explicitParent / "tree");

    // Cancellation after a directory merge decision must be reported as cancelled,
    // retain the source and leave an existing nested destination untouched.
    manager.setConflictResolver([](const std::string& source, const std::string&, bool, const auto&,
                                   GCancellable* cancellable) {
      using Action = FileOperationManager::ConflictAction;
      if (std::filesystem::path(source).filename() == "tree")
        return FileOperationManager::ConflictDecision{Action::Merge, false};
      g_cancellable_cancel(cancellable);
      return FileOperationManager::ConflictDecision{Action::Cancel, false};
    });
    const auto beforeCancel = root / "before-cancel.txt";
    write(beforeCancel, "completed before cancellation");
    const auto cancelledMerge = manager.move({beforeCancel.string(), mergeSource.string()}, mergeParent.string());
    TEST_CHECK(!waitFor(manager, cancelledMerge));
    const auto cancelledMergeResult =
        std::ranges::find(manager.operations(), cancelledMerge, &FileOperationManager::Operation::id);
    TEST_CHECK(cancelledMergeResult->cancelled && !cancelledMergeResult->succeeded);
    TEST_CHECK(cancelledMergeResult->completedEntries.size() == 1);
    assertCompletedEntry(*cancelledMergeResult, 0, mergeParent / "before-cancel.txt");
    TEST_CHECK(read(mergeParent / "before-cancel.txt") == "completed before cancellation");
    TEST_CHECK(std::filesystem::exists(mergeSource / "nested" / "replace.txt"));
    TEST_CHECK(read(mergeTarget / "nested" / "replace.txt") == "new");

    // A failed nested decision cannot leave a partially completed merge marked successful.
    manager.setConflictResolver([](const std::string& source, const std::string&, bool, const auto&, GCancellable*) {
      using Action = FileOperationManager::ConflictAction;
      if (std::filesystem::path(source).filename() == "tree")
        return FileOperationManager::ConflictDecision{Action::Merge, true};
      return FileOperationManager::ConflictDecision{Action::Rename, false, "../invalid"};
    });
    const auto beforeFailure = root / "before-failure.txt";
    write(beforeFailure, "completed before failure");
    TEST_CHECK(!waitFor(manager, manager.copy({beforeFailure.string(), mergeSource.string()}, mergeParent.string())));
    const auto failedMerge = std::ranges::find_if(manager.operations(), [&](const auto& operation) {
      return operation.kind == FileOperationManager::Kind::Copy
          && operation.source == beforeFailure.string()
          && operation.destination == mergeParent.string()
          && operation.finished
          && !operation.succeeded;
    });
    TEST_CHECK(failedMerge != manager.operations().end());
    TEST_CHECK(failedMerge->completedEntries.size() == 1);
    assertCompletedEntry(*failedMerge, 0, mergeParent / "before-failure.txt");
    TEST_CHECK(read(mergeParent / "before-failure.txt") == "completed before failure");
    TEST_CHECK(read(mergeTarget / "nested" / "replace.txt") == "new");

    // A successful merge move removes only transferred source children. Skipped
    // collisions remain at source; destination-only files survive.
    const auto moveMerge = root / "move-merge" / "tree";
    std::filesystem::create_directories(moveMerge);
    write(moveMerge / "move-only.txt", "move payload");
    write(moveMerge / "only-destination.txt", "skip payload");
    manager.setConflictResolver([](const std::string& source, const std::string&, bool, const auto&, GCancellable*) {
      using Action = FileOperationManager::ConflictAction;
      return FileOperationManager::ConflictDecision{
          std::filesystem::path(source).filename() == "tree" ? Action::Merge : Action::Skip, false
      };
    });
    const auto moveMergeId = manager.move({moveMerge.string()}, mergeParent.string());
    TEST_CHECK(waitFor(manager, moveMergeId));
    TEST_CHECK(read(mergeTarget / "move-only.txt") == "move payload");
    TEST_CHECK(!std::filesystem::exists(moveMerge / "move-only.txt"));
    const auto moveMergeOperation =
        std::ranges::find(manager.operations(), moveMergeId, &FileOperationManager::Operation::id);
    TEST_CHECK(moveMergeOperation->completedEntries.size() == 1);
    assertCompletedEntry(*moveMergeOperation, 0, mergeTarget);
    TEST_CHECK(read(moveMerge / "only-destination.txt") == "skip payload");
    TEST_CHECK(read(mergeTarget / "only-destination.txt") == "destination");
    const auto moveSource = root / "move-source.txt";
    {
      std::ofstream file(moveSource);
      file << "move fixture";
    }
    TEST_CHECK(waitFor(manager, manager.move({moveSource.string()}, to.string())));
    TEST_CHECK(!std::filesystem::exists(moveSource) && std::filesystem::exists(to / "move-source.txt"));
    {
      nythera::test::ScopedOwnedDir shmRoot("/dev/shm", nythera::test::uniqueToken("nythera-fileops"), false);
      if (shmRoot.created()) {
        FileOperationManager shmManager;
        struct stat sourceStat{};
        struct stat targetStat{};
        const auto crossSource = root / "cross-device-source.txt";
        const auto crossDestination = shmRoot.path() / "destination";
        std::filesystem::create_directory(crossDestination);
        {
          std::ofstream file(crossSource);
          file << "cross-device move fixture";
        }
        if (stat(root.c_str(), &sourceStat) == 0
            && stat(shmRoot.path().c_str(), &targetStat) == 0
            && sourceStat.st_dev != targetStat.st_dev) {
          TEST_CHECK(waitFor(shmManager, shmManager.move({crossSource.string()}, crossDestination.string())));
          TEST_CHECK(!std::filesystem::exists(crossSource));
          TEST_CHECK(std::filesystem::exists(crossDestination / crossSource.filename()));
          const auto crossMergeSource = root / "cross-merge" / "tree";
          std::filesystem::create_directories(crossMergeSource / "nested");
          std::filesystem::create_directories(crossDestination / "tree" / "nested");
          write(crossMergeSource / "nested" / "payload", "cross merge payload");
          write(crossDestination / "tree" / "destination-only", "preserve");
          shmManager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
            return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Merge, true};
          });
          TEST_CHECK(waitFor(shmManager, shmManager.move({crossMergeSource.string()}, crossDestination.string())));
          TEST_CHECK(!std::filesystem::exists(crossMergeSource));
          TEST_CHECK(read(crossDestination / "tree" / "nested" / "payload") == "cross merge payload");
          TEST_CHECK(read(crossDestination / "tree" / "destination-only") == "preserve");
        }
      }
    }
    // Every operation acts on the link entry, never its referent (including
    // dangling links). Keep this fixture independent of merge/conflict choices.
    const auto linkTarget = root / "link-target";
    const auto linkSource = root / "link-source";
    const auto linkCopy = root / "link-copy";
    const auto linkMove = root / "link-move";
    for (const auto& dir : {linkTarget, linkSource, linkCopy, linkMove})
      std::filesystem::create_directory(dir);
    write(linkTarget / "payload", "preserved target");
    for (const auto* name : {"directory", "dangling"}) {
      const auto target = std::string(name) == "directory" ? linkTarget : root / "missing-link-target";
      std::filesystem::create_symlink(target, linkSource / name);
      TEST_CHECK(waitFor(manager, manager.copy({(linkSource / name).string()}, linkCopy.string())));
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(linkCopy / name)));
      TEST_CHECK(std::filesystem::read_symlink(linkCopy / name) == target);
      TEST_CHECK(waitFor(
          manager, manager.rename((linkCopy / name).string(), (linkCopy / (std::string(name) + "-renamed")).string())
      ));
      const auto renamed = linkCopy / (std::string(name) + "-renamed");
      TEST_CHECK(waitFor(manager, manager.move({renamed.string()}, linkMove.string())));
      const auto moved = linkMove / renamed.filename();
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(moved)));
      TEST_CHECK(waitFor(manager, manager.trash({moved.string()})));
      TEST_CHECK(!std::filesystem::is_symlink(std::filesystem::symlink_status(moved)));
      TEST_CHECK(waitFor(manager, manager.removePermanently({(linkSource / name).string()})));
      TEST_CHECK(read(linkTarget / "payload") == "preserved target");
      TEST_CHECK(!std::filesystem::exists(root / "missing-link-target"));
    }
    const auto trashSource = root / "trash-fixture.txt";
    {
      std::ofstream file(trashSource);
      file << "trash fixture";
    }
    TEST_CHECK(waitFor(manager, manager.trash({trashSource.string()})));
    TEST_CHECK(!std::filesystem::exists(trashSource));
    TEST_CHECK(waitFor(manager, manager.removePermanently({(root / "created").string()})));
    // Inline rename uses checked GIO moves for files and directories alike.
    const auto inlineSource = root / "inline-folder";
    const auto inlineTarget = root / "inline-folder-renamed";
    std::filesystem::create_directory(inlineSource);
    write(inlineSource / "child.txt", "retained");
    struct stat inlineIdentity{};
    TEST_CHECK(lstat(inlineSource.c_str(), &inlineIdentity) == 0);
    const auto inlineRename = [&](const std::filesystem::path& destination) {
      return manager.renameChecked(
          inlineSource.string(), destination.string(), inlineIdentity.st_dev, inlineIdentity.st_ino, {}
      );
    };
    std::filesystem::create_directory(inlineTarget);
    TEST_CHECK(!waitFor(manager, inlineRename(inlineTarget)));
    TEST_CHECK(read(inlineSource / "child.txt") == "retained");
    TEST_CHECK(!waitFor(manager, inlineRename(root / "missing-parent" / "invalid")));
    TEST_CHECK(read(inlineSource / "child.txt") == "retained");
    std::filesystem::remove(inlineTarget);
    TEST_CHECK(waitFor(manager, inlineRename(inlineTarget)));
    TEST_CHECK(!std::filesystem::exists(inlineSource) && read(inlineTarget / "child.txt") == "retained");

    // Properties jobs share cancellation/progress infrastructure and reject duplicate identities.
    const auto metadataFile = root / "metadata-fixture";
    write(metadataFile, "metadata");
    TEST_CHECK(chmod(metadataFile.c_str(), 0600) == 0);
    struct stat metadataState{};
    TEST_CHECK(lstat(metadataFile.c_str(), &metadataState) == 0);
    filemanager::PropertyRequest metadataRequest{
        metadataFile, static_cast<std::uint64_t>(metadataState.st_dev), metadataState.st_ino, 0600, 0640, {}, {},
        false
    };
    bool metadataFinished = false;
    bool metadataCallbackOk = false;
    const auto metadataId = manager.modifyProperties(metadataRequest, geteuid(), [&](const auto& result) {
      metadataFinished = true;
      struct stat refreshed{};
      metadataCallbackOk = result.ok()
          && result.succeeded == 1
          && lstat(metadataFile.c_str(), &refreshed) == 0
          && (refreshed.st_mode & 0777) == 0640;
    });
    TEST_CHECK(metadataId != 0 && manager.modifyProperties(metadataRequest, geteuid(), {}) == 0);
    TEST_CHECK(waitFor(manager, metadataId) && metadataFinished && metadataCallbackOk);

    const auto renameSource = root / "properties-name.txt";
    write(renameSource, "preserved original");
    struct stat renameIdentity{};
    TEST_CHECK(lstat(renameSource.c_str(), &renameIdentity) == 0);
    bool renameCallback = false;
    bool renameMessageNonEmpty = true;
    auto checkedRename = [&](const std::filesystem::path& source, const std::filesystem::path& destination) {
      return manager.renameChecked(
          source.string(), destination.string(), renameIdentity.st_dev, renameIdentity.st_ino,
          [&](bool success, const std::string& message) {
            renameCallback = true;
            if (message.empty())
              renameMessageNonEmpty = false;
            (void)success;
          }
      );
    };
    const auto renameDestination = root / "renamed.txt-backup";
    TEST_CHECK(
        waitFor(manager, checkedRename(renameSource, renameDestination)) && renameCallback && renameMessageNonEmpty
    );
    TEST_CHECK(!std::filesystem::exists(renameSource) && read(renameDestination) == "preserved original");
    TEST_CHECK(!waitFor(manager, checkedRename(renameDestination, root / "")));
    write(root / "collision", "do not overwrite");
    TEST_CHECK(!waitFor(manager, checkedRename(renameDestination, root / "collision")));
    TEST_CHECK(read(root / "collision") == "do not overwrite" && std::filesystem::exists(renameDestination));
    const auto renamedOld = root / "original-inode";
    std::filesystem::rename(renameDestination, renamedOld);
    write(renameDestination, "replacement");
    TEST_CHECK(!waitFor(manager, checkedRename(renameDestination, root / "must-not-exist")));
    TEST_CHECK(read(renameDestination) == "replacement" && !std::filesystem::exists(root / "must-not-exist"));
    const auto propertiesLink = root / "properties-link";
    std::filesystem::create_symlink(renamedOld, propertiesLink);
    TEST_CHECK(lstat(propertiesLink.c_str(), &renameIdentity) == 0);
    const auto newLink = root / "renamed-link";
    TEST_CHECK(waitFor(manager, checkedRename(propertiesLink, newLink)));
    TEST_CHECK(
        std::filesystem::is_symlink(std::filesystem::symlink_status(newLink))
        && read(renamedOld) == "preserved original"
    );

    const auto edit = root / "edit";
    std::filesystem::create_directory(edit);
    const auto archive = edit / "archive.tar.gz";
    const auto manualLink = edit / "manual";
    const auto dangling = edit / "archive-link.tar.gz";
    const auto autoLink = edit / "archive-link-2.tar.gz";
    write(archive, "archive");
    std::filesystem::create_symlink("already-missing", dangling);
    {
      FileOperationManager links;
      TEST_CHECK(!waitFor(links, links.createSymlink("replacement", dangling.string())));
      TEST_CHECK(std::filesystem::read_symlink(dangling) == "already-missing");
      TEST_CHECK(waitFor(links, links.createSymlink("../missing/./file", manualLink.string())));
      TEST_CHECK(std::filesystem::read_symlink(manualLink) == "../missing/./file");
      assertCompletedEntry(links.operations().back(), 0, manualLink);
      TEST_CHECK(waitFor(links, links.undo()));
      TEST_CHECK(!std::filesystem::is_symlink(std::filesystem::symlink_status(manualLink)));
      TEST_CHECK(waitFor(links, links.redo()));
      TEST_CHECK(std::filesystem::read_symlink(manualLink) == "../missing/./file");
      TEST_CHECK(waitFor(links, links.createSymlink(archive.string(), dangling.string(), true)));
      TEST_CHECK(std::filesystem::read_symlink(autoLink) == archive);
      assertCompletedEntry(links.operations().back(), 0, autoLink);
      TEST_CHECK(waitFor(links, links.undo()) && links.canRedo());
      TEST_CHECK(waitFor(links, links.createSymlink(edit.string(), (edit / "edit-link").string(), true)));
      TEST_CHECK(std::filesystem::read_symlink(edit / "edit-link") == edit);
      TEST_CHECK(!links.canRedo());
    }
    {
      FileOperationManager duplicate;
      const auto ordinary = edit / "document.txt";
      const auto folder = edit / "folder";
      std::filesystem::create_directory(folder);
      write(folder / "child", "nested");
      write(ordinary, "original");
      std::filesystem::create_symlink("not-there", edit / "document (copy).txt");
      const auto firstCopy = edit / "document (copy 2).txt";
      TEST_CHECK(waitFor(
          duplicate, duplicate.duplicate({folder.string(), manualLink.string(), ordinary.string()}, edit.string())
      ));
      TEST_CHECK(read(edit / "folder (copy)" / "child") == "nested");
      TEST_CHECK(std::filesystem::read_symlink(edit / "manual (copy)") == "../missing/./file");
      TEST_CHECK(std::filesystem::read_symlink(edit / "document (copy).txt") == "not-there");
      TEST_CHECK(duplicate.operations().back().completedEntries.size() == 3);
      assertCompletedEntry(duplicate.operations().back(), 2, firstCopy);
      TEST_CHECK(waitFor(duplicate, duplicate.undo()));
      TEST_CHECK(!std::filesystem::exists(firstCopy));
      TEST_CHECK(waitFor(duplicate, duplicate.redo()));
      TEST_CHECK(read(firstCopy) == "original");
      write(firstCopy, "external edits");
      TEST_CHECK(!waitFor(duplicate, duplicate.undo()));
      TEST_CHECK(!duplicate.canUndo() && read(firstCopy) == "external edits");
    }
    {
      FileOperationManager links;
      const auto repeatedLink = edit / "manual (copy 2)";
      TEST_CHECK(waitFor(links, links.duplicate({manualLink.string()}, edit.string())));
      assertCompletedEntry(links.operations().back(), 0, repeatedLink);
      TEST_CHECK(waitFor(links, links.undo()));
      TEST_CHECK(!std::filesystem::is_symlink(std::filesystem::symlink_status(repeatedLink)));
      TEST_CHECK(waitFor(links, links.redo()));
      TEST_CHECK(std::filesystem::read_symlink(repeatedLink) == "../missing/./file");
    }
    {
      FileOperationManager creates;
      const auto emptyFolder = edit / "empty";
      const auto emptyFile = edit / "empty-file";
      TEST_CHECK(waitFor(creates, creates.createFolder(emptyFolder.string())));
      TEST_CHECK(waitFor(creates, creates.undo()) && !std::filesystem::exists(emptyFolder));
      TEST_CHECK(waitFor(creates, creates.redo()) && std::filesystem::is_empty(emptyFolder));
      TEST_CHECK(waitFor(creates, creates.createFile(emptyFile.string())));
      TEST_CHECK(waitFor(creates, creates.undo()) && !std::filesystem::exists(emptyFile));
      TEST_CHECK(waitFor(creates, creates.redo()));
      TEST_CHECK(std::filesystem::file_size(emptyFile) == 0);
      std::filesystem::rename(emptyFile, edit / "former-empty-file");
      write(emptyFile, "replacement");
      TEST_CHECK(!waitFor(creates, creates.undo()));
      TEST_CHECK(read(emptyFile) == "replacement");
    }
    {
      FileOperationManager creates;
      const auto parent = edit / "parent";
      const auto relocated = edit / "relocated-parent";
      std::filesystem::create_directory(parent);
      TEST_CHECK(waitFor(creates, creates.createFile((parent / "file").string())));
      std::filesystem::rename(parent, relocated);
      std::filesystem::create_directory(parent);
      write(parent / "file", "external file in replacement directory");
      TEST_CHECK(!waitFor(creates, creates.undo()));
      TEST_CHECK(read(parent / "file") == "external file in replacement directory");
      TEST_CHECK(std::filesystem::file_size(relocated / "file") == 0);
    }
    {
      FileOperationManager replay;
      FileOperationManager otherWindow;
      const auto first = edit / "first";
      const auto second = edit / "second";
      const auto third = edit / "third";
      write(first, "keep this");
      TEST_CHECK(waitFor(replay, replay.rename(first.string(), second.string())));
      TEST_CHECK(waitFor(replay, replay.rename(second.string(), third.string())));
      TEST_CHECK(waitFor(otherWindow, otherWindow.createSymlink("elsewhere", second.string())));
      TEST_CHECK(!waitFor(replay, replay.undo()));
      TEST_CHECK(std::filesystem::read_symlink(second) == "elsewhere" && read(third) == "keep this");
      TEST_CHECK(waitFor(otherWindow, otherWindow.undo()));
      TEST_CHECK(waitFor(replay, replay.undo()) && read(second) == "keep this");
      TEST_CHECK(waitFor(replay, replay.undo()) && read(first) == "keep this");
      std::filesystem::create_symlink("redo-conflict", second);
      TEST_CHECK(!waitFor(replay, replay.redo()));
      TEST_CHECK(read(first) == "keep this" && std::filesystem::read_symlink(second) == "redo-conflict");
      std::filesystem::remove(second);
      TEST_CHECK(waitFor(replay, replay.redo()) && read(second) == "keep this");
      TEST_CHECK(waitFor(replay, replay.redo()) && read(third) == "keep this");
      TEST_CHECK(waitFor(replay, replay.move({third.string()}, to.string())));
      TEST_CHECK(waitFor(replay, replay.undo()) && read(third) == "keep this");
      TEST_CHECK(waitFor(replay, replay.redo()) && read(to / third.filename()) == "keep this");
      TEST_CHECK(waitFor(replay, replay.copy({archive.string()}, to.string())));
      TEST_CHECK(waitFor(replay, replay.undo()) && !std::filesystem::exists(to / archive.filename()));
      write(archive, "externally changed source");
      TEST_CHECK(!waitFor(replay, replay.redo()));
      TEST_CHECK(!replay.canRedo() && !std::filesystem::exists(to / archive.filename()));
    }
    {
      FileOperationManager protectsCopy;
      const auto onlyOriginal = edit / "only-original";
      write(onlyOriginal, "keep the only surviving copy");
      TEST_CHECK(waitFor(protectsCopy, protectsCopy.copy({onlyOriginal.string()}, to.string())));
      std::filesystem::remove(onlyOriginal);
      TEST_CHECK(!waitFor(protectsCopy, protectsCopy.undo()));
      TEST_CHECK(read(to / onlyOriginal.filename()) == "keep the only surviving copy");
    }
    {
      FileOperationManager partial;
      // The first item commits before the second encounters the conflict pause.
      const auto earlier = edit / "committed.txt";
      const auto later = edit / "blocked.txt";
      const auto committed = to / earlier.filename();
      write(earlier, "first payload");
      write(later, "second payload");
      write(to / later.filename(), "occupied");
      std::atomic_bool resolving{false};
      partial.setConflictResolver([&](const auto&, const auto&, bool, const auto&, GCancellable* cancellable) {
        resolving.store(true, std::memory_order_release);
        while (!g_cancellable_is_cancelled(cancellable))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel};
      });
      const auto job = partial.copy({earlier.string(), later.string()}, to.string());
      while (!resolving.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      TEST_CHECK(!partial.canUndo() && partial.undo() == 0);
      partial.cancel(job);
      TEST_CHECK(!waitFor(partial, job) && partial.operations().back().cancelled);
      assertCompletedEntry(partial.operations().back(), 0, committed);
      const auto undoJob = partial.undo();
      TEST_CHECK(partial.createFile((edit / "overlap").string()) == 0);
      TEST_CHECK(waitFor(partial, undoJob));
      TEST_CHECK(!std::filesystem::exists(committed) && read(to / later.filename()) == "occupied");
      TEST_CHECK(waitFor(partial, partial.redo()) && read(committed) == "first payload");
      const auto failedDuplicate =
          partial.duplicate({earlier.string(), (to / later.filename()).string()}, edit.string());
      TEST_CHECK(!waitFor(partial, failedDuplicate));
      assertCompletedEntry(partial.operations().back(), 0, edit / "committed (copy).txt");
      TEST_CHECK(waitFor(partial, partial.undo()));
      TEST_CHECK(!std::filesystem::exists(edit / "committed (copy).txt"));
    }
    {
      // F-02 regressions: destination appearing or changing mid-transfer or during
      // Replace must never be silently overwritten, and failed rollback/cleanup
      // must report the surviving backup path.
      const auto raceRoot = root / "f02-races";
      const auto raceTo = raceRoot / "to";
      std::filesystem::create_directories(raceTo);

      // 1. Non-replace Copy where destination appears after conflict check while
      // staging is in progress must fail with EEXIST rather than clobbering it.
      FileOperationManager raceManager;
      const auto lateSource = raceRoot / "late-appearance.txt";
      const auto lateTarget = raceTo / lateSource.filename();
      write(lateSource, "incoming payload");
      unsigned lateBoundaryCalls = 0;
      bool injectedLateTarget = false;
      FileOperationManagerTestAccess::boundary(raceManager, [&](std::uint64_t, std::uint64_t) {
        ++lateBoundaryCalls;
        if (lateBoundaryCalls >= 3 && !injectedLateTarget) {
          injectedLateTarget = true;
          write(lateTarget, "appeared during copy");
        }
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.copy({lateSource.string()}, raceTo.string())));
      TEST_CHECK(injectedLateTarget);
      TEST_CHECK(read(lateTarget) == "appeared during copy");
      for (const auto& entry : std::filesystem::directory_iterator(raceTo))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-partial-"));
      FileOperationManagerTestAccess::boundary(raceManager, {});

      // 2. Replace Copy where the destination inode is replaced by a different
      // file (or directory) between conflict resolution and finalisation must
      // refuse to move/delete the unapproved replacement item.
      const auto swappedSource = raceRoot / "swapped-target.txt";
      const auto swappedTarget = raceTo / swappedSource.filename();
      const auto heldApproved = raceRoot / "held-approved-target.txt";
      write(swappedSource, "incoming replacement");
      write(swappedTarget, "approved target");
      raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        std::filesystem::rename(swappedTarget, heldApproved);
        write(swappedTarget, "unapproved replacement inode");
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.copy({swappedSource.string()}, raceTo.string())));
      TEST_CHECK(read(swappedTarget) == "unapproved replacement inode");
      TEST_CHECK(read(heldApproved) == "approved target");
      TEST_CHECK(
          raceManager.operations().back().resultDescription.find("changed before it could be replaced")
          != std::string::npos
      );

      // 3. Replace Rename where the destination inode is swapped during conflict
      // resolution must refuse to clobber the new destination and leave source intact.
      const auto renameSwapSource = raceRoot / "rename-swap-source.txt";
      const auto renameSwapTarget = raceRoot / "rename-swap-target.txt";
      const auto heldRenameTarget = raceRoot / "held-rename-target.txt";
      write(renameSwapSource, "rename source payload");
      write(renameSwapTarget, "approved rename target");
      raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        std::filesystem::rename(renameSwapTarget, heldRenameTarget);
        write(renameSwapTarget, "new unapproved rename target");
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.rename(renameSwapSource.string(), renameSwapTarget.string())));
      TEST_CHECK(read(renameSwapSource) == "rename source payload");
      TEST_CHECK(read(renameSwapTarget) == "new unapproved rename target");

      // 4. Non-replace Rename where the destination appears after the initial
      // existence check (simulated by removing the conflict resolver so a
      // pre-existing destination is not replaced) or during checked rename must
      // never overwrite the destination.
      const auto noreplaceSource = raceRoot / "noreplace-source.txt";
      const auto noreplaceTarget = raceRoot / "noreplace-target.txt";
      write(noreplaceSource, "do not overwrite with this");
      write(noreplaceTarget, "must survive rename");
      raceManager.setConflictResolver({});
      TEST_CHECK(!waitFor(raceManager, raceManager.rename(noreplaceSource.string(), noreplaceTarget.string())));
      TEST_CHECK(read(noreplaceSource) == "do not overwrite with this");
      TEST_CHECK(read(noreplaceTarget) == "must survive rename");

      // 5. Replace Copy where the destination is replaced during copyTree
      // (after the user approved replacing the original destination inode) must
      // refuse to move/delete the newly appeared destination inode and clean up
      // the temporary staged file.
      const auto midCopySwapSource = raceRoot / "midcopy-swap.txt";
      const auto midCopySwapTarget = raceTo / midCopySwapSource.filename();
      const auto heldMidCopyTarget = raceRoot / "held-midcopy-target.txt";
      write(midCopySwapSource, "incoming copy");
      write(midCopySwapTarget, "approved before copy");
      bool conflictApproved = false;
      bool swappedDuringCopy = false;
      raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        conflictApproved = true;
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      FileOperationManagerTestAccess::boundary(raceManager, [&](std::uint64_t, std::uint64_t) {
        if (conflictApproved && !swappedDuringCopy) {
          swappedDuringCopy = true;
          std::filesystem::rename(midCopySwapTarget, heldMidCopyTarget);
          write(midCopySwapTarget, "swapped while copying");
        }
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.copy({midCopySwapSource.string()}, raceTo.string())));
      TEST_CHECK(swappedDuringCopy);
      TEST_CHECK(read(midCopySwapTarget) == "swapped while copying");
      TEST_CHECK(read(heldMidCopyTarget) == "approved before copy");
      for (const auto& entry : std::filesystem::directory_iterator(raceTo))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-partial-"));
      FileOperationManagerTestAccess::boundary(raceManager, {});

      // 6. Replace Rename where finalising source -> target fails after the
      // destination has been moved to its temporary backup must roll back the
      // backup so the original destination is preserved intact.
      const auto rollbackSource = raceRoot / "rollback-source.txt";
      const auto rollbackTarget = raceRoot / "rollback-target.txt";
      write(rollbackSource, "vanishing source");
      write(rollbackTarget, "original target must be restored");
      raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        std::filesystem::remove(rollbackSource);
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.rename(rollbackSource.string(), rollbackTarget.string())));
      TEST_CHECK(read(rollbackTarget) == "original target must be restored");
      for (const auto& entry : std::filesystem::directory_iterator(raceRoot))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-backup-"));

      // 7a. Replacement timing gap in Copy: destination swapped right after
      // pre-verification and before moving target -> backup must be detected via
      // post-move verification and atomically rolled back without deleting the
      // unapproved item.
      const auto postCopySource = raceRoot / "post-copy-source.txt";
      const auto postCopyTarget = raceTo / postCopySource.filename();
      const auto heldPostCopy = raceRoot / "held-post-copy.txt";
      write(postCopySource, "incoming copy payload");
      write(postCopyTarget, "approved original");
      raceManager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      bool swappedBeforeBackup = false;
      setFileOperationReplaceBackupHookForTesting([&](const char* targetPath, const char*, bool afterMove) {
        if (!afterMove && !swappedBeforeBackup && targetPath != nullptr && targetPath == postCopyTarget) {
          swappedBeforeBackup = true;
          std::filesystem::rename(postCopyTarget, heldPostCopy);
          write(postCopyTarget, "unapproved mid-backup file");
        }
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.copy({postCopySource.string()}, raceTo.string())));
      setFileOperationReplaceBackupHookForTesting({});
      TEST_CHECK(swappedBeforeBackup);
      TEST_CHECK(read(postCopyTarget) == "unapproved mid-backup file");
      TEST_CHECK(read(postCopySource) == "incoming copy payload");
      TEST_CHECK(read(heldPostCopy) == "approved original");
      TEST_CHECK(
          raceManager.operations().back().resultDescription.find("changed before it could be replaced")
          != std::string::npos
      );
      for (const auto& entry : std::filesystem::directory_iterator(raceTo)) {
        const auto fname = entry.path().filename().string();
        TEST_CHECK(!fname.starts_with(".nythera-backup-") && !fname.starts_with(".nythera-partial-"));
      }

      // 7b. Replacement timing gap in Rename: destination swapped to a directory
      // containing user data right before moving target -> backup must roll back
      // the directory intact and never remove it.
      const auto postRenameSource = raceRoot / "post-rename-source.txt";
      const auto postRenameTarget = raceRoot / "post-rename-target.txt";
      const auto heldPostRename = raceRoot / "held-post-rename.txt";
      write(postRenameSource, "incoming rename payload");
      write(postRenameTarget, "approved rename original");
      bool swappedToDir = false;
      setFileOperationReplaceBackupHookForTesting([&](const char* targetPath, const char*, bool afterMove) {
        if (!afterMove && !swappedToDir && targetPath != nullptr && targetPath == postRenameTarget) {
          swappedToDir = true;
          std::filesystem::rename(postRenameTarget, heldPostRename);
          std::filesystem::create_directory(postRenameTarget);
          write(postRenameTarget / "child.txt", "preserve directory child");
        }
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.rename(postRenameSource.string(), postRenameTarget.string())));
      setFileOperationReplaceBackupHookForTesting({});
      TEST_CHECK(swappedToDir);
      TEST_CHECK(std::filesystem::is_directory(postRenameTarget));
      TEST_CHECK(read(postRenameTarget / "child.txt") == "preserve directory child");
      TEST_CHECK(read(postRenameSource) == "incoming rename payload");
      TEST_CHECK(read(heldPostRename) == "approved rename original");
      for (const auto& entry : std::filesystem::directory_iterator(raceRoot))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-backup-"));

      // 8. Post-verification rollback collision preservation: if target swaps
      // right before target -> backup and another new item appears at target
      // before rollback runs, neither unapproved item is overwritten and the
      // surviving backup path is reported.
      const auto collideSource = raceRoot / "swap-collide.txt";
      const auto collideTarget = raceTo / collideSource.filename();
      const auto heldCollide = raceRoot / "held-swap-collide.txt";
      write(collideSource, "incoming collide");
      write(collideTarget, "approved collide");
      bool didPreSwap = false;
      bool didPostAppear = false;
      setFileOperationReplaceBackupHookForTesting([&](const char* targetPath, const char*, bool afterMove) {
        if (targetPath == nullptr || targetPath != collideTarget)
          return;
        if (!afterMove && !didPreSwap) {
          didPreSwap = true;
          std::filesystem::rename(collideTarget, heldCollide);
          write(collideTarget, "unapproved-1");
        } else if (afterMove && !didPostAppear) {
          didPostAppear = true;
          write(collideTarget, "unapproved-2");
        }
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.copy({collideSource.string()}, raceTo.string())));
      setFileOperationReplaceBackupHookForTesting({});
      TEST_CHECK(didPreSwap && didPostAppear);
      TEST_CHECK(read(collideTarget) == "unapproved-2");
      TEST_CHECK(read(heldCollide) == "approved collide");
      std::filesystem::path survivingBackup;
      for (const auto& entry : std::filesystem::directory_iterator(raceTo)) {
        const auto fname = entry.path().filename().string();
        TEST_CHECK(!fname.starts_with(".nythera-partial-"));
        if (fname.starts_with(".nythera-backup-")) {
          TEST_CHECK(survivingBackup.empty());
          survivingBackup = entry.path();
        }
      }
      TEST_CHECK(!survivingBackup.empty());
      TEST_CHECK(read(survivingBackup) == "unapproved-1");
      const auto& collideDesc = raceManager.operations().back().resultDescription;
      TEST_CHECK(collideDesc.find("previous destination remains at ") != std::string::npos);
      TEST_CHECK(collideDesc.find(survivingBackup.filename().string()) != std::string::npos);
      std::filesystem::remove(survivingBackup);

      // 9. Mid-finalisation destination appearance during Replace: after the
      // approved target moves to backup and passes post-verification, a new
      // file appearing at target before staged -> target must fail with EEXIST
      // and leave both the newly appeared file and the backup intact.
      const auto finalAppearSource = raceRoot / "final-appear.txt";
      const auto finalAppearTarget = raceTo / finalAppearSource.filename();
      write(finalAppearSource, "incoming final appear");
      write(finalAppearTarget, "approved backup content");
      bool injectedFinalAppear = false;
      setFileOperationReplaceBackupHookForTesting([&](const char* targetPath, const char*, bool afterMove) {
        if (afterMove && !injectedFinalAppear && targetPath != nullptr && targetPath == finalAppearTarget) {
          injectedFinalAppear = true;
          write(finalAppearTarget, "appeared before finalise");
        }
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.copy({finalAppearSource.string()}, raceTo.string())));
      setFileOperationReplaceBackupHookForTesting({});
      TEST_CHECK(injectedFinalAppear);
      TEST_CHECK(read(finalAppearTarget) == "appeared before finalise");
      std::filesystem::path approvedBackup;
      for (const auto& entry : std::filesystem::directory_iterator(raceTo)) {
        const auto fname = entry.path().filename().string();
        TEST_CHECK(!fname.starts_with(".nythera-partial-"));
        if (fname.starts_with(".nythera-backup-")) {
          TEST_CHECK(approvedBackup.empty());
          approvedBackup = entry.path();
        }
      }
      TEST_CHECK(!approvedBackup.empty());
      TEST_CHECK(read(approvedBackup) == "approved backup content");
      const auto& appearDesc = raceManager.operations().back().resultDescription;
      TEST_CHECK(appearDesc.find("previous destination remains at ") != std::string::npos);
      TEST_CHECK(appearDesc.find(approvedBackup.filename().string()) != std::string::npos);
      std::filesystem::remove(approvedBackup);

      // 10. Filesystems lacking RENAME_NOREPLACE: Copy, Move, and Rename must
      // fail safely without falling back to non-atomic g_file_move(), while
      // cross-filesystem transfers into a supporting destination and Create*
      // operations (using O_CREAT|O_EXCL, mkdir, symlink) still succeed safely.
      const auto noReplaceDir = raceRoot / "no-noreplace";
      const auto normalDir = raceRoot / "normal-dest";
      std::filesystem::create_directories(noReplaceDir);
      std::filesystem::create_directories(normalDir);
      const std::string noReplacePrefix = noReplaceDir.string() + "/";
      int simulatedErrno = EINVAL;
      setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
        const bool touchesNoReplace = (sourcePath != nullptr && std::string(sourcePath).starts_with(noReplacePrefix))
            || (destinationPath != nullptr && std::string(destinationPath).starts_with(noReplacePrefix));
        if (touchesNoReplace) {
          errno = simulatedErrno;
          return -1;
        }
        return static_cast<int>(
            syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
        );
      });

      // When RENAME_NOREPLACE is unsupported (EINVAL/EOPNOTSUPP/ENOSYS), regular
      // files and symlinks succeed via linkat + verified unlinkat, and directories
      // succeed via Option A (fstatat + renameat), while collisions still fail closed.
      raceManager.setConflictResolver({});
      const auto nrCopySource = raceRoot / "nr-copy-source.txt";
      write(nrCopySource, "copied via linkat fallback");
      TEST_CHECK(waitFor(raceManager, raceManager.copy({nrCopySource.string()}, noReplaceDir.string())));
      TEST_CHECK(read(noReplaceDir / "nr-copy-source.txt") == "copied via linkat fallback");
      for (const auto& entry : std::filesystem::directory_iterator(noReplaceDir))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-partial-"));

      // Existing destination without Replace approval must not be clobbered.
      const auto nrMoveSource = raceRoot / "nr-move-source.txt";
      const auto nrMoveExisting = noReplaceDir / "nr-move-source.txt";
      write(nrMoveSource, "incoming move payload");
      write(nrMoveExisting, "must stay untouched on move");
      TEST_CHECK(!waitFor(raceManager, raceManager.move({nrMoveSource.string()}, noReplaceDir.string())));
      TEST_CHECK(read(nrMoveSource) == "incoming move payload");
      TEST_CHECK(read(nrMoveExisting) == "must stay untouched on move");
      for (const auto& entry : std::filesystem::directory_iterator(noReplaceDir)) {
        const auto fname = entry.path().filename().string();
        TEST_CHECK(!fname.starts_with(".nythera-partial-") && !fname.starts_with(".nythera-backup-"));
      }

      // Same-FS rename to absent destination succeeds via linkat + verified unlinkat,
      // while rename onto an occupied destination without Replace approval fails safely.
      const auto nrRenameSource = noReplaceDir / "nr-rename-src.txt";
      const auto nrRenameAbsentTarget = noReplaceDir / "nr-rename-dst.txt";
      const auto nrRenameExistingTarget = noReplaceDir / "nr-rename-occupied.txt";
      write(nrRenameSource, "rename payload");
      write(nrRenameExistingTarget, "existing rename target");
      TEST_CHECK(waitFor(raceManager, raceManager.rename(nrRenameSource.string(), nrRenameAbsentTarget.string())));
      TEST_CHECK(!std::filesystem::exists(nrRenameSource));
      TEST_CHECK(read(nrRenameAbsentTarget) == "rename payload");
      TEST_CHECK(
          !waitFor(raceManager, raceManager.rename(nrRenameAbsentTarget.string(), nrRenameExistingTarget.string()))
      );
      TEST_CHECK(read(nrRenameAbsentTarget) == "rename payload");
      TEST_CHECK(read(nrRenameExistingTarget) == "existing rename target");

      // EOPNOTSUPP and ENOSYS also trigger the linkat + verified unlinkat fallback.
      simulatedErrno = EOPNOTSUPP;
      std::filesystem::remove(noReplaceDir / "nr-copy-source.txt");
      TEST_CHECK(waitFor(raceManager, raceManager.copy({nrCopySource.string()}, noReplaceDir.string())));
      TEST_CHECK(read(noReplaceDir / "nr-copy-source.txt") == "copied via linkat fallback");
      simulatedErrno = ENOSYS;
      TEST_CHECK(waitFor(raceManager, raceManager.rename(nrRenameAbsentTarget.string(), nrRenameSource.string())));
      TEST_CHECK(read(nrRenameSource) == "rename payload");
      simulatedErrno = EINVAL;

      // When BOTH RENAME_NOREPLACE and linkat are unsupported on a destination for
      // regular files, preflightTransferDestination fails fast before copying data.
      setFileOperationLinkAtHookForTesting([&](const char*, const char* dstPath) -> int {
        if (dstPath != nullptr && std::string(dstPath).starts_with(noReplacePrefix)) {
          errno = EOPNOTSUPP;
          return -1;
        }
        return 0;
      });
      std::filesystem::remove(noReplaceDir / "nr-copy-source.txt");
      TEST_CHECK(!waitFor(raceManager, raceManager.copy({nrCopySource.string()}, noReplaceDir.string())));
      TEST_CHECK(
          raceManager.operations().back().resultDescription.find("does not support atomic non-overwriting moves")
          != std::string::npos
      );
      TEST_CHECK(!std::filesystem::exists(noReplaceDir / "nr-copy-source.txt"));

      // For symlinks, when linkat returns EOPNOTSUPP, exclusive symlinkat fallback succeeds!
      const auto nrSymSrc = noReplaceDir / "sym-src";
      const auto nrSymRenamed = noReplaceDir / "sym-renamed";
      std::filesystem::create_symlink("relative-target-value", nrSymSrc);
      TEST_CHECK(waitFor(raceManager, raceManager.rename(nrSymSrc.string(), nrSymRenamed.string())));
      TEST_CHECK(!std::filesystem::exists(std::filesystem::symlink_status(nrSymSrc)));
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(nrSymRenamed)));
      TEST_CHECK(std::filesystem::read_symlink(nrSymRenamed) == "relative-target-value");
      setFileOperationLinkAtHookForTesting({});

      // Split-state preservation: if linkat publishes the new destination during
      // same-FS rename/move on a RENAME_NOREPLACE-unsupported mount, and unlinkat
      // of the source fails (EIO), the operation must report failure with both
      // paths preserved rather than falsely reporting success.
      const auto splitSrc = noReplaceDir / "split-src.txt";
      const auto splitDst = noReplaceDir / "split-dst.txt";
      write(splitSrc, "split state payload");
      setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
        return path != nullptr && std::string(path) == splitSrc.string() ? EIO : 0;
      });
      TEST_CHECK(!waitFor(raceManager, raceManager.rename(splitSrc.string(), splitDst.string())));
      setFileOperationRemoveEntryHookForTesting({});
      TEST_CHECK(read(splitSrc) == "split state payload");
      TEST_CHECK(read(splitDst) == "split state payload");
      TEST_CHECK(raceManager.operations().back().mutated);

      // Checked replacement on a RENAME_NOREPLACE-unsupported destination uses a
      // private .nythera-backup-* directory, replaces atomically, and cleans up.
      const auto nrRepSrc = raceRoot / "nr-rep-src.txt";
      const auto nrRepDst = noReplaceDir / "nr-rep-src.txt";
      write(nrRepSrc, "replacement new content");
      write(nrRepDst, "replacement old content");
      raceManager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      TEST_CHECK(waitFor(raceManager, raceManager.copy({nrRepSrc.string()}, noReplaceDir.string())));
      TEST_CHECK(read(nrRepDst) == "replacement new content");
      for (const auto& entry : std::filesystem::directory_iterator(noReplaceDir))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-backup-"));

      // Directory operations under Option A on noReplaceDir:
      // Copy, Move, Rename, Duplicate, and collision protection against non-empty
      // directories, regular files, and symlinks.
      raceManager.setConflictResolver({});
      const auto nrDirSrc = raceRoot / "nr-dir-tree";
      std::filesystem::create_directories(nrDirSrc / "sub");
      write(nrDirSrc / "sub" / "leaf.txt", "directory leaf payload");
      TEST_CHECK(waitFor(raceManager, raceManager.copy({nrDirSrc.string()}, noReplaceDir.string())));
      TEST_CHECK(read(noReplaceDir / "nr-dir-tree" / "sub" / "leaf.txt") == "directory leaf payload");
      TEST_CHECK(
          waitFor(raceManager, raceManager.duplicate({(noReplaceDir / "nr-dir-tree").string()}, noReplaceDir.string()))
      );
      TEST_CHECK(read(noReplaceDir / "nr-dir-tree (copy)" / "sub" / "leaf.txt") == "directory leaf payload");
      TEST_CHECK(waitFor(
          raceManager,
          raceManager.rename((noReplaceDir / "nr-dir-tree (copy)").string(), (noReplaceDir / "nr-dir-renamed").string())
      ));
      TEST_CHECK(read(noReplaceDir / "nr-dir-renamed" / "sub" / "leaf.txt") == "directory leaf payload");
      // Attempting to rename a directory over a non-empty directory without Merge must fail.
      TEST_CHECK(!waitFor(
          raceManager,
          raceManager.rename((noReplaceDir / "nr-dir-renamed").string(), (noReplaceDir / "nr-dir-tree").string())
      ));
      TEST_CHECK(std::filesystem::is_directory(noReplaceDir / "nr-dir-renamed"));
      TEST_CHECK(std::filesystem::is_directory(noReplaceDir / "nr-dir-tree"));

      // Cross-filesystem Copy and Move from noReplaceDir into normalDir (which
      // supports RENAME_NOREPLACE) remain safe and succeed via staging.
      const auto crossCopySrc = noReplaceDir / "cross-copy.txt";
      const auto crossMoveSrc = noReplaceDir / "cross-move.txt";
      write(crossCopySrc, "cross copy payload");
      write(crossMoveSrc, "cross move payload");
      TEST_CHECK(waitFor(raceManager, raceManager.copy({crossCopySrc.string()}, normalDir.string())));
      TEST_CHECK(read(normalDir / "cross-copy.txt") == "cross copy payload");
      TEST_CHECK(waitFor(raceManager, raceManager.move({crossMoveSrc.string()}, normalDir.string())));
      TEST_CHECK(read(normalDir / "cross-move.txt") == "cross move payload");
      TEST_CHECK(!std::filesystem::exists(crossMoveSrc));

      // CreateFile, CreateFolder, and CreateSymlink inside noReplaceDir succeed
      // and record Undo/Redo history on RENAME_NOREPLACE-unsupported mounts.
      FileOperationManager nrUndoManager([](const FileOperationManager::Operation&) {});
      const auto nrCreatedFile = noReplaceDir / "created.txt";
      const auto nrCreatedDir = noReplaceDir / "created-dir";
      const auto nrCreatedLink = noReplaceDir / "created-link";
      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.createFile(nrCreatedFile.string())));
      TEST_CHECK(std::filesystem::is_regular_file(nrCreatedFile));
      TEST_CHECK(nrUndoManager.canUndo());
      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.undo()));
      TEST_CHECK(!std::filesystem::exists(nrCreatedFile));
      TEST_CHECK(nrUndoManager.canRedo());
      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.redo()));
      TEST_CHECK(std::filesystem::is_regular_file(nrCreatedFile));
      write(nrCreatedFile, "keep created file");
      TEST_CHECK(!waitFor(nrUndoManager, nrUndoManager.createFile(nrCreatedFile.string())));
      TEST_CHECK(read(nrCreatedFile) == "keep created file");

      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.createFolder(nrCreatedDir.string())));
      TEST_CHECK(std::filesystem::is_directory(nrCreatedDir));
      TEST_CHECK(nrUndoManager.canUndo());
      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.undo()));
      TEST_CHECK(!std::filesystem::exists(nrCreatedDir));
      TEST_CHECK(nrUndoManager.canRedo());
      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.redo()));
      TEST_CHECK(std::filesystem::is_directory(nrCreatedDir));

      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.createSymlink(nrCreatedFile.string(), nrCreatedLink.string())));
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(nrCreatedLink)));
      TEST_CHECK(nrUndoManager.canUndo());
      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.undo()));
      TEST_CHECK(!std::filesystem::exists(std::filesystem::symlink_status(nrCreatedLink)));
      TEST_CHECK(nrUndoManager.canRedo());
      TEST_CHECK(waitFor(nrUndoManager, nrUndoManager.redo()));
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(nrCreatedLink)));

      setFileOperationRenameNoReplaceHookForTesting({});
      raceManager.setConflictResolver({});
    }

    {
      // F-01 regressions: FileOperationManager destruction while workers, conflict
      // resolvers, pause checkpoints, or queued GMainContext callbacks remain
      // active must drain workers safely and never UAF on freed state.
      const auto f01Root = root / "f01-lifetimes";
      const auto f01Dest = f01Root / "dest";
      const auto f01Tree = f01Root / "tree";
      std::filesystem::create_directories(f01Dest);
      std::filesystem::create_directories(f01Tree);
      for (int index = 0; index < 150; ++index)
        write(f01Tree / ("item-" + std::to_string(index) + ".txt"), "active transfer payload");

      // 1. Destruction during an active multi-file transfer with un-pumped
      // progress/reveal/completion callbacks queued on GMainContext.
      {
        auto transient = std::make_unique<FileOperationManager>();
        std::atomic_bool enteredCopy{false};
        FileOperationManagerTestAccess::boundary(*transient, [&](std::uint64_t, std::uint64_t) {
          enteredCopy.store(true, std::memory_order_release);
        });
        const auto copyId = transient->copy({f01Tree.string()}, f01Dest.string());
        TEST_CHECK(copyId != 0);
        while (!enteredCopy.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        transient.reset();
        while (g_main_context_iteration(nullptr, false)) {
        }
        for (const auto& entry : std::filesystem::directory_iterator(f01Dest))
          TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-partial-"));
      }

      // 2. Destruction while blocked inside conflict resolution (with scope-local
      // state and a local mutex captured by the resolver) and during WastebinBackend.
      {
        const auto conflictSrc = f01Root / "conflict-src.txt";
        const auto conflictDst = f01Dest / "conflict-src.txt";
        write(conflictSrc, "incoming conflict payload");
        write(conflictDst, "existing conflict payload");
        std::atomic_bool resolverExited{false};
        {
          std::mutex localConflictMutex;
          std::atomic_bool blockedInResolver{false};
          auto transient = std::make_unique<FileOperationManager>();
          transient->setConflictResolver([&](const std::string&, const std::string&, bool, const auto&,
                                             GCancellable* cancellable) {
            std::lock_guard lock(localConflictMutex);
            blockedInResolver.store(true, std::memory_order_release);
            while (!g_cancellable_is_cancelled(cancellable))
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            resolverExited.store(true, std::memory_order_release);
            return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
          });
          const auto copyId = transient->copy({conflictSrc.string()}, f01Dest.string());
          TEST_CHECK(copyId != 0);
          while (!blockedInResolver.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          transient.reset();
          // shutdown() must have drained the worker before localConflictMutex
          // and resolverExited's lambda owner leave scope.
          TEST_CHECK(resolverExited.load(std::memory_order_acquire));
        }
        while (g_main_context_iteration(nullptr, false)) {
        }
        TEST_CHECK(read(conflictDst) == "existing conflict payload");

        std::atomic_bool backendExited{false};
        {
          std::mutex localBackendMutex;
          std::atomic_bool blockedInBackend{false};
          FileOperationManager::WastebinBackend backend{
              .originalPath = [&](const std::string&, GCancellable* cancellable, std::string& out, std::string&) {
                std::lock_guard lock(localBackendMutex);
                blockedInBackend.store(true, std::memory_order_release);
                while (!g_cancellable_is_cancelled(cancellable))
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
                out = (f01Dest / "restored-never.txt").string();
                backendExited.store(true, std::memory_order_release);
                return true;
              },
          };
          auto transient = std::make_unique<FileOperationManager>(FileOperationManager::Changed{}, std::move(backend));
          GError* uriError = nullptr;
          g_autofree char* uri = g_filename_to_uri(conflictSrc.c_str(), nullptr, &uriError);
          TEST_CHECK(uri != nullptr && uriError == nullptr);
          const auto restoreId = transient->restore({uri});
          TEST_CHECK(restoreId != 0);
          while (!blockedInBackend.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          transient.reset();
          TEST_CHECK(backendExited.load(std::memory_order_acquire));
        }
        while (g_main_context_iteration(nullptr, false)) {
        }
        TEST_CHECK(!std::filesystem::exists(f01Dest / "restored-never.txt"));
        TEST_CHECK(read(conflictSrc) == "incoming conflict payload");
      }

      // 3. Destruction while a transfer is paused in FileTransferPauseState::checkpoint.
      {
        const auto pauseDest = f01Root / "pause-dest";
        std::filesystem::create_directory(pauseDest);
        std::atomic_bool entered{false};
        std::atomic_bool pausedTriggered{false};
        auto transient = std::make_unique<FileOperationManager>();
        const auto releasePauseGuard =
            nythera::test::makeScopeExit([&] { pausedTriggered.store(true, std::memory_order_release); });
        FileOperationManagerTestAccess::boundary(*transient, [&](std::uint64_t, std::uint64_t) {
          entered.store(true, std::memory_order_release);
          while (!pausedTriggered.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        const auto pauseId = transient->copy({f01Tree.string()}, pauseDest.string());
        TEST_CHECK(pauseId != 0);
        while (!entered.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        transient->pause(pauseId);
        pausedTriggered.store(true, std::memory_order_release);
        // Allow worker to enter pause checkpoint and queue acknowledgePause idle.
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        transient.reset();
        while (g_main_context_iteration(nullptr, false)) {
        }
        for (const auto& entry : std::filesystem::directory_iterator(pauseDest))
          TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-partial-"));
      }

      // 4. Destruction triggered directly from inside the Changed completion callback.
      {
        const auto selfDestructTarget = f01Dest / "created-during-self-destruct.txt";
        std::unique_ptr<FileOperationManager> selfDestructing;
        bool destroyedInCallback = false;
        bool callbackSucceeded = false;
        selfDestructing = std::make_unique<FileOperationManager>([&](const FileOperationManager::Operation& operation) {
          if (operation.finished) {
            callbackSucceeded = operation.succeeded;
            destroyedInCallback = true;
            selfDestructing.reset();
          }
        });
        const auto createId = selfDestructing->createFile(selfDestructTarget.string());
        TEST_CHECK(createId != 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!destroyedInCallback && std::chrono::steady_clock::now() < deadline) {
          while (g_main_context_iteration(nullptr, false)) {
          }
          if (!destroyedInCallback)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_CHECK(destroyedInCallback);
        TEST_CHECK(callbackSucceeded);
        TEST_CHECK(selfDestructing == nullptr);
        TEST_CHECK(std::filesystem::is_regular_file(selfDestructTarget));
      }
    }

    {
      // F-03 regressions: same-filesystem moves use atomic RENAME_NOREPLACE with
      // full Undo/Redo inode fidelity, and cross-filesystem move failures or late
      // cancellations preserve data, clean up read-only staging, and report split state.
      const auto f03Root = root / "f03-moves";
      const auto sameSrcDir = f03Root / "same-src";
      const auto sameDstDir = f03Root / "same-dst";
      std::filesystem::create_directories(sameSrcDir);
      std::filesystem::create_directories(sameDstDir);

      const auto setModeAndTime = [](const std::filesystem::path& path, mode_t mode, std::time_t seconds) {
        TEST_CHECK(chmod(path.c_str(), mode) == 0);
        const std::array<timespec, 2> times{{{seconds, 123456000}, {seconds, 654321000}}};
        TEST_CHECK(utimensat(AT_FDCWD, path.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) == 0);
      };
      const auto inodeOf = [](const std::filesystem::path& path) -> std::uint64_t {
        struct stat state{};
        TEST_CHECK(lstat(path.c_str(), &state) == 0);
        return static_cast<std::uint64_t>(state.st_ino);
      };
      const auto modeOf = [](const std::filesystem::path& path) -> std::uint32_t {
        struct stat state{};
        TEST_CHECK(lstat(path.c_str(), &state) == 0);
        return static_cast<std::uint32_t>(state.st_mode & 07777);
      };
      const auto mtimeOf = [](const std::filesystem::path& path) -> std::pair<std::int64_t, std::int64_t> {
        struct stat state{};
        TEST_CHECK(lstat(path.c_str(), &state) == 0);
        return {state.st_mtim.tv_sec, state.st_mtim.tv_nsec};
      };

      // 1. Same-filesystem non-replace Move of file, valid & dangling symlinks,
      // and non-empty directory preserves inodes, bypasses copyTree, and keeps
      // Undo/Redo working cleanly for regular files and symlinks.
      FileOperationManager sameFsManager;
      std::atomic_bool sawStagingOrCopyBytes{false};
      FileOperationManagerTestAccess::boundary(sameFsManager, [&](std::uint64_t, std::uint64_t bytes) {
        if (bytes > 0)
          sawStagingOrCopyBytes.store(true, std::memory_order_relaxed);
        for (const auto& entry : std::filesystem::directory_iterator(sameDstDir)) {
          if (entry.path().filename().string().starts_with(".nythera-partial-"))
            sawStagingOrCopyBytes.store(true, std::memory_order_relaxed);
        }
      });

      const auto movedFileSrc = sameSrcDir / "atomic-file.bin";
      const auto movedFileDst = sameDstDir / "atomic-file.bin";
      write(movedFileSrc, "atomic payload");
      setModeAndTime(movedFileSrc, 0750, 1700001234);
      const auto fileInodeBefore = inodeOf(movedFileSrc);
      const auto fileMtimeBefore = mtimeOf(movedFileSrc);

      TEST_CHECK(waitFor(sameFsManager, sameFsManager.move({movedFileSrc.string()}, sameDstDir.string())));
      TEST_CHECK(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
      TEST_CHECK(!std::filesystem::exists(movedFileSrc));
      TEST_CHECK(read(movedFileDst) == "atomic payload");
      TEST_CHECK(inodeOf(movedFileDst) == fileInodeBefore);
      TEST_CHECK(modeOf(movedFileDst) == 0750);
      TEST_CHECK(mtimeOf(movedFileDst) == fileMtimeBefore);
      TEST_CHECK(sameFsManager.canUndo());
      TEST_CHECK(!sameFsManager.operations().back().undoUnavailable);
      TEST_CHECK(waitFor(sameFsManager, sameFsManager.undo()));
      TEST_CHECK(inodeOf(movedFileSrc) == fileInodeBefore && !std::filesystem::exists(movedFileDst));
      TEST_CHECK(waitFor(sameFsManager, sameFsManager.redo()));
      TEST_CHECK(inodeOf(movedFileDst) == fileInodeBefore && !std::filesystem::exists(movedFileSrc));

      const auto linkTargetFile = sameSrcDir / "target.txt";
      write(linkTargetFile, "symlink target payload");
      const auto validLinkSrc = sameSrcDir / "valid-link";
      const auto danglingLinkSrc = sameSrcDir / "dangling-link";
      std::filesystem::create_symlink(linkTargetFile, validLinkSrc);
      std::filesystem::create_symlink(sameSrcDir / "nonexistent-target", danglingLinkSrc);
      const auto validLinkInode = inodeOf(validLinkSrc);
      const auto danglingLinkInode = inodeOf(danglingLinkSrc);

      TEST_CHECK(waitFor(sameFsManager, sameFsManager.move({validLinkSrc.string()}, sameDstDir.string())));
      TEST_CHECK(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
      TEST_CHECK(inodeOf(sameDstDir / "valid-link") == validLinkInode);
      TEST_CHECK(std::filesystem::read_symlink(sameDstDir / "valid-link") == linkTargetFile);
      TEST_CHECK(waitFor(sameFsManager, sameFsManager.undo()));
      TEST_CHECK(inodeOf(validLinkSrc) == validLinkInode);
      TEST_CHECK(waitFor(sameFsManager, sameFsManager.redo()));
      TEST_CHECK(inodeOf(sameDstDir / "valid-link") == validLinkInode);

      TEST_CHECK(waitFor(sameFsManager, sameFsManager.move({danglingLinkSrc.string()}, sameDstDir.string())));
      TEST_CHECK(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
      TEST_CHECK(inodeOf(sameDstDir / "dangling-link") == danglingLinkInode);
      TEST_CHECK(std::filesystem::read_symlink(sameDstDir / "dangling-link") == sameSrcDir / "nonexistent-target");
      TEST_CHECK(waitFor(sameFsManager, sameFsManager.undo()));
      TEST_CHECK(inodeOf(danglingLinkSrc) == danglingLinkInode);

      const auto movedTreeSrc = sameSrcDir / "atomic-tree";
      const auto movedTreeDst = sameDstDir / "atomic-tree";
      std::filesystem::create_directories(movedTreeSrc / "nested");
      write(movedTreeSrc / "nested" / "child.txt", "nested atomic payload");
      const auto nestedInodeBefore = inodeOf(movedTreeSrc / "nested" / "child.txt");
      setModeAndTime(movedTreeSrc, 0750, 1700002345);
      const auto treeInodeBefore = inodeOf(movedTreeSrc);
      const auto treeMtimeBefore = mtimeOf(movedTreeSrc);
      TEST_CHECK(waitFor(sameFsManager, sameFsManager.move({movedTreeSrc.string()}, sameDstDir.string())));
      TEST_CHECK(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
      TEST_CHECK(!std::filesystem::exists(movedTreeSrc));
      TEST_CHECK(inodeOf(movedTreeDst) == treeInodeBefore);
      TEST_CHECK(inodeOf(movedTreeDst / "nested" / "child.txt") == nestedInodeBefore);
      TEST_CHECK(modeOf(movedTreeDst) == 0750);
      TEST_CHECK(mtimeOf(movedTreeDst) == treeMtimeBefore);
      TEST_CHECK(read(movedTreeDst / "nested" / "child.txt") == "nested atomic payload");
      TEST_CHECK(sameFsManager.operations().back().undoUnavailable);
      FileOperationManagerTestAccess::boundary(sameFsManager, {});

      // 2. Same-filesystem Move with ConflictAction::Replace preserves moved inode,
      // skips copyTree, cleans backup, and retains F-02 target-swap / appear guards.
      const auto replaceSrc = sameSrcDir / "replace-atomic.txt";
      const auto replaceDst = sameDstDir / "replace-atomic.txt";
      write(replaceSrc, "replacement source");
      write(replaceDst, "old destination");
      const auto replaceSrcInode = inodeOf(replaceSrc);
      sawStagingOrCopyBytes.store(false, std::memory_order_relaxed);
      FileOperationManagerTestAccess::boundary(sameFsManager, [&](std::uint64_t, std::uint64_t bytes) {
        if (bytes > 0)
          sawStagingOrCopyBytes.store(true, std::memory_order_relaxed);
        for (const auto& entry : std::filesystem::directory_iterator(sameDstDir)) {
          if (entry.path().filename().string().starts_with(".nythera-partial-"))
            sawStagingOrCopyBytes.store(true, std::memory_order_relaxed);
        }
      });
      sameFsManager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      TEST_CHECK(waitFor(sameFsManager, sameFsManager.move({replaceSrc.string()}, sameDstDir.string())));
      TEST_CHECK(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
      TEST_CHECK(!std::filesystem::exists(replaceSrc));
      TEST_CHECK(read(replaceDst) == "replacement source");
      TEST_CHECK(inodeOf(replaceDst) == replaceSrcInode);
      TEST_CHECK(sameFsManager.operations().back().undoUnavailable);
      for (const auto& entry : std::filesystem::directory_iterator(sameDstDir))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-backup-"));
      FileOperationManagerTestAccess::boundary(sameFsManager, {});

      // 2b. F-02 destination swap during same-filesystem Move Replace rolls back safely.
      const auto moveSwapSrc = sameSrcDir / "move-swap.txt";
      const auto moveSwapDst = sameDstDir / "move-swap.txt";
      const auto heldMoveSwap = sameSrcDir / "held-move-swap.txt";
      write(moveSwapSrc, "incoming swapped move");
      write(moveSwapDst, "approved replace target");
      bool swappedMoveBeforeBackup = false;
      setFileOperationReplaceBackupHookForTesting([&](const char* targetPath, const char*, bool afterMove) {
        if (!afterMove && !swappedMoveBeforeBackup && targetPath != nullptr && targetPath == moveSwapDst) {
          swappedMoveBeforeBackup = true;
          std::filesystem::rename(moveSwapDst, heldMoveSwap);
          write(moveSwapDst, "unapproved concurrent item");
        }
      });
      TEST_CHECK(!waitFor(sameFsManager, sameFsManager.move({moveSwapSrc.string()}, sameDstDir.string())));
      setFileOperationReplaceBackupHookForTesting({});
      TEST_CHECK(swappedMoveBeforeBackup);
      TEST_CHECK(read(moveSwapSrc) == "incoming swapped move");
      TEST_CHECK(read(moveSwapDst) == "unapproved concurrent item");
      TEST_CHECK(read(heldMoveSwap) == "approved replace target");
      TEST_CHECK(
          sameFsManager.operations().back().resultDescription.find("changed before it could be replaced")
          != std::string::npos
      );
      for (const auto& entry : std::filesystem::directory_iterator(sameDstDir))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-backup-"));

      // 2c. F-02 mid-finalisation appearance during same-filesystem Move Replace preserves both.
      const auto moveAppearSrc = sameSrcDir / "move-appear.txt";
      const auto moveAppearDst = sameDstDir / "move-appear.txt";
      write(moveAppearSrc, "incoming move appear");
      write(moveAppearDst, "approved move backup");
      bool injectedMoveAppear = false;
      setFileOperationReplaceBackupHookForTesting([&](const char* targetPath, const char*, bool afterMove) {
        if (afterMove && !injectedMoveAppear && targetPath != nullptr && targetPath == moveAppearDst) {
          injectedMoveAppear = true;
          write(moveAppearDst, "appeared during same-fs move");
        }
      });
      TEST_CHECK(!waitFor(sameFsManager, sameFsManager.move({moveAppearSrc.string()}, sameDstDir.string())));
      setFileOperationReplaceBackupHookForTesting({});
      TEST_CHECK(injectedMoveAppear);
      TEST_CHECK(read(moveAppearSrc) == "incoming move appear");
      TEST_CHECK(read(moveAppearDst) == "appeared during same-fs move");
      std::filesystem::path survivingMoveBackup;
      for (const auto& entry : std::filesystem::directory_iterator(sameDstDir)) {
        if (entry.path().filename().string().starts_with(".nythera-backup-")) {
          TEST_CHECK(survivingMoveBackup.empty());
          survivingMoveBackup = entry.path();
        }
      }
      TEST_CHECK(!survivingMoveBackup.empty() && read(survivingMoveBackup) == "approved move backup");
      TEST_CHECK(
          sameFsManager.operations().back().resultDescription.find(survivingMoveBackup.filename().string())
          != std::string::npos
      );
      std::filesystem::remove(survivingMoveBackup);
      sameFsManager.setConflictResolver({});

      // 3. Cross-filesystem Move (via EXDEV hook and /dev/shm when distinct device):
      // preserves directory/file mode, timestamps, and symlinks; marks Undo unavailable.
      const auto crossSimSrc = f03Root / "cross-sim-src";
      const auto crossSimDst = f03Root / "cross-sim-dst";
      std::filesystem::create_directories(crossSimSrc);
      std::filesystem::create_directories(crossSimDst);
      const std::string crossDstPrefix = crossSimDst.string() + "/";
      setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
        const bool directCross = sourcePath != nullptr
            && destinationPath != nullptr
            && !std::string(sourcePath).starts_with(crossDstPrefix)
            && std::string(destinationPath).starts_with(crossDstPrefix);
        if (directCross) {
          errno = EXDEV;
          return -1;
        }
        return static_cast<int>(
            syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
        );
      });

      FileOperationManager crossManager;
      const auto crossMetaFile = crossSimSrc / "cross-file.bin";
      write(crossMetaFile, "cross payload");
      setModeAndTime(crossMetaFile, 0750, 1700003456);
      const auto crossFileMtime = mtimeOf(crossMetaFile);
      const auto crossMetaDir = crossSimSrc / "cross-dir";
      std::filesystem::create_directories(crossMetaDir / "sub");
      write(crossMetaDir / "sub" / "item.txt", "cross tree item");
      std::filesystem::create_symlink(crossSimSrc / "missing-target", crossMetaDir / "dangling");
      setModeAndTime(crossMetaDir, 0750, 1700004567);
      const auto crossDirMtime = mtimeOf(crossMetaDir);

      TEST_CHECK(waitFor(
          crossManager, crossManager.move({crossMetaFile.string(), crossMetaDir.string()}, crossSimDst.string())
      ));
      TEST_CHECK(!std::filesystem::exists(crossMetaFile) && !std::filesystem::exists(crossMetaDir));
      TEST_CHECK(read(crossSimDst / "cross-file.bin") == "cross payload");
      TEST_CHECK(modeOf(crossSimDst / "cross-file.bin") == 0750);
      TEST_CHECK(mtimeOf(crossSimDst / "cross-file.bin") == crossFileMtime);
      TEST_CHECK(modeOf(crossSimDst / "cross-dir") == 0750);
      TEST_CHECK(mtimeOf(crossSimDst / "cross-dir") == crossDirMtime);
      TEST_CHECK(read(crossSimDst / "cross-dir" / "sub" / "item.txt") == "cross tree item");
      TEST_CHECK(
          std::filesystem::read_symlink(crossSimDst / "cross-dir" / "dangling") == crossSimSrc / "missing-target"
      );
      TEST_CHECK(crossManager.operations().back().undoUnavailable && !crossManager.canUndo());
      setFileOperationRenameNoReplaceHookForTesting({});

      {
        nythera::test::ScopedOwnedDir f03ShmRoot("/dev/shm", nythera::test::uniqueToken("nythera-f03"), false);
        if (f03ShmRoot.created()) {
          FileOperationManager shmCrossManager;
          struct stat rootState{};
          struct stat shmState{};
          if (stat(f03Root.c_str(), &rootState) == 0
              && stat(f03ShmRoot.path().c_str(), &shmState) == 0
              && rootState.st_dev != shmState.st_dev) {
            const auto shmSrcFile = crossSimSrc / "shm-file.bin";
            const auto shmSrcDir = crossSimSrc / "shm-dir";
            const auto shmDst = f03ShmRoot.path() / "dst";
            std::filesystem::create_directory(shmDst);
            write(shmSrcFile, "real cross-dev file");
            setModeAndTime(shmSrcFile, 0750, 1700005555);
            std::filesystem::create_directories(shmSrcDir / "deep");
            write(shmSrcDir / "deep" / "data", "real cross-dev dir");
            std::filesystem::create_symlink(crossSimSrc / "dangling-shm", shmSrcDir / "link");
            setModeAndTime(shmSrcDir, 0750, 1700006666);
            const auto savedDirTime = mtimeOf(shmSrcDir);
            TEST_CHECK(waitFor(
                shmCrossManager, shmCrossManager.move({shmSrcFile.string(), shmSrcDir.string()}, shmDst.string())
            ));
            TEST_CHECK(!std::filesystem::exists(shmSrcFile) && !std::filesystem::exists(shmSrcDir));
            TEST_CHECK(modeOf(shmDst / "shm-file.bin") == 0750);
            TEST_CHECK(modeOf(shmDst / "shm-dir") == 0750);
            TEST_CHECK(mtimeOf(shmDst / "shm-dir") == savedDirTime);
            TEST_CHECK(read(shmDst / "shm-dir" / "deep" / "data") == "real cross-dev dir");
            TEST_CHECK(std::filesystem::read_symlink(shmDst / "shm-dir" / "link") == crossSimSrc / "dangling-shm");
            TEST_CHECK(shmCrossManager.operations().back().undoUnavailable && !shmCrossManager.canUndo());
          }
        }
      }

      // 4a. Cancellation before destination commit while copying a 0500 subdirectory
      // must clean up the read-only staged subtree completely and keep source intact.
      setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
        const bool directCross = sourcePath != nullptr
            && destinationPath != nullptr
            && !std::string(sourcePath).starts_with(crossDstPrefix)
            && std::string(destinationPath).starts_with(crossDstPrefix);
        if (directCross) {
          errno = EXDEV;
          return -1;
        }
        return static_cast<int>(
            syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
        );
      });
      const auto preCancelTree = crossSimSrc / "pre-cancel-tree";
      const auto preCancelLockedSub = preCancelTree / "locked-sub";
      std::filesystem::create_directories(preCancelLockedSub);
      write(preCancelLockedSub / "nested.txt", "nested read-only subfile");
      write(preCancelTree / "sibling.txt", "sibling payload");
      TEST_CHECK(chmod(preCancelLockedSub.c_str(), 0500) == 0);
      {
        std::mutex preCancelMutex;
        std::condition_variable preCancelCv;
        bool stagedLockedReady = false;
        bool releasePreCancel = false;
        FileOperationManager preCancelMgr;
        const auto releasePreCancelGuard = nythera::test::makeScopeExit([&] {
          {
            std::lock_guard lock(preCancelMutex);
            releasePreCancel = true;
          }
          preCancelCv.notify_all();
        });
        FileOperationManagerTestAccess::boundary(preCancelMgr, [&](std::uint64_t, std::uint64_t) {
          std::unique_lock lock(preCancelMutex);
          if (stagedLockedReady)
            return;
          for (const auto& entry : std::filesystem::directory_iterator(crossSimDst)) {
            if (!entry.path().filename().string().starts_with(".nythera-partial-"))
              continue;
            const auto stagedSub = entry.path() / "locked-sub";
            struct stat st{};
            if (lstat(stagedSub.c_str(), &st) == 0 && (st.st_mode & 0777) == 0500) {
              stagedLockedReady = true;
              preCancelCv.notify_all();
              preCancelCv.wait(lock, [&] { return releasePreCancel; });
              break;
            }
          }
        });
        const auto preCancelId = preCancelMgr.move({preCancelTree.string()}, crossSimDst.string());
        {
          std::unique_lock lock(preCancelMutex);
          preCancelCv.wait(lock, [&] { return stagedLockedReady; });
        }
        preCancelMgr.cancel(preCancelId);
        {
          std::lock_guard lock(preCancelMutex);
          releasePreCancel = true;
        }
        preCancelCv.notify_all();
        TEST_CHECK(!waitFor(preCancelMgr, preCancelId));
        FileOperationManagerTestAccess::boundary(preCancelMgr, {});
        const auto& preCancelOp = preCancelMgr.operations().back();
        TEST_CHECK(preCancelOp.cancelled && !preCancelOp.succeeded);
      }
      TEST_CHECK(!std::filesystem::exists(crossSimDst / "pre-cancel-tree"));
      for (const auto& entry : std::filesystem::directory_iterator(crossSimDst))
        TEST_CHECK(!entry.path().filename().string().starts_with(".nythera-partial-"));
      TEST_CHECK(read(preCancelLockedSub / "nested.txt") == "nested read-only subfile");
      TEST_CHECK(read(preCancelTree / "sibling.txt") == "sibling payload");
      TEST_CHECK(chmod(preCancelLockedSub.c_str(), 0700) == 0);

      // 4b. Cancellation right after stagePath -> plannedPath commits must leave
      // both source and destination intact and clearly report the split state.
      const auto postCancelSrc = crossSimSrc / "post-commit-cancel.txt";
      const auto postCancelDst = crossSimDst / "post-commit-cancel.txt";
      write(postCancelSrc, "survive both on late cancel");
      {
        std::mutex postCancelMutex;
        std::condition_variable postCancelCv;
        bool postCommitReached = false;
        bool releasePostCancel = false;
        const auto clearHookGuard =
            nythera::test::makeScopeExit([] { setFileOperationRenameNoReplaceHookForTesting({}); });
        FileOperationManager postCancelMgr;
        const auto releasePostCancelGuard = nythera::test::makeScopeExit([&] {
          {
            std::lock_guard lock(postCancelMutex);
            releasePostCancel = true;
          }
          postCancelCv.notify_all();
        });
        setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
          const bool directCross = sourcePath != nullptr
              && destinationPath != nullptr
              && sourcePath == postCancelSrc
              && destinationPath == postCancelDst;
          if (directCross) {
            errno = EXDEV;
            return -1;
          }
          const int rc = static_cast<int>(
              syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
          );
          if (rc == 0
              && sourcePath != nullptr
              && destinationPath != nullptr
              && std::filesystem::path(sourcePath).filename().string().starts_with(".nythera-partial-")
              && destinationPath == postCancelDst) {
            std::unique_lock lock(postCancelMutex);
            postCommitReached = true;
            postCancelCv.notify_all();
            postCancelCv.wait(lock, [&] { return releasePostCancel; });
          }
          return rc;
        });
        const auto postCancelId = postCancelMgr.move({postCancelSrc.string()}, crossSimDst.string());
        {
          std::unique_lock lock(postCancelMutex);
          postCancelCv.wait(lock, [&] { return postCommitReached; });
        }
        postCancelMgr.cancel(postCancelId);
        {
          std::lock_guard lock(postCancelMutex);
          releasePostCancel = true;
        }
        postCancelCv.notify_all();
        TEST_CHECK(!waitFor(postCancelMgr, postCancelId));
        setFileOperationRenameNoReplaceHookForTesting({});
        const auto& postCancelOp = postCancelMgr.operations().back();
        TEST_CHECK(postCancelOp.cancelled && !postCancelOp.succeeded && postCancelOp.undoUnavailable);
        TEST_CHECK(read(postCancelSrc) == "survive both on late cancel");
        TEST_CHECK(read(postCancelDst) == "survive both on late cancel");
        TEST_CHECK(postCancelOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
        TEST_CHECK(postCancelOp.resultDescription.find(postCancelDst.string()) != std::string::npos);
        TEST_CHECK(postCancelOp.resultDescription.find("source still exists at ") != std::string::npos);
        TEST_CHECK(postCancelOp.resultDescription.find(postCancelSrc.string()) != std::string::npos);
        TEST_CHECK(
            postCancelOp.resultDescription.find("cancelled before source cleanup finished") != std::string::npos
        );
        TEST_CHECK(postCancelOp.status.find(postCancelOp.resultDescription) == 0);
      }

      // 5. Cross-filesystem Move source-deletion failures (without and with Replace)
      // must keep destination, source, and backup intact and report every location.
      if (geteuid() != 0) {
        setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
          const bool directCross = sourcePath != nullptr
              && destinationPath != nullptr
              && !std::string(sourcePath).starts_with(crossDstPrefix)
              && std::string(destinationPath).starts_with(crossDstPrefix);
          if (directCross) {
            errno = EXDEV;
            return -1;
          }
          return static_cast<int>(
              syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
          );
        });

        const auto lockedRootSrc = crossSimSrc / "unremovable-tree";
        const auto lockedChildDir = lockedRootSrc / "locked";
        std::filesystem::create_directories(lockedChildDir);
        write(lockedChildDir / "keep.txt", "cannot unlink source child");
        TEST_CHECK(chmod(lockedChildDir.c_str(), 0500) == 0);

        TEST_CHECK(!waitFor(crossManager, crossManager.move({lockedRootSrc.string()}, crossSimDst.string())));
        const auto& lockedFailOp = crossManager.operations().back();
        TEST_CHECK(!lockedFailOp.succeeded && !lockedFailOp.cancelled && lockedFailOp.undoUnavailable);
        TEST_CHECK(read(crossSimDst / "unremovable-tree" / "locked" / "keep.txt") == "cannot unlink source child");
        TEST_CHECK(read(lockedChildDir / "keep.txt") == "cannot unlink source child");
        TEST_CHECK(lockedFailOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
        TEST_CHECK(
            lockedFailOp.resultDescription.find((crossSimDst / "unremovable-tree").string()) != std::string::npos
        );
        TEST_CHECK(lockedFailOp.resultDescription.find("source still exists at ") != std::string::npos);
        TEST_CHECK(lockedFailOp.resultDescription.find(lockedRootSrc.string()) != std::string::npos);
        TEST_CHECK(lockedFailOp.status.find(lockedFailOp.resultDescription) == 0);
        TEST_CHECK(chmod(lockedChildDir.c_str(), 0700) == 0);
        TEST_CHECK(chmod((crossSimDst / "unremovable-tree" / "locked").c_str(), 0700) == 0);

        const auto lockedParent = crossSimSrc / "locked-parent";
        std::filesystem::create_directory(lockedParent);
        const auto replaceLockedSrc = lockedParent / "replace-fail.txt";
        const auto replaceLockedDst = crossSimDst / "replace-fail.txt";
        write(replaceLockedSrc, "new moved copy");
        write(replaceLockedDst, "previous target backup");
        TEST_CHECK(chmod(lockedParent.c_str(), 0500) == 0);
        crossManager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
          return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
        });
        TEST_CHECK(!waitFor(crossManager, crossManager.move({replaceLockedSrc.string()}, crossSimDst.string())));
        crossManager.setConflictResolver({});
        setFileOperationRenameNoReplaceHookForTesting({});
        const auto& replaceLockedOp = crossManager.operations().back();
        TEST_CHECK(!replaceLockedOp.succeeded && !replaceLockedOp.cancelled && replaceLockedOp.undoUnavailable);
        TEST_CHECK(read(replaceLockedDst) == "new moved copy");
        TEST_CHECK(read(replaceLockedSrc) == "new moved copy");
        std::filesystem::path preservedBackup;
        for (const auto& entry : std::filesystem::directory_iterator(crossSimDst)) {
          if (entry.path().filename().string().starts_with(".nythera-backup-")) {
            TEST_CHECK(preservedBackup.empty());
            preservedBackup = entry.path();
          }
        }
        TEST_CHECK(!preservedBackup.empty() && read(preservedBackup) == "previous target backup");
        TEST_CHECK(replaceLockedOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
        TEST_CHECK(replaceLockedOp.resultDescription.find(replaceLockedDst.string()) != std::string::npos);
        TEST_CHECK(replaceLockedOp.resultDescription.find("source still exists at ") != std::string::npos);
        TEST_CHECK(replaceLockedOp.resultDescription.find(replaceLockedSrc.string()) != std::string::npos);
        TEST_CHECK(
            replaceLockedOp.resultDescription.find("previous destination backup remains at " + preservedBackup.string())
            != std::string::npos
        );
        TEST_CHECK(replaceLockedOp.status.find(replaceLockedOp.resultDescription) == 0);
        TEST_CHECK(chmod(lockedParent.c_str(), 0700) == 0);
        std::filesystem::remove(preservedBackup);
      } else {
        setFileOperationRenameNoReplaceHookForTesting({});
      }

      // 6. Deterministic fault-injected source-cleanup failures (user- and
      // permission-independent): partial cross-filesystem single-item/directory
      // moves (without and with Replace), merged-directory source cleanup failure,
      // and non-mutating failure tracking (op.mutated == false).
      {
        const auto faultRoot = f03Root / "fault-injected";
        const auto faultSrc = faultRoot / "src";
        const auto faultDst = faultRoot / "dst";
        std::filesystem::create_directories(faultSrc);
        std::filesystem::create_directories(faultDst);
        const std::string faultDstPrefix = faultDst.string() + "/";
        setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
          const bool directCross = sourcePath != nullptr
              && destinationPath != nullptr
              && !std::string(sourcePath).starts_with(faultDstPrefix)
              && std::string(destinationPath).starts_with(faultDstPrefix);
          if (directCross) {
            errno = EXDEV;
            return -1;
          }
          return static_cast<int>(
              syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
          );
        });

        FileOperationManager faultManager;

        // 6a. Cross-filesystem single-file move: destination succeeds, source unlink fails with EIO.
        const auto faultFileSrc = faultSrc / "injected-file.txt";
        const auto faultFileDst = faultDst / "injected-file.txt";
        write(faultFileSrc, "injected cross-fs payload");
        setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
          return path != nullptr && path == faultFileSrc ? EIO : 0;
        });
        TEST_CHECK(!waitFor(faultManager, faultManager.move({faultFileSrc.string()}, faultDst.string())));
        setFileOperationRemoveEntryHookForTesting({});
        const auto& faultFileOp = faultManager.operations().back();
        TEST_CHECK(
            !faultFileOp.succeeded && !faultFileOp.cancelled && faultFileOp.mutated && faultFileOp.undoUnavailable
        );
        TEST_CHECK(faultFileOp.completedEntries.empty());
        TEST_CHECK(read(faultFileDst) == "injected cross-fs payload");
        TEST_CHECK(read(faultFileSrc) == "injected cross-fs payload");
        TEST_CHECK(faultFileOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
        TEST_CHECK(faultFileOp.resultDescription.find(faultFileDst.string()) != std::string::npos);
        TEST_CHECK(faultFileOp.resultDescription.find("source still exists at ") != std::string::npos);
        TEST_CHECK(faultFileOp.resultDescription.find(faultFileSrc.string()) != std::string::npos);
        TEST_CHECK(faultFileOp.status.find(faultFileOp.resultDescription) == 0);

        // 6b. Cross-filesystem directory move: destination succeeds, nested source deletion fails with EACCES.
        const auto faultDirSrc = faultSrc / "injected-dir";
        const auto faultDirDst = faultDst / "injected-dir";
        std::filesystem::create_directories(faultDirSrc / "sub");
        write(faultDirSrc / "sub" / "stuck.txt", "stuck nested source");
        setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
          return path != nullptr && path == (faultDirSrc / "sub" / "stuck.txt") ? EACCES : 0;
        });
        TEST_CHECK(!waitFor(faultManager, faultManager.move({faultDirSrc.string()}, faultDst.string())));
        setFileOperationRemoveEntryHookForTesting({});
        const auto& faultDirOp = faultManager.operations().back();
        TEST_CHECK(!faultDirOp.succeeded && !faultDirOp.cancelled && faultDirOp.mutated && faultDirOp.undoUnavailable);
        TEST_CHECK(read(faultDirDst / "sub" / "stuck.txt") == "stuck nested source");
        TEST_CHECK(read(faultDirSrc / "sub" / "stuck.txt") == "stuck nested source");
        TEST_CHECK(faultDirOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
        TEST_CHECK(faultDirOp.resultDescription.find(faultDirDst.string()) != std::string::npos);
        TEST_CHECK(faultDirOp.resultDescription.find("source still exists at ") != std::string::npos);
        TEST_CHECK(faultDirOp.resultDescription.find(faultDirSrc.string()) != std::string::npos);

        // 6c. Cross-filesystem Replace move: destination succeeds, backup preserved, source unlink fails with EIO.
        const auto faultReplaceSrc = faultSrc / "injected-replace.txt";
        const auto faultReplaceDst = faultDst / "injected-replace.txt";
        write(faultReplaceSrc, "injected replacement copy");
        write(faultReplaceDst, "injected original backup");
        faultManager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
          return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
        });
        setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
          return path != nullptr && path == faultReplaceSrc ? EIO : 0;
        });
        TEST_CHECK(!waitFor(faultManager, faultManager.move({faultReplaceSrc.string()}, faultDst.string())));
        setFileOperationRemoveEntryHookForTesting({});
        faultManager.setConflictResolver({});
        const auto& faultReplaceOp = faultManager.operations().back();
        TEST_CHECK(
            !faultReplaceOp.succeeded
            && !faultReplaceOp.cancelled
            && faultReplaceOp.mutated
            && faultReplaceOp.undoUnavailable
        );
        TEST_CHECK(read(faultReplaceDst) == "injected replacement copy");
        TEST_CHECK(read(faultReplaceSrc) == "injected replacement copy");
        std::filesystem::path faultBackup;
        for (const auto& entry : std::filesystem::directory_iterator(faultDst)) {
          if (entry.path().filename().string().starts_with(".nythera-backup-")) {
            TEST_CHECK(faultBackup.empty());
            faultBackup = entry.path();
          }
        }
        TEST_CHECK(!faultBackup.empty() && read(faultBackup) == "injected original backup");
        TEST_CHECK(faultReplaceOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
        TEST_CHECK(faultReplaceOp.resultDescription.find(faultReplaceDst.string()) != std::string::npos);
        TEST_CHECK(faultReplaceOp.resultDescription.find("source still exists at ") != std::string::npos);
        TEST_CHECK(faultReplaceOp.resultDescription.find(faultReplaceSrc.string()) != std::string::npos);
        TEST_CHECK(
            faultReplaceOp.resultDescription.find("previous destination backup remains at " + faultBackup.string())
            != std::string::npos
        );
        std::filesystem::remove(faultBackup);

        // 6d. Merged-directory move where children move cleanly but removing the
        // now-empty source directory fails with EIO reports both surviving paths.
        setFileOperationRenameNoReplaceHookForTesting({});
        const auto mergeFailSrc = faultSrc / "merge-cleanup-tree";
        const auto mergeFailDst = faultDst / "merge-cleanup-tree";
        std::filesystem::create_directories(mergeFailSrc);
        std::filesystem::create_directories(mergeFailDst);
        write(mergeFailSrc / "moved-child.txt", "merged child payload");
        write(mergeFailDst / "existing-child.txt", "preserved destination child");
        faultManager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
          return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Merge, false};
        });
        setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
          return path != nullptr && path == mergeFailSrc ? EIO : 0;
        });
        TEST_CHECK(!waitFor(faultManager, faultManager.move({mergeFailSrc.string()}, faultDst.string())));
        setFileOperationRemoveEntryHookForTesting({});
        faultManager.setConflictResolver({});
        const auto& mergeFailOp = faultManager.operations().back();
        TEST_CHECK(!mergeFailOp.succeeded && !mergeFailOp.cancelled && mergeFailOp.mutated);
        TEST_CHECK(mergeFailOp.completedEntries.empty());
        TEST_CHECK(read(mergeFailDst / "moved-child.txt") == "merged child payload");
        TEST_CHECK(read(mergeFailDst / "existing-child.txt") == "preserved destination child");
        TEST_CHECK(std::filesystem::is_directory(mergeFailSrc) && std::filesystem::is_empty(mergeFailSrc));
        TEST_CHECK(mergeFailOp.resultDescription.find("Merged items remain at ") != std::string::npos);
        TEST_CHECK(mergeFailOp.resultDescription.find(mergeFailDst.string()) != std::string::npos);
        TEST_CHECK(mergeFailOp.resultDescription.find("source directory still exists at ") != std::string::npos);
        TEST_CHECK(mergeFailOp.resultDescription.find(mergeFailSrc.string()) != std::string::npos);
        TEST_CHECK(mergeFailOp.status.find(mergeFailOp.resultDescription) == 0);

        // 6e. Non-mutating failed/cancelled move reports mutated == false.
        const auto noMutateSrc = faultSrc / "no-mutate.txt";
        const auto noMutateDst = faultDst / "no-mutate.txt";
        write(noMutateSrc, "source unchanged");
        write(noMutateDst, "destination unchanged");
        TEST_CHECK(!waitFor(faultManager, faultManager.move({noMutateSrc.string()}, faultDst.string())));
        const auto& noMutateOp = faultManager.operations().back();
        TEST_CHECK(!noMutateOp.succeeded && noMutateOp.cancelled && !noMutateOp.mutated);
        TEST_CHECK(read(noMutateSrc) == "source unchanged" && read(noMutateDst) == "destination unchanged");
      }

      // F-08: Office template creation through FileOperationManager preserves contents,
      // user-writable permissions, Undo/Redo, and never overwrites existing files or dangling symlinks.
      {
        const auto tplDir = root / "f08-templates";
        const auto tplDstDir = root / "f08-dest";
        std::filesystem::create_directory(tplDir);
        std::filesystem::create_directory(tplDstDir);
        const auto tplFile = tplDir / "blank.odt";
        write(tplFile, "ODT-TEMPLATE-CONTENT-BYTES");
        TEST_CHECK(::chmod(tplFile.c_str(), 0444) == 0);

        FileOperationManager tplManager([](const FileOperationManager::Operation&) {});
        const auto createdDoc = tplDstDir / "New Document.odt";
        TEST_CHECK(waitFor(tplManager, tplManager.createFileFromTemplate(tplFile.string(), createdDoc.string())));
        TEST_CHECK(std::filesystem::exists(createdDoc));
        TEST_CHECK(read(createdDoc) == "ODT-TEMPLATE-CONTENT-BYTES");
        struct stat createdStat{};
        TEST_CHECK(::lstat(createdDoc.c_str(), &createdStat) == 0);
        TEST_CHECK((createdStat.st_mode & S_IWUSR) != 0);

        // Undo removes the created document; modifying the source template must not affect Redo.
        TEST_CHECK(tplManager.canUndo());
        TEST_CHECK(waitFor(tplManager, tplManager.undo()));
        TEST_CHECK(!std::filesystem::exists(createdDoc));
        TEST_CHECK(::chmod(tplFile.c_str(), 0644) == 0);
        write(tplFile, "MODIFIED-TEMPLATE-CONTENT");
        TEST_CHECK(tplManager.canRedo());
        TEST_CHECK(waitFor(tplManager, tplManager.redo()));
        TEST_CHECK(std::filesystem::exists(createdDoc));
        TEST_CHECK(read(createdDoc) == "ODT-TEMPLATE-CONTENT-BYTES");

        // Undo again and delete the source template completely: Redo must still succeed.
        TEST_CHECK(tplManager.canUndo());
        TEST_CHECK(waitFor(tplManager, tplManager.undo()));
        TEST_CHECK(!std::filesystem::exists(createdDoc));
        std::filesystem::remove(tplFile);
        TEST_CHECK(!std::filesystem::exists(tplFile));
        TEST_CHECK(tplManager.canRedo());
        TEST_CHECK(waitFor(tplManager, tplManager.redo()));
        TEST_CHECK(std::filesystem::exists(createdDoc));
        TEST_CHECK(read(createdDoc) == "ODT-TEMPLATE-CONTENT-BYTES");

        // Undo again and independently create a file at the destination: Redo must refuse to overwrite it.
        TEST_CHECK(tplManager.canUndo());
        TEST_CHECK(waitFor(tplManager, tplManager.undo()));
        TEST_CHECK(!std::filesystem::exists(createdDoc));
        write(createdDoc, "INDEPENDENT-USER-FILE");
        TEST_CHECK(tplManager.canRedo());
        TEST_CHECK(!waitFor(tplManager, tplManager.redo()));
        TEST_CHECK(read(createdDoc) == "INDEPENDENT-USER-FILE");

        // Restore template file for overwrite-refusal checks below.
        write(tplFile, "ODT-TEMPLATE-CONTENT-BYTES");

        // Must refuse to overwrite an existing regular file.
        write(createdDoc, "USER-EDITED-CONTENT");
        TEST_CHECK(!waitFor(tplManager, tplManager.createFileFromTemplate(tplFile.string(), createdDoc.string())));
        TEST_CHECK(read(createdDoc) == "USER-EDITED-CONTENT");

        // Must refuse to overwrite or follow a dangling symlink.
        const auto danglingDoc = tplDstDir / "Dangling.odt";
        const auto missingTarget = tplDstDir / "does-not-exist.odt";
        std::filesystem::create_symlink(missingTarget, danglingDoc);
        TEST_CHECK(!waitFor(tplManager, tplManager.createFileFromTemplate(tplFile.string(), danglingDoc.string())));
        TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(danglingDoc)));
        TEST_CHECK(!std::filesystem::exists(missingTarget));
      }
    }

    // Opt-in real-NFS acceptance test suite (executed ONLY when NYTHERA_RUN_REAL_NFS_TESTS=1
    // and valid mounted NYTHERA_REAL_NFS_PRIMARY / NYTHERA_REAL_NFS_SECONDARY roots are provided).
    if (const char* nfsOptIn = std::getenv("NYTHERA_RUN_REAL_NFS_TESTS");
        nfsOptIn != nullptr && std::string_view(nfsOptIn) == "1") {
      const char* nfsPrimaryEnv = std::getenv("NYTHERA_REAL_NFS_PRIMARY");
      const char* nfsSecondaryEnv = std::getenv("NYTHERA_REAL_NFS_SECONDARY");
      std::filesystem::path primaryCanon;
      std::filesystem::path secondaryCanon;
      std::string primaryErr;
      std::string secondaryErr;
      if (!nythera::test::validateRealNfsMountRoot(nfsPrimaryEnv, env.realUserHome(), primaryCanon, primaryErr)) {
        std::println(stderr, "Invalid NYTHERA_REAL_NFS_PRIMARY: {}", primaryErr);
        TEST_CHECK(false);
      }
      if (!nythera::test::validateRealNfsMountRoot(nfsSecondaryEnv, env.realUserHome(), secondaryCanon, secondaryErr)) {
        std::println(stderr, "Invalid NYTHERA_REAL_NFS_SECONDARY: {}", secondaryErr);
        TEST_CHECK(false);
      }
      TEST_CHECK(primaryCanon != secondaryCanon);

      const std::string tag1 = nythera::test::uniqueToken(".nythera-remediation-test-1");
      const std::string tag2 = nythera::test::uniqueToken(".nythera-remediation-test-2");
      nythera::test::ScopedOwnedDir nfs1Owner(primaryCanon, tag1, true);
      nythera::test::ScopedOwnedDir nfs2Owner(secondaryCanon, tag2, true);
      const auto& nfs1 = nfs1Owner.path();
      const auto& nfs2 = nfs2Owner.path();
      const auto localDir = root / "real-nfs-local";
      const auto isolatedXdgData = root / "real-nfs-xdg-data";
      std::filesystem::create_directories(localDir);
      std::filesystem::create_directories(isolatedXdgData);
      nythera::test::ScopedEnvVar isolatedXdgDataGuard("XDG_DATA_HOME", isolatedXdgData.string());

      FileOperationManager nfsMgr([](const FileOperationManager::Operation&) {});

      // 1. Create folder, empty file, template file, and symlink on NFS primary.
      const auto nfsFolder = nfs1 / "created-folder";
      const auto nfsEmptyFile = nfs1 / "empty.txt";
      const auto localTemplate = localDir / "template.odt";
      const auto nfsFromTemplate = nfs1 / "from-template.odt";
      const auto nfsSymlink = nfs1 / "created-link";
      write(localTemplate, "REAL-NFS-TEMPLATE-PAYLOAD");
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.createFolder(nfsFolder.string())));
      TEST_CHECK(std::filesystem::is_directory(nfsFolder));
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.createFile(nfsEmptyFile.string())));
      TEST_CHECK(std::filesystem::is_regular_file(nfsEmptyFile));
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.createFileFromTemplate(localTemplate.string(), nfsFromTemplate.string())));
      TEST_CHECK(read(nfsFromTemplate) == "REAL-NFS-TEMPLATE-PAYLOAD");
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.createSymlink("empty.txt", nfsSymlink.string())));
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(nfsSymlink)));
      TEST_CHECK(std::filesystem::read_symlink(nfsSymlink) == "empty.txt");

      // 2. Undo and Redo on NFS primary for creation and rename.
      TEST_CHECK(nfsMgr.canUndo());
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.undo()));
      TEST_CHECK(!std::filesystem::exists(std::filesystem::symlink_status(nfsSymlink)));
      TEST_CHECK(nfsMgr.canRedo());
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.redo()));
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(nfsSymlink)));

      const auto nfsRenamedFile = nfs1 / "renamed-from-template.odt";
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.rename(nfsFromTemplate.string(), nfsRenamedFile.string())));
      TEST_CHECK(!std::filesystem::exists(nfsFromTemplate));
      TEST_CHECK(read(nfsRenamedFile) == "REAL-NFS-TEMPLATE-PAYLOAD");
      TEST_CHECK(nfsMgr.canUndo());
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.undo()));
      TEST_CHECK(read(nfsFromTemplate) == "REAL-NFS-TEMPLATE-PAYLOAD");
      TEST_CHECK(!std::filesystem::exists(nfsRenamedFile));
      TEST_CHECK(nfsMgr.canRedo());
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.redo()));
      TEST_CHECK(read(nfsRenamedFile) == "REAL-NFS-TEMPLATE-PAYLOAD");

      // 3. Same-NFS copy, move, duplicate of files, symlinks, and non-empty directories.
      const auto nfsSubA = nfs1 / "subA";
      const auto nfsSubB = nfs1 / "subB";
      std::filesystem::create_directories(nfsSubA);
      std::filesystem::create_directories(nfsSubB);
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.copy({nfsRenamedFile.string(), nfsSymlink.string()}, nfsSubA.string())));
      TEST_CHECK(read(nfsSubA / "renamed-from-template.odt") == "REAL-NFS-TEMPLATE-PAYLOAD");
      TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(nfsSubA / "created-link")));
      TEST_CHECK(
          waitFor(nfsMgr, nfsMgr.duplicate({(nfsSubA / "renamed-from-template.odt").string()}, nfsSubA.string()))
      );
      TEST_CHECK(read(nfsSubA / "renamed-from-template (copy).odt") == "REAL-NFS-TEMPLATE-PAYLOAD");

      // Non-empty directory copy, duplicate, rename, and same-FS move on NFS.
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.copy({nfsSubA.string()}, nfsSubB.string())));
      TEST_CHECK(read(nfsSubB / "subA" / "renamed-from-template.odt") == "REAL-NFS-TEMPLATE-PAYLOAD");
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.move({(nfsSubB / "subA").string()}, nfsFolder.string())));
      TEST_CHECK(!std::filesystem::exists(nfsSubB / "subA"));
      TEST_CHECK(read(nfsFolder / "subA" / "renamed-from-template.odt") == "REAL-NFS-TEMPLATE-PAYLOAD");

      // 4. Conflict handling on NFS: Skip, Replace, Rename, and Directory Merge.
      const auto conflictSrc = nfs1 / "conflict-item.txt";
      write(conflictSrc, "NEW-NFS-CONFLICT-DATA");
      write(nfsSubB / "conflict-item.txt", "OLD-NFS-CONFLICT-DATA");
      nfsMgr.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, false};
      });
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.copy({conflictSrc.string()}, nfsSubB.string())));
      TEST_CHECK(read(nfsSubB / "conflict-item.txt") == "OLD-NFS-CONFLICT-DATA");

      nfsMgr.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.copy({conflictSrc.string()}, nfsSubB.string())));
      TEST_CHECK(read(nfsSubB / "conflict-item.txt") == "NEW-NFS-CONFLICT-DATA");

      nfsMgr.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        return FileOperationManager::ConflictDecision{
            FileOperationManager::ConflictAction::Rename, false, "conflict-item-alt.txt"
        };
      });
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.copy({conflictSrc.string()}, nfsSubB.string())));
      TEST_CHECK(read(nfsSubB / "conflict-item-alt.txt") == "NEW-NFS-CONFLICT-DATA");

      // Directory Merge on NFS.
      const auto mergeSrcDir = nfs1 / "merge-dir";
      const auto mergeDstDir = nfsSubB / "merge-dir";
      std::filesystem::create_directories(mergeSrcDir);
      std::filesystem::create_directories(mergeDstDir);
      write(mergeSrcDir / "from-src.txt", "from-src");
      write(mergeDstDir / "from-dst.txt", "from-dst");
      nfsMgr.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Merge, false};
      });
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.move({mergeSrcDir.string()}, nfsSubB.string())));
      TEST_CHECK(!std::filesystem::exists(mergeSrcDir));
      TEST_CHECK(read(mergeDstDir / "from-src.txt") == "from-src");
      TEST_CHECK(read(mergeDstDir / "from-dst.txt") == "from-dst");
      nfsMgr.setConflictResolver({});

      // 5. Local <-> NFS and NFS1 <-> NFS2 transfers (Copy & Move in both directions).
      const auto localSrc = localDir / "local-to-nfs.txt";
      write(localSrc, "LOCAL-TO-NFS-DATA");
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.copy({localSrc.string()}, nfs1.string())));
      TEST_CHECK(read(nfs1 / "local-to-nfs.txt") == "LOCAL-TO-NFS-DATA");
      std::filesystem::remove(localSrc);
      TEST_CHECK(!std::filesystem::exists(localSrc));
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.move({(nfs1 / "local-to-nfs.txt").string()}, nfs2.string())));
      TEST_CHECK(!std::filesystem::exists(nfs1 / "local-to-nfs.txt"));
      TEST_CHECK(read(nfs2 / "local-to-nfs.txt") == "LOCAL-TO-NFS-DATA");
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.move({(nfs2 / "local-to-nfs.txt").string()}, localDir.string())));
      TEST_CHECK(!std::filesystem::exists(nfs2 / "local-to-nfs.txt"));
      TEST_CHECK(read(localSrc) == "LOCAL-TO-NFS-DATA");

      // 6. Wastebin on NFS primary:
      //    - Verify trash() fails closed (via injected hook) without writing to mount Wastebin
      //      and offers Home Wastebin fallback.
      //    - Verify trashToHomeWastebin() moves item into isolated XDG_DATA_HOME/Trash and removes NFS source.
      //    - Verify restore() restores item from isolated Home Wastebin back onto NFS primary.
      const auto trashTarget = nfs1 / "Borderless Test Item.exe";
      write(trashTarget, "NFS-WASTEBIN-FALLBACK-PAYLOAD");
      {
        const auto trashHookGuard = nythera::test::makeScopeExit([] { setFileOperationTrashHookForTesting({}); });
        setFileOperationTrashHookForTesting([&](const char* path) -> int {
          return path != nullptr && std::string(path) == trashTarget.string() ? EACCES : 0;
        });
        TEST_CHECK(!waitFor(nfsMgr, nfsMgr.trash({trashTarget.string()})));
      }
      const auto& failedTrashOp = nfsMgr.operations().back();
      TEST_CHECK(failedTrashOp.trashFallback.has_value());
      TEST_CHECK(failedTrashOp.trashFallback->sources == std::vector<std::string>{trashTarget.string()});
      TEST_CHECK(std::filesystem::exists(trashTarget));

      TEST_CHECK(waitFor(nfsMgr, nfsMgr.trashToHomeWastebin({trashTarget.string()})));
      TEST_CHECK(!std::filesystem::exists(trashTarget));
      const auto homeTrashFiles = isolatedXdgData / "Trash" / "files" / "Borderless Test Item.exe";
      const auto homeTrashInfo = isolatedXdgData / "Trash" / "info" / "Borderless Test Item.exe.trashinfo";
      TEST_CHECK(read(homeTrashFiles) == "NFS-WASTEBIN-FALLBACK-PAYLOAD");
      TEST_CHECK(std::filesystem::is_regular_file(homeTrashInfo));
      TEST_CHECK(read(homeTrashInfo).find("Path=") != std::string::npos);
      TEST_CHECK(read(homeTrashInfo).find("Borderless%20Test%20Item.exe") != std::string::npos);

      // Restore from isolated Home Wastebin back onto NFS primary (/mnt/Gogeta).
      char* homeTrashFileUriRaw = g_filename_to_uri(homeTrashFiles.c_str(), nullptr, nullptr);
      TEST_CHECK(homeTrashFileUriRaw != nullptr);
      const std::string homeTrashFileUri(homeTrashFileUriRaw);
      g_free(homeTrashFileUriRaw);
      FileOperationManagerTestAccess::setWastebinBackend(
          nfsMgr,
          FileOperationManager::WastebinBackend{
              .originalPath = [&](const std::string& uri, GCancellable*, std::string& out, std::string&) {
                if (uri == homeTrashFileUri) {
                  out = trashTarget.string();
                  return true;
                }
                return false;
              },
          }
      );
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.restore({homeTrashFileUri})));
      FileOperationManagerTestAccess::setWastebinBackend(nfsMgr, {});
      TEST_CHECK(read(trashTarget) == "NFS-WASTEBIN-FALLBACK-PAYLOAD");
      TEST_CHECK(!std::filesystem::exists(homeTrashFiles));

      // 7. Permanent deletion of exclusively owned NFS fixtures (ScopedOwnedDir also cleans up on unwind).
      TEST_CHECK(waitFor(nfsMgr, nfsMgr.removePermanently({nfs1.string(), nfs2.string()})));
      TEST_CHECK(!std::filesystem::exists(nfs1));
      TEST_CHECK(!std::filesystem::exists(nfs2));
    }
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-fileops"});
    runTests(env);
  });
}
