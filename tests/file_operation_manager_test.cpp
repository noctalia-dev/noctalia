#include "filemanager/file_operation_manager.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  struct Wait { GMainLoop* loop = nullptr; FileOperationManager* manager = nullptr; std::uint64_t id = 0; bool done = false; bool ok = false; };
  gboolean poll(gpointer data) {
    auto* wait = static_cast<Wait*>(data);
    for (const auto& operation : wait->manager->operations()) {
      if (operation.id == wait->id && operation.finished) {
        wait->done = true; wait->ok = operation.succeeded; g_main_loop_quit(wait->loop); return G_SOURCE_REMOVE;
      }
    }
    return G_SOURCE_CONTINUE;
  }
  bool waitFor(FileOperationManager& manager, std::uint64_t id) {
    assert(id != 0);
    Wait state{.loop=g_main_loop_new(nullptr, false), .manager=&manager, .id=id};
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
  { std::ofstream file(from / "nested" / "payload.txt"); file << "synthetic test payload"; }

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
  { std::ifstream existing(to / "nested" / "payload.txt"); std::string contents; std::getline(existing, contents); assert(contents == "synthetic test payload"); }
  assert(!waitFor(manager, manager.copy({from.string()}, (from / "nested").string())));
  const auto invalidCopy = std::ranges::find_if(manager.operations(), [](const auto& operation) {
    return operation.finished && !operation.succeeded && operation.status.find("into itself") != std::string::npos;
  });
  assert(invalidCopy != manager.operations().end());
  assert(invalidCopy->resultTitle == "Copy failed");
  assert(invalidCopy->resultDescription == "Cannot copy \"from\" into itself");
  const auto batchSourceA = root / "batch-a.txt";
  const auto batchSourceB = root / "batch-b.txt";
  { std::ofstream file(batchSourceA); file << "batch a"; }
  { std::ofstream file(batchSourceB); file << "batch b"; }
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
  { std::ofstream file(mountA / "first.txt"); file << "first"; }
  { std::ofstream file(mountB / "second.txt"); file << "second"; }
  { std::ofstream file(mountDestination / "first.txt"); file << "existing"; }
  std::atomic_bool safetyConflictReached{false};
  std::atomic_bool releaseSafetyWorker{false};
  manager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
    safetyConflictReached.store(true, std::memory_order_release);
    while (!releaseSafetyWorker.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto protectedId = manager.copy({(mountA / "first.txt").string(), (mountB / "second.txt").string()}, mountDestination.string());
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
    while (g_main_context_iteration(nullptr, false)) {}
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
  assert(waitFor(manager, manager.rename((root / "created" / "empty").string(), (root / "created" / "renamed").string())));
  assert(std::filesystem::exists(root / "created" / "renamed"));
  const auto incoming = root / "incoming.txt";
  const auto existing = to / "incoming.txt";
  { std::ofstream file(incoming); file << "replacement"; }
  { std::ofstream file(existing); file << "preserve"; }
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
  });
  assert(waitFor(manager, manager.copy({incoming.string()}, to.string())));
  { std::ifstream file(existing); std::string value; file >> value; assert(value == "replacement"); }
  { std::ofstream file(existing); file << "keep"; }
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, false};
  });
  assert(waitFor(manager, manager.copy({incoming.string()}, to.string())));
  { std::ifstream file(existing); std::string value; file >> value; assert(value == "keep"); }
  int selfConflictCount = 0;
  manager.setConflictResolver([&selfConflictCount](const std::string& source, const std::string& destination, bool, GCancellable*) {
    assert(source == destination);
    ++selfConflictCount;
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
  });
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
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false, "chosen-name.txt"};
  });
  assert(waitFor(manager, manager.copy({incoming.string()}, to.string())));
  assert(std::filesystem::exists(to / "chosen-name.txt"));
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false, "../escape.txt"};
  });
  assert(!waitFor(manager, manager.copy({incoming.string()}, to.string())));
  assert(!std::filesystem::exists(root / "escape.txt"));
  { std::ofstream file(to / "occupied-name.txt"); file << "must stay"; }
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false, "occupied-name.txt"};
  });
  assert(!waitFor(manager, manager.copy({incoming.string()}, to.string())));
  { std::ifstream file(to / "occupied-name.txt"); std::string value; file >> value; assert(value == "must"); }
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto cancelledConflictId = manager.copy({incoming.string()}, to.string());
  assert(!waitFor(manager, cancelledConflictId));
  const auto cancelledConflict = std::ranges::find(manager.operations(), cancelledConflictId, &FileOperationManager::Operation::id);
  assert(cancelledConflict->cancelled && cancelledConflict->resultTitle == "Copy cancelled");
  assert(cancelledConflict->completedEntries.empty());
  { std::ifstream file(existing); std::string value; file >> value; assert(value == "keep"); }

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
  { std::ofstream file(identityFirstSource); file << "first copy"; }
  { std::ofstream file(identityConflictSource); file << "cancel after replacement"; }
  { std::ofstream file(to / identityConflictSource.filename()); file << "existing conflict"; }
  bool changedEarlierDestination = false;
  manager.setConflictResolver([&](const std::string&, const std::string&, bool, GCancellable*) {
    std::filesystem::rename(identityFirstDestination, heldOriginalDestination);
    { std::ofstream file(identityFirstDestination); file << "unrelated replacement"; }
    changedEarlierDestination = true;
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto replacedPriorId = manager.copy({identityFirstSource.string(), identityConflictSource.string()}, to.string());
  assert(!waitFor(manager, replacedPriorId));
  assert(changedEarlierDestination);
  const auto replacedPrior = std::ranges::find(manager.operations(), replacedPriorId, &FileOperationManager::Operation::id);
  assert(replacedPrior->cancelled && replacedPrior->completedEntries.empty());
  { std::ifstream file(heldOriginalDestination); std::string value; file >> value; assert(value == "first"); }
  { std::ifstream file(identityFirstDestination); std::string value; file >> value; assert(value == "unrelated"); }
  std::atomic_bool conflictReached{false};
  manager.setConflictResolver([&conflictReached](const std::string&, const std::string&, bool, GCancellable* cancellable) {
    conflictReached.store(true, std::memory_order_release);
    while (!g_cancellable_is_cancelled(cancellable)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto cancelledReplaceId = manager.copy({incoming.string()}, to.string());
  while (!conflictReached.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  manager.cancel(cancelledReplaceId);
  assert(!waitFor(manager, cancelledReplaceId));
  const auto cancelledReplace = std::ranges::find(manager.operations(), cancelledReplaceId, &FileOperationManager::Operation::id);
  assert(cancelledReplace->cancelled && cancelledReplace->resultTitle == "Copy cancelled");
  assert(cancelledReplace->resultDescription.starts_with("Transfer to "));
  assert(std::filesystem::exists(incoming));
  { std::ifstream file(existing); std::string value; file >> value; assert(value == "keep"); }
  for (const auto& entry : std::filesystem::directory_iterator(to))
    assert(!entry.path().filename().string().starts_with(".nythera-partial-"));
  const auto allA = root / "all-a.txt";
  const auto allB = root / "all-b.txt";
  { std::ofstream file(allA); file << "new a"; }
  { std::ofstream file(allB); file << "new b"; }
  { std::ofstream file(to / "all-a.txt"); file << "old a"; }
  { std::ofstream file(to / "all-b.txt"); file << "old b"; }
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
  { std::ifstream file(to / "all-a.txt"); std::string value; file >> value; assert(value == "old"); }
  { std::ifstream file(to / "all-b.txt"); std::string value; file >> value; assert(value == "old"); }
  const auto mixedFile = root / "mixed-conflict.txt";
  const auto mixedFileAfterFolder = root / "mixed-after-folder.txt";
  const auto mixedFolder = root / "mixed-conflict-folder";
  { std::ofstream file(mixedFile); file << "incoming"; }
  { std::ofstream file(mixedFileAfterFolder); file << "incoming later"; }
  { std::ofstream file(to / mixedFile.filename()); file << "existing"; }
  { std::ofstream file(to / mixedFileAfterFolder.filename()); file << "existing later"; }
  std::filesystem::create_directory(mixedFolder);
  { std::ofstream file(mixedFolder / "payload.txt"); file << "incoming payload"; }
  std::filesystem::create_directory(to / mixedFolder.filename());
  { std::ofstream file(to / mixedFolder.filename() / "payload.txt"); file << "existing payload"; }
  int mixedConflictCount = 0;
  manager.setConflictResolver([&mixedConflictCount](const std::string&, const std::string&, bool, GCancellable*) {
    ++mixedConflictCount;
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, mixedConflictCount == 1};
  });
  assert(waitFor(manager, manager.copy({mixedFile.string(), mixedFolder.string(), mixedFileAfterFolder.string()}, to.string())));
  assert(mixedConflictCount == 2);
  { std::ifstream file(to / mixedFile.filename()); std::string value; file >> value; assert(value == "existing"); }
  { std::ifstream file(to / mixedFolder.filename() / "payload.txt"); std::string value; file >> value; assert(value == "existing"); }
  { std::ifstream file(to / mixedFileAfterFolder.filename()); std::string value; file >> value; assert(value == "existing"); }
  const auto collidingDirectory = root / "colliding-folder";
  std::filesystem::create_directory(collidingDirectory);
  { std::ofstream file(collidingDirectory / "incoming.txt"); file << "incoming folder payload"; }
  std::filesystem::create_directory(to / collidingDirectory.filename());
  { std::ofstream file(to / collidingDirectory.filename() / "existing.txt"); file << "existing folder payload"; }
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
  auto write = [](const auto& path, const std::string& text) { std::ofstream file(path); file << text; };
  auto read = [](const auto& path) { std::ifstream file(path); return std::string(std::istreambuf_iterator<char>(file), {}); };
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
    if (name == "tree") { ++merges; return FileOperationManager::ConflictDecision{Action::Merge, true}; }
    if (name == "replace.txt") return FileOperationManager::ConflictDecision{Action::Replace, false};
    if (name == "both.txt") return FileOperationManager::ConflictDecision{Action::Rename, false, "chosen.txt"};
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
    if (std::filesystem::path(source).filename() == "tree") return FileOperationManager::ConflictDecision{Action::Merge, false};
    g_cancellable_cancel(cancellable);
    return FileOperationManager::ConflictDecision{Action::Cancel, false};
  });
  const auto beforeCancel = root / "before-cancel.txt";
  write(beforeCancel, "completed before cancellation");
  const auto cancelledMerge = manager.move({beforeCancel.string(), mergeSource.string()}, mergeParent.string());
  assert(!waitFor(manager, cancelledMerge));
  const auto cancelledMergeResult = std::ranges::find(manager.operations(), cancelledMerge, &FileOperationManager::Operation::id);
  assert(cancelledMergeResult->cancelled && !cancelledMergeResult->succeeded);
  assert(cancelledMergeResult->completedEntries.size() == 1);
  assertCompletedEntry(*cancelledMergeResult, 0, mergeParent / "before-cancel.txt");
  assert(read(mergeParent / "before-cancel.txt") == "completed before cancellation");
  assert(std::filesystem::exists(mergeSource / "nested" / "replace.txt"));
  assert(read(mergeTarget / "nested" / "replace.txt") == "new");

  // A failed nested decision cannot leave a partially completed merge marked successful.
  manager.setConflictResolver([](const std::string& source, const std::string&, bool, GCancellable*) {
    using Action = FileOperationManager::ConflictAction;
    if (std::filesystem::path(source).filename() == "tree") return FileOperationManager::ConflictDecision{Action::Merge, true};
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
    return FileOperationManager::ConflictDecision{std::filesystem::path(source).filename() == "tree" ? Action::Merge : Action::Skip, false};
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
  { std::ofstream file(moveSource); file << "move fixture"; }
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
    { std::ofstream file(crossSource); file << "cross-device move fixture"; }
    if (stat(root.c_str(), &sourceStat) == 0 && stat(sharedMemoryRoot.c_str(), &targetStat) == 0
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
    assert(waitFor(manager, manager.rename((linkCopy / name).string(), (linkCopy / (std::string(name) + "-renamed")).string())));
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
  { std::ofstream file(trashSource); file << "trash fixture"; }
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
    return manager.renameChecked(inlineSource.string(), destination.string(),
        inlineIdentity.st_dev, inlineIdentity.st_ino, {});
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
    assert(waitFor(duplicate, duplicate.duplicate({folder.string(), manualLink.string(), ordinary.string()}, edit.string())));
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

  std::filesystem::remove_all(root);
  return 0;
}
