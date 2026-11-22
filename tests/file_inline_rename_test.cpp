#include "filemanager/file_operation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "test_isolation.h"
#include "ui/controls/input.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  template <typename Entry> void checkEditor(std::unique_ptr<Entry> entry) {
    Node root;
    auto* item = entry.get();
    item->setVisible(true);
    root.addChild(std::move(entry));
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&root);
    std::string proposed;
    int submissions = 0;
    bool cancelled = false;
    auto* editor = item->beginInlineEdit(
        "/tmp/original.txt", "original.txt",
        [&](const std::string& value) {
          proposed = value;
          ++submissions;
        },
        [&] {
          cancelled = true;
          item->endInlineEdit("/tmp/original.txt");
        }
    );
    dispatcher.setFocus(editor);
    dispatcher.keyEvent('x', 'x', 0, true);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    TEST_CHECK(submissions == 1 && proposed == "x.txt");
    // A rejected submission must leave the edited value available for retry.
    dispatcher.keyEvent('y', 'y', 0, true);
    dispatcher.keyEvent(XKB_KEY_KP_Enter, 0, 0, true);
    TEST_CHECK(submissions == 2 && proposed == "xy.txt");
    dispatcher.keyEvent(XKB_KEY_Escape, 0, 0, true);
    TEST_CHECK(cancelled && submissions == 2);
  }

  void checkFilesystem(const std::filesystem::path& parent) {
    std::error_code ec;
    TEST_CHECK(!parent.empty() && std::filesystem::is_directory(parent, ec) && !ec);
    nythera::test::ScopedTempDir tempDir(".nythera-rename-test", parent);
    const auto& root = tempDir.path();
    FileOperationManager operations;
    const auto rename = [&](const std::filesystem::path& source, const std::filesystem::path& destination) {
      struct stat identity{};
      TEST_CHECK(lstat(source.c_str(), &identity) == 0);
      bool done = false;
      bool success = false;
      operations.renameChecked(
          source.string(), destination.string(), identity.st_dev, identity.st_ino, [&](bool ok, const std::string&) {
            success = ok;
            done = true;
          }
      );
      bool timedOut = false;
      const auto timeout = g_timeout_add_seconds(
          10,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &timedOut
      );
      while (!done && !timedOut)
        g_main_context_iteration(nullptr, true);
      if (!timedOut)
        g_source_remove(timeout);
      TEST_CHECK(!timedOut && done);
      return success;
    };
    const auto source = root / "original";
    const auto target = root / "renamed";
    std::filesystem::create_directory(source);
    {
      std::ofstream file(source / "child.txt");
      file << "preserved";
    }
    std::filesystem::create_directory(target);
    TEST_CHECK(!rename(source, target));
    TEST_CHECK(std::filesystem::exists(source / "child.txt"));
    TEST_CHECK(!rename(source, root / "missing" / "target"));
    // A valid filename can still fail in the filesystem (NAME_MAX).
    TEST_CHECK(!rename(source, root / std::string(300, 'x')));
    TEST_CHECK(std::filesystem::exists(source / "child.txt"));
    std::filesystem::remove(target);
    TEST_CHECK(rename(source, target));
    TEST_CHECK(!std::filesystem::exists(source) && std::filesystem::exists(target / "child.txt"));
    TEST_CHECK(rename(target / "child.txt", target / "changed.txt"));
    std::string contents;
    {
      std::ifstream file(target / "changed.txt");
      file >> contents;
    }
    TEST_CHECK(contents == "preserved");
  }
} // namespace

int main(int argc, char** argv) {
  return nythera::test::runTest([&] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-inline-rename"});
    Input::setValidateKeyMatcher({});
    checkEditor(std::make_unique<FileEntryRow>(1.0F));
    checkEditor(std::make_unique<FileEntryTile>(1.0F, nullptr));
    checkFilesystem(argc > 1 ? std::filesystem::path(argv[1]) : env.work());
  });
}
