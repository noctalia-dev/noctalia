#include "filemanager/file_operation_manager.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  struct WaitState {
    GMainLoop* loop = nullptr;
    FileOperationManager* manager = nullptr;
    std::uint64_t id = 0;
    bool succeeded = false;
  };

  gboolean pollFinished(gpointer data) {
    auto* state = static_cast<WaitState*>(data);
    const auto found = std::ranges::find(state->manager->operations(), state->id, &FileOperationManager::Operation::id);
    if (found == state->manager->operations().end() || !found->finished)
      return G_SOURCE_CONTINUE;
    state->succeeded = found->succeeded;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }

  bool waitFor(FileOperationManager& manager, std::uint64_t id) {
    assert(id != 0);
    WaitState state{.loop = g_main_loop_new(nullptr, false), .manager = &manager, .id = id};
    g_timeout_add(5, pollFinished, &state);
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    return state.succeeded;
  }

  struct Fixture {
    std::filesystem::path root;
    std::map<std::string, std::string> originals;
    std::vector<std::string> entries;
    unsigned enumerateCalls = 0;
    unsigned metadataCalls = 0;

    std::string uriFor(const std::filesystem::path& path) const {
      GError* error = nullptr;
      char* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
      assert(uri != nullptr && error == nullptr);
      std::string result(uri);
      g_free(uri);
      return result;
    }

    void add(const std::filesystem::path& source, const std::filesystem::path& original) {
      const auto uri = uriFor(source);
      entries.push_back(uri);
      originals.emplace(uri, original.string());
    }

    FileOperationManager::WastebinBackend backend() {
      return {
          .enumerateTopLevel =
              [this](GCancellable*, std::vector<std::string>& uris, std::string&) {
                ++enumerateCalls;
                uris = entries;
                return true;
              },
          .originalPath =
              [this](const std::string& uri, GCancellable*, std::string& original, std::string& message) {
                ++metadataCalls;
                const auto found = originals.find(uri);
                if (found == originals.end()) {
                  message = "Synthetic original-location metadata is unavailable.";
                  return false;
                }
                original = found->second;
                return true;
              },
      };
    }
  };

  void write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << contents;
    assert(file.good());
  }

  std::string read(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  const FileOperationManager::Operation& operation(FileOperationManager& manager, std::uint64_t id) {
    const auto found = std::ranges::find(manager.operations(), id, &FileOperationManager::Operation::id);
    assert(found != manager.operations().end() && found->finished);
    return *found;
  }
} // namespace

int main() {
  setenv("GIO_USE_VFS", "local", 1);
  std::string rootTemplate = "/tmp/nythera-wastebin-test-XXXXXX";
  char* rootRaw = mkdtemp(rootTemplate.data());
  assert(rootRaw != nullptr);
  const std::filesystem::path root(rootRaw);
  const auto isolatedData = root / "user-data";
  const auto isolatedCache = root / "user-cache";
  std::filesystem::create_directories(isolatedData);
  std::filesystem::create_directories(isolatedCache);
  setenv("XDG_DATA_HOME", isolatedData.c_str(), 1);
  setenv("XDG_CACHE_HOME", isolatedCache.c_str(), 1);

  Fixture fixture{.root = root};
  std::vector<std::uint64_t> finishedCallbacks;
  FileOperationManager manager(
      [&](const FileOperationManager::Operation& item) {
        if (item.finished)
          finishedCallbacks.push_back(item.id);
      },
      fixture.backend()
  );

  // Default restore destinations come from original-location metadata.
  const auto originalDirectory = root / "original";
  const auto fileEntry = root / "fixture-trash" / "plain.txt";
  write(fileEntry, "plain payload");
  std::filesystem::create_directories(originalDirectory);
  const auto fileUri = fixture.uriFor(fileEntry);
  fixture.originals[fileUri] = (originalDirectory / "plain.txt").string();
  const auto originalRestoreId = manager.restore({fileUri});
  assert(waitFor(manager, originalRestoreId));
  assert(!std::filesystem::exists(fileEntry));
  assert(read(originalDirectory / "plain.txt") == "plain payload");
  assert(operation(manager, originalRestoreId).kind == FileOperationManager::Kind::Restore);

  // Explicit destination applies to every selected item and preserves file,
  // directory, and symlink entries as their original filesystem objects.
  const auto selectedDestination = root / "selected-destination";
  std::filesystem::create_directory(selectedDestination);
  const auto batchRoot = root / "fixture-trash";
  const auto batchFile = batchRoot / "batch.txt";
  const auto batchDirectory = batchRoot / "tree";
  const auto linkTarget = root / "link-target.txt";
  const auto batchLink = batchRoot / "link.txt";
  write(batchFile, "batch payload");
  write(batchDirectory / "nested" / "inside.txt", "nested payload");
  write(linkTarget, "link target");
  std::filesystem::create_symlink(linkTarget, batchLink);
  const auto batchFileUri = fixture.uriFor(batchFile);
  const auto batchDirectoryUri = fixture.uriFor(batchDirectory);
  const auto batchLinkUri = fixture.uriFor(batchLink);
  fixture.originals[batchFileUri] = (root / "unused-original" / "batch.txt").string();
  fixture.originals[batchDirectoryUri] = (root / "unused-original" / "tree").string();
  fixture.originals[batchLinkUri] = (root / "unused-original" / "link.txt").string();
  const auto selectedRestoreId =
      manager.restore({batchFileUri, batchDirectoryUri, batchLinkUri}, selectedDestination.string());
  assert(waitFor(manager, selectedRestoreId));
  assert(read(selectedDestination / "batch.txt") == "batch payload");
  assert(read(selectedDestination / "tree" / "nested" / "inside.txt") == "nested payload");
  assert(std::filesystem::is_symlink(std::filesystem::symlink_status(selectedDestination / "link.txt")));
  assert(std::filesystem::read_symlink(selectedDestination / "link.txt") == linkTarget);
  assert(read(linkTarget) == "link target");
  assert(operation(manager, selectedRestoreId).requestedItemCount == 3);

  // A cancelled conflict keeps both entries intact. Replace moves the wastebin
  // item into place and removes the old destination only after success.
  const auto conflictEntry = batchRoot / "collision.txt";
  const auto collisionDestination = root / "collision-destination";
  std::filesystem::create_directory(collisionDestination);
  write(conflictEntry, "replacement");
  write(collisionDestination / "collision.txt", "keep");
  const auto conflictUri = fixture.uriFor(conflictEntry);
  fixture.originals[conflictUri] = (root / "unused-original" / "collision.txt").string();
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
  });
  const auto cancelledId = manager.restore({conflictUri}, collisionDestination.string());
  assert(!waitFor(manager, cancelledId));
  assert(operation(manager, cancelledId).cancelled);
  assert(std::filesystem::exists(conflictEntry));
  assert(read(collisionDestination / "collision.txt") == "keep");

  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
  });
  const auto replaceId = manager.restore({conflictUri}, collisionDestination.string());
  assert(waitFor(manager, replaceId));
  assert(!std::filesystem::exists(conflictEntry));
  assert(read(collisionDestination / "collision.txt") == "replacement");

  const auto renameEntry = batchRoot / "rename-collision.txt";
  write(renameEntry, "renamed wastebin item");
  write(collisionDestination / "rename-collision.txt", "keep existing");
  const auto renameUri = fixture.uriFor(renameEntry);
  fixture.originals[renameUri] = (root / "unused-original" / "rename-collision.txt").string();
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
  });
  const auto renameId = manager.restore({renameUri}, collisionDestination.string());
  assert(waitFor(manager, renameId));
  assert(read(collisionDestination / "rename-collision.txt") == "keep existing");
  assert(read(collisionDestination / "rename-collision (restored).txt") == "renamed wastebin item");

  const auto skipEntry = batchRoot / "skip-collision.txt";
  write(skipEntry, "keep in wastebin");
  write(collisionDestination / "skip-collision.txt", "keep destination");
  const auto skipUri = fixture.uriFor(skipEntry);
  fixture.originals[skipUri] = (root / "unused-original" / "skip-collision.txt").string();
  manager.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, false};
  });
  const auto skipId = manager.restore({skipUri}, collisionDestination.string());
  assert(waitFor(manager, skipId));
  assert(std::filesystem::exists(skipEntry));
  assert(read(collisionDestination / "skip-collision.txt") == "keep destination");

  const auto applyAllA = batchRoot / "apply-all-a.txt";
  const auto applyAllB = batchRoot / "apply-all-b.txt";
  write(applyAllA, "new a");
  write(applyAllB, "new b");
  write(collisionDestination / "apply-all-a.txt", "old a");
  write(collisionDestination / "apply-all-b.txt", "old b");
  const auto applyAllAUri = fixture.uriFor(applyAllA);
  const auto applyAllBUri = fixture.uriFor(applyAllB);
  fixture.originals[applyAllAUri] = (root / "unused-original" / "apply-all-a.txt").string();
  fixture.originals[applyAllBUri] = (root / "unused-original" / "apply-all-b.txt").string();
  unsigned applyAllResolverCalls = 0;
  manager.setConflictResolver([&](const std::string&, const std::string&, bool moreItems, GCancellable*) {
    ++applyAllResolverCalls;
    assert(moreItems);
    return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, true};
  });
  const auto applyAllId = manager.restore({applyAllAUri, applyAllBUri}, collisionDestination.string());
  assert(waitFor(manager, applyAllId));
  assert(applyAllResolverCalls == 1);
  assert(read(collisionDestination / "apply-all-a.txt") == "new a");
  assert(read(collisionDestination / "apply-all-b.txt") == "new b");
  manager.setConflictResolver({});

  // Missing metadata, a missing source, and an absent parent are reported as
  // failed jobs without manufacturing or overwriting destination files.
  const auto noMetadataEntry = batchRoot / "no-metadata.txt";
  write(noMetadataEntry, "source");
  const auto noMetadataUri = fixture.uriFor(noMetadataEntry);
  fixture.originals.erase(noMetadataUri);
  const auto noMetadataId = manager.restore({noMetadataUri});
  assert(!waitFor(manager, noMetadataId));
  assert(std::filesystem::exists(noMetadataEntry));

  const auto missingParentEntry = batchRoot / "missing-parent.txt";
  write(missingParentEntry, "missing parent source");
  const auto missingParentUri = fixture.uriFor(missingParentEntry);
  fixture.originals[missingParentUri] = (root / "absent-parent" / "missing-parent.txt").string();
  const auto missingParentId = manager.restore({missingParentUri});
  assert(!waitFor(manager, missingParentId));
  assert(read(missingParentEntry) == "missing parent source");

  const auto missingSourceEntry = batchRoot / "missing-source.txt";
  const auto missingSourceUri = fixture.uriFor(missingSourceEntry);
  fixture.originals[missingSourceUri] = (originalDirectory / "missing-source.txt").string();
  const auto missingSourceId = manager.restore({missingSourceUri});
  assert(!waitFor(manager, missingSourceId));
  assert(!std::filesystem::exists(originalDirectory / "missing-source.txt"));

  const auto invalidUriId = manager.restore({"trash:///not-a-local-fixture"});
  assert(!waitFor(manager, invalidUriId));

  // Batch restore reports a partial failure while preserving successful work.
  const auto partialEntry = batchRoot / "partial-success.txt";
  write(partialEntry, "partial");
  const auto partialUri = fixture.uriFor(partialEntry);
  const auto neverThereUri = fixture.uriFor(batchRoot / "not-present.txt");
  fixture.originals[partialUri] = (root / "partial-destination" / "partial-success.txt").string();
  fixture.originals[neverThereUri] = (root / "partial-destination" / "not-present.txt").string();
  std::filesystem::create_directory(root / "partial-destination");
  const auto partialId = manager.restore({partialUri, neverThereUri}, (root / "partial-destination").string());
  assert(!waitFor(manager, partialId));
  assert(read(root / "partial-destination" / "partial-success.txt") == "partial");
  assert(!std::filesystem::exists(partialEntry));
  assert(!std::filesystem::exists(root / "partial-destination" / "not-present.txt"));
  assert(operation(manager, partialId).requestedItemCount == 2);

  // An unwritable destination must leave its restore source in the Wastebin.
  // Root bypasses directory write permissions, so this check is for ordinary
  // user sessions only.
  if (geteuid() != 0) {
    const auto unwritableEntry = batchRoot / "unwritable-destination.txt";
    const auto unwritableDestination = root / "unwritable-destination";
    write(unwritableEntry, "retain on failure");
    std::filesystem::create_directory(unwritableDestination);
    assert(chmod(unwritableDestination.c_str(), 0555) == 0);
    const auto unwritableUri = fixture.uriFor(unwritableEntry);
    fixture.originals[unwritableUri] = (unwritableDestination / "unwritable-destination.txt").string();
    const auto unwritableId = manager.restore({unwritableUri});
    const bool unwritableSucceeded = waitFor(manager, unwritableId);
    assert(chmod(unwritableDestination.c_str(), 0700) == 0);
    assert(!unwritableSucceeded);
    assert(std::filesystem::exists(unwritableEntry));
    assert(!std::filesystem::exists(unwritableDestination / "unwritable-destination.txt"));
  }

  // Exercise GIO's cross-filesystem move fallback when /dev/shm is available
  // on a different device. All fixture data stays in task-owned temp dirs.
  if (std::filesystem::is_directory("/dev/shm")) {
    std::string sharedTemplate = "/dev/shm/nythera-wastebin-crossfs-XXXXXX";
    char* sharedRaw = mkdtemp(sharedTemplate.data());
    if (sharedRaw != nullptr) {
      const std::filesystem::path sharedRoot(sharedRaw);
      struct stat rootState{};
      struct stat sharedState{};
      if (stat(root.c_str(), &rootState) == 0
          && stat(sharedRoot.c_str(), &sharedState) == 0
          && rootState.st_dev != sharedState.st_dev) {
        const auto crossFile = sharedRoot / "cross-file.txt";
        const auto crossDirectory = sharedRoot / "cross-directory";
        const auto crossDestination = root / "crossfs-original";
        std::filesystem::create_directory(crossDestination);
        write(crossFile, "crossfs file");
        write(crossDirectory / "nested" / "payload.txt", "crossfs directory");
        assert(chmod(crossDirectory.c_str(), 0750) == 0);
        const auto timestamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        std::filesystem::last_write_time(crossDirectory, timestamp);
        const auto savedTimestamp = std::filesystem::last_write_time(crossDirectory);
        const auto crossFileUri = fixture.uriFor(crossFile);
        const auto crossDirectoryUri = fixture.uriFor(crossDirectory);
        fixture.originals[crossFileUri] = (crossDestination / "cross-file.txt").string();
        fixture.originals[crossDirectoryUri] = (crossDestination / "cross-directory").string();
        const auto crossFsId = manager.restore({crossFileUri, crossDirectoryUri});
        assert(waitFor(manager, crossFsId));
        assert(read(crossDestination / "cross-file.txt") == "crossfs file");
        assert(read(crossDestination / "cross-directory" / "nested" / "payload.txt") == "crossfs directory");
        assert(!std::filesystem::exists(crossFile) && !std::filesystem::exists(crossDirectory));
        struct stat restoredState{};
        assert(stat((crossDestination / "cross-directory").c_str(), &restoredState) == 0);
        assert((restoredState.st_mode & 0777) == 0750);
        const auto restoredTimestamp = std::filesystem::last_write_time(crossDestination / "cross-directory");
        assert(std::chrono::abs(restoredTimestamp - savedTimestamp) < std::chrono::seconds(1));
      }
      std::filesystem::remove_all(sharedRoot);
    }
  }

  // Empty confirmation is an absolute guard. Confirmed emptying acts only on
  // the provider's top-level items. The local GIO fixture cannot emulate the
  // trash backend's recursive removal of non-empty trash directories, so that
  // item must fail safely and retain its contents.
  const auto enumerateBeforeCancel = fixture.enumerateCalls;
  const auto operationCountBeforeCancel = manager.operations().size();
  assert(manager.emptyWastebin(false) == 0);
  assert(fixture.enumerateCalls == enumerateBeforeCancel);
  assert(manager.operations().size() == operationCountBeforeCancel);

  const auto emptyFile = root / "fixture-trash" / "empty-file.txt";
  const auto emptyDirectory = root / "fixture-trash" / "empty-directory";
  const auto nonEmptyDirectory = root / "fixture-trash" / "non-empty-tree";
  const auto outsideEntry = root / "fixture-trash" / "not-enumerated.txt";
  write(emptyFile, "remove me");
  std::filesystem::create_directory(emptyDirectory);
  write(nonEmptyDirectory / "nested" / "preserve.txt", "preserve me");
  write(outsideEntry, "keep me");
  fixture.entries = {fixture.uriFor(emptyFile), fixture.uriFor(emptyDirectory), fixture.uriFor(nonEmptyDirectory)};
  const auto emptyId = manager.emptyWastebin(true);
  assert(!waitFor(manager, emptyId));
  assert(!std::filesystem::exists(emptyFile));
  assert(!std::filesystem::exists(emptyDirectory));
  assert(read(nonEmptyDirectory / "nested" / "preserve.txt") == "preserve me");
  assert(read(outsideEntry) == "keep me");
  assert(operation(manager, emptyId).resultTitle == "Empty Wastebin partially completed");
  assert(operation(manager, emptyId).kind == FileOperationManager::Kind::EmptyWastebin);
  assert(operation(manager, emptyId).requestedItemCount == fixture.entries.size());
  assert(fixture.enumerateCalls == enumerateBeforeCancel + 1);

  const auto finalEmptyFile = root / "fixture-trash" / "final-empty.txt";
  const auto finalEmptyDirectory = root / "fixture-trash" / "final-empty-directory";
  write(finalEmptyFile, "delete all");
  std::filesystem::create_directory(finalEmptyDirectory);
  fixture.entries = {fixture.uriFor(finalEmptyFile), fixture.uriFor(finalEmptyDirectory)};
  const auto successfulEmptyId = manager.emptyWastebin(true);
  assert(waitFor(manager, successfulEmptyId));
  assert(!std::filesystem::exists(finalEmptyFile) && !std::filesystem::exists(finalEmptyDirectory));
  assert(operation(manager, successfulEmptyId).resultDescription == "2 of 2 items deleted");

  fixture.entries.clear();
  const auto alreadyEmptyId = manager.emptyWastebin(true);
  assert(waitFor(manager, alreadyEmptyId));
  assert(operation(manager, alreadyEmptyId).resultDescription == "Wastebin is already empty");

  // The manager's changed callback is the refresh/completion signal consumed
  // by FileManager; it must observe terminal restore and empty operations.
  assert(std::ranges::find(finishedCallbacks, originalRestoreId) != finishedCallbacks.end());
  assert(std::ranges::find(finishedCallbacks, emptyId) != finishedCallbacks.end());
  assert(std::ranges::find(finishedCallbacks, successfulEmptyId) != finishedCallbacks.end());
  assert(std::ranges::find(finishedCallbacks, alreadyEmptyId) != finishedCallbacks.end());

  std::filesystem::remove_all(root);
  return 0;
}
