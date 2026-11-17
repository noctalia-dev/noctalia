#include "filemanager/file_manager.h"

#include <algorithm>
#include <cassert>
#include <fstream>

namespace {
  class CancelledPicker final : public FileDialogPresenter {
  public:
    bool opened = false;
    bool openFileDialog() override {
      opened = true;
      assert(FileDialog::currentOptions().mode == FileDialogMode::SelectFolder);
      return false; // Failure or cancellation must never start a restore.
    }
    void closeFileDialogWithoutResult() override {}
  };
} // namespace

class FileManagerTestAccess {
public:
  static void run(FileManager& manager, const std::filesystem::path& root) {
    auto menus = manager.fileViewMenuEntries();
    assert(std::ranges::none_of(menus, [](const auto& entry) { return entry.id == 72; }));
    manager.activateContextMenuEntry({.id = 70});
    manager.activateContextMenuEntry({.id = 71});
    assert(manager.m_operations.operations().empty());

    manager.m_browsingTrash = true;
    menus = manager.fileViewMenuEntries();
    assert(std::ranges::any_of(menus, [](const auto& entry) { return entry.id == 72; }));
    assert(std::ranges::any_of(menus, [](const auto& entry) {
      return entry.id == 100 && std::ranges::any_of(entry.submenuEntries, [](const auto& child) {
               return child.id == 61 && child.label == "Type";
             });
    }));
    manager.m_entries = {{.name = "item", .absPath = "trash:/item"}};
    manager.m_selectedPaths = {"trash:/item"};
    manager.activateContextMenuEntry({.id = 70}); // Stale selection has no mapped GIO entry.
    assert(manager.m_operations.operations().empty());

    manager.m_trashUris["trash:/item"] = "trash:///item";
    CancelledPicker picker;
    FileDialog::setPresenter(&picker);
    manager.activateContextMenuEntry({.id = 71});
    assert(picker.opened && manager.m_operations.operations().empty());
    FileDialog::setPresenter(nullptr);

    // A disconnected display cannot show confirmation; this must fail closed.
    manager.activateContextMenuEntry({.id = 72});
    assert(manager.m_operations.operations().empty());
    assert(!manager.m_dialogSurface);

    // Even a failed batch must refresh the currently browsed destination and
    // preserve its result after asynchronous enumeration completes.
    manager.m_browsingTrash = false;
    manager.m_selectedPaths.clear();
    manager.m_path = root;
    const auto generation = manager.m_enumerationGeneration;
    const auto id = manager.m_operations.restore({"file:///not-a-wastebin-entry"});
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
           && (manager.m_operations.active() || manager.m_loading || manager.m_enumerationGeneration == generation))
      g_main_context_iteration(nullptr, TRUE);
    if (!timedOut)
      g_source_remove(timer);
    assert(!timedOut);
    assert(manager.m_entries.size() == 1 && manager.m_entries.front().name == "destination-file");
    const auto& operation = manager.m_operations.operations().back();
    assert(operation.id == id && operation.finished && !operation.succeeded);
    assert(manager.m_statusMessage == operation.status);
    assert(!manager.m_wastebinResult);
  }
};

int main() {
  g_setenv("GIO_USE_VFS", "local", TRUE);
  gchar* raw = g_dir_make_tmp("nythera-wastebin-ui-XXXXXX", nullptr);
  assert(raw);
  const std::filesystem::path root(raw);
  g_free(raw);
  for (const auto* name : {"config", "state", "cache", "data", "files"})
    std::filesystem::create_directory(root / name);
  for (const auto* variable : {"NYTHERA_CONFIG_HOME", "XDG_CONFIG_HOME"})
    g_setenv(variable, (root / "config").c_str(), TRUE);
  for (const auto* variable : {"NYTHERA_STATE_HOME", "XDG_STATE_HOME"})
    g_setenv(variable, (root / "state").c_str(), TRUE);
  for (const auto* variable : {"NYTHERA_DATA_HOME", "XDG_DATA_HOME"})
    g_setenv(variable, (root / "data").c_str(), TRUE);
  g_setenv("XDG_CACHE_HOME", (root / "cache").c_str(), TRUE);
  std::ofstream(root / "files/destination-file") << "preserve";
  {
    FileManager manager((root / "files").string());
    FileManagerTestAccess::run(manager, root / "files");
  }
  std::filesystem::remove_all(root);
}
