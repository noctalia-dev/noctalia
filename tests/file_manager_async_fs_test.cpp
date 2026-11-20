#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "ui/controls/label.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <wayland-client-protocol.h>

class FileManagerTestAccess {
public:
  static bool waitFor(const std::function<bool()>& pred) {
    struct Wait {
      const std::function<bool()>& pred;
      GMainLoop* loop;
      bool matched = false;
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    } wait{pred, g_main_loop_new(nullptr, false)};
    g_timeout_add(
        1,
        [](gpointer data) -> gboolean {
          auto& state = *static_cast<Wait*>(data);
          state.matched = state.pred();
          if (state.matched || std::chrono::steady_clock::now() >= state.deadline) {
            g_main_loop_quit(state.loop);
            return G_SOURCE_REMOVE;
          }
          return G_SOURCE_CONTINUE;
        },
        &wait
    );
    g_main_loop_run(wait.loop);
    g_main_loop_unref(wait.loop);
    return wait.matched;
  }

  static void attachStatusLabels(FileManager& manager, Label& primary, Label& detail, Label& freeSpace) {
    manager.m_statusLabel = &primary;
    manager.m_statusDetailLabel = &detail;
    manager.m_statusFreeLabel = &freeSpace;
  }

  static void detachStatusLabels(FileManager& manager) {
    manager.m_statusLabel = nullptr;
    manager.m_statusDetailLabel = nullptr;
    manager.m_statusFreeLabel = nullptr;
  }

  static void run(const std::filesystem::path& root) {
    const auto dirA = root / "dir-a";
    const auto dirB = root / "dir-b";
    std::filesystem::create_directory(dirA);
    std::filesystem::create_directory(dirB);
    const auto fileA = dirA / "alpha.txt";
    const auto fileB = dirB / "beta.txt";
    const auto linkB = dirB / "beta-link";
    {
      std::ofstream(fileA) << "hello";
      std::ofstream(fileB) << "world-content";
    }
    std::filesystem::create_symlink("beta.txt", linkB);

    // 1. Delayed free-space, writable, and selection-info queries: non-blocking UI, navigation away,
    //    stale completion rejection, and current completion application.
    {
      Label primary, detail, freeLabel;
      FileManager manager(dirA.string());
      attachStatusLabels(manager, primary, detail, freeLabel);

      std::vector<std::pair<std::filesystem::path, std::function<void(std::optional<std::uint64_t>)>>> freePending;
      std::vector<std::pair<std::filesystem::path, std::function<void(bool)>>> writablePending;
      std::vector<std::pair<std::filesystem::path, std::function<void(std::optional<FileManager::SelectionInfo>)>>>
          selectionPending;

      manager.m_freeSpaceQueryHook = [&](const std::filesystem::path& p, auto complete) {
        freePending.emplace_back(p, std::move(complete));
        return true;
      };
      manager.m_session.m_directoryWritableQueryHook = [&](const std::filesystem::path& p, auto complete) {
        writablePending.emplace_back(p, std::move(complete));
        return true;
      };
      manager.m_selectionInfoQueryHook = [&](const std::filesystem::path& p, auto complete) {
        selectionPending.emplace_back(p, std::move(complete));
        return true;
      };

      // Trigger async queries for dirA without blocking.
      manager.m_session.m_path = dirA;
      manager.queryStatusFreeSpaceAsync(dirA);
      manager.m_session.queryDirectoryWritableAsync(dirA);
      manager.updateStatusBar();
      assert(freePending.size() == 1 && freePending[0].first == dirA);
      assert(writablePending.size() == 1 && writablePending[0].first == dirA);
      assert(freeLabel.text().empty());
      assert(!manager.currentDirectoryWritable());

      // Navigate away to dirB while dirA queries are still unfinished.
      manager.m_session.m_path = dirB;
      manager.queryStatusFreeSpaceAsync(dirB);
      manager.m_session.queryDirectoryWritableAsync(dirB);
      manager.updateStatusBar();
      assert(freePending.size() == 2 && freePending[1].first == dirB);
      assert(writablePending.size() == 2 && writablePending[1].first == dirB);

      // Deliver stale dirA completions AFTER navigating to dirB: must be ignored.
      freePending[0].second(999ULL * 1024ULL * 1024ULL * 1024ULL);
      writablePending[0].second(true);
      assert(freeLabel.text().empty());
      assert(!manager.currentDirectoryWritable());

      // Deliver current dirB completions: must update UI state.
      freePending[1].second(4ULL * 1024ULL * 1024ULL * 1024ULL);
      writablePending[1].second(true);
      assert(freeLabel.text().find("free") != std::string::npos);
      assert(manager.currentDirectoryWritable());

      // Single-selection metadata: immediate non-blocking fallback from m_entries, stale rejection, and update.
      manager.m_session.m_entries = {
          FileEntry{.name = "alpha.txt", .absPath = fileA, .isDir = false, .size = 5, .type = "text/plain"},
          FileEntry{.name = "beta-link", .absPath = linkB, .isDir = false, .size = 8, .type = "text/plain"},
      };
      manager.m_selectedPaths = {fileA.string()};
      manager.updateStatusBar();
      assert(selectionPending.size() == 1 && selectionPending[0].first == fileA);
      assert(primary.text() == "alpha.txt");
      assert(!detail.text().empty());

      // Switch selection to linkB before fileA's query finishes.
      manager.m_selectedPaths = {linkB.string()};
      manager.updateStatusBar();
      assert(selectionPending.size() == 2 && selectionPending[1].first == linkB);

      // Stale completion for fileA must be ignored.
      selectionPending[0].second(
          FileManager::SelectionInfo{
              .path = fileA,
              .type = "STALE_TYPE",
              .target = {},
              .symlink = false,
              .hasSize = true,
              .size = 5,
          }
      );
      assert(detail.text().find("STALE_TYPE") == std::string::npos);

      // Current completion for linkB must apply.
      selectionPending[1].second(
          FileManager::SelectionInfo{
              .path = linkB,
              .type = "Plain text document (symbolic link)",
              .target = fileB.string(),
              .symlink = true,
              .hasSize = true,
              .size = 8,
          }
      );
      assert(detail.text().find("Target: " + fileB.string()) != std::string::npos);
      assert(detail.text().find("(symbolic link)") != std::string::npos);

      detachStatusLabels(manager);
    }

    // 2. Destruction while delayed requests remain unfinished must not block or UAF on late completion.
    {
      std::function<void(std::optional<std::uint64_t>)> lateFree;
      std::function<void(bool)> lateWritable;
      std::function<void(std::optional<FileManager::SelectionInfo>)> lateSelection;
      {
        auto transient = std::make_unique<FileManager>(dirA.string());
        transient->m_freeSpaceQueryHook = [&](const std::filesystem::path&, auto complete) {
          lateFree = std::move(complete);
          return true;
        };
        transient->m_session.m_directoryWritableQueryHook = [&](const std::filesystem::path&, auto complete) {
          lateWritable = std::move(complete);
          return true;
        };
        transient->m_selectionInfoQueryHook = [&](const std::filesystem::path&, auto complete) {
          lateSelection = std::move(complete);
          return true;
        };
        transient->m_session.m_path = dirA;
        transient->m_selectedPaths = {fileA.string()};
        transient->queryStatusFreeSpaceAsync(dirA);
        transient->m_session.queryDirectoryWritableAsync(dirA);
        transient->querySelectionInfoAsync(fileA);
      }
      assert(lateFree && lateWritable && lateSelection);
      lateFree(123456ULL);
      lateWritable(true);
      lateSelection(FileManager::SelectionInfo{.path = fileA, .type = "Late", .hasSize = true, .size = 5});
    }

    // 3. Real GIO async queries succeed and populate free space, writability, and symlink target.
    {
      Label primary, detail, freeLabel;
      FileManager manager(dirB.string());
      attachStatusLabels(manager, primary, detail, freeLabel);
      manager.m_session.m_path = dirB;
      manager.queryStatusFreeSpaceAsync(dirB);
      manager.m_session.queryDirectoryWritableAsync(dirB);
      assert(waitFor([&] {
        return manager.m_statusFreeSpaceBytes.has_value() && manager.m_session.m_directoryWritable.has_value();
      }));
      assert(manager.currentDirectoryWritable());
      assert(freeLabel.text().find("free") != std::string::npos);

      manager.m_session.m_entries = {
          FileEntry{.name = "beta-link", .absPath = linkB, .isDir = false, .size = 0, .type = {}},
      };
      manager.m_selectedPaths = {linkB.string()};
      manager.updateStatusBar();
      assert(waitFor([&] { return manager.m_selectionInfo.has_value() && manager.m_selectionInfo->path == linkB; }));
      assert(manager.m_selectionInfo->symlink);
      assert(manager.m_selectionInfo->target == fileB.string());
      assert(detail.text().find("Target: " + fileB.string()) != std::string::npos);

      detachStatusLabels(manager);
    }

    // 4. Places discovery & saved-place handling: slow/unavailable locations, symlink canonical duplicates,
    //    stale results, and safe destruction.
    {
      const auto realDir = root / "real-place";
      const auto linkDir = root / "link-place";
      const auto missingDir = root / "missing-place";
      std::filesystem::create_directory(realDir);
      std::filesystem::create_directory_symlink(realDir, linkDir);

      Label primary, detail, freeLabel;
      FileManager manager(dirA.string());
      attachStatusLabels(manager, primary, detail, freeLabel);

      std::vector<std::pair<std::filesystem::path, std::function<void(FileManager::PlacePathResolution)>>> placePending;
      manager.m_places.m_placePathResolveHook = [&](const std::filesystem::path& p, auto done) {
        placePending.emplace_back(p, std::move(done));
        return true;
      };

      // Unavailable folder is rejected asynchronously without blocking.
      manager.m_places.savePlace(missingDir);
      assert(placePending.size() == 1 && placePending[0].first == missingDir);
      assert(manager.m_places.m_savedPlaces.empty());
      placePending[0].second(FileManager::PlacePathResolution{.exists = false, .isDirectory = false});
      assert(manager.m_places.m_savedPlaces.empty());
      assert(manager.m_statusMessage == "This folder is unavailable");

      // Valid folder is pinned once resolved; symlink to the same canonical target is detected as duplicate.
      manager.m_places.savePlace(realDir);
      assert(placePending.size() == 2 && placePending[1].first == realDir);
      placePending[1].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realDir}
      );
      assert(manager.m_places.m_savedPlaces.size() == 1);

      manager.m_places.savePlace(linkDir);
      assert(placePending.size() == 3 && placePending[2].first == linkDir);
      placePending[2].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realDir}
      );
      assert(manager.m_places.samePersonalPlace(realDir, linkDir));
      assert(manager.m_places.m_savedPlaces.size() == 1);
      assert(manager.m_statusMessage == "This folder is already saved in Places");

      // Stale place resolution arriving after cancelPathResolutions() is ignored.
      const auto slowDir = root / "slow-place";
      manager.m_places.savePlace(slowDir);
      assert(placePending.size() == 4 && placePending[3].first == slowDir);
      manager.m_places.cancelPathResolutions();
      placePending[3].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = slowDir}
      );
      assert(manager.m_places.m_savedPlaces.size() == 1);

      detachStatusLabels(manager);

      // Destruction with unfinished place resolution must not block or UAF.
      std::function<void(FileManager::PlacePathResolution)> latePlace;
      {
        auto transient = std::make_unique<FileManager>(dirA.string());
        transient->m_places.m_placePathResolveHook = [&](const std::filesystem::path&, auto done) {
          latePlace = std::move(done);
          return true;
        };
        transient->m_places.savePlace(slowDir);
      }
      assert(latePlace);
      latePlace(FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = slowDir});
    }

    // 5. Drag-and-drop async validation: fail-closed while pending, stale target rejection,
    //    navigation cancellation, unsafe/unavailable target rejection, and destruction safety.
    {
      const auto destA = root / "drop-a";
      const auto destB = root / "drop-b";
      std::filesystem::create_directory(destA);
      std::filesystem::create_directory(destB);
      const std::vector<std::string> sources{fileA.string()};

      FileManager manager(dirA.string());
      manager.m_windowOpen = true;
      std::vector<std::function<void(FileManager::DropValidation)>> dropPending;
      manager.m_transfers.m_dropValidationHook = [&](const std::vector<std::string>&, const std::filesystem::path&, auto done) {
        dropPending.push_back(std::move(done));
        return true;
      };

      // Hover validation: fail-closed while pending, stale target A ignored after moving to target B.
      manager.m_transfers.m_dragSources = sources;
      manager.m_transfers.m_dragTargetValid = false;
      manager.m_transfers.requestHoverDropValidation(sources, destA);
      assert(!manager.m_transfers.m_dragTargetValid);
      assert(dropPending.size() == 1);

      manager.m_transfers.requestHoverDropValidation(sources, destB);
      assert(dropPending.size() == 2);

      // Stale completion for destA must be ignored.
      dropPending[0](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destA,
          .target = destA,
          .copyByDefault = true,
      });
      assert(!manager.m_transfers.m_dropValidation.has_value());

      // Current completion for destB is cached.
      dropPending[1](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destB,
          .target = destB,
          .copyByDefault = false,
      });
      assert(manager.m_transfers.m_dropValidation.has_value());
      assert(manager.m_transfers.m_dropValidation->target == destB);
      assert(!manager.m_transfers.m_dropValidation->copyByDefault);

      // Protocol drop rejected when navigation changes while validation is pending.
      dropPending.clear();
      manager.m_transfers.cancelDropValidation();
      manager.m_transfers.m_dragTargetValid = true;
      manager.m_transfers.m_protocolDropDestination = destA;
      auto receiverNav = manager.m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
      assert(receiverNav);
      std::optional<bool> navDropResult;
      receiverNav(filemanager::encodeFileTransfer(sources, false), [&](bool ok) { navDropResult = ok; });
      assert(dropPending.size() == 1);
      assert(!navDropResult.has_value());
      ++manager.m_session.m_navigationGeneration;
      dropPending[0](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destA,
          .target = destA,
          .copyByDefault = false,
      });
      dropPending.clear();
      assert(navDropResult.has_value() && !*navDropResult);
      assert(manager.m_operations.operations().empty());

      // Protocol drop rejected when target is unsafe/unavailable (target == std::nullopt).
      manager.m_transfers.cancelDropValidation();
      manager.m_transfers.m_dragTargetValid = true;
      manager.m_transfers.m_protocolDropDestination = destA;
      auto receiverUnsafe = manager.m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
      assert(receiverUnsafe);
      std::optional<bool> unsafeDropResult;
      receiverUnsafe(filemanager::encodeFileTransfer(sources, false), [&](bool ok) { unsafeDropResult = ok; });
      assert(dropPending.size() == 1);
      dropPending[0](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destA,
          .target = std::nullopt,
          .copyByDefault = true,
      });
      assert(unsafeDropResult.has_value() && !*unsafeDropResult);
      assert(manager.m_operations.operations().empty());

      // Destruction while protocol drop validation is pending invokes finish(false) and does not UAF.
      std::function<void(FileManager::DropValidation)> lateDrop;
      std::optional<bool> destroyedDropResult;
      {
        auto transient = std::make_unique<FileManager>(dirA.string());
        transient->m_windowOpen = true;
        transient->m_transfers.m_dropValidationHook = [&](const std::vector<std::string>&, const std::filesystem::path&,
                                              auto done) {
          lateDrop = std::move(done);
          return true;
        };
        transient->m_transfers.m_dragTargetValid = true;
        transient->m_transfers.m_protocolDropDestination = destA;
        auto recv = transient->m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        assert(recv);
        recv(filemanager::encodeFileTransfer(sources, false), [&](bool ok) { destroyedDropResult = ok; });
        assert(lateDrop);
      }
      lateDrop(
          FileManager::DropValidation{
              .sources = sources,
              .rawDestination = destA,
              .target = destA,
              .copyByDefault = true,
          }
      );
      lateDrop = {};
      assert(destroyedDropResult.has_value() && !*destroyedDropResult);
    }

    // 6. Async directory-equivalence comparison (replacing synchronous sameDirectoryPath / equivalent):
    //    lexical fast path, symlink equivalence, different directories, delayed resolution,
    //    navigation cancellation, and safe destruction.
    {
      const auto realTarget = root / "equiv-real";
      const auto linkTarget = root / "equiv-link";
      const auto otherTarget = root / "equiv-other";
      std::filesystem::create_directory(realTarget);
      std::filesystem::create_directory(otherTarget);
      std::filesystem::create_directory_symlink(realTarget, linkTarget);
      const auto pastedFile = realTarget / "pasted.txt";
      {
        std::ofstream(pastedFile) << "pasted payload";
      }
      struct stat pastedStat{};
      assert(::lstat(pastedFile.c_str(), &pastedStat) == 0);

      // Lexical fast path requires no filesystem I/O.
      FileManager manager(linkTarget.string());
      manager.m_windowOpen = true;
      assert(manager.m_places.sameDirectoryPathCached(realTarget, realTarget / ".") == true);
      assert(!manager.m_places.sameDirectoryPathCached(linkTarget, realTarget).has_value());

      // Real async resolution confirms symlink equivalence and distinguishes different directories.
      std::optional<bool> sameViaSymlink;
      manager.m_places.compareDirectoriesAsync(linkTarget, realTarget, [&](bool eq) { sameViaSymlink = eq; });
      assert(waitFor([&] { return sameViaSymlink.has_value(); }));
      assert(*sameViaSymlink);

      std::optional<bool> diffDirs;
      manager.m_places.compareDirectoriesAsync(linkTarget, otherTarget, [&](bool eq) { diffDirs = eq; });
      assert(waitFor([&] { return diffDirs.has_value(); }));
      assert(!*diffDirs);

      // Delayed comparison in recordPasteCompletion: non-blocking, selects when resolved,
      // and ignores stale completions after navigation.
      FileManager delayed(linkTarget.string());
      delayed.m_windowOpen = true;
      delayed.m_session.m_path = linkTarget;
      delayed.m_session.m_entries = {
          FileEntry{
              .name = "pasted.txt",
              .absPath = linkTarget / "pasted.txt",
              .isDir = false,
              .size = 14,
              .device = static_cast<std::uint64_t>(pastedStat.st_dev),
              .inode = static_cast<std::uint64_t>(pastedStat.st_ino),
              .hasIdentity = true,
          },
      };
      std::vector<std::pair<std::filesystem::path, std::function<void(FileManager::PlacePathResolution)>>> pendingCmp;
      delayed.m_places.m_placePathResolveHook = [&](const std::filesystem::path& p, auto done) {
        pendingCmp.emplace_back(p, std::move(done));
        return true;
      };

      const std::uint64_t opId = 42;
      delayed.m_transfers.m_pasteRequests[opId] = {realTarget, delayed.m_session.m_navigationGeneration, std::nullopt};
      FileOperationManager::Operation op;
      op.id = opId;
      op.succeeded = true;
      op.completedEntries.push_back(
          FileOperationManager::CompletedEntry{
              .path = pastedFile.string(),
              .device = static_cast<std::uint64_t>(pastedStat.st_dev),
              .inode = static_cast<std::uint64_t>(pastedStat.st_ino),
          }
      );
      assert(!delayed.recordPasteCompletion(op));
      assert(pendingCmp.size() == 2);
      assert(delayed.m_selectedPaths.empty());

      // Resolve both paths to the same canonical target: paste selection applies cleanly.
      pendingCmp[0].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      pendingCmp[1].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      assert(delayed.m_selectedPaths == std::unordered_set<std::string>{(linkTarget / "pasted.txt").string()});

      // Navigation while comparison is pending must discard stale paste selection.
      pendingCmp.clear();
      delayed.m_places.m_placePathCache.clear();
      delayed.m_selectedPaths.clear();
      const std::uint64_t opId2 = 43;
      delayed.m_transfers.m_pasteRequests[opId2] = {realTarget, delayed.m_session.m_navigationGeneration, std::nullopt};
      op.id = opId2;
      assert(!delayed.recordPasteCompletion(op));
      assert(pendingCmp.size() == 2);
      ++delayed.m_session.m_navigationGeneration;
      pendingCmp[0].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      pendingCmp[1].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      assert(delayed.m_selectedPaths.empty());

      // Destruction while comparison is pending must not block or UAF.
      std::vector<std::function<void(FileManager::PlacePathResolution)>> lateCmp;
      {
        auto transient = std::make_unique<FileManager>(linkTarget.string());
        transient->m_windowOpen = true;
        transient->m_session.m_path = linkTarget;
        transient->m_places.m_placePathResolveHook = [&](const std::filesystem::path&, auto done) {
          lateCmp.push_back(std::move(done));
          return true;
        };
        transient->m_places.compareDirectoriesAsync(linkTarget, realTarget, [](bool) {});
        assert(lateCmp.size() == 2);
      }
      for (auto& fn : lateCmp)
        fn(FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget});
    }

    // 7. Non-blocking inode-based paste selection restoration:
    //    real enumerate() populates FileEntry (device, inode), mismatched inodes are rejected,
    //    symlinks resolve NOFOLLOW identity asynchronously, and stale navigation is ignored.
    {
      const auto enumDir = root / "enum-identity";
      std::filesystem::create_directory(enumDir);
      const auto regFile = enumDir / "item.txt";
      const auto subDir = enumDir / "subdir";
      const auto symFile = enumDir / "item-link";
      {
        std::ofstream(regFile) << "identity test";
      }
      std::filesystem::create_directory(subDir);
      std::filesystem::create_symlink("item.txt", symFile);

      struct stat regStat{};
      struct stat dirStat{};
      struct stat symStat{};
      assert(::lstat(regFile.c_str(), &regStat) == 0);
      assert(::lstat(subDir.c_str(), &dirStat) == 0);
      assert(::lstat(symFile.c_str(), &symStat) == 0);

      FileManager manager(enumDir.string());
      manager.m_windowOpen = true;
      manager.enumerate(enumDir);
      assert(waitFor([&] { return !manager.m_session.m_loading && manager.m_session.m_entries.size() == 3; }));
      manager.m_session.closeMonitor();

      // Verify enumerate() populated (device, inode) on regular files and directories,
      // and flagged the symlink without polluting it with the target's inode.
      const auto regIt = std::ranges::find(manager.m_session.m_entries, regFile, &FileEntry::absPath);
      const auto dirIt = std::ranges::find(manager.m_session.m_entries, subDir, &FileEntry::absPath);
      const auto symIt = std::ranges::find(manager.m_session.m_entries, symFile, &FileEntry::absPath);
      assert(regIt != manager.m_session.m_entries.end() && regIt->hasIdentity && !regIt->isSymlink);
      assert(
          regIt->device == static_cast<std::uint64_t>(regStat.st_dev)
          && regIt->inode == static_cast<std::uint64_t>(regStat.st_ino)
      );
      assert(dirIt != manager.m_session.m_entries.end() && dirIt->hasIdentity && !dirIt->isSymlink);
      assert(
          dirIt->device == static_cast<std::uint64_t>(dirStat.st_dev)
          && dirIt->inode == static_cast<std::uint64_t>(dirStat.st_ino)
      );
      assert(symIt != manager.m_session.m_entries.end() && symIt->isSymlink && !symIt->hasIdentity);

      // Matching regular file + directory restores selection synchronously from enumerated identity.
      manager.m_selectedPaths.clear();
      manager.m_transfers.m_pendingPasteSelection = FileManager::PasteSelection{
          .request = {enumDir, manager.m_session.m_navigationGeneration, std::nullopt},
          .entries = {
              {.path = regFile.string(),
               .device = static_cast<std::uint64_t>(regStat.st_dev),
               .inode = static_cast<std::uint64_t>(regStat.st_ino)},
              {.path = subDir.string(),
               .device = static_cast<std::uint64_t>(dirStat.st_dev),
               .inode = static_cast<std::uint64_t>(dirStat.st_ino)},
          },
      };
      manager.applyPendingPasteSelection();
      assert(manager.m_selectedPaths == std::unordered_set<std::string>{regFile.string(), subDir.string()});

      // Mismatched inode (e.g. replaced file) must NOT be selected by filename alone.
      manager.m_selectedPaths.clear();
      manager.m_transfers.m_pendingPasteSelection = FileManager::PasteSelection{
          .request = {enumDir, manager.m_session.m_navigationGeneration, std::nullopt},
          .entries = {
              {.path = regFile.string(),
               .device = static_cast<std::uint64_t>(regStat.st_dev),
               .inode = static_cast<std::uint64_t>(regStat.st_ino) + 999999ULL},
          },
      };
      manager.applyPendingPasteSelection();
      assert(manager.m_selectedPaths.empty());

      // Symlink selection resolves NOFOLLOW (device, inode) asynchronously and matches the symlink's own inode.
      manager.m_selectedPaths.clear();
      manager.m_transfers.m_pendingPasteSelection = FileManager::PasteSelection{
          .request = {enumDir, manager.m_session.m_navigationGeneration, std::nullopt},
          .entries = {
              {.path = symFile.string(),
               .device = static_cast<std::uint64_t>(symStat.st_dev),
               .inode = static_cast<std::uint64_t>(symStat.st_ino)},
          },
      };
      manager.applyPendingPasteSelection();
      assert(waitFor([&] { return manager.m_selectedPaths == std::unordered_set<std::string>{symFile.string()}; }));

      // Stale navigation generation must never restore selection.
      manager.m_selectedPaths.clear();
      manager.m_transfers.m_pendingPasteSelection = FileManager::PasteSelection{
          .request = {enumDir, manager.m_session.m_navigationGeneration, std::nullopt},
          .entries = {
              {.path = regFile.string(),
               .device = static_cast<std::uint64_t>(regStat.st_dev),
               .inode = static_cast<std::uint64_t>(regStat.st_ino)},
          },
      };
      ++manager.m_session.m_navigationGeneration;
      manager.applyPendingPasteSelection();
      assert(manager.m_selectedPaths.empty());
    }
  }
};

int main() {
  char* raw = g_dir_make_tmp("nythera-fm-async-fs-XXXXXX", nullptr);
  assert(raw != nullptr);
  const std::filesystem::path root(raw);
  g_free(raw);
  FileManagerTestAccess::run(root);
  std::filesystem::remove_all(root);
  return 0;
}
