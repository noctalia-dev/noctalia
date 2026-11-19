#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "tests/test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <linux/input-event-codes.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

void setFileOperationRenameNoReplaceHookForTesting(std::function<int(const char*, const char*)> hook);
void setFileOperationRemoveEntryHookForTesting(std::function<int(const char*)> hook);

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
  static void verifySymlinkDrop(FileManager& manager, const std::filesystem::path& root) {
    const auto pictures = root.parent_path() / "Pictures";
    const auto incoming = root.parent_path() / "symlink-incoming";
    std::filesystem::create_directory_symlink(root, pictures);
    std::filesystem::create_directory(incoming);
    Adapter adapter(manager.m_entries);
    VirtualGridView grid;
    StubRenderer renderer;
    grid.setAdapter(&adapter);
    grid.setColumns(1);
    grid.setSquareCells(false);
    grid.setCellHeight(30);
    grid.setRowGap(0);
    grid.setSize(200, 90);
    manager.m_grid = &grid;
    manager.navigate(pictures, false);
    manager.m_sortOrder = FileDialogSortOrder::Ascending;
    for (const auto action : {WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE}) {
      const std::string name =
          action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY ? "2026-11-17 22-10-26.mp4" : "zz-symlink-move.mp4";
      const auto source = incoming / name;
      std::ofstream(source) << "isolated video-named transfer fixture";
      manager.m_dragTargetValid = true;
      manager.m_protocolDropDestination = pictures;
      auto receive = manager.receiveProtocolDrop(action);
      TEST_CHECK(receive);
      std::optional<bool> finished;
      receive(filemanager::encodeFileTransfer({source.string()}, false), [&](bool success) { finished = success; });
      bool timeout = false;
      const auto timer = g_timeout_add(
          5000,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &timeout
      );
      while ((!finished || manager.m_loading || manager.m_pendingMonitorQueries != 0) && !timeout)
        g_main_context_iteration(nullptr, TRUE);
      if (!timeout)
        g_source_remove(timer);
      TEST_CHECK(!timeout && finished && *finished);
      manager.closeMonitor();
      TEST_CHECK(manager.m_path == pictures);
      TEST_CHECK(manager.m_selectedPaths == std::unordered_set<std::string>{(pictures / name).string()});
      TEST_CHECK(manager.m_selectedPath == pictures / name);
      TEST_CHECK(std::filesystem::exists(source) == (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY));
      grid.layout(renderer);
      manager.finishPendingTransferReveal(renderer);
      const auto entry = std::ranges::find(manager.m_entries, pictures / name, &FileEntry::absPath);
      const auto index = static_cast<std::size_t>(entry - manager.m_entries.begin());
      TEST_CHECK(grid.selectedIndex() == index);
      TEST_CHECK(grid.scrollView().scrollOffset() <= index * 30);
      TEST_CHECK(grid.scrollView().scrollOffset() + 90 >= (index + 1) * 30);
    }
    // Matching names and even a matching inode in another directory are not
    // enough: the completed entry must belong to the displayed destination.
    const auto unrelated = incoming / "unrelated";
    std::filesystem::create_directory(unrelated);
    const auto name = "2026-11-17 22-10-26.mp4";
    std::filesystem::create_hard_link(root / name, unrelated / name);
    manager.m_selectedPaths.clear();
    manager.m_selectedPath.reset();
    FileOperationManager::Operation other{.id = 9999};
    other.completedEntries = {identity(unrelated / name)};
    manager.m_pasteRequests[other.id] = {root, manager.m_navigationGeneration};
    TEST_CHECK(manager.recordPasteCompletion(other));
    manager.applyPendingPasteSelection();
    TEST_CHECK(manager.m_selectedPaths.empty());
    manager.m_grid = nullptr;
    grid.setAdapter(nullptr);
  }

  static void verifyPasteCompletion(FileManager& manager, const std::filesystem::path& root) {
    const auto source = root.parent_path() / "clipboard-sources";
    std::filesystem::create_directory(source);
    const auto waitForRefresh = [&] {
      bool timedOut = false;
      const auto timer = g_timeout_add(
          5000,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &timedOut
      );
      while ((manager.m_operations.active() || manager.m_loading) && !timedOut)
        g_main_context_iteration(nullptr, TRUE);
      if (!timedOut)
        g_source_remove(timer);
      TEST_CHECK(!timedOut);
      TEST_CHECK(manager.m_pasteRequests.empty());
    };
    // Paths entered in the location bar (and supplied on the command line) may
    // end in a separator. GIO's actual destination paths do not retain it.
    for (const bool trailingSlash : {false, true}) {
      manager.navigate(trailingSlash ? root / "" : root, false);
      for (const bool move : {false, true}) {
        const std::string prefix = trailingSlash ? "slash-" : "plain-";
        const auto name = prefix + (move ? "pasted-move.txt" : "pasted-copy.txt");
        std::ofstream(source / name) << "clipboard transfer";
        const auto id = move ? manager.m_operations.move({(source / name).string()}, manager.m_path.string())
                             : manager.m_operations.copy({(source / name).string()}, manager.m_path.string());
        manager.m_pasteRequests[id] = {manager.m_path, manager.m_navigationGeneration};
        waitForRefresh();
        TEST_CHECK(manager.m_selectedPaths == std::unordered_set<std::string>{(root / name).string()});
        TEST_CHECK(manager.m_selectedPath == root / name);
        TEST_CHECK(std::filesystem::exists(source / name) != move);
        // A same-directory refresh supersedes an in-flight monitor query. It
        // must keep the new selection after enumeration and metadata callbacks.
        manager.queryMonitoredFile(root / name);
        manager.enumerate(manager.m_path);
        waitForRefresh();
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        TEST_CHECK(manager.m_selectedPaths == std::unordered_set<std::string>{(root / name).string()});
      }
    }
    // Exercise real multi-source completion: Keep Both, Skip and cancellation
    // after earlier successes must select only finalized destination identities.
    for (const auto* name : {"batch-first.txt", "batch-renamed.txt", "batch-skipped.txt", "batch-cancelled.txt"})
      std::ofstream(source / name) << "batch source";
    for (const auto* name : {"batch-renamed.txt", "batch-skipped.txt", "batch-cancelled.txt"})
      std::ofstream(root / name) << "existing destination";
    manager.m_operations.setConflictResolver([](const std::string& path, const std::string&, bool, GCancellable*) {
      using Action = FileOperationManager::ConflictAction;
      if (std::filesystem::path(path).filename() == "batch-renamed.txt")
        return FileOperationManager::ConflictDecision{.action = Action::Rename, .alternativeName = "batch-actual.txt"};
      if (std::filesystem::path(path).filename() == "batch-skipped.txt")
        return FileOperationManager::ConflictDecision{.action = Action::Skip};
      return FileOperationManager::ConflictDecision{.action = Action::Cancel};
    });
    const auto id = manager.m_operations.copy(
        {(source / "batch-first.txt").string(), (source / "batch-renamed.txt").string(),
         (source / "batch-skipped.txt").string(), (source / "batch-cancelled.txt").string()},
        manager.m_path.string()
    );
    manager.m_pasteRequests[id] = {manager.m_path, manager.m_navigationGeneration};
    waitForRefresh();
    const std::unordered_set<std::string> expected{
        (root / "batch-first.txt").string(), (root / "batch-actual.txt").string()
    };
    TEST_CHECK(manager.m_selectedPaths == expected);
    manager.m_operations.setConflictResolver({});
    manager.closeMonitor();
    manager.navigate(root, false);
    waitForRefresh();
    manager.closeMonitor();
  }

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

    // Both copied and moved incoming drops select their actual new entries.
    // This exercises the drop-to-operation completion path, not a fabricated result.
    const auto incoming = root.parent_path() / "incoming";
    std::filesystem::create_directory(incoming);
    grid.setAdapter(&adapter);
    grid.setColumns(1);
    grid.setSquareCells(false);
    grid.setCellHeight(30);
    grid.setRowGap(0);
    grid.setSize(200, 90);
    grid.layout(renderer);
    manager.m_grid = &grid;
    manager.m_sortOrder = FileDialogSortOrder::Ascending;
    const auto waitForDrop = [&] {
      bool timeout = false;
      const auto timeoutId = g_timeout_add(
          5000,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &timeout
      );
      while ((!finished || manager.m_loading || manager.m_pendingMonitorQueries != 0) && !timeout)
        g_main_context_iteration(nullptr, TRUE);
      if (!timeout)
        g_source_remove(timeoutId);
      TEST_CHECK(!timeout && finished && *finished);
      manager.closeMonitor();
    };
    for (const auto action : {WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE}) {
      manager.m_sortOrder = action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY ? FileDialogSortOrder::Ascending
                                                                             : FileDialogSortOrder::Descending;
      grid.scrollView().setScrollOffset(
          action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY ? 0.0F : grid.scrollView().maxScrollOffset()
      );
      const std::string prefix = action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY ? "zz-copy-drop-" : "zz-move-drop-";
      std::vector<std::string> sources;
      for (const auto* suffix : {"a.txt", "b.txt"}) {
        const auto source = incoming / (prefix + suffix);
        std::ofstream(source) << "incoming drop";
        sources.push_back(source.string());
      }
      manager.m_dragTargetValid = true;
      manager.m_protocolDropDestination = root;
      receive = manager.receiveProtocolDrop(action);
      TEST_CHECK(receive);
      finished.reset();
      receive(filemanager::encodeFileTransfer(sources, false), [&](bool success) { finished = success; });
      waitForDrop();
      TEST_CHECK(manager.m_selectedPaths.size() == 2);
      TEST_CHECK(
          manager.m_selectedPath
          == root / (prefix + (manager.m_sortOrder == FileDialogSortOrder::Ascending ? "a.txt" : "b.txt"))
      );
      TEST_CHECK(manager.m_pasteRequests.empty());
      // A frame while a follow-up enumeration is pending must not consume the
      // reveal. New sorted entries can move the selected row after that frame.
      manager.enumerate(root);
      grid.layout(renderer);
      manager.finishPendingTransferReveal(renderer);
      TEST_CHECK(manager.m_pendingPasteRevealPath == manager.m_selectedPath);
      waitForDrop();
      manager.queryMonitoredFile(root / (prefix + "a.txt"));
      grid.layout(renderer);
      manager.finishPendingTransferReveal(renderer);
      TEST_CHECK(manager.m_pendingPasteRevealPath == manager.m_selectedPath);
      waitForDrop();
      grid.layout(renderer);
      manager.finishPendingTransferReveal(renderer);
      const auto first = std::ranges::find(manager.m_entries, *manager.m_selectedPath, &FileEntry::absPath);
      const auto index = static_cast<std::size_t>(first - manager.m_entries.begin());
      TEST_CHECK(grid.selectedIndex() == index);
      TEST_CHECK(grid.scrollView().scrollOffset() <= index * 30);
      TEST_CHECK(grid.scrollView().scrollOffset() + 90 >= (index + 1) * 30);
      TEST_CHECK(!manager.m_pendingPasteRevealPath);
      TEST_CHECK(grid.scrollView().userScrollGeneration() == 0);
      for (const auto& source : sources) {
        const auto destination = root / std::filesystem::path(source).filename();
        TEST_CHECK(manager.m_selectedPaths.contains(destination.string()));
        TEST_CHECK(std::filesystem::exists(destination));
        TEST_CHECK(std::filesystem::exists(source) == (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY));
      }
    }

    // User scrolling between the physical drop and completion cancels only
    // the reveal, even if the user subsequently returns to the old offset.
    manager.m_sortOrder = FileDialogSortOrder::Ascending;
    manager.sortEntries();
    grid.notifyDataChanged();
    grid.layout(renderer);
    const auto manualSource = incoming / "zzzz-manual-scroll.txt";
    std::ofstream(manualSource) << "manual scroll during drop";
    manager.m_dragTargetValid = true;
    manager.m_protocolDropDestination = root;
    receive = manager.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    auto& scroll = grid.scrollView();
    TEST_CHECK(scroll.viewportArea()->dispatchAxis(
        10, 10, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_FINGER, -30, 0, 0, 0
    ));
    TEST_CHECK(scroll.userScrollGeneration() > 0);
    scroll.setScrollOffset(0);
    finished.reset();
    receive(filemanager::encodeFileTransfer({manualSource.string()}, false), [&](bool success) { finished = success; });
    waitForDrop();
    grid.layout(renderer);
    manager.finishPendingTransferReveal(renderer);
    TEST_CHECK(manager.m_selectedPaths == std::unordered_set<std::string>{(root / manualSource.filename()).string()});
    TEST_CHECK(scroll.scrollOffset() == 0);
    TEST_CHECK(!manager.m_pendingPasteRevealPath);

    // Scrolling after completion but before its final layout also wins.
    const auto lateSource = incoming / "zzzz-late-scroll.txt";
    std::ofstream(lateSource) << "scroll before deferred reveal";
    manager.m_dragTargetValid = true;
    manager.m_protocolDropDestination = root;
    receive = manager.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    finished.reset();
    receive(filemanager::encodeFileTransfer({lateSource.string()}, false), [&](bool success) { finished = success; });
    waitForDrop();
    TEST_CHECK(manager.m_pendingPasteRevealPath);
    TEST_CHECK(scroll.viewportArea()->dispatchAxis(
        10, 10, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, 30, 1, 120, 0
    ));
    const auto manualOffset = scroll.scrollOffset();
    grid.layout(renderer);
    manager.finishPendingTransferReveal(renderer);
    TEST_CHECK(scroll.scrollOffset() == manualOffset);
    TEST_CHECK(manager.m_selectedPaths.contains((root / lateSource.filename()).string()));
    TEST_CHECK(!manager.m_pendingPasteRevealPath);
    const auto beforeTouch = scroll.userScrollGeneration();
    scroll.viewportArea()->dispatchPress(10, 40, BTN_LEFT, true);
    scroll.viewportArea()->dispatchMotion(10, 10);
    scroll.viewportArea()->dispatchPress(10, 10, BTN_LEFT, false);
    TEST_CHECK(scroll.userScrollGeneration() > beforeTouch);

    // Navigation between physical drop and URI receipt must not select in the
    // later view, even when the user returns to the same directory.
    const auto delayedSource = incoming / "delayed-drop.txt";
    std::ofstream(delayedSource) << "delayed incoming drop";
    manager.m_dragTargetValid = true;
    manager.m_protocolDropDestination = root;
    auto delayedReceive = manager.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    manager.navigate(root / "other", false);
    manager.navigate(root, false);
    waitForDrop();
    const auto selectedBeforeDelayedDrop = manager.m_selectedPaths;
    finished.reset();
    delayedReceive(filemanager::encodeFileTransfer({delayedSource.string()}, false), [&](bool success) {
      finished = success;
    });
    waitForDrop();
    TEST_CHECK(manager.m_path == root);
    TEST_CHECK(std::filesystem::exists(root / "delayed-drop.txt"));
    TEST_CHECK(manager.m_selectedPaths == selectedBeforeDelayedDrop);

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
    manager.m_grid = nullptr;
    grid.setAdapter(nullptr);
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

  static void verifyAsyncCallbackLifetimes(const std::filesystem::path& root) {
    const auto f06Root = root / "f06-lifetimes";
    const auto dirA = f06Root / "dir-a";
    const auto dirB = f06Root / "dir-b";
    const auto subDir = dirA / "sub";
    std::filesystem::create_directories(subDir);
    std::filesystem::create_directories(dirB);
    for (int i = 0; i < 160; ++i)
      std::ofstream(dirA / ("item-" + std::to_string(i) + ".txt")) << "f06 payload";
    std::ofstream(subDir / "child.txt") << "child";
    std::ofstream(dirB / "only-in-b.txt") << "target b";

    const auto pumpMs = [](guint ms) {
      bool elapsed = false;
      g_timeout_add(
          ms,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &elapsed
      );
      while (!elapsed)
        g_main_context_iteration(nullptr, TRUE);
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
    };

    // 1. Destruction while enumerate(), queryMonitoredFile(), folderItemCount(),
    // and closeDialog() have callbacks pending on GMainContext / GIO workers.
    bool dialogCallbackInvoked = false;
    {
      auto transient = std::make_unique<FileManager>(dirA.string());
      transient->m_dialogResponse = [&](int) { dialogCallbackInvoked = true; };
      transient->enumerate(dirA);
      transient->queryMonitoredFile(dirA / "item-0.txt");
      (void)transient->folderItemCount(FileEntry{.name = "sub", .absPath = subDir, .isDir = true});
      transient->closeDialog(1);
      transient.reset();
    }
    pumpMs(50);
    TEST_CHECK(!dialogCallbackInvoked);

    // 2. Destruction while fetchNext() (g_file_enumerator_next_files_async) is in flight.
    {
      auto transient = std::make_unique<FileManager>(dirA.string());
      GFile* gf = g_file_new_for_path(dirA.c_str());
      GFileEnumerator* enumerator =
          g_file_enumerate_children(gf, "standard::name,standard::type", G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
      TEST_CHECK(enumerator != nullptr);
      if (transient->m_enumerationCancellable == nullptr)
        transient->m_enumerationCancellable = g_cancellable_new();
      transient->fetchNext(enumerator, ++transient->m_enumerationGeneration);
      g_object_unref(enumerator);
      g_object_unref(gf);
      transient.reset();
    }
    pumpMs(50);

    // 3. Cancellation and generation superseding on rapid navigate().
    {
      FileManager manager(dirB.string());
      manager.enumerate(dirA);
      manager.queryMonitoredFile(dirA / "item-0.txt");
      manager.navigate(dirB, false);
      bool timedOut = false;
      const auto timer = g_timeout_add(
          5000,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &timedOut
      );
      while ((manager.m_loading || manager.m_pendingMonitorQueries != 0) && !timedOut)
        g_main_context_iteration(nullptr, TRUE);
      if (!timedOut)
        g_source_remove(timer);
      TEST_CHECK(!timedOut);
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      manager.closeMonitor();
      TEST_CHECK(manager.m_path == dirB);
      TEST_CHECK(manager.m_entries.size() == 1);
      TEST_CHECK(manager.m_entries.front().absPath == dirB / "only-in-b.txt");
    }

    // 4. Late receiveProtocolDrop closure invoked after FileManager destruction.
    {
      auto transient = std::make_unique<FileManager>(dirB.string());
      transient->m_dragTargetValid = true;
      transient->m_protocolDropDestination = dirB;
      auto receiver = transient->receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
      TEST_CHECK(static_cast<bool>(receiver));
      transient.reset();
      std::optional<bool> finishResult;
      receiver(filemanager::encodeFileTransfer({(dirA / "item-0.txt").string()}, false), [&](bool ok) {
        finishResult = ok;
      });
      TEST_CHECK(finishResult && !*finishResult);
    }
  }

  static void verifyPartialMoveRefresh(const std::filesystem::path& base) {
    const auto fixture = base / "f03-ui-refresh";
    const auto srcDir = fixture / "src";
    const auto dstDir = fixture / "dst";
    std::filesystem::create_directories(srcDir);
    std::filesystem::create_directories(dstDir);
    std::ofstream(dstDir / "existing.txt") << "keep selected";

    const auto dstPrefix = dstDir.string() + "/";
    setFileOperationRenameNoReplaceHookForTesting([&](const char* sourcePath, const char* destinationPath) -> int {
      const bool directCross = sourcePath != nullptr
          && destinationPath != nullptr
          && !std::string(sourcePath).starts_with(dstPrefix)
          && std::string(destinationPath).starts_with(dstPrefix);
      if (directCross) {
        errno = EXDEV;
        return -1;
      }
      return static_cast<int>(
          syscall(SYS_renameat2, AT_FDCWD, sourcePath, AT_FDCWD, destinationPath, RENAME_NOREPLACE)
      );
    });

    FileManager manager(dstDir.string());
    const auto waitForPredicate = [&](auto predicate) {
      bool timedOut = false;
      const auto timer = g_timeout_add(
          5000,
          [](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &timedOut
      );
      while (!predicate() && !timedOut)
        g_main_context_iteration(nullptr, TRUE);
      if (!timedOut)
        g_source_remove(timer);
      TEST_CHECK(!timedOut && predicate());
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
    };

    manager.navigate(dstDir, false);
    waitForPredicate([&] { return !manager.m_loading; });
    manager.closeMonitor();
    manager.m_selectedPath = dstDir / "existing.txt";
    manager.m_selectedPaths = {(dstDir / "existing.txt").string()};

    // 1. Partial single-item cross-filesystem move: destination commits, source
    // removal fails. The directory view must refresh to show the new destination
    // item, preserve existing selection, and retain the split-state error message.
    const auto partialSrc = srcDir / "partial-move.txt";
    const auto partialDst = dstDir / "partial-move.txt";
    std::ofstream(partialSrc) << "partial ui payload";
    setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
      return path != nullptr && path == partialSrc ? EACCES : 0;
    });
    const auto genBeforePartial = manager.m_enumerationGeneration;
    const auto partialId = manager.m_operations.move({partialSrc.string()}, dstDir.string());
    TEST_CHECK(partialId != 0);
    waitForPredicate([&] {
      return !manager.m_operations.active() && !manager.m_loading && manager.m_enumerationGeneration > genBeforePartial;
    });
    setFileOperationRemoveEntryHookForTesting({});
    const auto& partialOp = manager.m_operations.operations().back();
    TEST_CHECK(partialOp.id == partialId && !partialOp.succeeded && !partialOp.cancelled && partialOp.mutated);
    TEST_CHECK(partialOp.completedEntries.empty());
    TEST_CHECK(std::ranges::any_of(manager.m_entries, [&](const FileEntry& entry) {
      return entry.absPath == partialDst;
    }));
    TEST_CHECK(manager.m_selectedPath == dstDir / "existing.txt");
    TEST_CHECK(manager.m_selectedPaths == std::unordered_set<std::string>{(dstDir / "existing.txt").string()});
    TEST_CHECK(manager.m_statusMessage == partialOp.status);
    TEST_CHECK(manager.m_statusMessage.find("Moved copy remains at ") != std::string::npos);
    TEST_CHECK(manager.m_statusMessage.find(partialDst.string()) != std::string::npos);
    TEST_CHECK(manager.m_statusMessage.find("source still exists at ") != std::string::npos);
    TEST_CHECK(manager.m_statusMessage.find(partialSrc.string()) != std::string::npos);
    TEST_CHECK(!manager.m_operationResultAfterRefresh);

    // 2. Partial cross-filesystem Replace move: destination commits, backup survives,
    // source removal fails. Refresh must run and preserve destination, source, and
    // backup paths in the status message.
    const auto replaceSrc = srcDir / "replace-ui.txt";
    const auto replaceDst = dstDir / "replace-ui.txt";
    std::ofstream(replaceSrc) << "replacement ui payload";
    std::ofstream(replaceDst) << "original ui backup";
    manager.m_operations.setConflictResolver([](const std::string&, const std::string&, bool, GCancellable*) {
      return FileOperationManager::ConflictDecision{FileOperationManager::ConflictAction::Replace, false};
    });
    setFileOperationRemoveEntryHookForTesting([&](const char* path) -> int {
      return path != nullptr && path == replaceSrc ? EIO : 0;
    });
    const auto genBeforeReplace = manager.m_enumerationGeneration;
    const auto replaceId = manager.m_operations.move({replaceSrc.string()}, dstDir.string());
    TEST_CHECK(replaceId != 0);
    waitForPredicate([&] {
      return !manager.m_operations.active() && !manager.m_loading && manager.m_enumerationGeneration > genBeforeReplace;
    });
    setFileOperationRemoveEntryHookForTesting({});
    manager.m_operations.setConflictResolver({});
    const auto& replaceOp = manager.m_operations.operations().back();
    TEST_CHECK(replaceOp.id == replaceId && !replaceOp.succeeded && replaceOp.mutated);
    TEST_CHECK(manager.m_statusMessage == replaceOp.status);
    TEST_CHECK(manager.m_statusMessage.find("Moved copy remains at ") != std::string::npos);
    TEST_CHECK(manager.m_statusMessage.find("previous destination backup remains at ") != std::string::npos);
    TEST_CHECK(!manager.m_operationResultAfterRefresh);

    // 3. Non-mutating failed/cancelled move must NOT trigger an unnecessary refresh.
    const auto noChangeSrc = srcDir / "existing.txt";
    std::ofstream(noChangeSrc) << "blocked by existing destination";
    const auto genBeforeNoChange = manager.m_enumerationGeneration;
    const auto noChangeId = manager.m_operations.move({noChangeSrc.string()}, dstDir.string());
    TEST_CHECK(noChangeId != 0);
    waitForPredicate([&] { return !manager.m_operations.active(); });
    const auto& noChangeOp = manager.m_operations.operations().back();
    TEST_CHECK(noChangeOp.id == noChangeId && !noChangeOp.succeeded && noChangeOp.cancelled && !noChangeOp.mutated);
    TEST_CHECK(manager.m_enumerationGeneration == genBeforeNoChange);
    TEST_CHECK(!manager.m_loading);
    TEST_CHECK(manager.m_statusMessage == noChangeOp.status);
    TEST_CHECK(manager.m_selectedPath == dstDir / "existing.txt");

    setFileOperationRenameNoReplaceHookForTesting({});
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
    FileManagerTestAccess::verifyPasteCompletion(manager, root / "files");
    FileManagerTestAccess::verifyDropTargets(manager, root / "files");
    FileManagerTestAccess::verifySymlinkDrop(manager, root / "files");
  }
  FileManagerTestAccess::verifyAsyncCallbackLifetimes(root / "files");
  FileManagerTestAccess::verifyPartialMoveRefresh(root / "files");
  std::filesystem::remove_all(root);
}
