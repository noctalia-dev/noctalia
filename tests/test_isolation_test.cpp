#include "filemanager/file_manager.h"
#include "filemanager/file_operation_manager.h"
#include "filemanager/wastebin_fallback.h"
#include "test_isolation.h"
#include "util/file_utils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <iterator>
#include <print>
#include <string>
#include <sys/stat.h>

class FileManagerTestAccess {
public:
  static void runSavedPlacesCheck(const nythera::test::ScopedTestEnv& env) {
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
    const StatSnapshot realBefore = realStateFile.empty() ? StatSnapshot{} : snapshotStat(realStateFile);

    const auto pinnedDir = env.work() / "pinned-place";
    std::filesystem::create_directory(pinnedDir);

    FileManager manager(env.work().string());
    bool resolved = false;
    manager.m_places.m_placePathResolveHook = [&](const std::filesystem::path&, auto done) {
      done(FileManager::PlacePathResolution{.exists = true, .isDirectory = true, .canonicalPath = pinnedDir});
      resolved = true;
      return true;
    };
    manager.m_places.savePlace(pinnedDir);
    TEST_CHECK(resolved);
    TEST_CHECK(manager.m_places.m_savedPlaces.size() == 1);

    const auto isolatedStateFile = env.state() / "nythera" / "state.toml";
    TEST_CHECK(std::filesystem::is_regular_file(isolatedStateFile));
    std::ifstream in(isolatedStateFile);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TEST_CHECK(content.find(pinnedDir.string()) != std::string::npos);

    if (!realStateFile.empty()) {
      const StatSnapshot realAfter = snapshotStat(realStateFile);
      TEST_CHECK(realAfter.exists == realBefore.exists);
      TEST_CHECK(realAfter.size == realBefore.size);
      TEST_CHECK(realAfter.mtimeSec == realBefore.mtimeSec);
      TEST_CHECK(realAfter.mtimeNsec == realBefore.mtimeNsec);
    }
  }

  static void verifyTrashBrowsingFailsClosed(const nythera::test::ScopedTestEnv& env) {
    FileManager manager("trash:/");
    bool timedOut = false;
    const auto timer = g_timeout_add(
        5000,
        [](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &timedOut
    );
    while (!timedOut && manager.activeSession().m_loading)
      g_main_context_iteration(nullptr, TRUE);
    if (!timedOut)
      g_source_remove(timer);
    TEST_CHECK(!timedOut);
    TEST_CHECK(manager.activeSession().browsingTrash());
    TEST_CHECK(manager.activeSession().entries().empty());
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::stateDir(), env.root()));
  }
};

namespace {

  bool waitForOp(FileOperationManager& manager, std::uint64_t id) {
    TEST_CHECK(id != 0);
    bool timedOut = false;
    const auto timer = g_timeout_add(
        5000,
        [](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &timedOut
    );
    auto isFinished = [&] {
      for (const auto& op : manager.operations()) {
        if (op.id == id && op.finished)
          return true;
      }
      return false;
    };
    while (!timedOut && !isFinished())
      g_main_context_iteration(nullptr, TRUE);
    if (!timedOut)
      g_source_remove(timer);
    TEST_CHECK(!timedOut && isFinished());
    for (const auto& op : manager.operations()) {
      if (op.id == id)
        return op.succeeded;
    }
    return false;
  }

  void testEnvironmentAndDbusIsolation() {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-isolation-env"});
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::stateDir(), env.root()));
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::configDir(), env.root()));
    TEST_CHECK(std::filesystem::path(g_getenv("HOME")) == env.home());
    TEST_CHECK(std::filesystem::path(g_getenv("NYTHERA_CONFIG_HOME")) == env.config());
    TEST_CHECK(std::filesystem::path(g_getenv("NYTHERA_STATE_HOME")) == env.state());
    TEST_CHECK(std::filesystem::path(g_getenv("NYTHERA_DATA_HOME")) == env.data());
    TEST_CHECK(std::filesystem::path(g_getenv("XDG_CONFIG_HOME")) == env.config());
    TEST_CHECK(std::filesystem::path(g_getenv("XDG_STATE_HOME")) == env.state());
    TEST_CHECK(std::filesystem::path(g_getenv("XDG_DATA_HOME")) == env.data());
    TEST_CHECK(std::filesystem::path(g_getenv("XDG_CACHE_HOME")) == env.cache());
    TEST_CHECK(std::filesystem::path(g_getenv("XDG_RUNTIME_DIR")) == env.runtime());
    TEST_CHECK(std::string_view(g_getenv("GIO_USE_VFS")) == "local");
    TEST_CHECK(filemanager::wastebin_fallback::productionWastebinForbidden());

    GError* busError = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &busError);
    TEST_CHECK(bus == nullptr);
    g_clear_error(&busError);
  }

  void testFailClosedWastebinGuards() {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-isolation-wastebin"});

    {
      FileOperationManager unmocked;
      const auto emptyId = unmocked.emptyWastebin(true);
      TEST_CHECK(!waitForOp(unmocked, emptyId));
      TEST_CHECK(unmocked.operations().back().status.find("disabled in test isolation mode") != std::string::npos);

      const auto restoreId = unmocked.restore({"trash:///nonexistent-item"});
      TEST_CHECK(!waitForOp(unmocked, restoreId));
      TEST_CHECK(!unmocked.operations().back().succeeded);
    }

    {
      const auto restoreDest = env.work() / "forged-restore-target.txt";
      FileOperationManager forgedMgr(
          [](const FileOperationManager::Operation&) {},
          FileOperationManager::WastebinBackend{
              .enumerateTopLevel =
                  [](GCancellable*, std::vector<std::string>& uris, std::string&) {
                    uris.push_back("trash:///forged-entry");
                    return true;
                  },
              .originalPath =
                  [&](const std::string&, GCancellable*, std::string& original, std::string&) {
                    original = restoreDest.string();
                    return true;
                  },
          }
      );
      const auto emptyId = forgedMgr.emptyWastebin(true);
      TEST_CHECK(!waitForOp(forgedMgr, emptyId));
      TEST_CHECK(forgedMgr.operations().back().status.find("disabled in test isolation mode") != std::string::npos);

      const auto restoreId = forgedMgr.restore({"trash:///forged-entry"});
      TEST_CHECK(!waitForOp(forgedMgr, restoreId));
      TEST_CHECK(!std::filesystem::exists(restoreDest));
    }

    {
      nythera::test::ScopedTempDir outsideDir("nythera-isolation-outside");
      const auto outsideFile = outsideDir.path() / "preserve.txt";
      {
        std::ofstream out(outsideFile);
        out << "preserve-outside";
        TEST_CHECK(out.good());
      }

      FileOperationManager mgr;
      const auto trashOutsideId = mgr.trash({outsideFile.string()});
      TEST_CHECK(!waitForOp(mgr, trashOutsideId));
      TEST_CHECK(std::filesystem::is_regular_file(outsideFile));

      {
        nythera::test::ScopedEnvVar xdgOutside("XDG_DATA_HOME", outsideDir.path().string());
        std::filesystem::path rootOut;
        std::filesystem::path infoOut;
        std::filesystem::path filesOut;
        GError* err = nullptr;
        TEST_CHECK(!filemanager::wastebin_fallback::ensureHomeWastebinDirs(rootOut, infoOut, filesOut, &err));
        TEST_CHECK(err != nullptr);
        g_clear_error(&err);
        TEST_CHECK(!std::filesystem::exists(outsideDir.path() / "Trash"));

        const auto envFile = env.work() / "inside-file.txt";
        {
          std::ofstream out(envFile);
          out << "inside-keep";
          TEST_CHECK(out.good());
        }
        const auto mismatchedTrashId = mgr.trash({envFile.string()});
        TEST_CHECK(!waitForOp(mgr, mismatchedTrashId));
        TEST_CHECK(std::filesystem::is_regular_file(envFile));
      }
    }

    FileManagerTestAccess::verifyTrashBrowsingFailsClosed(env);
  }

  void testControlledFailureAndRaiiCleanup() {
    const auto getenvOpt = [](const char* name) -> std::optional<std::string> {
      if (const char* v = std::getenv(name); v != nullptr)
        return std::string(v);
      return std::nullopt;
    };
    const auto outerHome = getenvOpt("HOME");
    const auto outerXdgData = getenvOpt("XDG_DATA_HOME");
    const auto outerDbus = getenvOpt("DBUS_SESSION_BUS_ADDRESS");
    const auto outerVfs = getenvOpt("GIO_USE_VFS");

    std::filesystem::path capturedInnerRoot;
    std::println(stderr, "Note: intentional TEST_CHECK(false) below verifies RAII failure cleanup.");
    const int rc = nythera::test::runTest([&] {
      nythera::test::ScopedTestEnv innerEnv({.prefix = "nythera-isolation-fail"});
      capturedInnerRoot = innerEnv.root();
      const auto lockedSub = innerEnv.work() / "locked-0000";
      std::filesystem::create_directory(lockedSub);
      {
        std::ofstream out(lockedSub / "payload.txt");
        out << "cleanup-me";
        TEST_CHECK(out.good());
      }
      TEST_CHECK(::chmod(lockedSub.c_str(), 0000) == 0);

      FileOperationManager opMgr;
      const auto createdPath = innerEnv.work() / "created.txt";
      TEST_CHECK(waitForOp(opMgr, opMgr.createFile(createdPath.string())));
      TEST_CHECK(std::filesystem::is_regular_file(createdPath));

      TEST_CHECK(false);
    });

    TEST_CHECK(rc == EXIT_FAILURE);
    TEST_CHECK(!capturedInnerRoot.empty());
    TEST_CHECK(!std::filesystem::exists(capturedInnerRoot));
    TEST_CHECK(getenvOpt("HOME") == outerHome);
    TEST_CHECK(getenvOpt("XDG_DATA_HOME") == outerXdgData);
    TEST_CHECK(getenvOpt("DBUS_SESSION_BUS_ADDRESS") == outerDbus);
    TEST_CHECK(getenvOpt("GIO_USE_VFS") == outerVfs);
  }

  void testValidateRealNfsMountRoot() {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-isolation-nfs"});
    const auto regularFile = env.work() / "regular.txt";
    {
      std::ofstream out(regularFile);
      out << "not-a-directory";
      TEST_CHECK(out.good());
    }

    std::filesystem::path out;
    std::string err;
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot(nullptr, env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("relative/path", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("/", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("/mnt", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("/tmp", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("/var/tmp", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("/etc", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot("/tmp/../mnt", env.realUserHome(), out, err));
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot(env.work().c_str(), env.realUserHome(), out, err));
    TEST_CHECK(
        !nythera::test::validateRealNfsMountRoot((env.work() / "missing").c_str(), env.realUserHome(), out, err)
    );
    TEST_CHECK(!nythera::test::validateRealNfsMountRoot(regularFile.c_str(), env.realUserHome(), out, err));

    if (!env.realUserHome().empty() && std::filesystem::is_directory(env.realUserHome())) {
      TEST_CHECK(!nythera::test::validateRealNfsMountRoot(env.realUserHome().c_str(), env.realUserHome(), out, err));
      const auto subHome = env.realUserHome() / ".config";
      if (std::filesystem::exists(subHome))
        TEST_CHECK(!nythera::test::validateRealNfsMountRoot(subHome.c_str(), env.realUserHome(), out, err));
    }
  }

  void testSavedPlacesConfinement() {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-isolation-places"});
    FileManagerTestAccess::runSavedPlacesCheck(env);
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    testEnvironmentAndDbusIsolation();
    testFailClosedWastebinGuards();
    testControlledFailureAndRaiiCleanup();
    testValidateRealNfsMountRoot();
    testSavedPlacesConfinement();
  });
}
