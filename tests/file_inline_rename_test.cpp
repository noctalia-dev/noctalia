#include "filemanager/file_operation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/input.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <sys/stat.h>
#include <memory>
#include <string>
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
        [&](const std::string& value) { proposed = value; ++submissions; },
        [&] { cancelled = true; item->endInlineEdit("/tmp/original.txt"); }
    );
    dispatcher.setFocus(editor);
    dispatcher.keyEvent('x', 'x', 0, true);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    assert(submissions == 1 && proposed == "x.txt");
    // A rejected submission must leave the edited value available for retry.
    dispatcher.keyEvent('y', 'y', 0, true);
    dispatcher.keyEvent(XKB_KEY_KP_Enter, 0, 0, true);
    assert(submissions == 2 && proposed == "xy.txt");
    dispatcher.keyEvent(XKB_KEY_Escape, 0, 0, true);
    assert(cancelled && submissions == 2);
  }
}

namespace {
  void checkFilesystem(const std::filesystem::path& parent) {
    std::string pattern = (parent / ".nythera-rename-test-XXXXXX").string();
    const auto* temporary = mkdtemp(pattern.data());
    assert(temporary != nullptr);
    const std::filesystem::path root(temporary);
    FileOperationManager operations;
    const auto rename = [&](const std::filesystem::path& source, const std::filesystem::path& destination) {
      struct stat identity{};
      assert(lstat(source.c_str(), &identity) == 0);
      bool done = false;
      bool success = false;
      operations.renameChecked(source.string(), destination.string(), identity.st_dev, identity.st_ino,
          [&](bool ok, const std::string&) { success = ok; done = true; });
      const auto timeout = g_timeout_add_seconds(10, [](gpointer) -> gboolean {
        g_error("Timed out waiting for checked rename");
        return G_SOURCE_REMOVE;
      }, nullptr);
      while (!done)
        g_main_context_iteration(nullptr, true);
      g_source_remove(timeout);
      return success;
    };
    const auto source = root / "original";
    const auto target = root / "renamed";
    std::filesystem::create_directory(source);
    { std::ofstream file(source / "child.txt"); file << "preserved"; }
    std::filesystem::create_directory(target);
    assert(!rename(source, target));
    assert(std::filesystem::exists(source / "child.txt"));
    assert(!rename(source, root / "missing" / "target"));
    // A valid filename can still fail in the filesystem (NAME_MAX).
    assert(!rename(source, root / std::string(300, 'x')));
    assert(std::filesystem::exists(source / "child.txt"));
    std::filesystem::remove(target);
    assert(rename(source, target));
    assert(!std::filesystem::exists(source) && std::filesystem::exists(target / "child.txt"));
    assert(rename(target / "child.txt", target / "changed.txt"));
    std::string contents;
    { std::ifstream file(target / "changed.txt"); file >> contents; }
    assert(contents == "preserved");
    std::filesystem::remove_all(root);
  }
}

int main(int argc, char** argv) {
  Input::setValidateKeyMatcher({});
  checkEditor(std::make_unique<FileEntryRow>(1.0F));
  checkEditor(std::make_unique<FileEntryTile>(1.0F, nullptr));
  checkFilesystem(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path());
}
