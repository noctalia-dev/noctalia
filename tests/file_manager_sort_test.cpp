#include "core/deferred_call.h"
#include "filemanager/file_manager.h"
#include "render/scene/input_area.h"
#include "test_isolation.h"
#include "ui/controls/virtual_grid_view.h"
#include "wayland/popup_surface.h"

#include <fstream>
#include <linux/input-event-codes.h>

class ContextMenuPopupTestAccess {
public:
  static wl_surface* childSurface(ContextMenuPopup& root) {
    root.m_surface = std::make_unique<PopupSurface>(root.m_wayland);
    root.m_wlSurface = reinterpret_cast<wl_surface*>(1);
    root.m_child = std::make_unique<ContextMenuPopup>(root.m_wayland, root.m_renderContext);
    auto& child = *root.m_child;
    child.m_surface = std::make_unique<PopupSurface>(root.m_wayland);
    child.m_wlSurface = reinterpret_cast<wl_surface*>(2);
    child.m_parentPopup = &root;
    return child.m_wlSurface;
  }
  static void bindLeaf(ContextMenuPopup& root, int id) {
    auto& child = *root.m_child;
    child.m_sceneRoot = std::make_unique<InputArea>();
    auto* area = static_cast<InputArea*>(child.m_sceneRoot.get());
    area->setFrameSize(100, 32);
    area->setOnClick([&child, id](const InputArea::PointerData&) { child.deferActivation({.id = id}); });
    child.m_inputDispatcher.setSceneRoot(area);
    child.setOnActivate([&root](const ContextMenuControlEntry& entry) {
      auto action = root.m_onActivate;
      root.close();
      action(entry);
    });
  }
};

class FileManagerTestAccess {
public:
  static void clickSort(FileManager& manager, int id) {
    manager.m_menus.m_contextMenuPopup = std::make_unique<ContextMenuPopup>(manager.m_wayland, manager.m_renderContext);
    auto& menu = *manager.m_menus.m_contextMenuPopup;
    menu.setOnActivate([&manager](const ContextMenuControlEntry& entry) { manager.activateContextMenuEntry(entry); });
    auto* surface = ContextMenuPopupTestAccess::childSurface(menu);
    ContextMenuPopupTestAccess::bindLeaf(menu, id);
    PointerEvent event{};
    event.surface = surface;
    event.type = PointerEvent::Type::Button;
    event.sx = event.sy = 10;
    event.button = BTN_LEFT;
    event.pressed = true;
    manager.dispatchPointer(event); // Formerly dismissed the entire menu on child press.
    event.pressed = false;
    manager.dispatchPointer(event);
    for (auto& callback : DeferredCall::takePending())
      callback();
  }
  static void run(FileManager& m, const std::filesystem::path& root) {
    FileEntry a{.name = "a", .absPath = root / "a", .size = 4};
    a.mtime = std::filesystem::file_time_type{} + std::chrono::seconds(5);
    a.created = 100;
    a.hasCreated = true;
    a.accessed = 200;
    a.hasAccessed = true;
    a.type = "text/plain";
    FileEntry b{.name = "b", .absPath = root / "b", .size = 1};
    b.mtime = std::filesystem::file_time_type{} + std::chrono::seconds(10);
    b.created = 200;
    b.hasCreated = true;
    b.accessed = 100;
    b.hasAccessed = true;
    b.type = "image/png";
    auto grid = std::make_unique<VirtualGridView>();
    m.m_grid = grid.get();
    const auto detachGrid = nythera::test::makeScopeExit([&] { m.m_grid = nullptr; });
    auto& tab = m.activeTab();
    auto& session = tab.session;
    session.m_foldersFirst = session.m_hiddenLast = false;
    tab.selectedPath = a.absPath;
    tab.selectedPaths = {a.absPath.string()};
    for (const auto& [id, first] : {std::pair{53, "a"}, {54, "b"}, {55, "a"}, {56, "a"}, {57, "b"}, {61, "b"}}) {
      session.m_entries = {a, b};
      m.restoreSelection();
      clickSort(m, 58);
      clickSort(m, id);
      TEST_CHECK(session.m_entries.front().name == first);
      TEST_CHECK(m.m_grid->selectedIndex() && session.m_entries[*m.m_grid->selectedIndex()].absPath == a.absPath);
      TEST_CHECK(tab.selectedPaths.contains(a.absPath.string()));
      clickSort(m, 62);
      TEST_CHECK(session.m_entries.back().name == first);
      TEST_CHECK(m.m_listHeader.m_sortOrder == FileDialogSortOrder::Descending);
    }
    FileEntry folder{.name = "folder", .absPath = root / "folder", .isDir = true};
    FileEntry hidden{.name = ".hidden", .absPath = root / ".hidden", .size = 2};
    session.m_entries = {hidden, b, folder, a};
    clickSort(m, 51); // Folders First, independent of descending.
    TEST_CHECK(session.m_entries.front().isDir);
    clickSort(m, 52);
    TEST_CHECK(session.m_entries.back().name == ".hidden");
    clickSort(m, 51);
    clickSort(m, 52);
    session.m_entries = {a, b};
    clickSort(m, 54);
    clickSort(m, 58);
    TEST_CHECK(m.m_listHeader.m_sortField == FileDialogSortField::Size && session.m_entries.front().name == "b");
    std::ofstream(root / "a", std::ios::trunc); // A real isolated metadata change.
    session.queryMonitoredFile(root / "a");
    waitFor([&] { return session.m_entries.front().name == "a" && session.m_entries.front().size == 0; });
    m.enumerate(root);
    waitFor([&] { return !session.m_loading; });
    TEST_CHECK(session.m_entries.size() == 2 && session.m_entries.front().name == "a");
    TEST_CHECK(
        m.m_listHeader.m_sortField == FileDialogSortField::Size
        && m.m_listHeader.m_sortOrder == FileDialogSortOrder::Ascending
    );
    TEST_CHECK(m.m_grid->selectedIndex() && session.m_entries[*m.m_grid->selectedIndex()].absPath == a.absPath);
    TEST_CHECK(m.m_configService.stateString("files", "sort_field") == "1");
    TEST_CHECK(m.m_configService.stateBool("files", "sort_descending") == false);
  }
  template <class Predicate> static void waitFor(Predicate predicate) {
    bool timeout = false;
    const auto timer = g_timeout_add(
        3000,
        [](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &timeout
    );
    while (!predicate() && !timeout)
      g_main_context_iteration(nullptr, TRUE);
    if (!timeout)
      g_source_remove(timer);
    TEST_CHECK(!timeout && predicate());
  }
  static void verifyRestored(const FileManager& m) {
    TEST_CHECK(
        m.m_listHeader.m_sortField == FileDialogSortField::Size
        && m.m_listHeader.m_sortOrder == FileDialogSortOrder::Ascending
    );
    TEST_CHECK(!m.activeSession().m_foldersFirst && !m.activeSession().m_hiddenLast);
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-sort-integration"});
    const auto filesDir = env.work() / "files";
    std::filesystem::create_directory(filesDir);
    std::ofstream(filesDir / "a") << "1234";
    std::ofstream(filesDir / "b") << "1";
    {
      FileManager manager(filesDir.string());
      FileManagerTestAccess::run(manager, filesDir);
    }
    {
      FileManager restored(filesDir.string());
      FileManagerTestAccess::verifyRestored(restored);
    }
  });
}
