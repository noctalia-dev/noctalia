#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "tests/test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <fstream>
#include <sys/stat.h>
#include <wayland-client-protocol.h>

namespace {
  class StubRenderer final : public Renderer {
  public:
    TextMetrics
    measureText(std::string_view, float size, const TextFont&, float, int, TextAlign, TextEllipsize, bool) override {
      return {.bottom = size};
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextMetrics measureGlyph(char32_t, float) override { return {}; }
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1.0F; }
  };
  class Adapter final : public VirtualGridAdapter {
  public:
    explicit Adapter(const std::vector<FileEntry>& entries) : m_entries(entries) {}
    std::size_t itemCount() const override { return m_entries.size(); }
    std::unique_ptr<Node> createTile() override { return std::make_unique<Node>(); }
    void bindTile(Node&, std::size_t, bool, bool) override {}

  private:
    const std::vector<FileEntry>& m_entries;
  };
  FileOperationManager::CompletedEntry identity(const std::filesystem::path& path) {
    struct stat state{};
    TEST_CHECK(lstat(path.c_str(), &state) == 0);
    return {path.string(), static_cast<std::uint64_t>(state.st_dev), static_cast<std::uint64_t>(state.st_ino)};
  }
} // namespace

class FileManagerTestAccess {
public:
  static void verifyDropTargets(FileManager& manager, const std::filesystem::path& root) {
    manager.m_entries = {
        {.name = "cancelled.txt", .absPath = root / "cancelled.txt"},
        {.name = "other", .absPath = root / "other", .isDir = true}
    };
    Adapter adapter(manager.m_entries);
    VirtualGridView grid;
    StubRenderer renderer;
    grid.setAdapter(&adapter);
    grid.setColumns(1);
    grid.setSquareCells(false);
    grid.setCellHeight(30);
    grid.setRowGap(0);
    grid.setSize(200, 90);
    grid.layout(renderer);
    manager.m_grid = &grid;
    TEST_CHECK(grid.indexAtSurface(10, 15) == 0);
    TEST_CHECK(!manager.dropDestination(10, 15)); // Preserve internal file-row rejection.
    TEST_CHECK(manager.dropDestination(10, 15, true) == root);
    TEST_CHECK(manager.dropDestination(10, 45) == root / "other");
    TEST_CHECK(manager.dropDestination(10, 45, true) == root / "other");
    TEST_CHECK(manager.dropDestination(10, 75, true) == root);
    TEST_CHECK(!manager.dropDestination(210, 15, true));
    manager.m_browsingTrash = true;
    TEST_CHECK(!manager.dropDestination(10, 15, true));
    manager.m_browsingTrash = false;

    ScrollView places;
    places.setPosition(300, 0);
    places.setSize(100, 90);
    Button row;
    row.setPosition(300, 0);
    row.setSize(100, 30);
    manager.m_placesScroll = &places;
    manager.m_placeRows = {{&row, {.identity = "test:mounted", .path = root / "other"}}};
    TEST_CHECK(manager.dropDestination(310, 15, true) == root / "other");
    TEST_CHECK(manager.dropDestination(310, 15) == root / "other");
    manager.m_placeRows.front().second.mounted = false;
    TEST_CHECK(!manager.dropDestination(310, 15, true));
    manager.m_placeRows.clear();
    manager.m_placesScroll = nullptr;
    manager.m_grid = nullptr;
    grid.setAdapter(nullptr);

    // Destination is the accepted target at drop time, not later hover state.
    manager.m_dragTargetValid = true;
    manager.m_protocolDropDestination = root / "other";
    auto receive = manager.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    TEST_CHECK(receive);
    manager.m_protocolDropDestination = root;
    const auto before = manager.m_operations.operations().size();
    std::optional<bool> finished;
    receive(filemanager::encodeFileTransfer({(root / "cancelled.txt").string()}, false), [&](bool success) {
      finished = success;
    });
    bool timedOut = false;
    const auto timer = g_timeout_add(
        5000,
        [](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &timedOut
    );
    while ((!finished || manager.m_loading) && !timedOut)
      g_main_context_iteration(nullptr, TRUE);
    if (!timedOut)
      g_source_remove(timer);
    TEST_CHECK(!timedOut && finished && *finished);
    TEST_CHECK(manager.m_operations.operations().size() == before + 1);
    TEST_CHECK(std::filesystem::exists(root / "other/cancelled.txt"));
    TEST_CHECK(std::filesystem::exists(root / "cancelled.txt"));
    manager.closeMonitor();

    // Unsupported authorities and cancelled reads never enqueue an operation.
    manager.m_dragTargetValid = true;
    manager.m_protocolDropDestination = root / "other";
    receive = manager.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    const auto after = manager.m_operations.operations().size();
    finished.reset();
    receive(NativeDataTransfer::Payload{{"text/uri-list", "file://remote-host/tmp/item\r\n"}}, [&](bool success) {
      finished = success;
    });
    TEST_CHECK(finished && !*finished);
    TEST_CHECK(manager.m_operations.operations().size() == after);
    finished.reset();
    receive(std::nullopt, [&](bool success) { finished = success; });
    TEST_CHECK(finished && !*finished);
    TEST_CHECK(manager.m_operations.operations().size() == after);
    TEST_CHECK(std::filesystem::exists(root / "cancelled.txt"));
  }

  static void run(FileManager& manager, const std::filesystem::path& root) {
    // Reported destination names may differ from source names after Keep Both.
    const auto first = root / "pasted-renamed.txt";
    const auto second = root / "pasted-second.txt";
    const auto replaced = root / "replaced.txt";
    const auto cancelled = root / "cancelled.txt";
    std::ofstream(first) << "first";
    std::ofstream(second) << "second";
    std::ofstream(replaced) << "original";
    std::ofstream(cancelled) << "unrelated existing entry";
    auto replacedIdentity = identity(replaced);
    // Keep the original inode alive to make this a deterministic replacement.
    std::filesystem::rename(replaced, root / "held-original");
    std::ofstream(replaced) << "replacement must not be selected";
    for (int i = 0; i < 25; ++i)
      std::ofstream(root / ("existing-" + std::to_string(i))) << i;
    manager.m_path = root;
    manager.m_sortField = FileDialogSortField::Name;
    manager.m_sortOrder = FileDialogSortOrder::Ascending;
    manager.m_pasteRequests[1] = {root, manager.m_navigationGeneration};
    FileOperationManager::Operation partial{.id = 1};
    partial.finished = true;
    partial.cancelled = true;
    partial.completedEntries = {identity(second), identity(first), replacedIdentity};
    TEST_CHECK(manager.recordPasteCompletion(partial));
    // Completion reports only successful entries; a same-name preexisting file
    // absent from that report must remain unselected.
    manager.enumerate(root);
    bool timedOut = false;
    const auto timer = g_timeout_add(
        5000,
        [](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &timedOut
    );
    while (manager.m_loading && !timedOut)
      g_main_context_iteration(nullptr, TRUE);
    if (!timedOut)
      g_source_remove(timer);
    TEST_CHECK(!timedOut);
    manager.closeMonitor();
    TEST_CHECK(manager.m_selectedPaths.size() == 2);
    TEST_CHECK(manager.m_selectedPaths.contains(first.string()));
    TEST_CHECK(manager.m_selectedPaths.contains(second.string()));
    TEST_CHECK(!manager.m_selectedPaths.contains(replaced.string()));
    TEST_CHECK(!manager.m_selectedPaths.contains(cancelled.string()));
    TEST_CHECK(manager.m_selectedPath == first);

    // Exercise the real grid's deferred scroll with the current sorted index.
    Adapter adapter(manager.m_entries);
    VirtualGridView grid;
    StubRenderer renderer;
    grid.setAdapter(&adapter);
    grid.setColumns(1);
    grid.setSquareCells(false);
    grid.setCellHeight(30);
    grid.setSize(200, 90);
    grid.layout(renderer);
    manager.m_grid = &grid;
    manager.m_pendingPasteSelection =
        FileManager::PasteSelection{{root, manager.m_navigationGeneration}, partial.completedEntries};
    manager.applyPendingPasteSelection();
    manager.restoreSelection(); // A monitor refresh must preserve the deferred reveal.
    grid.layout(renderer);
    const auto selected = std::ranges::find(manager.m_entries, first, &FileEntry::absPath);
    TEST_CHECK(grid.selectedIndex() == static_cast<std::size_t>(selected - manager.m_entries.begin()));
    TEST_CHECK(grid.scrollView().scrollOffset() > 0);

    manager.m_sortOrder = FileDialogSortOrder::Descending;
    manager.sortEntries();
    manager.m_pendingPasteSelection =
        FileManager::PasteSelection{{root, manager.m_navigationGeneration}, partial.completedEntries};
    manager.applyPendingPasteSelection();
    TEST_CHECK(manager.m_selectedPath == second);
    const auto descending = std::ranges::find(manager.m_entries, second, &FileEntry::absPath);
    TEST_CHECK(grid.selectedIndex() == static_cast<std::size_t>(descending - manager.m_entries.begin()));
    manager.m_grid = nullptr;
    grid.setAdapter(nullptr);

    // Navigation away and back invalidates the request even at the same path.
    manager.m_pasteRequests[2] = {root, manager.m_navigationGeneration};
    std::filesystem::create_directory(root / "other");
    manager.navigate(root / "other", false);
    manager.navigate(root, false);
    timedOut = false;
    const auto navigationTimer = g_timeout_add(
        5000,
        [](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &timedOut
    );
    while (manager.m_loading && !timedOut)
      g_main_context_iteration(nullptr, TRUE);
    if (!timedOut)
      g_source_remove(navigationTimer);
    TEST_CHECK(!timedOut);
    manager.closeMonitor();
    const auto before = manager.m_selectedPaths;
    TEST_CHECK(manager.m_path == root);
    partial.id = 2;
    TEST_CHECK(!manager.recordPasteCompletion(partial));
    TEST_CHECK(manager.m_selectedPaths == before);
    manager.m_pendingPasteSelection =
        FileManager::PasteSelection{{root, manager.m_navigationGeneration - 1}, partial.completedEntries};
    manager.applyPendingPasteSelection();
    TEST_CHECK(manager.m_selectedPaths == before);
    // Failed/skipped-only operations have no completed entries to select.
    manager.m_pasteRequests[3] = {root, manager.m_navigationGeneration};
    partial.id = 3;
    partial.completedEntries.clear();
    TEST_CHECK(!manager.recordPasteCompletion(partial));
    TEST_CHECK(!manager.m_pendingPasteSelection);
  }
};

int main() {
  g_setenv("GIO_USE_VFS", "local", TRUE);
  char* raw = g_dir_make_tmp("nythera-paste-selection-XXXXXX", nullptr);
  TEST_CHECK(raw);
  const std::filesystem::path root(raw);
  g_free(raw);
  for (const auto* name : {"config", "state", "cache", "data", "files"})
    std::filesystem::create_directory(root / name);
  for (const auto* key : {"NYTHERA_CONFIG_HOME", "XDG_CONFIG_HOME"})
    g_setenv(key, (root / "config").c_str(), TRUE);
  for (const auto* key : {"NYTHERA_STATE_HOME", "XDG_STATE_HOME"})
    g_setenv(key, (root / "state").c_str(), TRUE);
  for (const auto* key : {"NYTHERA_DATA_HOME", "XDG_DATA_HOME"})
    g_setenv(key, (root / "data").c_str(), TRUE);
  g_setenv("XDG_CACHE_HOME", (root / "cache").c_str(), TRUE);
  {
    FileManager manager((root / "files").string());
    FileManagerTestAccess::run(manager, root / "files");
    FileManagerTestAccess::verifyDropTargets(manager, root / "files");
  }
  std::filesystem::remove_all(root);
}
