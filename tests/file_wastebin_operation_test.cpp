#include "filemanager/file_operation_manager.h"
#include "filemanager/wastebin_fallback.h"
#include "test_isolation.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <iterator>
#include <linux/fs.h>
#include <map>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

void setFileOperationRenameNoReplaceHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationReplaceBackupHookForTesting(std::function<void(const char*, const char*, bool)> hook);
void setFileOperationLinkAtHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationRemoveEntryHookForTesting(std::function<int(const char*)> hook);

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
    TEST_CHECK(id != 0);
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
      TEST_CHECK(uri != nullptr && error == nullptr);
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
    TEST_CHECK(file.good());
  }

  std::string read(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  const FileOperationManager::Operation& operation(FileOperationManager& manager, std::uint64_t id) {
    const auto found = std::ranges::find(manager.operations(), id, &FileOperationManager::Operation::id);
    TEST_CHECK(found != manager.operations().end() && found->finished);
    return *found;
  }

  void runTests(const nythera::test::ScopedTestEnv& env) {
    const auto resetHooks = nythera::test::makeScopeExit([] {
      setFileOperationRenameNoReplaceHookForTesting({});
      setFileOperationReplaceBackupHookForTesting({});
      setFileOperationLinkAtHookForTesting({});
      setFileOperationRemoveEntryHookForTesting({});
      setWastebinAvailableSpaceHookForTesting({});
    });
    const auto& root = env.work();

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
    TEST_CHECK(waitFor(manager, originalRestoreId));
    TEST_CHECK(!std::filesystem::exists(fileEntry));
    TEST_CHECK(read(originalDirectory / "plain.txt") == "plain payload");
    TEST_CHECK(operation(manager, originalRestoreId).kind == FileOperationManager::Kind::Restore);

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
    TEST_CHECK(waitFor(manager, selectedRestoreId));
    TEST_CHECK(read(selectedDestination / "batch.txt") == "batch payload");
    TEST_CHECK(read(selectedDestination / "tree" / "nested" / "inside.txt") == "nested payload");
    TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(selectedDestination / "link.txt")));
    TEST_CHECK(std::filesystem::read_symlink(selectedDestination / "link.txt") == linkTarget);
    TEST_CHECK(read(linkTarget) == "link target");
    TEST_CHECK(operation(manager, selectedRestoreId).requestedItemCount == 3);

    // A cancelled conflict keeps both entries intact. Replace moves the wastebin
    // item into place and removes the old destination only after success.
    const auto conflictEntry = batchRoot / "collision.txt";
    const auto collisionDestination = root / "collision-destination";
    std::filesystem::create_directory(collisionDestination);
    write(conflictEntry, "replacement");
    write(collisionDestination / "collision.txt", "keep");
    const auto conflictUri = fixture.uriFor(conflictEntry);
    fixture.originals[conflictUri] = (root / "unused-original" / "collision.txt").string();
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Cancel, false};
    });
    const auto cancelledId = manager.restore({conflictUri}, collisionDestination.string());
    TEST_CHECK(!waitFor(manager, cancelledId));
    TEST_CHECK(operation(manager, cancelledId).cancelled);
    TEST_CHECK(std::filesystem::exists(conflictEntry));
    TEST_CHECK(read(collisionDestination / "collision.txt") == "keep");

    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    const auto replaceId = manager.restore({conflictUri}, collisionDestination.string());
    TEST_CHECK(waitFor(manager, replaceId));
    TEST_CHECK(!std::filesystem::exists(conflictEntry));
    TEST_CHECK(read(collisionDestination / "collision.txt") == "replacement");

    // If the destination inode is swapped after Replace is chosen, restore must
    // refuse to clobber the unapproved item and keep the Wastebin source intact.
    const auto swapRestoreEntry = batchRoot / "swap-restore.txt";
    const auto heldRestoreDestination = root / "held-restore-destination.txt";
    write(swapRestoreEntry, "incoming restore");
    write(collisionDestination / "swap-restore.txt", "approved destination");
    const auto swapRestoreUri = fixture.uriFor(swapRestoreEntry);
    fixture.originals[swapRestoreUri] = (root / "unused-original" / "swap-restore.txt").string();
    manager.setConflictResolver([&](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      std::filesystem::rename(collisionDestination / "swap-restore.txt", heldRestoreDestination);
      write(collisionDestination / "swap-restore.txt", "unapproved replacement");
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    const auto swapRestoreId = manager.restore({swapRestoreUri}, collisionDestination.string());
    TEST_CHECK(!waitFor(manager, swapRestoreId));
    TEST_CHECK(read(swapRestoreEntry) == "incoming restore");
    TEST_CHECK(read(collisionDestination / "swap-restore.txt") == "unapproved replacement");
    TEST_CHECK(read(heldRestoreDestination) == "approved destination");

    // If the destination inode swaps between pre-verification and moving
    // target -> backup, post-verification must detect the mismatch, roll back the
    // unapproved item to destination, and preserve both it and the Wastebin item.
    const auto midBackupRestoreEntry = batchRoot / "mid-backup-restore.txt";
    const auto midBackupRestoreDest = collisionDestination / "mid-backup-restore.txt";
    const auto heldMidBackupRestore = root / "held-mid-backup-restore.txt";
    write(midBackupRestoreEntry, "incoming mid-backup restore");
    write(midBackupRestoreDest, "approved before restore");
    const auto midBackupRestoreUri = fixture.uriFor(midBackupRestoreEntry);
    fixture.originals[midBackupRestoreUri] = (root / "unused-original" / "mid-backup-restore.txt").string();
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    bool swappedMidBackupRestore = false;
    setFileOperationReplaceBackupHookForTesting([&](const char* targetPath, const char*, bool afterMove) {
      if (!afterMove && !swappedMidBackupRestore && targetPath != nullptr && targetPath == midBackupRestoreDest) {
        swappedMidBackupRestore = true;
        std::filesystem::rename(midBackupRestoreDest, heldMidBackupRestore);
        write(midBackupRestoreDest, "unapproved right before backup");
      }
    });
    const auto midBackupRestoreId = manager.restore({midBackupRestoreUri}, collisionDestination.string());
    TEST_CHECK(!waitFor(manager, midBackupRestoreId));
    setFileOperationReplaceBackupHookForTesting({});
    TEST_CHECK(swappedMidBackupRestore);
    TEST_CHECK(read(midBackupRestoreEntry) == "incoming mid-backup restore");
    TEST_CHECK(read(midBackupRestoreDest) == "unapproved right before backup");
    TEST_CHECK(read(heldMidBackupRestore) == "approved before restore");
    for (const auto& entry : std::filesystem::directory_iterator(collisionDestination)) {
      const auto fname = entry.path().filename().string();
      TEST_CHECK(!fname.starts_with(".nythera-backup-") && !fname.starts_with(".nythera-restore-"));
    }

    // On a destination lacking RENAME_NOREPLACE, Restore succeeds safely via linkat +
    // verified unlinkat (and private .nythera-backup-* directories for Replace), and
    // fails closed only when both RENAME_NOREPLACE and hard-link publication are unsupported.
    const auto noReplaceRestoreDir = root / "no-noreplace-restore";
    std::filesystem::create_directory(noReplaceRestoreDir);
    const auto nrRestoreOccupied = batchRoot / "nr-existing.txt";
    const auto nrRestoreFresh = batchRoot / "nr-fresh.txt";
    const auto crossSimEntry = batchRoot / "cross-sim.txt";
    const auto crossSimDest = selectedDestination / "cross-sim.txt";
    write(nrRestoreOccupied, "incoming replace nr");
    write(noReplaceRestoreDir / "nr-existing.txt", "keep existing nr");
    write(nrRestoreFresh, "incoming fresh nr");
    write(crossSimEntry, "cross sim payload");
    const auto nrRestoreOccupiedUri = fixture.uriFor(nrRestoreOccupied);
    const auto nrRestoreFreshUri = fixture.uriFor(nrRestoreFresh);
    const auto crossSimUri = fixture.uriFor(crossSimEntry);
    fixture.originals[nrRestoreOccupiedUri] = (root / "unused-original" / "nr-existing.txt").string();
    fixture.originals[nrRestoreFreshUri] = (root / "unused-original" / "nr-fresh.txt").string();
    fixture.originals[crossSimUri] = (root / "unused-original" / "cross-sim.txt").string();
    const std::string nrRestorePrefix = noReplaceRestoreDir.string() + "/";
    setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
      const bool touchesNoReplace = (sourcePath != nullptr && std::string(sourcePath).starts_with(nrRestorePrefix))
          || (destinationPath != nullptr && std::string(destinationPath).starts_with(nrRestorePrefix));
      if (touchesNoReplace) {
        errno = EINVAL;
        return -1;
      }
      if (sourcePath != nullptr
          && destinationPath != nullptr
          && sourcePath == crossSimEntry
          && destinationPath == crossSimDest) {
        errno = EXDEV;
        return -1;
      }
      return static_cast<int>(
          syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
      );
    });
    const auto nrOccupiedId = manager.restore({nrRestoreOccupiedUri}, noReplaceRestoreDir.string());
    TEST_CHECK(waitFor(manager, nrOccupiedId));
    TEST_CHECK(!std::filesystem::exists(nrRestoreOccupied));
    TEST_CHECK(read(noReplaceRestoreDir / "nr-existing.txt") == "incoming replace nr");

    const auto nrFreshId = manager.restore({nrRestoreFreshUri}, noReplaceRestoreDir.string());
    TEST_CHECK(waitFor(manager, nrFreshId));
    TEST_CHECK(!std::filesystem::exists(nrRestoreFresh));
    TEST_CHECK(read(noReplaceRestoreDir / "nr-fresh.txt") == "incoming fresh nr");
    for (const auto& entry : std::filesystem::directory_iterator(noReplaceRestoreDir)) {
      const auto fname = entry.path().filename().string();
      TEST_CHECK(!fname.starts_with(".nythera-backup-") && !fname.starts_with(".nythera-restore-"));
    }

    // When both RENAME_NOREPLACE and linkat are unsupported, Restore fails closed without clobbering.
    const auto nrUnsupportedItem = batchRoot / "nr-unsupported.txt";
    write(nrUnsupportedItem, "blocked restore");
    const auto nrUnsupportedUri = fixture.uriFor(nrUnsupportedItem);
    fixture.originals[nrUnsupportedUri] = (root / "unused-original" / "nr-unsupported.txt").string();
    setFileOperationLinkAtHookForTesting([&](const char*, const char* dstPath) -> int {
      if (dstPath != nullptr && std::string(dstPath).starts_with(nrRestorePrefix)) {
        errno = EOPNOTSUPP;
        return -1;
      }
      return 0;
    });
    const auto nrUnsupportedId = manager.restore({nrUnsupportedUri}, noReplaceRestoreDir.string());
    TEST_CHECK(!waitFor(manager, nrUnsupportedId));
    TEST_CHECK(
        operation(manager, nrUnsupportedId).resultDescription.find("does not support atomic non-overwriting moves")
        != std::string::npos
    );
    TEST_CHECK(read(nrUnsupportedItem) == "blocked restore");
    TEST_CHECK(!std::filesystem::exists(noReplaceRestoreDir / "nr-unsupported.txt"));
    setFileOperationLinkAtHookForTesting({});

    const auto crossSimId = manager.restore({crossSimUri}, selectedDestination.string());
    TEST_CHECK(waitFor(manager, crossSimId));
    TEST_CHECK(!std::filesystem::exists(crossSimEntry));
    TEST_CHECK(read(crossSimDest) == "cross sim payload");
    setFileOperationRenameNoReplaceHookForTesting({});

    const auto renameEntry = batchRoot / "rename-collision.txt";
    write(renameEntry, "renamed wastebin item");
    write(collisionDestination / "rename-collision.txt", "keep existing");
    const auto renameUri = fixture.uriFor(renameEntry);
    fixture.originals[renameUri] = (root / "unused-original" / "rename-collision.txt").string();
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Rename, false};
    });
    const auto renameId = manager.restore({renameUri}, collisionDestination.string());
    TEST_CHECK(waitFor(manager, renameId));
    TEST_CHECK(read(collisionDestination / "rename-collision.txt") == "keep existing");
    TEST_CHECK(read(collisionDestination / "rename-collision (restored).txt") == "renamed wastebin item");

    const auto skipEntry = batchRoot / "skip-collision.txt";
    write(skipEntry, "keep in wastebin");
    write(collisionDestination / "skip-collision.txt", "keep destination");
    const auto skipUri = fixture.uriFor(skipEntry);
    fixture.originals[skipUri] = (root / "unused-original" / "skip-collision.txt").string();
    manager.setConflictResolver([](const std::string&, const std::string&, bool, const auto&, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Skip, false};
    });
    const auto skipId = manager.restore({skipUri}, collisionDestination.string());
    TEST_CHECK(waitFor(manager, skipId));
    TEST_CHECK(std::filesystem::exists(skipEntry));
    TEST_CHECK(read(collisionDestination / "skip-collision.txt") == "keep destination");

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
    std::atomic_uint applyAllResolverCalls{0};
    std::atomic_bool applyAllSawMoreItems{true};
    manager.setConflictResolver([&](const std::string&, const std::string&, bool moreItems, const auto&,
                                    GCancellable*) {
      ++applyAllResolverCalls;
      if (!moreItems)
        applyAllSawMoreItems.store(false, std::memory_order_relaxed);
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, true};
    });
    const auto applyAllId = manager.restore({applyAllAUri, applyAllBUri}, collisionDestination.string());
    TEST_CHECK(waitFor(manager, applyAllId));
    TEST_CHECK(applyAllSawMoreItems.load(std::memory_order_relaxed));
    TEST_CHECK(applyAllResolverCalls.load() == 1);
    TEST_CHECK(read(collisionDestination / "apply-all-a.txt") == "new a");
    TEST_CHECK(read(collisionDestination / "apply-all-b.txt") == "new b");
    manager.setConflictResolver({});

    // Missing metadata, a missing source, and an absent parent are reported as
    // failed jobs without manufacturing or overwriting destination files.
    const auto noMetadataEntry = batchRoot / "no-metadata.txt";
    write(noMetadataEntry, "source");
    const auto noMetadataUri = fixture.uriFor(noMetadataEntry);
    fixture.originals.erase(noMetadataUri);
    const auto noMetadataId = manager.restore({noMetadataUri});
    TEST_CHECK(!waitFor(manager, noMetadataId));
    TEST_CHECK(std::filesystem::exists(noMetadataEntry));

    const auto missingParentEntry = batchRoot / "missing-parent.txt";
    write(missingParentEntry, "missing parent source");
    const auto missingParentUri = fixture.uriFor(missingParentEntry);
    fixture.originals[missingParentUri] = (root / "absent-parent" / "missing-parent.txt").string();
    const auto missingParentId = manager.restore({missingParentUri});
    TEST_CHECK(!waitFor(manager, missingParentId));
    TEST_CHECK(read(missingParentEntry) == "missing parent source");

    const auto missingSourceEntry = batchRoot / "missing-source.txt";
    const auto missingSourceUri = fixture.uriFor(missingSourceEntry);
    fixture.originals[missingSourceUri] = (originalDirectory / "missing-source.txt").string();
    const auto missingSourceId = manager.restore({missingSourceUri});
    TEST_CHECK(!waitFor(manager, missingSourceId));
    TEST_CHECK(!std::filesystem::exists(originalDirectory / "missing-source.txt"));

    const auto invalidUriId = manager.restore({"trash:///not-a-local-fixture"});
    TEST_CHECK(!waitFor(manager, invalidUriId));

    // Batch restore reports a partial failure while preserving successful work.
    const auto partialEntry = batchRoot / "partial-success.txt";
    write(partialEntry, "partial");
    const auto partialUri = fixture.uriFor(partialEntry);
    const auto neverThereUri = fixture.uriFor(batchRoot / "not-present.txt");
    fixture.originals[partialUri] = (root / "partial-destination" / "partial-success.txt").string();
    fixture.originals[neverThereUri] = (root / "partial-destination" / "not-present.txt").string();
    std::filesystem::create_directory(root / "partial-destination");
    const auto partialId = manager.restore({partialUri, neverThereUri}, (root / "partial-destination").string());
    TEST_CHECK(!waitFor(manager, partialId));
    TEST_CHECK(read(root / "partial-destination" / "partial-success.txt") == "partial");
    TEST_CHECK(!std::filesystem::exists(partialEntry));
    TEST_CHECK(!std::filesystem::exists(root / "partial-destination" / "not-present.txt"));
    TEST_CHECK(operation(manager, partialId).requestedItemCount == 2);

    // An unwritable destination must leave its restore source in the Wastebin.
    // Root bypasses directory write permissions, so this check is for ordinary
    // user sessions only.
    if (geteuid() != 0) {
      const auto unwritableEntry = batchRoot / "unwritable-destination.txt";
      const auto unwritableDestination = root / "unwritable-destination";
      write(unwritableEntry, "retain on failure");
      std::filesystem::create_directory(unwritableDestination);
      TEST_CHECK(chmod(unwritableDestination.c_str(), 0555) == 0);
      const auto unwritableUri = fixture.uriFor(unwritableEntry);
      fixture.originals[unwritableUri] = (unwritableDestination / "unwritable-destination.txt").string();
      const auto unwritableId = manager.restore({unwritableUri});
      const bool unwritableSucceeded = waitFor(manager, unwritableId);
      TEST_CHECK(chmod(unwritableDestination.c_str(), 0700) == 0);
      TEST_CHECK(!unwritableSucceeded);
      TEST_CHECK(std::filesystem::exists(unwritableEntry));
      TEST_CHECK(!std::filesystem::exists(unwritableDestination / "unwritable-destination.txt"));
    }

    // Exercise GIO's cross-filesystem move fallback when /dev/shm is available
    // on a different device. All fixture data stays in task-owned temp dirs.
    {
      nythera::test::ScopedOwnedDir sharedRoot(
          "/dev/shm", nythera::test::uniqueToken("nythera-wastebin-crossfs"), false
      );
      if (sharedRoot.created()) {
        FileOperationManager crossFsManager([](const FileOperationManager::Operation&) {}, fixture.backend());
        struct stat rootState{};
        struct stat sharedState{};
        if (stat(root.c_str(), &rootState) == 0
            && stat(sharedRoot.path().c_str(), &sharedState) == 0
            && rootState.st_dev != sharedState.st_dev) {
          const auto crossFile = sharedRoot.path() / "cross-file.txt";
          const auto crossDirectory = sharedRoot.path() / "cross-directory";
          const auto crossDestination = root / "crossfs-original";
          std::filesystem::create_directory(crossDestination);
          write(crossFile, "crossfs file");
          write(crossDirectory / "nested" / "payload.txt", "crossfs directory");
          TEST_CHECK(chmod(crossDirectory.c_str(), 0750) == 0);
          const auto timestamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
          std::filesystem::last_write_time(crossDirectory, timestamp);
          const auto savedTimestamp = std::filesystem::last_write_time(crossDirectory);
          const auto crossFileUri = fixture.uriFor(crossFile);
          const auto crossDirectoryUri = fixture.uriFor(crossDirectory);
          fixture.originals[crossFileUri] = (crossDestination / "cross-file.txt").string();
          fixture.originals[crossDirectoryUri] = (crossDestination / "cross-directory").string();
          const auto crossFsId = crossFsManager.restore({crossFileUri, crossDirectoryUri});
          TEST_CHECK(waitFor(crossFsManager, crossFsId));
          TEST_CHECK(read(crossDestination / "cross-file.txt") == "crossfs file");
          TEST_CHECK(read(crossDestination / "cross-directory" / "nested" / "payload.txt") == "crossfs directory");
          TEST_CHECK(!std::filesystem::exists(crossFile) && !std::filesystem::exists(crossDirectory));
          struct stat restoredState{};
          TEST_CHECK(stat((crossDestination / "cross-directory").c_str(), &restoredState) == 0);
          TEST_CHECK((restoredState.st_mode & 0777) == 0750);
          const auto restoredTimestamp = std::filesystem::last_write_time(crossDestination / "cross-directory");
          TEST_CHECK(std::chrono::abs(restoredTimestamp - savedTimestamp) < std::chrono::seconds(1));
        }
      }
    }

    // Empty confirmation is an absolute guard. Confirmed emptying acts only on
    // the provider's top-level items. The local GIO fixture cannot emulate the
    // trash backend's recursive removal of non-empty trash directories, so that
    // item must fail safely and retain its contents.
    const auto enumerateBeforeCancel = fixture.enumerateCalls;
    const auto operationCountBeforeCancel = manager.operations().size();
    TEST_CHECK(manager.emptyWastebin(false) == 0);
    TEST_CHECK(fixture.enumerateCalls == enumerateBeforeCancel);
    TEST_CHECK(manager.operations().size() == operationCountBeforeCancel);

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
    TEST_CHECK(!waitFor(manager, emptyId));
    TEST_CHECK(!std::filesystem::exists(emptyFile));
    TEST_CHECK(!std::filesystem::exists(emptyDirectory));
    TEST_CHECK(read(nonEmptyDirectory / "nested" / "preserve.txt") == "preserve me");
    TEST_CHECK(read(outsideEntry) == "keep me");
    TEST_CHECK(operation(manager, emptyId).resultTitle == "Empty Wastebin partially completed");
    TEST_CHECK(operation(manager, emptyId).kind == FileOperationManager::Kind::EmptyWastebin);
    TEST_CHECK(operation(manager, emptyId).requestedItemCount == fixture.entries.size());
    TEST_CHECK(fixture.enumerateCalls == enumerateBeforeCancel + 1);

    const auto finalEmptyFile = root / "fixture-trash" / "final-empty.txt";
    const auto finalEmptyDirectory = root / "fixture-trash" / "final-empty-directory";
    write(finalEmptyFile, "delete all");
    std::filesystem::create_directory(finalEmptyDirectory);
    fixture.entries = {fixture.uriFor(finalEmptyFile), fixture.uriFor(finalEmptyDirectory)};
    const auto successfulEmptyId = manager.emptyWastebin(true);
    TEST_CHECK(waitFor(manager, successfulEmptyId));
    TEST_CHECK(!std::filesystem::exists(finalEmptyFile) && !std::filesystem::exists(finalEmptyDirectory));
    TEST_CHECK(operation(manager, successfulEmptyId).resultDescription == "2 of 2 items deleted");

    fixture.entries.clear();
    const auto alreadyEmptyId = manager.emptyWastebin(true);
    TEST_CHECK(waitFor(manager, alreadyEmptyId));
    TEST_CHECK(operation(manager, alreadyEmptyId).resultDescription == "Wastebin is already empty");

    // The manager's changed callback is the refresh/completion signal consumed
    // by FileManager; it must observe terminal restore and empty operations.
    TEST_CHECK(std::ranges::find(finishedCallbacks, originalRestoreId) != finishedCallbacks.end());
    TEST_CHECK(std::ranges::find(finishedCallbacks, emptyId) != finishedCallbacks.end());
    TEST_CHECK(std::ranges::find(finishedCallbacks, successfulEmptyId) != finishedCallbacks.end());
    TEST_CHECK(std::ranges::find(finishedCallbacks, alreadyEmptyId) != finishedCallbacks.end());

    // Stage 4 tests: FreeDesktop Home Wastebin fallback (.trashinfo reservation,
    // percent-encoding, free-space check, staged transfer, split-state reporting).
    {
      const auto isolatedXdg = root / "isolated-xdg-data";
      std::filesystem::create_directories(isolatedXdg);
      nythera::test::ScopedEnvVar xdgDataGuard("XDG_DATA_HOME", isolatedXdg.string());

      TEST_CHECK(
          filemanager::wastebin_fallback::encodeTrashInfoPath("/mnt/Gogeta/My File 100%.txt")
          == "/mnt/Gogeta/My%20File%20100%25.txt"
      );
      const auto formattedDate = filemanager::wastebin_fallback::formatTrashDeletionDate(1700000000);
      TEST_CHECK(formattedDate.size() == 19 && formattedDate[10] == 'T');

      const auto homeSrcDir = root / "home-fallback-sources";
      std::filesystem::create_directories(homeSrcDir);
      const auto fileA = homeSrcDir / "BorderlessMinecraft.exe";
      const auto fileACollision = homeSrcDir / "sub" / "BorderlessMinecraft.exe";
      write(fileA, "first trashed exe");
      write(fileACollision, "second trashed exe");

      // 1. Insufficient free space blocks trashToHomeWastebin before transferring data.
      setWastebinAvailableSpaceHookForTesting([](const char*) -> std::optional<std::uint64_t> { return 4; });
      const auto noSpaceId = manager.trashToHomeWastebin({fileA.string()});
      TEST_CHECK(!waitFor(manager, noSpaceId));
      TEST_CHECK(operation(manager, noSpaceId).resultDescription.find("Insufficient free space") != std::string::npos);
      TEST_CHECK(read(fileA) == "first trashed exe");
      setWastebinAvailableSpaceHookForTesting({});

      // 2. Successful trashToHomeWastebin creates .trashinfo and files entry, and handles name collisions.
      const auto homeTrash1 = manager.trashToHomeWastebin({fileA.string()});
      TEST_CHECK(waitFor(manager, homeTrash1));
      TEST_CHECK(!std::filesystem::exists(fileA));
      TEST_CHECK(operation(manager, homeTrash1).movedSources == std::vector<std::string>{fileA.string()});
      const auto trashFilesDir = isolatedXdg / "Trash" / "files";
      const auto trashInfoDir = isolatedXdg / "Trash" / "info";
      TEST_CHECK(read(trashFilesDir / "BorderlessMinecraft.exe") == "first trashed exe");
      TEST_CHECK(read(trashInfoDir / "BorderlessMinecraft.exe.trashinfo").find("Path=") != std::string::npos);

      const auto homeTrash2 = manager.trashToHomeWastebin({fileACollision.string()});
      TEST_CHECK(waitFor(manager, homeTrash2));
      TEST_CHECK(!std::filesystem::exists(fileACollision));
      TEST_CHECK(read(trashFilesDir / "BorderlessMinecraft.2.exe") == "second trashed exe");
      TEST_CHECK(std::filesystem::exists(trashInfoDir / "BorderlessMinecraft.2.exe.trashinfo"));

      // 3. Source cleanup failure after committing to Home Wastebin reports split state.
      const auto splitTrashSrc = homeSrcDir / "split-trash.txt";
      write(splitTrashSrc, "split trash payload");
      // Simulate cross-device staging by returning EXDEV on direct renameNoReplace.
      setFileOperationRenameNoReplaceHookForTesting([&](const char* src, const char* dst) -> int {
        if (src != nullptr && std::string(src) == splitTrashSrc.string()) {
          errno = EXDEV;
          return -1;
        }
        return static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE));
      });
      setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
        return path != nullptr && std::string(path) == splitTrashSrc.string() ? EIO : 0;
      });
      const auto splitTrashId = manager.trashToHomeWastebin({splitTrashSrc.string()});
      TEST_CHECK(!waitFor(manager, splitTrashId));
      setFileOperationRemoveEntryHookForTesting({});
      setFileOperationRenameNoReplaceHookForTesting({});
      TEST_CHECK(read(splitTrashSrc) == "split trash payload");
      TEST_CHECK(read(trashFilesDir / "split-trash.txt") == "split trash payload");
      TEST_CHECK(
          operation(manager, splitTrashId).resultDescription.find("Trashed copy remains in Home Wastebin")
          != std::string::npos
      );
    }
  }
} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-wastebin-test"});
    runTests(env);
  });
}
