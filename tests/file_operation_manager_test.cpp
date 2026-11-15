#include "filemanager/file_operation_manager.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <string>
#include <cstdlib>
#include <thread>
#include <sys/stat.h>
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
    Wait state{.loop=g_main_loop_new(nullptr, false), .manager=&manager, .id=id};
    const guint timer = g_timeout_add(10, poll, &state);
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return state.ok;
  }
}

int main() {
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
  assert(waitFor(manager, manager.copy({batchSourceA.string(), batchSourceB.string()}, to.string())));
  assert(std::filesystem::exists(to / "batch-a.txt") && std::filesystem::exists(to / "batch-b.txt"));
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
  assert(waitFor(manager, manager.copy({incoming.string()}, to.string())));
  assert(std::filesystem::exists(to / "incoming (copy).txt"));
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
  { std::ifstream file(existing); std::string value; file >> value; assert(value == "keep"); }
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
  assert(waitFor(manager, manager.copy({allA.string(), allB.string()}, to.string())));
  assert(conflictCount == 1);
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
    }
    std::filesystem::remove_all(sharedMemoryRoot);
  }
  const auto trashSource = root / "trash-fixture.txt";
  { std::ofstream file(trashSource); file << "trash fixture"; }
  assert(waitFor(manager, manager.trash({trashSource.string()})));
  assert(!std::filesystem::exists(trashSource));
  assert(waitFor(manager, manager.removePermanently({(root / "created").string()})));
  std::filesystem::remove_all(root);
  return 0;
}
