#include "filemanager/file_manager.h"
#include "test_isolation.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "util/file_utils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

  static std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  static void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    TEST_CHECK(out.good());
  }

  static void collectLabels(const Node* node, std::vector<std::string>& out) {
    if (node == nullptr)
      return;
    if (const auto* label = dynamic_cast<const Label*>(node))
      out.push_back(label->text());
    for (const auto& child : node->children())
      collectLabels(child.get(), out);
  }

  static void run(const nythera::test::ScopedTestEnv& env) {
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::stateDir(), env.root()));
    TEST_CHECK(nythera::test::isPathWithinRoot(FileUtils::configDir(), env.root()));
    {
      GError* busError = nullptr;
      GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &busError);
      TEST_CHECK(bus == nullptr);
      g_clear_error(&busError);
    }

    const auto& root = env.work();
    // 1. Display sanitization escapes control chars and Unicode bidi overrides while keeping normal Unicode.
    {
      const std::string sanitized = FileManager::sanitizeConflictDisplayText(
          "evil\n\nFolder\r\t\xE2\x80\xAE"
          "fdp.exe | résumé 日本語 🚀"
      );
      TEST_CHECK(sanitized.find('\n') == std::string::npos);
      TEST_CHECK(sanitized.find('\r') == std::string::npos);
      TEST_CHECK(sanitized.find("\\n\\nFolder\\r\\t") != std::string::npos);
      TEST_CHECK(sanitized.find("<U+202E>") != std::string::npos);
      TEST_CHECK(sanitized.find("résumé 日本語 🚀") != std::string::npos);
    }

    const auto srcDir = root / "src";
    const auto dstDir = root / "dst";
    std::filesystem::create_directory(srcDir);
    std::filesystem::create_directory(dstDir);

    // Adversarial filename with double newlines, carriage return, fake "Folder" metadata,
    // fake path text, Unicode bidi override (U+202E), and delimiter-like characters.
    const std::string evilName = "report\n\nFolder\r\nfake-dest-path | \xE2\x80\xAE"
                                 "txt.sh";
    const std::string unicodeName = "café\nline2\r·日本語.txt";

    // 2. Structured conflict data and buildDialog() card verification + Replace decision.
    {
      const auto srcFile = srcDir / evilName;
      const auto dstFile = dstDir / evilName;
      writeFile(srcFile, "NEW_EVIL_CONTENT");
      writeFile(dstFile, "OLD_EVIL_CONTENT");

      FileManager manager(dstDir.string());
      std::optional<FileManager::ConflictDialogData> captured;
      bool dialogChecksOk = true;
      manager.m_modalDialog.m_conflictDialogHook = [&](const FileManager::ConflictDialogData& data, bool,
                                                       auto respond) {
        captured = data;
        // Build the actual dialog widget tree from the structured conflict data to verify no card corruption.
        manager.m_modalDialog.m_title = "File already exists";
        manager.m_modalDialog.m_alternativeNameEnabled = true;
        manager.m_modalDialog.m_conflict = data;
        manager.m_modalDialog.buildDialog(640.0F);
        dialogChecksOk = dialogChecksOk && manager.m_modalDialog.m_cards.size() == 2;
        std::vector<std::string> incomingLabels;
        std::vector<std::string> existingLabels;
        if (manager.m_modalDialog.m_cards.size() == 2) {
          collectLabels(manager.m_modalDialog.m_cards[0], incomingLabels);
          collectLabels(manager.m_modalDialog.m_cards[1], existingLabels);
        }
        dialogChecksOk = dialogChecksOk && !incomingLabels.empty() && incomingLabels.front() == "INCOMING ITEM";
        dialogChecksOk = dialogChecksOk && !existingLabels.empty() && existingLabels.front() == "EXISTING DESTINATION";
        if (!incomingLabels.empty()) {
          dialogChecksOk = dialogChecksOk
              && incomingLabels.back().find(FileManager::sanitizeConflictDisplayText(srcFile.string()))
                  != std::string::npos;
        }
        if (!existingLabels.empty()) {
          dialogChecksOk = dialogChecksOk
              && existingLabels.back().find(FileManager::sanitizeConflictDisplayText(dstFile.string()))
                  != std::string::npos;
          dialogChecksOk = dialogChecksOk && existingLabels.back().find('\r') == std::string::npos;
          dialogChecksOk = dialogChecksOk && existingLabels.back().find("\n\n") == std::string::npos;
        }
        manager.m_modalDialog.m_cards.clear();
        manager.m_modalDialog.m_root.reset();
        manager.m_modalDialog.m_conflict.reset();

        respond(1, {}); // Replace
        return true;
      };

      const auto opId = manager.m_operations.copy({srcFile.string()}, dstDir.string());
      TEST_CHECK(opId != 0);
      TEST_CHECK(waitFor([&] {
        const auto ops = manager.m_operations.operations();
        return !ops.empty() && ops.back().id == opId && ops.back().finished;
      }));

      TEST_CHECK(dialogChecksOk);
      TEST_CHECK(captured.has_value());
      TEST_CHECK(captured->kind == FileManager::ConflictDialogKind::File);
      TEST_CHECK(captured->incoming.name == evilName);
      TEST_CHECK(captured->incoming.path == srcFile);
      TEST_CHECK(!captured->incoming.isDirectory);
      TEST_CHECK(captured->existing.name == evilName);
      TEST_CHECK(captured->existing.path == dstFile);
      TEST_CHECK(!captured->existing.isDirectory);
      TEST_CHECK(readFile(dstFile) == "NEW_EVIL_CONTENT");
    }

    // 3. Skip decision leaves the exact adversarial destination untouched.
    {
      const auto srcFile = srcDir / evilName;
      const auto dstFile = dstDir / evilName;
      writeFile(srcFile, "SHOULD_NOT_OVERWRITE");
      writeFile(dstFile, "KEEP_ME");

      FileManager manager(dstDir.string());
      bool namesMatched = true;
      manager.m_modalDialog.m_conflictDialogHook = [&](const FileManager::ConflictDialogData& data, bool,
                                                       auto respond) {
        namesMatched = namesMatched && data.incoming.path.filename().string() == data.existing.path.filename().string();
        respond(3, {}); // Skip
        return true;
      };

      const auto opId = manager.m_operations.copy({srcFile.string()}, dstDir.string());
      TEST_CHECK(opId != 0);
      TEST_CHECK(waitFor([&] {
        const auto ops = manager.m_operations.operations();
        return !ops.empty() && ops.back().id == opId && ops.back().finished;
      }));
      TEST_CHECK(namesMatched);
      TEST_CHECK(readFile(dstFile) == "KEEP_ME");
    }

    // 4. Rename (Keep Both) decision with explicit alternative name preserves existing destination.
    {
      const auto srcFile = srcDir / evilName;
      const auto dstFile = dstDir / evilName;
      const auto renamedDst = dstDir / "renamed-safe.txt";
      writeFile(srcFile, "RENAMED_PAYLOAD");
      writeFile(dstFile, "ORIGINAL_PAYLOAD");

      FileManager manager(dstDir.string());
      manager.m_modalDialog.m_conflictDialogHook = [](const FileManager::ConflictDialogData&, bool, auto respond) {
        respond(2, "renamed-safe.txt"); // Rename / Keep Both
        return true;
      };

      const auto opId = manager.m_operations.copy({srcFile.string()}, dstDir.string());
      TEST_CHECK(opId != 0);
      TEST_CHECK(waitFor([&] {
        const auto ops = manager.m_operations.operations();
        return !ops.empty() && ops.back().id == opId && ops.back().finished;
      }));
      TEST_CHECK(readFile(dstFile) == "ORIGINAL_PAYLOAD");
      TEST_CHECK(std::filesystem::exists(renamedDst));
      TEST_CHECK(readFile(renamedDst) == "RENAMED_PAYLOAD");
    }

    // 5. Apply to All (Replace all = choice 11) resolves multiple adversarial conflicts with one prompt.
    {
      const auto src1 = srcDir / evilName;
      const auto dst1 = dstDir / evilName;
      const auto src2 = srcDir / unicodeName;
      const auto dst2 = dstDir / unicodeName;
      writeFile(src1, "BATCH_NEW_1");
      writeFile(dst1, "BATCH_OLD_1");
      writeFile(src2, "BATCH_NEW_2");
      writeFile(dst2, "BATCH_OLD_2");

      FileManager manager(dstDir.string());
      int prompts = 0;
      bool sawRemaining = true;
      manager.m_modalDialog.m_conflictDialogHook = [&](const FileManager::ConflictDialogData&, bool hasRemaining,
                                                       auto respond) {
        ++prompts;
        sawRemaining = sawRemaining && hasRemaining;
        respond(11, {}); // Replace + Apply to All
        return true;
      };

      const auto opId = manager.m_operations.copy({src1.string(), src2.string()}, dstDir.string());
      TEST_CHECK(opId != 0);
      TEST_CHECK(waitFor([&] {
        const auto ops = manager.m_operations.operations();
        return !ops.empty() && ops.back().id == opId && ops.back().finished;
      }));
      TEST_CHECK(prompts == 1 && sawRemaining);
      TEST_CHECK(readFile(dst1) == "BATCH_NEW_1");
      TEST_CHECK(readFile(dst2) == "BATCH_NEW_2");
    }

    // 6. Cancellation while a conflict dialogue is pending completes cleanly without modifying destination.
    {
      const auto srcFile = srcDir / evilName;
      const auto dstFile = dstDir / evilName;
      writeFile(srcFile, "CANCEL_NEW");
      writeFile(dstFile, "CANCEL_KEEP");

      FileManager manager(dstDir.string());
      std::function<void(int, std::string)> pendingRespond;
      manager.m_modalDialog.m_conflictDialogHook = [&](const FileManager::ConflictDialogData&, bool, auto respond) {
        pendingRespond = std::move(respond);
        return true;
      };

      const auto opId = manager.m_operations.copy({srcFile.string()}, dstDir.string());
      TEST_CHECK(opId != 0);
      TEST_CHECK(waitFor([&] { return static_cast<bool>(pendingRespond); }));
      manager.m_operations.cancel(opId);
      TEST_CHECK(waitFor([&] {
        const auto ops = manager.m_operations.operations();
        return !ops.empty() && ops.back().id == opId && ops.back().finished;
      }));
      TEST_CHECK(readFile(dstFile) == "CANCEL_KEEP");
      // Responding after cancellation must be a safe no-op.
      pendingRespond(1, {});
    }

    // 7. Destroying FileManager while a conflict dialogue is pending does not deadlock or UAF.
    {
      const auto srcFile = srcDir / evilName;
      const auto dstFile = dstDir / evilName;
      writeFile(srcFile, "DESTROY_NEW");
      writeFile(dstFile, "DESTROY_KEEP");

      std::function<void(int, std::string)> lateRespond;
      {
        auto transient = std::make_unique<FileManager>(dstDir.string());
        transient->m_modalDialog.m_conflictDialogHook = [&](const FileManager::ConflictDialogData&, bool,
                                                            auto respond) {
          lateRespond = std::move(respond);
          return true;
        };
        const auto opId = transient->m_operations.copy({srcFile.string()}, dstDir.string());
        TEST_CHECK(opId != 0);
        TEST_CHECK(waitFor([&] { return static_cast<bool>(lateRespond); }));
      }
      TEST_CHECK(readFile(dstFile) == "DESTROY_KEEP");
      lateRespond(1, {});
    }

    // 8. Non-conflict UI surfaces (status bar, symlink target, breadcrumbs) sanitize control/bidi
    //    characters for display while preserving exact underlying filesystem paths.
    {
      const std::string trickyDirName = "dir\n\xE2\x80\xAE"
                                        "spoof";
      const std::string trickyFileName = "doc\r\n\xE2\x80\xA8\xE2\x80\xAE"
                                         "fdp.exe";
      const auto trickyDir = root / trickyDirName;
      const auto trickyFile = trickyDir / trickyFileName;
      std::filesystem::create_directory(trickyDir);
      writeFile(trickyFile, "exact");

      Label statusPrimary, statusDetail, statusFree;
      Flex breadcrumbBar;
      FileManager manager(trickyDir.string());
      manager.m_statusLabel = &statusPrimary;
      manager.m_statusDetailLabel = &statusDetail;
      manager.m_statusFreeLabel = &statusFree;
      manager.m_breadcrumbBar = &breadcrumbBar;
      auto& tab = manager.activeTab();
      auto& session = tab.session;
      session.m_path = trickyDir;
      session.m_entries = {
          FileEntry{.name = trickyFileName, .absPath = trickyFile, .isDir = false, .size = 5},
      };
      tab.selectedPaths = {trickyFile.string()};
      tab.m_selectionInfo = FileManager::SelectionInfo{
          .path = trickyFile,
          .type = "Plain text document (symbolic link)",
          .target = (trickyDir
                     / "target\n\xE2\x80\xAE"
                       "txt")
                        .string(),
          .symlink = true,
          .hasSize = true,
          .size = 5,
      };
      manager.updateStatusBar();
      manager.rebuildBreadcrumbs();

      // Status bar and symlink target are sanitized for display.
      TEST_CHECK(statusPrimary.text().find('\n') == std::string::npos);
      TEST_CHECK(statusPrimary.text().find('\r') == std::string::npos);
      TEST_CHECK(statusPrimary.text().find("\\r\\n<U+2028><U+202E>fdp.exe") != std::string::npos);
      TEST_CHECK(statusDetail.text().find('\n') == std::string::npos);
      TEST_CHECK(statusDetail.text().find("<U+202E>") != std::string::npos);

      // Breadcrumb labels are sanitized while m_path and m_entries keep exact raw paths.
      std::vector<std::string> crumbLabels;
      collectLabels(&breadcrumbBar, crumbLabels);
      TEST_CHECK(!crumbLabels.empty());
      for (const auto& text : crumbLabels) {
        TEST_CHECK(text.find('\n') == std::string::npos);
        TEST_CHECK(text.find("\xE2\x80\xAE") == std::string::npos);
      }
      TEST_CHECK(session.m_path == trickyDir);
      TEST_CHECK(session.m_entries[0].absPath == trickyFile);
      TEST_CHECK(session.m_entries[0].name == trickyFileName);

      manager.m_statusLabel = nullptr;
      manager.m_statusDetailLabel = nullptr;
      manager.m_statusFreeLabel = nullptr;
      manager.m_breadcrumbBar = nullptr;
    }
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-fm-conflict-dialog"});
    FileManagerTestAccess::run(env);
  });
}
