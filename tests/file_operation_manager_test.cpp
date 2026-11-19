#include "filemanager/file_operation_manager.h"

#include <atomic>
#include <cassert>
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
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

void setFileOperationRenameNoReplaceHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationReplaceBackupHookForTesting(std::function<void(const char*, const char*, bool)> hook);
void setFileOperationRemoveEntryHookForTesting(std::function<int(const char*)> hook);

class FileOperationManagerTestAccess {
public:
  static void boundary(FileOperationManager& manager, std::function<void(std::uint64_t, std::uint64_t)> observer) {
    manager.m_transferBoundary = std::move(observer);
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
    assert(id != 0);
    Wait state{.loop = g_main_loop_new(nullptr, false), .manager = &manager, .id = id};
    const guint timer = g_timeout_add(10, poll, &state);
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return state.ok;
  }

  void assertCompletedEntry(
      const FileOperationManager::Operation& operation, std::size_t index, const std::filesystem::path& expectedPath
  ) {
    assert(index < operation.completedEntries.size());
    const auto& entry = operation.completedEntries[index];
    assert(entry.path == expectedPath.string());
    struct stat state{};
    assert(lstat(expectedPath.c_str(), &state) == 0);
    assert(entry.device == static_cast<std::uint64_t>(state.st_dev));
    assert(entry.inode == static_cast<std::uint64_t>(state.st_ino));
  }
} // namespace

int main() {
  setenv("GIO_USE_VFS", "local", 1);
  const char* home = std::getenv("HOME");
  assert(home != nullptr);
  std::string rootTemplate = std::string(home) + "/.cache/nythera-fileops-XXXXXX";
  char* rootRaw = mkdtemp(rootTemplate.data());
  assert(rootRaw != nullptr);
  const std::filesystem::path root(rootRaw);
  const auto isolatedData = root / "user-data";
  std::filesystem::create_directories(isolatedData);
  setenv("XDG_DATA_HOME", isolatedData.c_str(), 1);
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
  assert(waitFor(manager, nestedCopyId));
  assert(std::filesystem::exists(to / "nested" / "payload.txt"));
  const auto copied = std::ranges::find(manager.operations(), nestedCopyId, &FileOperationManager::Operation::id);
  assert(copied != manager.operations().end());
  assert(copied->totalBytes == std::filesystem::file_size(from / "nested" / "payload.txt"));
  assert(copied->processedBytes == copied->totalBytes && copied->totalFiles == 1 && copied->processedFiles == 1);
  assert(copied->resultTitle == "Copy completed");
  assert(copied->resultDescription.starts_with("1 file copied to "));
  assert(copied->completedEntries.size() == 1);
  assertCompletedEntry(*copied, 0, to / "nested");
  assert(!waitFor(manager, manager.copy({(from / "nested").string()}, to.string())));
  {
    std::ifstream existing(to / "nested" / "payload.txt");
    std::string contents;
    std::getline(existing, contents);
    assert(contents == "synthetic test payload");
  }
  assert(!waitFor(manager, manager.copy({from.string()}, (from / "nested").string())));
  const auto invalidCopy = std::ranges::find_if(manager.operations(), [](const auto& operation) {
    return operation.finished && !operation.succeeded && operation.status.find("into itself") != std::string::npos;
  });
  assert(invalidCopy != manager.operations().end());
  assert(invalidCopy->resultTitle == "Copy failed");
  assert(invalidCopy->resultDescription == "Cannot copy \"from\" into itself");
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
  assert(waitFor(manager, batchCopyId));
  assert(std::filesystem::exists(to / "batch-a.txt") && std::filesystem::exists(to / "batch-b.txt"));
  const auto batchCopy = std::ranges::find(manager.operations(), batchCopyId, &FileOperationManager::Operation::id);
  assert(batchCopy->completedEntries.size() == 2);
  assertCompletedEntry(*batchCopy, 0, to / "batch-a.txt");
  assertCompletedEntry(*batchCopy, 1, to / "batch-b.txt");
  // Every batch source and its destination must stay protected through
  // cancellation, until the worker has left its conflict/cleanup path.
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
  manager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
    safetyConflictReached.store(true, std::memory_order_release);
    while (!releaseSafetyWorker.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto protectedId =
      manager.copy({(mountA / "first.txt").string(), (mountB / "second.txt").string()}, mountDestination.string());
  while (!safetyConflictReached.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(manager.usesMount(mountA));
  assert(manager.usesMount(mountB));
  assert(manager.usesMount(mountDestination));
  assert(!manager.usesMount(unrelatedMount));
  manager.cancel(protectedId);
  // Drain the caller context while the cancelled worker deliberately remains
  // blocked: no early GTask completion may release these mounts.
  for (int iteration = 0; iteration < 10; ++iteration) {
    while (g_main_context_iteration(nullptr, false)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(manager.active());
  assert(manager.usesMount(mountB));
  assert(manager.usesMount(mountDestination));
  releaseSafetyWorker.store(true, std::memory_order_release);
  assert(!waitFor(manager, protectedId));
  assert(!manager.usesMount(mountA));
  assert(!manager.usesMount(mountB));
  assert(!manager.usesMount(mountDestination));
  assert(std::filesystem::exists(mountA / "first.txt"));
  assert(std::filesystem::exists(mountB / "second.txt"));
  manager.setConflictResolver({});
  assert(waitFor(manager, manager.createFolder((root / "created").string())));
  assert(waitFor(manager, manager.createFile((root / "created" / "empty").string())));
  assert(
      waitFor(manager, manager.rename((root / "created" / "empty").string(), (root / "created" / "renamed").string()))
  );
  assert(std::filesystem::exists(root / "created" / "renamed"));
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
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
  });
  assert(waitFor(manager, manager.copy({incoming.string()}, to.string())));
  {
    std::ifstream file(existing);
    std::string value;
    file >> value;
    assert(value == "replacement");
  }
  {
    std::ofstream file(existing);
    file << "keep";
  }
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, false};
  });
  assert(waitFor(manager, manager.copy({incoming.string()}, to.string())));
  {
    std::ifstream file(existing);
    std::string value;
    file >> value;
    assert(value == "keep");
  }
  int selfConflictCount = 0;
  manager.setConflictResolver(
      [&selfConflictCount](const std::string& source, const std::string& destination, bool, GCancellable*) {
        assert(source == destination);
        ++selfConflictCount;
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
      }
  );
  assert(waitFor(manager, manager.copy({incoming.string()}, incoming.parent_path().string())));
  assert(selfConflictCount == 1);
  assert(std::filesystem::exists(root / "incoming (copy).txt"));
  assert(!waitFor(manager, manager.move({incoming.string()}, incoming.parent_path().string())));
  assert(std::filesystem::exists(incoming));
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
  });
  const auto keepBothCopyId = manager.copy({incoming.string()}, to.string());
  assert(waitFor(manager, keepBothCopyId));
  assert(std::filesystem::exists(to / "incoming (copy).txt"));
  const auto keepBothCopy =
      std::ranges::find(manager.operations(), keepBothCopyId, &FileOperationManager::Operation::id);
  assert(keepBothCopy->completedEntries.size() == 1);
  assertCompletedEntry(*keepBothCopy, 0, to / "incoming (copy).txt");
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{
        FileOperationManager::ConflictAction::Rename, false, "chosen-name.txt"
    };
  });
  assert(waitFor(manager, manager.copy({incoming.string()}, to.string())));
  assert(std::filesystem::exists(to / "chosen-name.txt"));
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false, "../escape.txt"};
  });
  assert(!waitFor(manager, manager.copy({incoming.string()}, to.string())));
  assert(!std::filesystem::exists(root / "escape.txt"));
  {
    std::ofstream file(to / "occupied-name.txt");
    file << "must stay";
  }
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{
        FileOperationManager::ConflictAction::Rename, false, "occupied-name.txt"
    };
  });
  assert(!waitFor(manager, manager.copy({incoming.string()}, to.string())));
  {
    std::ifstream file(to / "occupied-name.txt");
    std::string value;
    file >> value;
    assert(value == "must");
  }
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto cancelledConflictId = manager.copy({incoming.string()}, to.string());
  assert(!waitFor(manager, cancelledConflictId));
  const auto cancelledConflict =
      std::ranges::find(manager.operations(), cancelledConflictId, &FileOperationManager::Operation::id);
  assert(cancelledConflict->cancelled && cancelledConflict->resultTitle == "Copy cancelled");
  assert(cancelledConflict->completedEntries.empty());
  {
    std::ifstream file(existing);
    std::string value;
    file >> value;
    assert(value == "keep");
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
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto partialBatchId =
      manager.copy({completedBeforeCancel.string(), conflictBeforeCancel.string()}, to.string());
  assert(!waitFor(manager, partialBatchId));
  const auto partialBatch =
      std::ranges::find(manager.operations(), partialBatchId, &FileOperationManager::Operation::id);
  assert(partialBatch->cancelled && !partialBatch->succeeded);
  assert(partialBatch->completedEntries.size() == 1);
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
  manager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
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
  assert(!waitFor(manager, replacedPriorId));
  assert(changedEarlierDestination);
  const auto replacedPrior =
      std::ranges::find(manager.operations(), replacedPriorId, &FileOperationManager::Operation::id);
  assert(replacedPrior->cancelled && replacedPrior->completedEntries.empty());
  {
    std::ifstream file(heldOriginalDestination);
    std::string value;
    file >> value;
    assert(value == "first");
  }
  {
    std::ifstream file(identityFirstDestination);
    std::string value;
    file >> value;
    assert(value == "unrelated");
  }
  std::atomic_bool conflictReached{false};
  manager.setConflictResolver(
      [&conflictReached](const std::string&, const std::string&, bool, GCancellable* cancellable) {
        conflictReached.store(true, std::memory_order_release);
        while (!g_cancellable_is_cancelled(cancellable))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
      }
  );
  const auto cancelledReplaceId = manager.copy({incoming.string()}, to.string());
  while (!conflictReached.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  manager.cancel(cancelledReplaceId);
  assert(!waitFor(manager, cancelledReplaceId));
  const auto cancelledReplace =
      std::ranges::find(manager.operations(), cancelledReplaceId, &FileOperationManager::Operation::id);
  assert(cancelledReplace->cancelled && cancelledReplace->resultTitle == "Copy cancelled");
  assert(cancelledReplace->resultDescription.starts_with("Transfer to "));
  assert(std::filesystem::exists(incoming));
  {
    std::ifstream file(existing);
    std::string value;
    file >> value;
    assert(value == "keep");
  }
  for (const auto& entry : std::filesystem::directory_iterator(to))
    assert(!entry.path().filename().string().starts_with(".nythera-partial-"));
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
  manager.setConflictResolver([&conflictCount](const std::string&, const std::string&, bool, GCancellable*) {
    ++conflictCount;
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, true};
  });
  const auto skippedBatchId = manager.copy({allA.string(), allB.string()}, to.string());
  assert(waitFor(manager, skippedBatchId));
  assert(conflictCount == 1);
  const auto skippedBatch =
      std::ranges::find(manager.operations(), skippedBatchId, &FileOperationManager::Operation::id);
  assert(skippedBatch->completedEntries.empty());
  {
    std::ifstream file(to / "all-a.txt");
    std::string value;
    file >> value;
    assert(value == "old");
  }
  {
    std::ifstream file(to / "all-b.txt");
    std::string value;
    file >> value;
    assert(value == "old");
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
  manager.setConflictResolver([&mixedConflictCount](const std::string&, const std::string&, bool, GCancellable*) {
    ++mixedConflictCount;
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, mixedConflictCount == 1};
  });
  assert(waitFor(
      manager, manager.copy({mixedFile.string(), mixedFolder.string(), mixedFileAfterFolder.string()}, to.string())
  ));
  assert(mixedConflictCount == 2);
  {
    std::ifstream file(to / mixedFile.filename());
    std::string value;
    file >> value;
    assert(value == "existing");
  }
  {
    std::ifstream file(to / mixedFolder.filename() / "payload.txt");
    std::string value;
    file >> value;
    assert(value == "existing");
  }
  {
    std::ifstream file(to / mixedFileAfterFolder.filename());
    std::string value;
    file >> value;
    assert(value == "existing");
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
  manager.setConflictResolver([&directoryConflictCount](const std::string&, const std::string&, bool, GCancellable*) {
    ++directoryConflictCount;
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
  });
  assert(waitFor(manager, manager.copy({collidingDirectory.string()}, to.string())));
  assert(directoryConflictCount == 1);
  assert(std::filesystem::exists(to / "colliding-folder" / "existing.txt"));
  assert(std::filesystem::exists(to / "colliding-folder (copy)" / "incoming.txt"));
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
  manager.setConflictResolver([&](const std::string& source, const std::string&, bool, GCancellable*) {
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
  assert(waitFor(manager, manager.copy({mergeSource.string()}, mergeParent.string())));
  const auto mergedCopy = std::ranges::find_if(manager.operations(), [&](const auto& operation) {
    return operation.kind == FileOperationManager::Kind::Copy
        && operation.source == mergeSource.string()
        && operation.destination == mergeParent.string()
        && operation.finished;
  });
  assert(mergedCopy != manager.operations().end());
  assert(mergedCopy->completedEntries.size() == 1);
  assertCompletedEntry(*mergedCopy, 0, mergeTarget);
  assert(merges == 1); // Explicit directory batch policy covers nested directories.
  assert(read(mergeTarget / "only-destination.txt") == "destination");
  assert(read(mergeTarget / "only-source.txt") == "source");
  assert(read(mergeTarget / "nested" / "replace.txt") == "new");
  assert(read(mergeTarget / "nested" / "skip.txt") == "keep");
  assert(read(mergeTarget / "nested" / "both.txt") == "existing both");
  assert(read(mergeTarget / "nested" / "chosen.txt") == "incoming both");
  assert(std::filesystem::is_directory(mergeTarget / "file-v-folder"));
  assert(read(mergeTarget / "folder-v-file") == "existing file");
  assert(std::filesystem::is_symlink(mergeTarget / "loop"));

  const auto allSkippedSource = root / "all-skipped-merge" / "tree";
  const auto allSkippedParent = root / "all-skipped-target";
  std::filesystem::create_directories(allSkippedSource);
  std::filesystem::create_directories(allSkippedParent / "tree");
  write(allSkippedSource / "same.txt", "incoming");
  write(allSkippedParent / "tree" / "same.txt", "existing");
  manager.setConflictResolver([](const std::string& source, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{
        std::filesystem::path(source).filename() == "tree" ? FileOperationManager::ConflictAction::Merge
                                                           : FileOperationManager::ConflictAction::Skip,
        false
    };
  });
  const auto allSkippedMergeId = manager.copy({allSkippedSource.string()}, allSkippedParent.string());
  assert(waitFor(manager, allSkippedMergeId));
  const auto allSkippedMerge =
      std::ranges::find(manager.operations(), allSkippedMergeId, &FileOperationManager::Operation::id);
  assert(allSkippedMerge->completedEntries.empty());
  assert(!waitFor(manager, manager.copy({mergeSource.string()}, (mergeSource / "nested").string())));
  assert(!waitFor(manager, manager.move({mergeSource.string()}, mergeSource.parent_path().string())));

  const auto explicitSource = root / "explicit-source" / "tree";
  const auto explicitParent = root / "explicit-target";
  std::filesystem::create_directories(explicitSource / "nested");
  std::filesystem::create_directories(explicitParent / "tree" / "nested");
  write(explicitSource / "nested" / "payload", "payload");
  int explicitMergeCount = 0;
  manager.setConflictResolver([&](const std::string&, const std::string&, bool hasRemaining, GCancellable*) {
    assert(hasRemaining); // A directory may contain further conflicts even in a single-item operation.
    ++explicitMergeCount;
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Merge, false};
  });
  assert(waitFor(manager, manager.move({explicitSource.string()}, explicitParent.string())));
  assert(explicitMergeCount == 2);
  assert(!std::filesystem::exists(explicitSource));
  assert(read(explicitParent / "tree" / "nested" / "payload") == "payload");
  const auto explicitMove = std::ranges::find_if(manager.operations(), [&](const auto& operation) {
    return operation.kind == FileOperationManager::Kind::Move
        && operation.source == explicitSource.string()
        && operation.destination == explicitParent.string()
        && operation.finished;
  });
  assert(explicitMove != manager.operations().end());
  assert(explicitMove->completedEntries.size() == 1);
  assertCompletedEntry(*explicitMove, 0, explicitParent / "tree");

  // Cancellation after a directory merge decision must be reported as cancelled,
  // retain the source and leave an existing nested destination untouched.
  manager.setConflictResolver([](const std::string& source, const std::string&, bool, GCancellable* cancellable) {
    using Action = FileOperationManager::ConflictAction;
    if (std::filesystem::path(source).filename() == "tree")
      return FileOperationManager::ConflictDecision{Action::Merge, false};
    g_cancellable_cancel(cancellable);
    return FileOperationManager::ConflictDecision{Action::Cancel, false};
  });
  const auto beforeCancel = root / "before-cancel.txt";
  write(beforeCancel, "completed before cancellation");
  const auto cancelledMerge = manager.move({beforeCancel.string(), mergeSource.string()}, mergeParent.string());
  assert(!waitFor(manager, cancelledMerge));
  const auto cancelledMergeResult =
      std::ranges::find(manager.operations(), cancelledMerge, &FileOperationManager::Operation::id);
  assert(cancelledMergeResult->cancelled && !cancelledMergeResult->succeeded);
  assert(cancelledMergeResult->completedEntries.size() == 1);
  assertCompletedEntry(*cancelledMergeResult, 0, mergeParent / "before-cancel.txt");
  assert(read(mergeParent / "before-cancel.txt") == "completed before cancellation");
  assert(std::filesystem::exists(mergeSource / "nested" / "replace.txt"));
  assert(read(mergeTarget / "nested" / "replace.txt") == "new");

  // A failed nested decision cannot leave a partially completed merge marked successful.
  manager.setConflictResolver([](const std::string& source, const std::string&, bool, GCancellable*) {
    using Action = FileOperationManager::ConflictAction;
    if (std::filesystem::path(source).filename() == "tree")
      return FileOperationManager::ConflictDecision{Action::Merge, true};
    return FileOperationManager::ConflictDecision{Action::Rename, false, "../invalid"};
  });
  const auto beforeFailure = root / "before-failure.txt";
  write(beforeFailure, "completed before failure");
  assert(!waitFor(manager, manager.copy({beforeFailure.string(), mergeSource.string()}, mergeParent.string())));
  const auto failedMerge = std::ranges::find_if(manager.operations(), [&](const auto& operation) {
    return operation.kind == FileOperationManager::Kind::Copy
        && operation.source == beforeFailure.string()
        && operation.destination == mergeParent.string()
        && operation.finished
        && !operation.succeeded;
  });
  assert(failedMerge != manager.operations().end());
  assert(failedMerge->completedEntries.size() == 1);
  assertCompletedEntry(*failedMerge, 0, mergeParent / "before-failure.txt");
  assert(read(mergeParent / "before-failure.txt") == "completed before failure");
  assert(read(mergeTarget / "nested" / "replace.txt") == "new");

  // A successful merge move removes only transferred source children. Skipped
  // collisions remain at source; destination-only files survive.
  const auto moveMerge = root / "move-merge" / "tree";
  std::filesystem::create_directories(moveMerge);
  write(moveMerge / "move-only.txt", "move payload");
  write(moveMerge / "only-destination.txt", "skip payload");
  manager.setConflictResolver([](const std::string& source, const std::string&, bool, GCancellable*) {
    using Action = FileOperationManager::ConflictAction;
    return FileOperationManager::ConflictDecision{
        std::filesystem::path(source).filename() == "tree" ? Action::Merge : Action::Skip, false
    };
  });
  const auto moveMergeId = manager.move({moveMerge.string()}, mergeParent.string());
  assert(waitFor(manager, moveMergeId));
  assert(read(mergeTarget / "move-only.txt") == "move payload");
  assert(!std::filesystem::exists(moveMerge / "move-only.txt"));
  const auto moveMergeOperation =
      std::ranges::find(manager.operations(), moveMergeId, &FileOperationManager::Operation::id);
  assert(moveMergeOperation->completedEntries.size() == 1);
  assertCompletedEntry(*moveMergeOperation, 0, mergeTarget);
  assert(read(moveMerge / "only-destination.txt") == "skip payload");
  assert(read(mergeTarget / "only-destination.txt") == "destination");
  const auto moveSource = root / "move-source.txt";
  {
    std::ofstream file(moveSource);
    file << "move fixture";
  }
  assert(waitFor(manager, manager.move({moveSource.string()}, to.string())));
  assert(!std::filesystem::exists(moveSource) && std::filesystem::exists(to / "move-source.txt"));
  const std::filesystem::path sharedMemoryRoot = "/dev/shm/nythera-fileops-" + std::to_string(getpid());
  std::error_code shmError;
  if (std::filesystem::create_directory(sharedMemoryRoot, shmError)) {
    struct stat sourceStat{};
    struct stat targetStat{};
    const auto crossSource = root / "cross-device-source.txt";
    const auto crossDestination = sharedMemoryRoot / "destination";
    std::filesystem::create_directory(crossDestination);
    {
      std::ofstream file(crossSource);
      file << "cross-device move fixture";
    }
    if (stat(root.c_str(), &sourceStat) == 0
        && stat(sharedMemoryRoot.c_str(), &targetStat) == 0
        && sourceStat.st_dev != targetStat.st_dev) {
      assert(waitFor(manager, manager.move({crossSource.string()}, crossDestination.string())));
      assert(!std::filesystem::exists(crossSource));
      assert(std::filesystem::exists(crossDestination / crossSource.filename()));
      const auto crossMergeSource = root / "cross-merge" / "tree";
      std::filesystem::create_directories(crossMergeSource / "nested");
      std::filesystem::create_directories(crossDestination / "tree" / "nested");
      write(crossMergeSource / "nested" / "payload", "cross merge payload");
      write(crossDestination / "tree" / "destination-only", "preserve");
      manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Merge, true};
      });
      assert(waitFor(manager, manager.move({crossMergeSource.string()}, crossDestination.string())));
      assert(!std::filesystem::exists(crossMergeSource));
      assert(read(crossDestination / "tree" / "nested" / "payload") == "cross merge payload");
      assert(read(crossDestination / "tree" / "destination-only") == "preserve");
    }
    std::filesystem::remove_all(sharedMemoryRoot);
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
    assert(waitFor(manager, manager.copy({(linkSource / name).string()}, linkCopy.string())));
    assert(std::filesystem::is_symlink(std::filesystem::symlink_status(linkCopy / name)));
    assert(std::filesystem::read_symlink(linkCopy / name) == target);
    assert(waitFor(
        manager, manager.rename((linkCopy / name).string(), (linkCopy / (std::string(name) + "-renamed")).string())
    ));
    const auto renamed = linkCopy / (std::string(name) + "-renamed");
    assert(waitFor(manager, manager.move({renamed.string()}, linkMove.string())));
    const auto moved = linkMove / renamed.filename();
    assert(std::filesystem::is_symlink(std::filesystem::symlink_status(moved)));
    assert(waitFor(manager, manager.trash({moved.string()})));
    assert(!std::filesystem::is_symlink(std::filesystem::symlink_status(moved)));
    assert(waitFor(manager, manager.removePermanently({(linkSource / name).string()})));
    assert(read(linkTarget / "payload") == "preserved target");
    assert(!std::filesystem::exists(root / "missing-link-target"));
  }
  const auto trashSource = root / "trash-fixture.txt";
  {
    std::ofstream file(trashSource);
    file << "trash fixture";
  }
  assert(waitFor(manager, manager.trash({trashSource.string()})));
  assert(!std::filesystem::exists(trashSource));
  assert(waitFor(manager, manager.removePermanently({(root / "created").string()})));
  // Inline rename uses checked GIO moves for files and directories alike.
  const auto inlineSource = root / "inline-folder";
  const auto inlineTarget = root / "inline-folder-renamed";
  std::filesystem::create_directory(inlineSource);
  write(inlineSource / "child.txt", "retained");
  struct stat inlineIdentity{};
  assert(lstat(inlineSource.c_str(), &inlineIdentity) == 0);
  const auto inlineRename = [&](const std::filesystem::path& destination) {
    return manager.renameChecked(
        inlineSource.string(), destination.string(), inlineIdentity.st_dev, inlineIdentity.st_ino, {}
    );
  };
  std::filesystem::create_directory(inlineTarget);
  assert(!waitFor(manager, inlineRename(inlineTarget)));
  assert(read(inlineSource / "child.txt") == "retained");
  assert(!waitFor(manager, inlineRename(root / "missing-parent" / "invalid")));
  assert(read(inlineSource / "child.txt") == "retained");
  std::filesystem::remove(inlineTarget);
  assert(waitFor(manager, inlineRename(inlineTarget)));
  assert(!std::filesystem::exists(inlineSource) && read(inlineTarget / "child.txt") == "retained");

  // Properties jobs share cancellation/progress infrastructure and reject duplicate identities.
  const auto metadataFile = root / "metadata-fixture";
  write(metadataFile, "metadata");
  assert(chmod(metadataFile.c_str(), 0600) == 0);
  struct stat metadataState{};
  assert(lstat(metadataFile.c_str(), &metadataState) == 0);
  filemanager::PropertyRequest metadataRequest{
      metadataFile, static_cast<std::uint64_t>(metadataState.st_dev), metadataState.st_ino, 0600, 0640, {}, {}, false
  };
  bool metadataFinished = false;
  const auto metadataId = manager.modifyProperties(metadataRequest, geteuid(), [&](const auto& result) {
    metadataFinished = true;
    assert(result.ok() && result.succeeded == 1);
    struct stat refreshed{};
    assert(lstat(metadataFile.c_str(), &refreshed) == 0 && (refreshed.st_mode & 0777) == 0640);
  });
  assert(metadataId != 0 && manager.modifyProperties(metadataRequest, geteuid(), {}) == 0);
  assert(waitFor(manager, metadataId) && metadataFinished);

  const auto renameSource = root / "properties-name.txt";
  write(renameSource, "preserved original");
  struct stat renameIdentity{};
  assert(lstat(renameSource.c_str(), &renameIdentity) == 0);
  bool renameCallback = false;
  auto checkedRename = [&](const std::filesystem::path& source, const std::filesystem::path& destination) {
    return manager.renameChecked(
        source.string(), destination.string(), renameIdentity.st_dev, renameIdentity.st_ino,
        [&](bool success, const std::string& message) {
          renameCallback = true;
          assert(!message.empty());
          (void)success;
        }
    );
  };
  const auto renameDestination = root / "renamed.txt-backup";
  assert(waitFor(manager, checkedRename(renameSource, renameDestination)) && renameCallback);
  assert(!std::filesystem::exists(renameSource) && read(renameDestination) == "preserved original");
  assert(!waitFor(manager, checkedRename(renameDestination, root / "")));
  write(root / "collision", "do not overwrite");
  assert(!waitFor(manager, checkedRename(renameDestination, root / "collision")));
  assert(read(root / "collision") == "do not overwrite" && std::filesystem::exists(renameDestination));
  const auto renamedOld = root / "original-inode";
  std::filesystem::rename(renameDestination, renamedOld);
  write(renameDestination, "replacement");
  assert(!waitFor(manager, checkedRename(renameDestination, root / "must-not-exist")));
  assert(read(renameDestination) == "replacement" && !std::filesystem::exists(root / "must-not-exist"));
  const auto propertiesLink = root / "properties-link";
  std::filesystem::create_symlink(renamedOld, propertiesLink);
  assert(lstat(propertiesLink.c_str(), &renameIdentity) == 0);
  const auto newLink = root / "renamed-link";
  assert(waitFor(manager, checkedRename(propertiesLink, newLink)));
  assert(
      std::filesystem::is_symlink(std::filesystem::symlink_status(newLink)) && read(renamedOld) == "preserved original"
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
    assert(!waitFor(links, links.createSymlink("replacement", dangling.string())));
    assert(std::filesystem::read_symlink(dangling) == "already-missing");
    assert(waitFor(links, links.createSymlink("../missing/./file", manualLink.string())));
    assert(std::filesystem::read_symlink(manualLink) == "../missing/./file");
    assertCompletedEntry(links.operations().back(), 0, manualLink);
    assert(waitFor(links, links.undo()));
    assert(!std::filesystem::is_symlink(std::filesystem::symlink_status(manualLink)));
    assert(waitFor(links, links.redo()));
    assert(std::filesystem::read_symlink(manualLink) == "../missing/./file");
    assert(waitFor(links, links.createSymlink(archive.string(), dangling.string(), true)));
    assert(std::filesystem::read_symlink(autoLink) == archive);
    assertCompletedEntry(links.operations().back(), 0, autoLink);
    assert(waitFor(links, links.undo()) && links.canRedo());
    assert(waitFor(links, links.createSymlink(edit.string(), (edit / "edit-link").string(), true)));
    assert(std::filesystem::read_symlink(edit / "edit-link") == edit);
    assert(!links.canRedo());
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
    assert(waitFor(
        duplicate, duplicate.duplicate({folder.string(), manualLink.string(), ordinary.string()}, edit.string())
    ));
    assert(read(edit / "folder (copy)" / "child") == "nested");
    assert(std::filesystem::read_symlink(edit / "manual (copy)") == "../missing/./file");
    assert(std::filesystem::read_symlink(edit / "document (copy).txt") == "not-there");
    assert(duplicate.operations().back().completedEntries.size() == 3);
    assertCompletedEntry(duplicate.operations().back(), 2, firstCopy);
    assert(waitFor(duplicate, duplicate.undo()));
    assert(!std::filesystem::exists(firstCopy));
    assert(waitFor(duplicate, duplicate.redo()));
    assert(read(firstCopy) == "original");
    write(firstCopy, "external edits");
    assert(!waitFor(duplicate, duplicate.undo()));
    assert(!duplicate.canUndo() && read(firstCopy) == "external edits");
  }
  {
    FileOperationManager links;
    const auto repeatedLink = edit / "manual (copy 2)";
    assert(waitFor(links, links.duplicate({manualLink.string()}, edit.string())));
    assertCompletedEntry(links.operations().back(), 0, repeatedLink);
    assert(waitFor(links, links.undo()));
    assert(!std::filesystem::is_symlink(std::filesystem::symlink_status(repeatedLink)));
    assert(waitFor(links, links.redo()));
    assert(std::filesystem::read_symlink(repeatedLink) == "../missing/./file");
  }
  {
    FileOperationManager creates;
    const auto emptyFolder = edit / "empty";
    const auto emptyFile = edit / "empty-file";
    assert(waitFor(creates, creates.createFolder(emptyFolder.string())));
    assert(waitFor(creates, creates.undo()) && !std::filesystem::exists(emptyFolder));
    assert(waitFor(creates, creates.redo()) && std::filesystem::is_empty(emptyFolder));
    assert(waitFor(creates, creates.createFile(emptyFile.string())));
    assert(waitFor(creates, creates.undo()) && !std::filesystem::exists(emptyFile));
    assert(waitFor(creates, creates.redo()));
    assert(std::filesystem::file_size(emptyFile) == 0);
    std::filesystem::rename(emptyFile, edit / "former-empty-file");
    write(emptyFile, "replacement");
    assert(!waitFor(creates, creates.undo()));
    assert(read(emptyFile) == "replacement");
  }
  {
    FileOperationManager creates;
    const auto parent = edit / "parent";
    const auto relocated = edit / "relocated-parent";
    std::filesystem::create_directory(parent);
    assert(waitFor(creates, creates.createFile((parent / "file").string())));
    std::filesystem::rename(parent, relocated);
    std::filesystem::create_directory(parent);
    write(parent / "file", "external file in replacement directory");
    assert(!waitFor(creates, creates.undo()));
    assert(read(parent / "file") == "external file in replacement directory");
    assert(std::filesystem::file_size(relocated / "file") == 0);
  }
  {
    FileOperationManager replay;
    FileOperationManager otherWindow;
    const auto first = edit / "first";
    const auto second = edit / "second";
    const auto third = edit / "third";
    write(first, "keep this");
    assert(waitFor(replay, replay.rename(first.string(), second.string())));
    assert(waitFor(replay, replay.rename(second.string(), third.string())));
    assert(waitFor(otherWindow, otherWindow.createSymlink("elsewhere", second.string())));
    assert(!waitFor(replay, replay.undo()));
    assert(std::filesystem::read_symlink(second) == "elsewhere" && read(third) == "keep this");
    assert(waitFor(otherWindow, otherWindow.undo()));
    assert(waitFor(replay, replay.undo()) && read(second) == "keep this");
    assert(waitFor(replay, replay.undo()) && read(first) == "keep this");
    std::filesystem::create_symlink("redo-conflict", second);
    assert(!waitFor(replay, replay.redo()));
    assert(read(first) == "keep this" && std::filesystem::read_symlink(second) == "redo-conflict");
    std::filesystem::remove(second);
    assert(waitFor(replay, replay.redo()) && read(second) == "keep this");
    assert(waitFor(replay, replay.redo()) && read(third) == "keep this");
    assert(waitFor(replay, replay.move({third.string()}, to.string())));
    assert(waitFor(replay, replay.undo()) && read(third) == "keep this");
    assert(waitFor(replay, replay.redo()) && read(to / third.filename()) == "keep this");
    assert(waitFor(replay, replay.copy({archive.string()}, to.string())));
    assert(waitFor(replay, replay.undo()) && !std::filesystem::exists(to / archive.filename()));
    write(archive, "externally changed source");
    assert(!waitFor(replay, replay.redo()));
    assert(!replay.canRedo() && !std::filesystem::exists(to / archive.filename()));
  }
  {
    FileOperationManager protectsCopy;
    const auto onlyOriginal = edit / "only-original";
    write(onlyOriginal, "keep the only surviving copy");
    assert(waitFor(protectsCopy, protectsCopy.copy({onlyOriginal.string()}, to.string())));
    std::filesystem::remove(onlyOriginal);
    assert(!waitFor(protectsCopy, protectsCopy.undo()));
    assert(read(to / onlyOriginal.filename()) == "keep the only surviving copy");
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
    partial.setConflictResolver([&](const auto&, const auto&, bool, GCancellable* cancellable) {
      resolving.store(true, std::memory_order_release);
      while (!g_cancellable_is_cancelled(cancellable))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel};
    });
    const auto job = partial.copy({earlier.string(), later.string()}, to.string());
    while (!resolving.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!partial.canUndo() && partial.undo() == 0);
    partial.cancel(job);
    assert(!waitFor(partial, job) && partial.operations().back().cancelled);
    assertCompletedEntry(partial.operations().back(), 0, committed);
    const auto undoJob = partial.undo();
    assert(partial.createFile((edit / "overlap").string()) == 0);
    assert(waitFor(partial, undoJob));
    assert(!std::filesystem::exists(committed) && read(to / later.filename()) == "occupied");
    assert(waitFor(partial, partial.redo()) && read(committed) == "first payload");
    const auto failedDuplicate = partial.duplicate({earlier.string(), (to / later.filename()).string()}, edit.string());
    assert(!waitFor(partial, failedDuplicate));
    assertCompletedEntry(partial.operations().back(), 0, edit / "committed (copy).txt");
    assert(waitFor(partial, partial.undo()));
    assert(!std::filesystem::exists(edit / "committed (copy).txt"));
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
    assert(!waitFor(raceManager, raceManager.copy({lateSource.string()}, raceTo.string())));
    assert(injectedLateTarget);
    assert(read(lateTarget) == "appeared during copy");
    for (const auto& entry : std::filesystem::directory_iterator(raceTo))
      assert(!entry.path().filename().string().starts_with(".nythera-partial-"));
    FileOperationManagerTestAccess::boundary(raceManager, {});

    // 2. Replace Copy where the destination inode is replaced by a different
    // file (or directory) between conflict resolution and finalisation must
    // refuse to move/delete the unapproved replacement item.
    const auto swappedSource = raceRoot / "swapped-target.txt";
    const auto swappedTarget = raceTo / swappedSource.filename();
    const auto heldApproved = raceRoot / "held-approved-target.txt";
    write(swappedSource, "incoming replacement");
    write(swappedTarget, "approved target");
    raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
      std::filesystem::rename(swappedTarget, heldApproved);
      write(swappedTarget, "unapproved replacement inode");
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    assert(!waitFor(raceManager, raceManager.copy({swappedSource.string()}, raceTo.string())));
    assert(read(swappedTarget) == "unapproved replacement inode");
    assert(read(heldApproved) == "approved target");
    assert(
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
    raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
      std::filesystem::rename(renameSwapTarget, heldRenameTarget);
      write(renameSwapTarget, "new unapproved rename target");
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    assert(!waitFor(raceManager, raceManager.rename(renameSwapSource.string(), renameSwapTarget.string())));
    assert(read(renameSwapSource) == "rename source payload");
    assert(read(renameSwapTarget) == "new unapproved rename target");

    // 4. Non-replace Rename where the destination appears after the initial
    // existence check (simulated by removing the conflict resolver so a
    // pre-existing destination is not replaced) or during checked rename must
    // never overwrite the destination.
    const auto noreplaceSource = raceRoot / "noreplace-source.txt";
    const auto noreplaceTarget = raceRoot / "noreplace-target.txt";
    write(noreplaceSource, "do not overwrite with this");
    write(noreplaceTarget, "must survive rename");
    raceManager.setConflictResolver({});
    assert(!waitFor(raceManager, raceManager.rename(noreplaceSource.string(), noreplaceTarget.string())));
    assert(read(noreplaceSource) == "do not overwrite with this");
    assert(read(noreplaceTarget) == "must survive rename");

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
    raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
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
    assert(!waitFor(raceManager, raceManager.copy({midCopySwapSource.string()}, raceTo.string())));
    assert(swappedDuringCopy);
    assert(read(midCopySwapTarget) == "swapped while copying");
    assert(read(heldMidCopyTarget) == "approved before copy");
    for (const auto& entry : std::filesystem::directory_iterator(raceTo))
      assert(!entry.path().filename().string().starts_with(".nythera-partial-"));
    FileOperationManagerTestAccess::boundary(raceManager, {});

    // 6. Replace Rename where finalising source -> target fails after the
    // destination has been moved to its temporary backup must roll back the
    // backup so the original destination is preserved intact.
    const auto rollbackSource = raceRoot / "rollback-source.txt";
    const auto rollbackTarget = raceRoot / "rollback-target.txt";
    write(rollbackSource, "vanishing source");
    write(rollbackTarget, "original target must be restored");
    raceManager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
      std::filesystem::remove(rollbackSource);
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    assert(!waitFor(raceManager, raceManager.rename(rollbackSource.string(), rollbackTarget.string())));
    assert(read(rollbackTarget) == "original target must be restored");
    for (const auto& entry : std::filesystem::directory_iterator(raceRoot))
      assert(!entry.path().filename().string().starts_with(".nythera-backup-"));

    // 7a. Replacement timing gap in Copy: destination swapped right after
    // pre-verification and before moving target -> backup must be detected via
    // post-move verification and atomically rolled back without deleting the
    // unapproved item.
    const auto postCopySource = raceRoot / "post-copy-source.txt";
    const auto postCopyTarget = raceTo / postCopySource.filename();
    const auto heldPostCopy = raceRoot / "held-post-copy.txt";
    write(postCopySource, "incoming copy payload");
    write(postCopyTarget, "approved original");
    raceManager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
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
    assert(!waitFor(raceManager, raceManager.copy({postCopySource.string()}, raceTo.string())));
    setFileOperationReplaceBackupHookForTesting({});
    assert(swappedBeforeBackup);
    assert(read(postCopyTarget) == "unapproved mid-backup file");
    assert(read(postCopySource) == "incoming copy payload");
    assert(read(heldPostCopy) == "approved original");
    assert(
        raceManager.operations().back().resultDescription.find("changed before it could be replaced")
        != std::string::npos
    );
    for (const auto& entry : std::filesystem::directory_iterator(raceTo)) {
      const auto fname = entry.path().filename().string();
      assert(!fname.starts_with(".nythera-backup-") && !fname.starts_with(".nythera-partial-"));
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
    assert(!waitFor(raceManager, raceManager.rename(postRenameSource.string(), postRenameTarget.string())));
    setFileOperationReplaceBackupHookForTesting({});
    assert(swappedToDir);
    assert(std::filesystem::is_directory(postRenameTarget));
    assert(read(postRenameTarget / "child.txt") == "preserve directory child");
    assert(read(postRenameSource) == "incoming rename payload");
    assert(read(heldPostRename) == "approved rename original");
    for (const auto& entry : std::filesystem::directory_iterator(raceRoot))
      assert(!entry.path().filename().string().starts_with(".nythera-backup-"));

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
    assert(!waitFor(raceManager, raceManager.copy({collideSource.string()}, raceTo.string())));
    setFileOperationReplaceBackupHookForTesting({});
    assert(didPreSwap && didPostAppear);
    assert(read(collideTarget) == "unapproved-2");
    assert(read(heldCollide) == "approved collide");
    std::filesystem::path survivingBackup;
    for (const auto& entry : std::filesystem::directory_iterator(raceTo)) {
      const auto fname = entry.path().filename().string();
      assert(!fname.starts_with(".nythera-partial-"));
      if (fname.starts_with(".nythera-backup-")) {
        assert(survivingBackup.empty());
        survivingBackup = entry.path();
      }
    }
    assert(!survivingBackup.empty());
    assert(read(survivingBackup) == "unapproved-1");
    const auto& collideDesc = raceManager.operations().back().resultDescription;
    assert(collideDesc.find("previous destination remains at ") != std::string::npos);
    assert(collideDesc.find(survivingBackup.filename().string()) != std::string::npos);
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
    assert(!waitFor(raceManager, raceManager.copy({finalAppearSource.string()}, raceTo.string())));
    setFileOperationReplaceBackupHookForTesting({});
    assert(injectedFinalAppear);
    assert(read(finalAppearTarget) == "appeared before finalise");
    std::filesystem::path approvedBackup;
    for (const auto& entry : std::filesystem::directory_iterator(raceTo)) {
      const auto fname = entry.path().filename().string();
      assert(!fname.starts_with(".nythera-partial-"));
      if (fname.starts_with(".nythera-backup-")) {
        assert(approvedBackup.empty());
        approvedBackup = entry.path();
      }
    }
    assert(!approvedBackup.empty());
    assert(read(approvedBackup) == "approved backup content");
    const auto& appearDesc = raceManager.operations().back().resultDescription;
    assert(appearDesc.find("previous destination remains at ") != std::string::npos);
    assert(appearDesc.find(approvedBackup.filename().string()) != std::string::npos);
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

    const auto nrCopySource = raceRoot / "nr-copy-source.txt";
    write(nrCopySource, "do not clobber");
    assert(!waitFor(raceManager, raceManager.copy({nrCopySource.string()}, noReplaceDir.string())));
    assert(
        raceManager.operations().back().resultDescription.find("does not support atomic non-overwriting moves")
        != std::string::npos
    );
    assert(!std::filesystem::exists(noReplaceDir / "nr-copy-source.txt"));
    for (const auto& entry : std::filesystem::directory_iterator(noReplaceDir))
      assert(!entry.path().filename().string().starts_with(".nythera-partial-"));

    const auto nrMoveSource = raceRoot / "nr-move-source.txt";
    const auto nrMoveExisting = noReplaceDir / "nr-move-source.txt";
    write(nrMoveSource, "incoming move payload");
    write(nrMoveExisting, "must stay untouched on move");
    assert(!waitFor(raceManager, raceManager.move({nrMoveSource.string()}, noReplaceDir.string())));
    assert(read(nrMoveSource) == "incoming move payload");
    assert(read(nrMoveExisting) == "must stay untouched on move");
    for (const auto& entry : std::filesystem::directory_iterator(noReplaceDir)) {
      const auto fname = entry.path().filename().string();
      assert(!fname.starts_with(".nythera-partial-") && !fname.starts_with(".nythera-backup-"));
    }

    const auto nrRenameSource = noReplaceDir / "nr-rename-src.txt";
    const auto nrRenameAbsentTarget = noReplaceDir / "nr-rename-dst.txt";
    const auto nrRenameExistingTarget = noReplaceDir / "nr-rename-occupied.txt";
    write(nrRenameSource, "rename payload");
    write(nrRenameExistingTarget, "existing rename target");
    assert(!waitFor(raceManager, raceManager.rename(nrRenameSource.string(), nrRenameAbsentTarget.string())));
    assert(read(nrRenameSource) == "rename payload");
    assert(!std::filesystem::exists(nrRenameAbsentTarget));
    assert(!waitFor(raceManager, raceManager.rename(nrRenameSource.string(), nrRenameExistingTarget.string())));
    assert(read(nrRenameSource) == "rename payload");
    assert(read(nrRenameExistingTarget) == "existing rename target");

    simulatedErrno = EOPNOTSUPP;
    assert(!waitFor(raceManager, raceManager.copy({nrCopySource.string()}, noReplaceDir.string())));
    assert(!std::filesystem::exists(noReplaceDir / "nr-copy-source.txt"));
    simulatedErrno = ENOSYS;
    assert(!waitFor(raceManager, raceManager.rename(nrRenameSource.string(), nrRenameAbsentTarget.string())));
    assert(!std::filesystem::exists(nrRenameAbsentTarget));
    simulatedErrno = EINVAL;

    // Cross-filesystem Copy and Move from noReplaceDir into normalDir (which
    // supports RENAME_NOREPLACE) remain safe and succeed via staging.
    const auto crossCopySrc = noReplaceDir / "cross-copy.txt";
    const auto crossMoveSrc = noReplaceDir / "cross-move.txt";
    write(crossCopySrc, "cross copy payload");
    write(crossMoveSrc, "cross move payload");
    assert(waitFor(raceManager, raceManager.copy({crossCopySrc.string()}, normalDir.string())));
    assert(read(normalDir / "cross-copy.txt") == "cross copy payload");
    assert(waitFor(raceManager, raceManager.move({crossMoveSrc.string()}, normalDir.string())));
    assert(read(normalDir / "cross-move.txt") == "cross move payload");
    assert(!std::filesystem::exists(crossMoveSrc));

    // CreateFile, CreateFolder, and CreateSymlink inside noReplaceDir still
    // succeed using their safe atomic kernel creation primitives.
    const auto nrCreatedFile = noReplaceDir / "created.txt";
    const auto nrCreatedDir = noReplaceDir / "created-dir";
    const auto nrCreatedLink = noReplaceDir / "created-link";
    assert(waitFor(raceManager, raceManager.createFile(nrCreatedFile.string())));
    assert(std::filesystem::is_regular_file(nrCreatedFile));
    write(nrCreatedFile, "keep created file");
    assert(!waitFor(raceManager, raceManager.createFile(nrCreatedFile.string())));
    assert(read(nrCreatedFile) == "keep created file");
    assert(waitFor(raceManager, raceManager.createFolder(nrCreatedDir.string())));
    assert(std::filesystem::is_directory(nrCreatedDir));
    assert(waitFor(raceManager, raceManager.createSymlink(nrCreatedFile.string(), nrCreatedLink.string())));
    assert(std::filesystem::is_symlink(std::filesystem::symlink_status(nrCreatedLink)));

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
      assert(copyId != 0);
      while (!enteredCopy.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      transient.reset();
      while (g_main_context_iteration(nullptr, false)) {
      }
      for (const auto& entry : std::filesystem::directory_iterator(f01Dest))
        assert(!entry.path().filename().string().starts_with(".nythera-partial-"));
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
        transient->setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable* cancellable) {
          std::lock_guard lock(localConflictMutex);
          blockedInResolver.store(true, std::memory_order_release);
          while (!g_cancellable_is_cancelled(cancellable))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          resolverExited.store(true, std::memory_order_release);
          return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
        });
        const auto copyId = transient->copy({conflictSrc.string()}, f01Dest.string());
        assert(copyId != 0);
        while (!blockedInResolver.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        transient.reset();
        // shutdown() must have drained the worker before localConflictMutex
        // and resolverExited's lambda owner leave scope.
        assert(resolverExited.load(std::memory_order_acquire));
      }
      while (g_main_context_iteration(nullptr, false)) {
      }
      assert(read(conflictDst) == "existing conflict payload");

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
        assert(uri != nullptr && uriError == nullptr);
        const auto restoreId = transient->restore({uri});
        assert(restoreId != 0);
        while (!blockedInBackend.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        transient.reset();
        assert(backendExited.load(std::memory_order_acquire));
      }
      while (g_main_context_iteration(nullptr, false)) {
      }
      assert(!std::filesystem::exists(f01Dest / "restored-never.txt"));
      assert(read(conflictSrc) == "incoming conflict payload");
    }

    // 3. Destruction while a transfer is paused in FileTransferPauseState::checkpoint.
    {
      const auto pauseDest = f01Root / "pause-dest";
      std::filesystem::create_directory(pauseDest);
      auto transient = std::make_unique<FileOperationManager>();
      std::atomic_bool entered{false};
      std::atomic_bool pausedTriggered{false};
      FileOperationManagerTestAccess::boundary(*transient, [&](std::uint64_t, std::uint64_t) {
        entered.store(true, std::memory_order_release);
        while (!pausedTriggered.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
      });
      const auto pauseId = transient->copy({f01Tree.string()}, pauseDest.string());
      assert(pauseId != 0);
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
        assert(!entry.path().filename().string().starts_with(".nythera-partial-"));
    }

    // 4. Destruction triggered directly from inside the Changed completion callback.
    {
      const auto selfDestructTarget = f01Dest / "created-during-self-destruct.txt";
      std::unique_ptr<FileOperationManager> selfDestructing;
      bool destroyedInCallback = false;
      selfDestructing = std::make_unique<FileOperationManager>([&](const FileOperationManager::Operation& operation) {
        if (operation.finished) {
          assert(operation.succeeded);
          destroyedInCallback = true;
          selfDestructing.reset();
        }
      });
      const auto createId = selfDestructing->createFile(selfDestructTarget.string());
      assert(createId != 0);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!destroyedInCallback && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        if (!destroyedInCallback)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      assert(destroyedInCallback);
      assert(selfDestructing == nullptr);
      assert(std::filesystem::is_regular_file(selfDestructTarget));
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
      assert(chmod(path.c_str(), mode) == 0);
      const std::array<timespec, 2> times{{{seconds, 123456000}, {seconds, 654321000}}};
      assert(utimensat(AT_FDCWD, path.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) == 0);
    };
    const auto inodeOf = [](const std::filesystem::path& path) -> std::uint64_t {
      struct stat state{};
      assert(lstat(path.c_str(), &state) == 0);
      return static_cast<std::uint64_t>(state.st_ino);
    };
    const auto modeOf = [](const std::filesystem::path& path) -> std::uint32_t {
      struct stat state{};
      assert(lstat(path.c_str(), &state) == 0);
      return static_cast<std::uint32_t>(state.st_mode & 07777);
    };
    const auto mtimeOf = [](const std::filesystem::path& path) -> std::pair<std::int64_t, std::int64_t> {
      struct stat state{};
      assert(lstat(path.c_str(), &state) == 0);
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

    assert(waitFor(sameFsManager, sameFsManager.move({movedFileSrc.string()}, sameDstDir.string())));
    assert(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
    assert(!std::filesystem::exists(movedFileSrc));
    assert(read(movedFileDst) == "atomic payload");
    assert(inodeOf(movedFileDst) == fileInodeBefore);
    assert(modeOf(movedFileDst) == 0750);
    assert(mtimeOf(movedFileDst) == fileMtimeBefore);
    assert(sameFsManager.canUndo());
    assert(!sameFsManager.operations().back().undoUnavailable);
    assert(waitFor(sameFsManager, sameFsManager.undo()));
    assert(inodeOf(movedFileSrc) == fileInodeBefore && !std::filesystem::exists(movedFileDst));
    assert(waitFor(sameFsManager, sameFsManager.redo()));
    assert(inodeOf(movedFileDst) == fileInodeBefore && !std::filesystem::exists(movedFileSrc));

    const auto linkTargetFile = sameSrcDir / "target.txt";
    write(linkTargetFile, "symlink target payload");
    const auto validLinkSrc = sameSrcDir / "valid-link";
    const auto danglingLinkSrc = sameSrcDir / "dangling-link";
    std::filesystem::create_symlink(linkTargetFile, validLinkSrc);
    std::filesystem::create_symlink(sameSrcDir / "nonexistent-target", danglingLinkSrc);
    const auto validLinkInode = inodeOf(validLinkSrc);
    const auto danglingLinkInode = inodeOf(danglingLinkSrc);

    assert(waitFor(sameFsManager, sameFsManager.move({validLinkSrc.string()}, sameDstDir.string())));
    assert(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
    assert(inodeOf(sameDstDir / "valid-link") == validLinkInode);
    assert(std::filesystem::read_symlink(sameDstDir / "valid-link") == linkTargetFile);
    assert(waitFor(sameFsManager, sameFsManager.undo()));
    assert(inodeOf(validLinkSrc) == validLinkInode);
    assert(waitFor(sameFsManager, sameFsManager.redo()));
    assert(inodeOf(sameDstDir / "valid-link") == validLinkInode);

    assert(waitFor(sameFsManager, sameFsManager.move({danglingLinkSrc.string()}, sameDstDir.string())));
    assert(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
    assert(inodeOf(sameDstDir / "dangling-link") == danglingLinkInode);
    assert(std::filesystem::read_symlink(sameDstDir / "dangling-link") == sameSrcDir / "nonexistent-target");
    assert(waitFor(sameFsManager, sameFsManager.undo()));
    assert(inodeOf(danglingLinkSrc) == danglingLinkInode);

    const auto movedTreeSrc = sameSrcDir / "atomic-tree";
    const auto movedTreeDst = sameDstDir / "atomic-tree";
    std::filesystem::create_directories(movedTreeSrc / "nested");
    write(movedTreeSrc / "nested" / "child.txt", "nested atomic payload");
    const auto nestedInodeBefore = inodeOf(movedTreeSrc / "nested" / "child.txt");
    setModeAndTime(movedTreeSrc, 0750, 1700002345);
    const auto treeInodeBefore = inodeOf(movedTreeSrc);
    const auto treeMtimeBefore = mtimeOf(movedTreeSrc);
    assert(waitFor(sameFsManager, sameFsManager.move({movedTreeSrc.string()}, sameDstDir.string())));
    assert(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
    assert(!std::filesystem::exists(movedTreeSrc));
    assert(inodeOf(movedTreeDst) == treeInodeBefore);
    assert(inodeOf(movedTreeDst / "nested" / "child.txt") == nestedInodeBefore);
    assert(modeOf(movedTreeDst) == 0750);
    assert(mtimeOf(movedTreeDst) == treeMtimeBefore);
    assert(read(movedTreeDst / "nested" / "child.txt") == "nested atomic payload");
    assert(sameFsManager.operations().back().undoUnavailable);
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
    sameFsManager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    assert(waitFor(sameFsManager, sameFsManager.move({replaceSrc.string()}, sameDstDir.string())));
    assert(!sawStagingOrCopyBytes.load(std::memory_order_relaxed));
    assert(!std::filesystem::exists(replaceSrc));
    assert(read(replaceDst) == "replacement source");
    assert(inodeOf(replaceDst) == replaceSrcInode);
    assert(sameFsManager.operations().back().undoUnavailable);
    for (const auto& entry : std::filesystem::directory_iterator(sameDstDir))
      assert(!entry.path().filename().string().starts_with(".nythera-backup-"));
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
    assert(!waitFor(sameFsManager, sameFsManager.move({moveSwapSrc.string()}, sameDstDir.string())));
    setFileOperationReplaceBackupHookForTesting({});
    assert(swappedMoveBeforeBackup);
    assert(read(moveSwapSrc) == "incoming swapped move");
    assert(read(moveSwapDst) == "unapproved concurrent item");
    assert(read(heldMoveSwap) == "approved replace target");
    assert(
        sameFsManager.operations().back().resultDescription.find("changed before it could be replaced")
        != std::string::npos
    );
    for (const auto& entry : std::filesystem::directory_iterator(sameDstDir))
      assert(!entry.path().filename().string().starts_with(".nythera-backup-"));

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
    assert(!waitFor(sameFsManager, sameFsManager.move({moveAppearSrc.string()}, sameDstDir.string())));
    setFileOperationReplaceBackupHookForTesting({});
    assert(injectedMoveAppear);
    assert(read(moveAppearSrc) == "incoming move appear");
    assert(read(moveAppearDst) == "appeared during same-fs move");
    std::filesystem::path survivingMoveBackup;
    for (const auto& entry : std::filesystem::directory_iterator(sameDstDir)) {
      if (entry.path().filename().string().starts_with(".nythera-backup-")) {
        assert(survivingMoveBackup.empty());
        survivingMoveBackup = entry.path();
      }
    }
    assert(!survivingMoveBackup.empty() && read(survivingMoveBackup) == "approved move backup");
    assert(
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

    assert(
        waitFor(crossManager, crossManager.move({crossMetaFile.string(), crossMetaDir.string()}, crossSimDst.string()))
    );
    assert(!std::filesystem::exists(crossMetaFile) && !std::filesystem::exists(crossMetaDir));
    assert(read(crossSimDst / "cross-file.bin") == "cross payload");
    assert(modeOf(crossSimDst / "cross-file.bin") == 0750);
    assert(mtimeOf(crossSimDst / "cross-file.bin") == crossFileMtime);
    assert(modeOf(crossSimDst / "cross-dir") == 0750);
    assert(mtimeOf(crossSimDst / "cross-dir") == crossDirMtime);
    assert(read(crossSimDst / "cross-dir" / "sub" / "item.txt") == "cross tree item");
    assert(std::filesystem::read_symlink(crossSimDst / "cross-dir" / "dangling") == crossSimSrc / "missing-target");
    assert(crossManager.operations().back().undoUnavailable && !crossManager.canUndo());
    setFileOperationRenameNoReplaceHookForTesting({});

    const std::filesystem::path f03ShmRoot = "/dev/shm/nythera-f03-" + std::to_string(getpid());
    std::error_code f03ShmError;
    if (std::filesystem::create_directory(f03ShmRoot, f03ShmError)) {
      struct stat rootState{};
      struct stat shmState{};
      if (stat(f03Root.c_str(), &rootState) == 0
          && stat(f03ShmRoot.c_str(), &shmState) == 0
          && rootState.st_dev != shmState.st_dev) {
        const auto shmSrcFile = crossSimSrc / "shm-file.bin";
        const auto shmSrcDir = crossSimSrc / "shm-dir";
        const auto shmDst = f03ShmRoot / "dst";
        std::filesystem::create_directory(shmDst);
        write(shmSrcFile, "real cross-dev file");
        setModeAndTime(shmSrcFile, 0750, 1700005555);
        std::filesystem::create_directories(shmSrcDir / "deep");
        write(shmSrcDir / "deep" / "data", "real cross-dev dir");
        std::filesystem::create_symlink(crossSimSrc / "dangling-shm", shmSrcDir / "link");
        setModeAndTime(shmSrcDir, 0750, 1700006666);
        const auto savedDirTime = mtimeOf(shmSrcDir);
        assert(waitFor(crossManager, crossManager.move({shmSrcFile.string(), shmSrcDir.string()}, shmDst.string())));
        assert(!std::filesystem::exists(shmSrcFile) && !std::filesystem::exists(shmSrcDir));
        assert(modeOf(shmDst / "shm-file.bin") == 0750);
        assert(modeOf(shmDst / "shm-dir") == 0750);
        assert(mtimeOf(shmDst / "shm-dir") == savedDirTime);
        assert(read(shmDst / "shm-dir" / "deep" / "data") == "real cross-dev dir");
        assert(std::filesystem::read_symlink(shmDst / "shm-dir" / "link") == crossSimSrc / "dangling-shm");
        assert(crossManager.operations().back().undoUnavailable && !crossManager.canUndo());
      }
      std::filesystem::remove_all(f03ShmRoot);
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
    assert(chmod(preCancelLockedSub.c_str(), 0500) == 0);
    std::mutex preCancelMutex;
    std::condition_variable preCancelCv;
    bool stagedLockedReady = false;
    bool releasePreCancel = false;
    FileOperationManagerTestAccess::boundary(crossManager, [&](std::uint64_t, std::uint64_t) {
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
    const auto preCancelId = crossManager.move({preCancelTree.string()}, crossSimDst.string());
    {
      std::unique_lock lock(preCancelMutex);
      preCancelCv.wait(lock, [&] { return stagedLockedReady; });
    }
    crossManager.cancel(preCancelId);
    {
      std::lock_guard lock(preCancelMutex);
      releasePreCancel = true;
    }
    preCancelCv.notify_all();
    assert(!waitFor(crossManager, preCancelId));
    FileOperationManagerTestAccess::boundary(crossManager, {});
    const auto& preCancelOp = crossManager.operations().back();
    assert(preCancelOp.cancelled && !preCancelOp.succeeded);
    assert(!std::filesystem::exists(crossSimDst / "pre-cancel-tree"));
    for (const auto& entry : std::filesystem::directory_iterator(crossSimDst))
      assert(!entry.path().filename().string().starts_with(".nythera-partial-"));
    assert(read(preCancelLockedSub / "nested.txt") == "nested read-only subfile");
    assert(read(preCancelTree / "sibling.txt") == "sibling payload");
    assert(chmod(preCancelLockedSub.c_str(), 0700) == 0);

    // 4b. Cancellation right after stagePath -> plannedPath commits must leave
    // both source and destination intact and clearly report the split state.
    const auto postCancelSrc = crossSimSrc / "post-commit-cancel.txt";
    const auto postCancelDst = crossSimDst / "post-commit-cancel.txt";
    write(postCancelSrc, "survive both on late cancel");
    std::mutex postCancelMutex;
    std::condition_variable postCancelCv;
    bool postCommitReached = false;
    bool releasePostCancel = false;
    setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
      const bool directCross = sourcePath != nullptr
          && destinationPath != nullptr
          && sourcePath == postCancelSrc
          && destinationPath == postCancelDst;
      if (directCross) {
        errno = EXDEV;
        return -1;
      }
      const int rc =
          static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE));
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
    const auto postCancelId = crossManager.move({postCancelSrc.string()}, crossSimDst.string());
    {
      std::unique_lock lock(postCancelMutex);
      postCancelCv.wait(lock, [&] { return postCommitReached; });
    }
    crossManager.cancel(postCancelId);
    {
      std::lock_guard lock(postCancelMutex);
      releasePostCancel = true;
    }
    postCancelCv.notify_all();
    assert(!waitFor(crossManager, postCancelId));
    setFileOperationRenameNoReplaceHookForTesting({});
    const auto& postCancelOp = crossManager.operations().back();
    assert(postCancelOp.cancelled && !postCancelOp.succeeded && postCancelOp.undoUnavailable);
    assert(read(postCancelSrc) == "survive both on late cancel");
    assert(read(postCancelDst) == "survive both on late cancel");
    assert(postCancelOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
    assert(postCancelOp.resultDescription.find(postCancelDst.string()) != std::string::npos);
    assert(postCancelOp.resultDescription.find("source still exists at ") != std::string::npos);
    assert(postCancelOp.resultDescription.find(postCancelSrc.string()) != std::string::npos);
    assert(postCancelOp.resultDescription.find("cancelled before source cleanup finished") != std::string::npos);
    assert(postCancelOp.status.find(postCancelOp.resultDescription) == 0);

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
      assert(chmod(lockedChildDir.c_str(), 0500) == 0);

      assert(!waitFor(crossManager, crossManager.move({lockedRootSrc.string()}, crossSimDst.string())));
      const auto& lockedFailOp = crossManager.operations().back();
      assert(!lockedFailOp.succeeded && !lockedFailOp.cancelled && lockedFailOp.undoUnavailable);
      assert(read(crossSimDst / "unremovable-tree" / "locked" / "keep.txt") == "cannot unlink source child");
      assert(read(lockedChildDir / "keep.txt") == "cannot unlink source child");
      assert(lockedFailOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
      assert(lockedFailOp.resultDescription.find((crossSimDst / "unremovable-tree").string()) != std::string::npos);
      assert(lockedFailOp.resultDescription.find("source still exists at ") != std::string::npos);
      assert(lockedFailOp.resultDescription.find(lockedRootSrc.string()) != std::string::npos);
      assert(lockedFailOp.status.find(lockedFailOp.resultDescription) == 0);
      assert(chmod(lockedChildDir.c_str(), 0700) == 0);
      assert(chmod((crossSimDst / "unremovable-tree" / "locked").c_str(), 0700) == 0);

      const auto lockedParent = crossSimSrc / "locked-parent";
      std::filesystem::create_directory(lockedParent);
      const auto replaceLockedSrc = lockedParent / "replace-fail.txt";
      const auto replaceLockedDst = crossSimDst / "replace-fail.txt";
      write(replaceLockedSrc, "new moved copy");
      write(replaceLockedDst, "previous target backup");
      assert(chmod(lockedParent.c_str(), 0500) == 0);
      crossManager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      assert(!waitFor(crossManager, crossManager.move({replaceLockedSrc.string()}, crossSimDst.string())));
      crossManager.setConflictResolver({});
      setFileOperationRenameNoReplaceHookForTesting({});
      const auto& replaceLockedOp = crossManager.operations().back();
      assert(!replaceLockedOp.succeeded && !replaceLockedOp.cancelled && replaceLockedOp.undoUnavailable);
      assert(read(replaceLockedDst) == "new moved copy");
      assert(read(replaceLockedSrc) == "new moved copy");
      std::filesystem::path preservedBackup;
      for (const auto& entry : std::filesystem::directory_iterator(crossSimDst)) {
        if (entry.path().filename().string().starts_with(".nythera-backup-")) {
          assert(preservedBackup.empty());
          preservedBackup = entry.path();
        }
      }
      assert(!preservedBackup.empty() && read(preservedBackup) == "previous target backup");
      assert(replaceLockedOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
      assert(replaceLockedOp.resultDescription.find(replaceLockedDst.string()) != std::string::npos);
      assert(replaceLockedOp.resultDescription.find("source still exists at ") != std::string::npos);
      assert(replaceLockedOp.resultDescription.find(replaceLockedSrc.string()) != std::string::npos);
      assert(
          replaceLockedOp.resultDescription.find("previous destination backup remains at " + preservedBackup.string())
          != std::string::npos
      );
      assert(replaceLockedOp.status.find(replaceLockedOp.resultDescription) == 0);
      assert(chmod(lockedParent.c_str(), 0700) == 0);
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
      assert(!waitFor(faultManager, faultManager.move({faultFileSrc.string()}, faultDst.string())));
      setFileOperationRemoveEntryHookForTesting({});
      const auto& faultFileOp = faultManager.operations().back();
      assert(!faultFileOp.succeeded && !faultFileOp.cancelled && faultFileOp.mutated && faultFileOp.undoUnavailable);
      assert(faultFileOp.completedEntries.empty());
      assert(read(faultFileDst) == "injected cross-fs payload");
      assert(read(faultFileSrc) == "injected cross-fs payload");
      assert(faultFileOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
      assert(faultFileOp.resultDescription.find(faultFileDst.string()) != std::string::npos);
      assert(faultFileOp.resultDescription.find("source still exists at ") != std::string::npos);
      assert(faultFileOp.resultDescription.find(faultFileSrc.string()) != std::string::npos);
      assert(faultFileOp.status.find(faultFileOp.resultDescription) == 0);

      // 6b. Cross-filesystem directory move: destination succeeds, nested source deletion fails with EACCES.
      const auto faultDirSrc = faultSrc / "injected-dir";
      const auto faultDirDst = faultDst / "injected-dir";
      std::filesystem::create_directories(faultDirSrc / "sub");
      write(faultDirSrc / "sub" / "stuck.txt", "stuck nested source");
      setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
        return path != nullptr && path == (faultDirSrc / "sub" / "stuck.txt") ? EACCES : 0;
      });
      assert(!waitFor(faultManager, faultManager.move({faultDirSrc.string()}, faultDst.string())));
      setFileOperationRemoveEntryHookForTesting({});
      const auto& faultDirOp = faultManager.operations().back();
      assert(!faultDirOp.succeeded && !faultDirOp.cancelled && faultDirOp.mutated && faultDirOp.undoUnavailable);
      assert(read(faultDirDst / "sub" / "stuck.txt") == "stuck nested source");
      assert(read(faultDirSrc / "sub" / "stuck.txt") == "stuck nested source");
      assert(faultDirOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
      assert(faultDirOp.resultDescription.find(faultDirDst.string()) != std::string::npos);
      assert(faultDirOp.resultDescription.find("source still exists at ") != std::string::npos);
      assert(faultDirOp.resultDescription.find(faultDirSrc.string()) != std::string::npos);

      // 6c. Cross-filesystem Replace move: destination succeeds, backup preserved, source unlink fails with EIO.
      const auto faultReplaceSrc = faultSrc / "injected-replace.txt";
      const auto faultReplaceDst = faultDst / "injected-replace.txt";
      write(faultReplaceSrc, "injected replacement copy");
      write(faultReplaceDst, "injected original backup");
      faultManager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
      });
      setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
        return path != nullptr && path == faultReplaceSrc ? EIO : 0;
      });
      assert(!waitFor(faultManager, faultManager.move({faultReplaceSrc.string()}, faultDst.string())));
      setFileOperationRemoveEntryHookForTesting({});
      faultManager.setConflictResolver({});
      const auto& faultReplaceOp = faultManager.operations().back();
      assert(
          !faultReplaceOp.succeeded
          && !faultReplaceOp.cancelled
          && faultReplaceOp.mutated
          && faultReplaceOp.undoUnavailable
      );
      assert(read(faultReplaceDst) == "injected replacement copy");
      assert(read(faultReplaceSrc) == "injected replacement copy");
      std::filesystem::path faultBackup;
      for (const auto& entry : std::filesystem::directory_iterator(faultDst)) {
        if (entry.path().filename().string().starts_with(".nythera-backup-")) {
          assert(faultBackup.empty());
          faultBackup = entry.path();
        }
      }
      assert(!faultBackup.empty() && read(faultBackup) == "injected original backup");
      assert(faultReplaceOp.resultDescription.find("Moved copy remains at ") != std::string::npos);
      assert(faultReplaceOp.resultDescription.find(faultReplaceDst.string()) != std::string::npos);
      assert(faultReplaceOp.resultDescription.find("source still exists at ") != std::string::npos);
      assert(faultReplaceOp.resultDescription.find(faultReplaceSrc.string()) != std::string::npos);
      assert(
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
      faultManager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
        return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Merge, false};
      });
      setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
        return path != nullptr && path == mergeFailSrc ? EIO : 0;
      });
      assert(!waitFor(faultManager, faultManager.move({mergeFailSrc.string()}, faultDst.string())));
      setFileOperationRemoveEntryHookForTesting({});
      faultManager.setConflictResolver({});
      const auto& mergeFailOp = faultManager.operations().back();
      assert(!mergeFailOp.succeeded && !mergeFailOp.cancelled && mergeFailOp.mutated);
      assert(mergeFailOp.completedEntries.empty());
      assert(read(mergeFailDst / "moved-child.txt") == "merged child payload");
      assert(read(mergeFailDst / "existing-child.txt") == "preserved destination child");
      assert(std::filesystem::is_directory(mergeFailSrc) && std::filesystem::is_empty(mergeFailSrc));
      assert(mergeFailOp.resultDescription.find("Merged items remain at ") != std::string::npos);
      assert(mergeFailOp.resultDescription.find(mergeFailDst.string()) != std::string::npos);
      assert(mergeFailOp.resultDescription.find("source directory still exists at ") != std::string::npos);
      assert(mergeFailOp.resultDescription.find(mergeFailSrc.string()) != std::string::npos);
      assert(mergeFailOp.status.find(mergeFailOp.resultDescription) == 0);

      // 6e. Non-mutating failed/cancelled move reports mutated == false.
      const auto noMutateSrc = faultSrc / "no-mutate.txt";
      const auto noMutateDst = faultDst / "no-mutate.txt";
      write(noMutateSrc, "source unchanged");
      write(noMutateDst, "destination unchanged");
      assert(!waitFor(faultManager, faultManager.move({noMutateSrc.string()}, faultDst.string())));
      const auto& noMutateOp = faultManager.operations().back();
      assert(!noMutateOp.succeeded && noMutateOp.cancelled && !noMutateOp.mutated);
      assert(read(noMutateSrc) == "source unchanged" && read(noMutateDst) == "destination unchanged");
    }
  }

  std::filesystem::remove_all(root);
  return 0;
}
