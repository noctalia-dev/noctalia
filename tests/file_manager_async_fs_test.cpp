#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "test_isolation.h"
#include "ui/controls/label.h"
#include "util/file_utils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <iterator>
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

  static void run(const nythera::test::ScopedTestEnv& env) {
    struct StatSnapshot {
      bool exists = false;
      std::uint64_t size = 0;
      std::int64_t mtimeSec = 0;
      std::int64_t mtimeNsec = 0;
    };
    const auto snapshotStat = [](const std::filesystem::path& path) {
      StatSnapshot snap;
      struct stat st{};
      if (::lstat(path.c_str(), &st) == 0) {
        snap.exists = true;
        snap.size = static_cast<std::uint64_t>(st.st_size);
        snap.mtimeSec = st.st_mtim.tv_sec;
        snap.mtimeNsec = st.st_mtim.tv_nsec;
      }
      return snap;
    };
    const std::filesystem::path realStateFile = env.realUserHome().empty()
        ? std::filesystem::path{}
        : (env.realUserHome() / ".local" / "state" / "nythera" / "state.toml");
    const StatSnapshot realStateBefore = realStateFile.empty() ? StatSnapshot{} : snapshotStat(realStateFile);

    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::stateDir(), env.root()));
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::configDir(), env.root()));
    {
      GError* busError = nullptr;
      GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &busError);
      TEST_CHECK(bus == nullptr);
      g_clear_error(&busError);
    }

    const auto& root = env.work();
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

      auto& tab = manager.activeTab();
      auto& session = tab.session;
      tab.m_freeSpaceQueryHook = [&](const std::filesystem::path& p, auto complete) {
        freePending.emplace_back(p, std::move(complete));
        return true;
      };
      session.m_directoryWritableQueryHook = [&](const std::filesystem::path& p, auto complete) {
        writablePending.emplace_back(p, std::move(complete));
        return true;
      };
      tab.m_selectionInfoQueryHook = [&](const std::filesystem::path& p, auto complete) {
        selectionPending.emplace_back(p, std::move(complete));
        return true;
      };

      // Trigger async queries for dirA without blocking.
      session.m_path = dirA;
      manager.queryStatusFreeSpaceAsync(dirA);
      session.queryDirectoryWritableAsync(dirA);
      manager.updateStatusBar();
      TEST_CHECK(freePending.size() == 1 && freePending[0].first == dirA);
      TEST_CHECK(writablePending.size() == 1 && writablePending[0].first == dirA);
      TEST_CHECK(freeLabel.text().empty());
      TEST_CHECK(!manager.currentDirectoryWritable());

      // Navigate away to dirB while dirA queries are still unfinished.
      session.m_path = dirB;
      manager.queryStatusFreeSpaceAsync(dirB);
      session.queryDirectoryWritableAsync(dirB);
      manager.updateStatusBar();
      TEST_CHECK(freePending.size() == 2 && freePending[1].first == dirB);
      TEST_CHECK(writablePending.size() == 2 && writablePending[1].first == dirB);

      // Deliver stale dirA completions AFTER navigating to dirB: must be ignored.
      freePending[0].second(999ULL * 1024ULL * 1024ULL * 1024ULL);
      writablePending[0].second(true);
      TEST_CHECK(freeLabel.text().empty());
      TEST_CHECK(!manager.currentDirectoryWritable());

      // Deliver current dirB completions: must update UI state.
      freePending[1].second(4ULL * 1024ULL * 1024ULL * 1024ULL);
      writablePending[1].second(true);
      TEST_CHECK(freeLabel.text().find("free") != std::string::npos);
      TEST_CHECK(manager.currentDirectoryWritable());

      // Single-selection metadata: immediate non-blocking fallback from m_entries, stale rejection, and update.
      session.m_entries = {
          FileEntry{.name = "alpha.txt", .absPath = fileA, .isDir = false, .size = 5, .type = "text/plain"},
          FileEntry{.name = "beta-link", .absPath = linkB, .isDir = false, .size = 8, .type = "text/plain"},
      };
      tab.selectedPaths = {fileA.string()};
      manager.updateStatusBar();
      TEST_CHECK(selectionPending.size() == 1 && selectionPending[0].first == fileA);
      TEST_CHECK(primary.text() == "alpha.txt");
      TEST_CHECK(!detail.text().empty());

      // Switch selection to linkB before fileA's query finishes.
      tab.selectedPaths = {linkB.string()};
      manager.updateStatusBar();
      TEST_CHECK(selectionPending.size() == 2 && selectionPending[1].first == linkB);

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
      TEST_CHECK(detail.text().find("STALE_TYPE") == std::string::npos);

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
      TEST_CHECK(detail.text().find("Target: " + fileB.string()) != std::string::npos);
      TEST_CHECK(detail.text().find("(symbolic link)") != std::string::npos);

      detachStatusLabels(manager);
    }

    // 2. Destruction while delayed requests remain unfinished must not block or UAF on late completion.
    {
      std::function<void(std::optional<std::uint64_t>)> lateFree;
      std::function<void(bool)> lateWritable;
      std::function<void(std::optional<FileManager::SelectionInfo>)> lateSelection;
      {
        auto transient = std::make_unique<FileManager>(dirA.string());
        auto& transientTab = transient->activeTab();
        auto& transientSession = transientTab.session;
        transientTab.m_freeSpaceQueryHook = [&](const std::filesystem::path&, auto complete) {
          lateFree = std::move(complete);
          return true;
        };
        transientSession.m_directoryWritableQueryHook = [&](const std::filesystem::path&, auto complete) {
          lateWritable = std::move(complete);
          return true;
        };
        transientTab.m_selectionInfoQueryHook = [&](const std::filesystem::path&, auto complete) {
          lateSelection = std::move(complete);
          return true;
        };
        transientSession.m_path = dirA;
        transientTab.selectedPaths = {fileA.string()};
        transient->queryStatusFreeSpaceAsync(dirA);
        transientSession.queryDirectoryWritableAsync(dirA);
        transient->querySelectionInfoAsync(fileA);
      }
      TEST_CHECK(lateFree && lateWritable && lateSelection);
      lateFree(123456ULL);
      lateWritable(true);
      lateSelection(FileManager::SelectionInfo{.path = fileA, .type = "Late", .hasSize = true, .size = 5});
    }

    // 3. Real GIO async queries succeed and populate free space, writability, and symlink target.
    {
      Label primary, detail, freeLabel;
      FileManager manager(dirB.string());
      attachStatusLabels(manager, primary, detail, freeLabel);
      auto& tab = manager.activeTab();
      auto& session = tab.session;
      session.m_path = dirB;
      manager.queryStatusFreeSpaceAsync(dirB);
      session.queryDirectoryWritableAsync(dirB);
      TEST_CHECK(waitFor([&] {
        return tab.m_statusFreeSpaceBytes.has_value() && session.m_directoryWritable.has_value();
      }));
      TEST_CHECK(manager.currentDirectoryWritable());
      TEST_CHECK(freeLabel.text().find("free") != std::string::npos);

      session.m_entries = {
          FileEntry{.name = "beta-link", .absPath = linkB, .isDir = false, .size = 0, .type = {}},
      };
      tab.selectedPaths = {linkB.string()};
      manager.updateStatusBar();
      TEST_CHECK(waitFor([&] { return tab.m_selectionInfo.has_value() && tab.m_selectionInfo->path == linkB; }));
      TEST_CHECK(tab.m_selectionInfo->symlink);
      TEST_CHECK(tab.m_selectionInfo->target == fileB.string());
      TEST_CHECK(detail.text().find("Target: " + fileB.string()) != std::string::npos);

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
      TEST_CHECK(placePending.size() == 1 && placePending[0].first == missingDir);
      TEST_CHECK(manager.m_places.m_savedPlaces.empty());
      placePending[0].second(FileManager::PlacePathResolution{.exists = false, .isDirectory = false});
      TEST_CHECK(manager.m_places.m_savedPlaces.empty());
      TEST_CHECK(manager.m_statusMessage == "This folder is unavailable");

      // Valid folder is pinned once resolved; symlink to the same canonical target is detected as duplicate.
      manager.m_places.savePlace(realDir);
      TEST_CHECK(placePending.size() == 2 && placePending[1].first == realDir);
      placePending[1].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realDir}
      );
      TEST_CHECK(manager.m_places.m_savedPlaces.size() == 1);
      {
        const auto isolatedStateFile = env.state() / "nythera" / "state.toml";
        TEST_CHECK(std::filesystem::is_regular_file(isolatedStateFile));
        std::ifstream stateIn(isolatedStateFile);
        const std::string stateText((std::istreambuf_iterator<char>(stateIn)), std::istreambuf_iterator<char>());
        TEST_CHECK(stateText.find(realDir.string()) != std::string::npos);
        if (!realStateFile.empty()) {
          const StatSnapshot realStateAfter = snapshotStat(realStateFile);
          TEST_CHECK(realStateAfter.exists == realStateBefore.exists);
          TEST_CHECK(realStateAfter.size == realStateBefore.size);
          TEST_CHECK(realStateAfter.mtimeSec == realStateBefore.mtimeSec);
          TEST_CHECK(realStateAfter.mtimeNsec == realStateBefore.mtimeNsec);
        }
      }

      manager.m_places.savePlace(linkDir);
      TEST_CHECK(placePending.size() == 3 && placePending[2].first == linkDir);
      placePending[2].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realDir}
      );
      TEST_CHECK(manager.m_places.samePersonalPlace(realDir, linkDir));
      TEST_CHECK(manager.m_places.m_savedPlaces.size() == 1);
      TEST_CHECK(manager.m_statusMessage == "This folder is already saved in Places");

      // Stale place resolution arriving after cancelPathResolutions() is ignored.
      const auto slowDir = root / "slow-place";
      manager.m_places.savePlace(slowDir);
      TEST_CHECK(placePending.size() == 4 && placePending[3].first == slowDir);
      manager.m_places.cancelPathResolutions();
      placePending[3].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = slowDir}
      );
      TEST_CHECK(manager.m_places.m_savedPlaces.size() == 1);

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
      TEST_CHECK(latePlace);
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
      manager.m_transfers.m_dropValidationHook = [&](const std::vector<std::string>&, const std::filesystem::path&,
                                                     auto done) {
        dropPending.push_back(std::move(done));
        return true;
      };

      // Hover validation: fail-closed while pending, stale target A ignored after moving to target B.
      manager.m_transfers.m_dragSources = sources;
      manager.m_transfers.m_dragTargetValid = false;
      manager.m_transfers.requestHoverDropValidation(sources, destA);
      TEST_CHECK(!manager.m_transfers.m_dragTargetValid);
      TEST_CHECK(dropPending.size() == 1);

      manager.m_transfers.requestHoverDropValidation(sources, destB);
      TEST_CHECK(dropPending.size() == 2);

      // Stale completion for destA must be ignored.
      dropPending[0](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destA,
          .target = destA,
          .copyByDefault = true,
      });
      TEST_CHECK(!manager.m_transfers.m_dropValidation.has_value());

      // Current completion for destB is cached.
      dropPending[1](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destB,
          .target = destB,
          .copyByDefault = false,
      });
      TEST_CHECK(manager.m_transfers.m_dropValidation.has_value());
      TEST_CHECK(manager.m_transfers.m_dropValidation->target == destB);
      TEST_CHECK(!manager.m_transfers.m_dropValidation->copyByDefault);

      // Protocol drop rejected when navigation changes while validation is pending.
      dropPending.clear();
      manager.m_transfers.cancelDropValidation();
      manager.m_transfers.m_dragTargetValid = true;
      manager.m_transfers.m_protocolDropDestination = destA;
      auto receiverNav = manager.m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
      TEST_CHECK(receiverNav);
      std::optional<bool> navDropResult;
      receiverNav(filemanager::encodeFileTransfer(sources, false), [&](bool ok) { navDropResult = ok; });
      TEST_CHECK(dropPending.size() == 1);
      TEST_CHECK(!navDropResult.has_value());
      ++manager.activeSession().m_navigationGeneration;
      dropPending[0](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destA,
          .target = destA,
          .copyByDefault = false,
      });
      dropPending.clear();
      TEST_CHECK(navDropResult.has_value() && !*navDropResult);
      TEST_CHECK(manager.m_operations.operations().empty());

      // Protocol drop rejected when target is unsafe/unavailable (target == std::nullopt).
      manager.m_transfers.cancelDropValidation();
      manager.m_transfers.m_dragTargetValid = true;
      manager.m_transfers.m_protocolDropDestination = destA;
      auto receiverUnsafe = manager.m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
      TEST_CHECK(receiverUnsafe);
      std::optional<bool> unsafeDropResult;
      receiverUnsafe(filemanager::encodeFileTransfer(sources, false), [&](bool ok) { unsafeDropResult = ok; });
      TEST_CHECK(dropPending.size() == 1);
      dropPending[0](FileManager::DropValidation{
          .sources = sources,
          .rawDestination = destA,
          .target = std::nullopt,
          .copyByDefault = true,
      });
      TEST_CHECK(unsafeDropResult.has_value() && !*unsafeDropResult);
      TEST_CHECK(manager.m_operations.operations().empty());

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
        TEST_CHECK(recv);
        recv(filemanager::encodeFileTransfer(sources, false), [&](bool ok) { destroyedDropResult = ok; });
        TEST_CHECK(lateDrop);
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
      TEST_CHECK(destroyedDropResult.has_value() && !*destroyedDropResult);
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
      TEST_CHECK(::lstat(pastedFile.c_str(), &pastedStat) == 0);

      // Lexical fast path requires no filesystem I/O.
      FileManager manager(linkTarget.string());
      manager.m_windowOpen = true;
      TEST_CHECK(manager.m_places.sameDirectoryPathCached(realTarget, realTarget / ".") == true);
      TEST_CHECK(!manager.m_places.sameDirectoryPathCached(linkTarget, realTarget).has_value());

      // Real async resolution confirms symlink equivalence and distinguishes different directories.
      std::optional<bool> sameViaSymlink;
      manager.m_places.compareDirectoriesAsync(linkTarget, realTarget, [&](bool eq) { sameViaSymlink = eq; });
      TEST_CHECK(waitFor([&] { return sameViaSymlink.has_value(); }));
      TEST_CHECK(*sameViaSymlink);

      std::optional<bool> diffDirs;
      manager.m_places.compareDirectoriesAsync(linkTarget, otherTarget, [&](bool eq) { diffDirs = eq; });
      TEST_CHECK(waitFor([&] { return diffDirs.has_value(); }));
      TEST_CHECK(!*diffDirs);

      // Delayed comparison in recordPasteCompletion: non-blocking, selects when resolved,
      // and ignores stale completions after navigation.
      FileManager delayed(linkTarget.string());
      delayed.m_windowOpen = true;
      auto& delayedTab = delayed.activeTab();
      auto& delayedSession = delayedTab.session;
      delayedSession.m_path = linkTarget;
      delayedSession.m_entries = {
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
      delayed.m_transfers.m_pasteRequests[opId] = {
          delayedTab.id, realTarget, delayedSession.m_navigationGeneration, std::nullopt
      };
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
      TEST_CHECK(!delayed.recordPasteCompletion(op));
      TEST_CHECK(pendingCmp.size() == 2);
      TEST_CHECK(delayedTab.selectedPaths.empty());

      // Resolve both paths to the same canonical target: paste selection applies cleanly.
      pendingCmp[0].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      pendingCmp[1].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      TEST_CHECK(delayedTab.selectedPaths == std::unordered_set<std::string>{(linkTarget / "pasted.txt").string()});

      // Navigation while comparison is pending must discard stale paste selection.
      pendingCmp.clear();
      delayed.m_places.m_placePathCache.clear();
      delayedTab.selectedPaths.clear();
      const std::uint64_t opId2 = 43;
      delayed.m_transfers.m_pasteRequests[opId2] = {
          delayedTab.id, realTarget, delayedSession.m_navigationGeneration, std::nullopt
      };
      op.id = opId2;
      TEST_CHECK(!delayed.recordPasteCompletion(op));
      TEST_CHECK(pendingCmp.size() == 2);
      ++delayedSession.m_navigationGeneration;
      pendingCmp[0].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      pendingCmp[1].second(
          FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = realTarget}
      );
      TEST_CHECK(delayedTab.selectedPaths.empty());

      // Destruction while comparison is pending must not block or UAF.
      std::vector<std::function<void(FileManager::PlacePathResolution)>> lateCmp;
      {
        auto transient = std::make_unique<FileManager>(linkTarget.string());
        transient->m_windowOpen = true;
        transient->activeSession().m_path = linkTarget;
        transient->m_places.m_placePathResolveHook = [&](const std::filesystem::path&, auto done) {
          lateCmp.push_back(std::move(done));
          return true;
        };
        transient->m_places.compareDirectoriesAsync(linkTarget, realTarget, [](bool) {});
        TEST_CHECK(lateCmp.size() == 2);
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
      TEST_CHECK(::lstat(regFile.c_str(), &regStat) == 0);
      TEST_CHECK(::lstat(subDir.c_str(), &dirStat) == 0);
      TEST_CHECK(::lstat(symFile.c_str(), &symStat) == 0);

      FileManager manager(enumDir.string());
      manager.m_windowOpen = true;
      auto& tab = manager.activeTab();
      auto& session = tab.session;
      manager.enumerate(enumDir);
      TEST_CHECK(waitFor([&] { return !session.m_loading && session.m_entries.size() == 3; }));
      session.closeMonitor();

      // Verify enumerate() populated (device, inode) on regular files and directories,
      // and flagged the symlink without polluting it with the target's inode.
      const auto regIt = std::ranges::find(session.m_entries, regFile, &FileEntry::absPath);
      const auto dirIt = std::ranges::find(session.m_entries, subDir, &FileEntry::absPath);
      const auto symIt = std::ranges::find(session.m_entries, symFile, &FileEntry::absPath);
      TEST_CHECK(regIt != session.m_entries.end() && regIt->hasIdentity && !regIt->isSymlink);
      TEST_CHECK(
          regIt->device == static_cast<std::uint64_t>(regStat.st_dev)
          && regIt->inode == static_cast<std::uint64_t>(regStat.st_ino)
      );
      TEST_CHECK(dirIt != session.m_entries.end() && dirIt->hasIdentity && !dirIt->isSymlink);
      TEST_CHECK(
          dirIt->device == static_cast<std::uint64_t>(dirStat.st_dev)
          && dirIt->inode == static_cast<std::uint64_t>(dirStat.st_ino)
      );
      TEST_CHECK(symIt != session.m_entries.end() && symIt->isSymlink && !symIt->hasIdentity);

      // Matching regular file + directory restores selection synchronously from enumerated identity.
      tab.selectedPaths.clear();
      tab.pendingPasteSelection = FileManager::PasteSelection{
          .request = {tab.id, enumDir, session.m_navigationGeneration, std::nullopt},
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
      TEST_CHECK(tab.selectedPaths == std::unordered_set<std::string>{regFile.string(), subDir.string()});

      // Mismatched inode (e.g. replaced file) must NOT be selected by filename alone.
      tab.selectedPaths.clear();
      tab.pendingPasteSelection = FileManager::PasteSelection{
          .request = {tab.id, enumDir, session.m_navigationGeneration, std::nullopt},
          .entries = {
              {.path = regFile.string(),
               .device = static_cast<std::uint64_t>(regStat.st_dev),
               .inode = static_cast<std::uint64_t>(regStat.st_ino) + 999999ULL},
          },
      };
      manager.applyPendingPasteSelection();
      TEST_CHECK(tab.selectedPaths.empty());

      // Symlink selection resolves NOFOLLOW (device, inode) asynchronously and matches the symlink's own inode.
      tab.selectedPaths.clear();
      tab.pendingPasteSelection = FileManager::PasteSelection{
          .request = {tab.id, enumDir, session.m_navigationGeneration, std::nullopt},
          .entries = {
              {.path = symFile.string(),
               .device = static_cast<std::uint64_t>(symStat.st_dev),
               .inode = static_cast<std::uint64_t>(symStat.st_ino)},
          },
      };
      manager.applyPendingPasteSelection();
      TEST_CHECK(waitFor([&] { return tab.selectedPaths == std::unordered_set<std::string>{symFile.string()}; }));

      // Stale navigation generation must never restore selection.
      tab.selectedPaths.clear();
      tab.pendingPasteSelection = FileManager::PasteSelection{
          .request = {tab.id, enumDir, session.m_navigationGeneration, std::nullopt},
          .entries = {
              {.path = regFile.string(),
               .device = static_cast<std::uint64_t>(regStat.st_dev),
               .inode = static_cast<std::uint64_t>(regStat.st_ino)},
          },
      };
      ++session.m_navigationGeneration;
      manager.applyPendingPasteSelection();
      TEST_CHECK(tab.selectedPaths.empty());
    }
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-fm-async-fs"});
    FileManagerTestAccess::run(env);
  });
}
