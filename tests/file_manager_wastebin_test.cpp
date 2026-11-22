#include "filemanager/file_manager.h"
#include "filemanager/wastebin_fallback.h"
#include "test_isolation.h"

#include <algorithm>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  class CancelledPicker final : public FileDialogPresenter {
  public:
    bool opened = false;
    bool selectFolderMode = false;
    bool openFileDialog() override {
      opened = true;
      selectFolderMode = FileDialog::currentOptions().mode == FileDialogMode::SelectFolder;
      return false; // Failure or cancellation must never start a restore.
    }
    void closeFileDialogWithoutResult() override {}
  };
} // namespace

class FileOperationManagerTestAccess {
public:
  static void useIsolatedWastebinBackend(FileOperationManager& operations) {
    operations.m_wastebinBackend.enumerateTopLevel = [](GCancellable*, std::vector<std::string>&, std::string&) {
      return true;
    };
    operations.m_wastebinBackend.originalPath = [](const std::string&, GCancellable*, std::string&,
                                                   std::string& error) {
      error = "Fixture item does not exist.";
      return false;
    };
  }
};

class FileManagerTestAccess {
public:
  static void run(FileManager& manager, const std::filesystem::path& root) {
    const auto findMenuEntry = [](const std::vector<ContextMenuControlEntry>& entries, int id) {
      return std::ranges::find(entries, id, &ContextMenuControlEntry::id);
    };

    const auto pumpOperationsAndSession = [&](std::uint64_t generation, bool waitForEnumGen) {
      bool timedOut = false;
      const auto timer = g_timeout_add(
          5000,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &timedOut
      );
      while (!timedOut
             && (manager.m_operations.active()
                 || manager.activeSession().m_loading
                 || (waitForEnumGen && manager.activeSession().m_enumerationGeneration == generation))) {
        g_main_context_iteration(nullptr, TRUE);
      }
      if (!timedOut)
        g_source_remove(timer);
      TEST_CHECK(!timedOut);
    };

    const auto initialFileMenus = manager.fileMenuEntries();
    const auto initialEditMenus = manager.editMenuEntries();
    const auto initialViewMenus = manager.viewMenuEntries();
    TEST_CHECK(findMenuEntry(initialFileMenus, 72) == initialFileMenus.end());
    TEST_CHECK(findMenuEntry(initialEditMenus, 70) == initialEditMenus.end());
    TEST_CHECK(findMenuEntry(initialEditMenus, 71) == initialEditMenus.end());
    TEST_CHECK(findMenuEntry(initialEditMenus, 72) == initialEditMenus.end());
    TEST_CHECK(findMenuEntry(initialViewMenus, 72) == initialViewMenus.end());
    manager.activateContextMenuEntry({.id = 70});
    manager.activateContextMenuEntry({.id = 71});
    TEST_CHECK(manager.m_operations.operations().empty());

    auto& tab = manager.activeTab();
    auto& session = tab.session;
    session.m_browsingTrash = true;
    const auto trashEditMenus = manager.editMenuEntries();
    const auto restoreEntry = findMenuEntry(trashEditMenus, 70);
    const auto restoreToEntry = findMenuEntry(trashEditMenus, 71);
    TEST_CHECK(restoreEntry != trashEditMenus.end() && restoreEntry->label == "Restore" && !restoreEntry->enabled);
    TEST_CHECK(
        restoreToEntry != trashEditMenus.end() && restoreToEntry->label == "Restore to…" && !restoreToEntry->enabled
    );
    TEST_CHECK(findMenuEntry(trashEditMenus, 4) == trashEditMenus.end());
    TEST_CHECK(findMenuEntry(trashEditMenus, 5) == trashEditMenus.end());
    TEST_CHECK(findMenuEntry(trashEditMenus, 6) == trashEditMenus.end());
    const auto trashViewMenus = manager.viewMenuEntries();
    TEST_CHECK(std::ranges::any_of(trashViewMenus, [](const auto& entry) {
      return entry.id == 100 && std::ranges::any_of(entry.submenuEntries, [](const auto& child) {
               return child.id == 61 && child.label == "Type";
             });
    }));
    session.m_entries = {{.name = "item", .absPath = "trash:/item"}};
    tab.selectedPaths = {"trash:/item"};
    const auto selectedTrashEditMenus = manager.editMenuEntries();
    const auto enabledRestore = findMenuEntry(selectedTrashEditMenus, 70);
    const auto enabledRestoreTo = findMenuEntry(selectedTrashEditMenus, 71);
    TEST_CHECK(enabledRestore != selectedTrashEditMenus.end() && enabledRestore->enabled);
    TEST_CHECK(enabledRestoreTo != selectedTrashEditMenus.end() && enabledRestoreTo->enabled);
    manager.activateContextMenuEntry({.id = 70}); // Stale selection has no mapped GIO entry.
    TEST_CHECK(manager.m_operations.operations().empty());

    session.m_trashUris["trash:/item"] = "trash:///item";
    {
      CancelledPicker picker;
      const auto resetPresenter = nythera::test::makeScopeExit([] { FileDialog::setPresenter(nullptr); });
      FileDialog::setPresenter(&picker);
      manager.activateContextMenuEntry({.id = 71});
      TEST_CHECK(picker.opened && picker.selectFolderMode && manager.m_operations.operations().empty());
    }

    // A disconnected display cannot show confirmation; this must fail closed.
    manager.activateContextMenuEntry({.id = 72});
    TEST_CHECK(manager.m_operations.operations().empty());
    TEST_CHECK(!manager.m_modalDialog.isOpen());

    // Fail-closed check: unmocked EmptyWastebin must be rejected in test isolation mode.
    const auto failClosedId = manager.m_operations.emptyWastebin(true);
    TEST_CHECK(failClosedId != 0);
    pumpOperationsAndSession(session.m_enumerationGeneration, false);
    const auto& failClosedOp = manager.m_operations.operations().back();
    TEST_CHECK(failClosedOp.id == failClosedId && failClosedOp.finished && !failClosedOp.succeeded);
    TEST_CHECK(failClosedOp.status.find("disabled in test isolation mode") != std::string::npos);

    // Install isolated WastebinBackend for subsequent operations.
    FileOperationManagerTestAccess::useIsolatedWastebinBackend(manager.m_operations);

    // Even a failed batch must refresh the currently browsed destination and
    // preserve its result after asynchronous enumeration completes.
    session.m_browsingTrash = false;
    tab.selectedPaths.clear();
    session.m_path = root;
    const auto generation = session.m_enumerationGeneration;
    const auto id = manager.m_operations.restore({"file:///not-a-wastebin-entry"});
    pumpOperationsAndSession(generation, true);
    TEST_CHECK(session.m_entries.size() == 1 && session.m_entries.front().name == "destination-file");
    const auto& operation = manager.m_operations.operations().back();
    TEST_CHECK(operation.id == id && operation.finished && !operation.succeeded);
    TEST_CHECK(manager.m_statusMessage == operation.status);
    TEST_CHECK(!tab.operationResultAfterRefresh);

    // Stage 4 diagnostic checks:
    // 1. A missing source or read-only parent directory must NOT offer Home Wastebin fallback.
    GError* fakeNotSupported = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Trash not supported");
    const auto missingDiag =
        filemanager::wastebin_fallback::diagnoseTrashFailure(root / "does-not-exist", fakeNotSupported);
    TEST_CHECK(!missingDiag.eligibleForHomeFallback);

    const auto roParent = root / "ro-parent";
    std::filesystem::create_directory(roParent);
    const auto roFile = roParent / "locked.txt";
    std::ofstream(roFile) << "locked";
    TEST_CHECK(::chmod(roParent.c_str(), 0500) == 0);
    auto restoreRoParent = nythera::test::makeScopeExit([&] { ::chmod(roParent.c_str(), 0700); });
    const auto roDiag = filemanager::wastebin_fallback::diagnoseTrashFailure(roFile, fakeNotSupported);
    TEST_CHECK(::chmod(roParent.c_str(), 0700) == 0);
    restoreRoParent.release();
    TEST_CHECK(!roDiag.eligibleForHomeFallback);
    TEST_CHECK(roDiag.summary.find("permission to remove items") != std::string::npos);
    g_error_free(fakeNotSupported);
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-wastebin-ui"});
    const auto filesDir = env.work() / "files";
    std::filesystem::create_directory(filesDir);
    std::ofstream(filesDir / "destination-file") << "preserve";
    FileManager manager(filesDir.string());
    FileManagerTestAccess::run(manager, filesDir);
  });
}
