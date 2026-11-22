#include "core/files/directory_scanner.h"
#include "core/files/display_text.h"
#include "core/input/key_modifiers.h"
#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/tab_presentation.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "test_isolation.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <ranges>
#include <source_location>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  using TempDir = nythera::test::ScopedTempDir;

  void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    TEST_CHECK(out.good());
  }

  template <typename Predicate>
  void pumpUntil(
      Predicate&& predicate, int timeoutMs = 5000, std::source_location caller = std::source_location::current()
  ) {
    bool timedOut = false;
    const guint timer = g_timeout_add(
        static_cast<guint>(timeoutMs),
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
    if (timedOut || !predicate())
      g_printerr("Timed out at %s:%u\n", caller.file_name(), caller.line());
    TEST_CHECK(!timedOut && predicate());
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
  }

  void pumpMs(guint ms) {
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
  }

  class StubTextures final : public TextureManager {
  public:
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRgba(const std::uint8_t*, int, int, bool) override { return {}; }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override {
      return {};
    }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * fontSize * 0.5F;
      const float w = maxWidth > 0.0F ? std::min(natural, maxWidth) : natural;
      return TextMetrics{
          .width = w,
          .right = w,
          .bottom = fontSize,
          .inkBottom = fontSize,
          .inkRight = w,
          .capHeight = fontSize * 0.72F,
          .lineCount = text.empty() ? 0 : 1,
      };
    }
    TextMetrics measureFont(float fontSize, const TextFont&) override {
      return TextMetrics{
          .width = fontSize,
          .right = fontSize,
          .bottom = fontSize,
          .inkBottom = fontSize,
          .inkRight = fontSize,
          .capHeight = fontSize * 0.72F,
      };
    }
    void measureTextCursorStops(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<float>& outStops, const TextFont&
    ) override {
      (void)text;
      outStops.clear();
      for (std::size_t offset : byteOffsets)
        outStops.push_back(static_cast<float>(offset) * fontSize * 0.5F);
    }
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>& byteOffsets, float,
        std::vector<TextCursorStop>& outStops, const TextFont&
    ) override {
      outStops.assign(byteOffsets.size(), TextCursorStop{});
    }
    TextMetrics measureGlyph(char32_t, float fontSize) override {
      return TextMetrics{.width = fontSize, .right = fontSize, .bottom = fontSize, .capHeight = fontSize};
    }
    TextureManager& textureManager() override { return m_textures; }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    StubTextures m_textures;
  };

  const RectNode* backgroundRect(const Node* node) {
    if (node == nullptr)
      return nullptr;
    for (const auto& child : node->children()) {
      if (const auto* rect = dynamic_cast<const RectNode*>(child.get()))
        return rect;
    }
    return nullptr;
  }

  void dispatchClick(InputDispatcher& dispatcher, const Node* node, std::uint32_t button = BTN_LEFT) {
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(node, x, y);
    const float cx = x + node->width() * 0.5F;
    const float cy = y + node->height() * 0.5F;
    dispatcher.pointerMotion(cx, cy, 1);
    dispatcher.pointerButton(cx, cy, button, true, 2, 10, false);
    dispatcher.pointerButton(cx, cy, button, false, 3, 20, false);
  }

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
  struct TabScene {
    StubRenderer renderer;
    std::unique_ptr<Flex> root;
    Flex* navRow = nullptr;
    Node* tabBarNode = nullptr;
    Flex* bottomPane = nullptr;

    TabScene(FileManager& manager, float width, float height, float padding = 0.0F) {
      root = std::make_unique<Flex>();
      root->setDirection(FlexDirection::Vertical);
      root->setAlign(FlexAlign::Stretch);
      root->setGap(0.0F);
      root->setPadding(padding);

      auto nav = std::make_unique<Flex>();
      nav->setMinHeight(32.0F);
      nav->setMaxHeight(32.0F);
      navRow = static_cast<Flex*>(root->addChild(std::move(nav)));

      tabBarNode = root->addChild(manager.m_tabBar.buildUi());

      auto bottom = std::make_unique<Flex>();
      bottom->setFlexGrow(1.0F);
      bottomPane = static_cast<Flex*>(root->addChild(std::move(bottom)));

      manager.m_dispatcher.setSceneRoot(root.get());
      relayout(manager, width, height);
    }

    void relayout(FileManager& manager, float width, float height) {
      (void)manager.m_tabBar.rebuildIfDirty();
      root->setSize(width, height);
      root->layout(renderer);
      manager.m_tabBar.syncLayout(renderer);
    }
  };

  struct SessionGridAdapter final : public VirtualGridAdapter {
    FileManager& manager;
    explicit SessionGridAdapter(FileManager& owner) : manager(owner) {}
    [[nodiscard]] std::size_t itemCount() const override { return manager.activeSession().entries().size(); }
    [[nodiscard]] std::optional<bool> selectedState(std::size_t index) const override {
      const auto& tab = manager.activeTab();
      if (index >= tab.session.entries().size())
        return false;
      return tab.selectedPaths.contains(tab.session.entries()[index].absPath.string());
    }
    [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<Box>(); }
    void bindTile(Node&, std::size_t, bool, bool) override {}
  };

  struct GridHarness {
    StubRenderer renderer;
    SessionGridAdapter adapter;
    VirtualGridView grid;
    FileManager& manager;
    bool attached = false;

    explicit GridHarness(FileManager& owner, float width = 800.0F, float height = 400.0F)
        : adapter(owner), manager(owner) {
      grid.setAdapter(&adapter);
      grid.setColumns(manager.m_listHeader.gridMode() ? 0 : 1);
      grid.setMinCellWidth(160.0F);
      grid.setCellHeight(30.0F);
      grid.bindScrollState(&manager.activeTab().scrollState);
      grid.setSize(width, height);
      grid.layout(renderer);
      manager.m_grid = &grid;
      manager.m_listHeader.attachGrid(&grid);
      attached = true;
    }
    void layout() {
      grid.setSize(grid.width(), grid.height());
      grid.layout(renderer);
    }
    void detach() {
      if (!attached)
        return;
      manager.m_listHeader.attachGrid(nullptr);
      manager.m_grid = nullptr;
      grid.bindScrollState(nullptr);
      grid.setAdapter(nullptr);
      attached = false;
    }
    ~GridHarness() { detach(); }
  };

  static void testIndependentTabStateAndSharedColumns(const std::filesystem::path& root) {
    const auto dirA = root / "indep_A";
    const auto dirASub = dirA / "sub";
    const auto dirB = root / "indep_B";
    std::filesystem::create_directories(dirASub);
    std::filesystem::create_directories(dirB);
    for (int i = 0; i < 40; ++i)
      writeFile(dirASub / ("item_" + std::to_string(i) + ".txt"), "hello");
    writeFile(dirB / "beta.txt", "beta");

    FileManager manager(dirA.string());
    GridHarness harness(manager);
    auto& tabManager = manager.m_tabs;
    const filemanager::TabId tabAId = tabManager.activeTabId();

    manager.navigate(dirASub, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    harness.layout();

    // Customize Tab A: view mode, sorting, foldersFirst, hiddenLast, zoom, showHidden, selection, scroll offset.
    manager.setGridMode(true);
    manager.setSortField(FileDialogSortField::Size);
    manager.toggleSortOrder(); // Descending
    manager.activeSession().setFoldersFirst(false);
    manager.activeSession().setHiddenLast(false);
    manager.activeSession().setShowHidden(true);
    manager.adjustViewScale(1.25F);
    manager.persistFileViewState();

    TEST_CHECK(manager.activeSession().entries().size() >= 20);
    const auto chosenA = manager.activeSession().entries()[10].absPath;
    manager.activeTab().selectedPath = chosenA;
    manager.activeTab().selectedPaths = {chosenA.string()};
    manager.restoreSelection(false);
    harness.grid.scrollView().setScrollOffset(120.0F);
    harness.layout();
    TEST_CHECK(manager.activeTab().scrollState.offset > 0.0F);
    const float savedScrollA = manager.activeTab().scrollState.offset;

    // Modify shared column layout and column widths in FileListHeaderController.
    manager.m_listHeader.columnLayout().visible[filemanager::columnIndex(filemanager::FileColumn::Type)] = true;
    manager.m_listHeader.m_listColumns[filemanager::FileColumn::Size] = 148.0F;
    manager.m_listHeader.persistColumnLayout();

    // Open Tab B directly via TabManager with default TabViewState so its view settings differ from Tab A.
    const filemanager::TabId tabBId = tabManager.openTab(dirB, filemanager::TabViewState{}, true);
    TEST_CHECK(tabBId != filemanager::kInvalidTabId);
    TEST_CHECK(tabBId != tabAId);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    harness.layout();

    TEST_CHECK(manager.activeTab().id == tabBId);
    TEST_CHECK(manager.activeSession().path() == dirB);
    TEST_CHECK(manager.activeSession().backHistory().empty());
    TEST_CHECK(manager.activeSession().forwardHistory().empty());
    TEST_CHECK(!manager.activeTab().selectedPath.has_value());
    TEST_CHECK(manager.activeTab().scrollState.offset == 0.0F);
    TEST_CHECK(!manager.m_listHeader.gridMode());
    TEST_CHECK(manager.m_listHeader.sortField() == FileDialogSortField::Name);
    TEST_CHECK(manager.m_listHeader.sortOrder() == FileDialogSortOrder::Ascending);
    TEST_CHECK(manager.activeSession().foldersFirst());
    TEST_CHECK(manager.activeSession().hiddenLast());
    TEST_CHECK(!manager.activeSession().showHidden());
    TEST_CHECK(manager.m_listHeader.viewScale() == 1.0F);
    // Shared column layout and column widths remain visible/sized identically on Tab B.
    TEST_CHECK(manager.m_listHeader.columnLayout().visible[filemanager::columnIndex(filemanager::FileColumn::Type)]);
    TEST_CHECK(manager.m_listHeader.listColumns()[filemanager::FileColumn::Size] == 148.0F);

    // Switch back to Tab A and verify all per-tab state is restored intact.
    TEST_CHECK(manager.activateTab(tabAId));
    harness.layout();
    TEST_CHECK(manager.activeSession().path() == dirASub);
    TEST_CHECK(manager.activeSession().backHistory().size() == 1);
    TEST_CHECK(manager.activeSession().backHistory().front() == dirA);
    TEST_CHECK(manager.m_listHeader.gridMode());
    TEST_CHECK(manager.m_listHeader.sortField() == FileDialogSortField::Size);
    TEST_CHECK(manager.m_listHeader.sortOrder() == FileDialogSortOrder::Descending);
    TEST_CHECK(!manager.activeSession().foldersFirst());
    TEST_CHECK(!manager.activeSession().hiddenLast());
    TEST_CHECK(manager.activeSession().showHidden());
    TEST_CHECK(manager.m_listHeader.viewScale() == 1.25F);
    TEST_CHECK(manager.activeTab().selectedPath == chosenA);
    TEST_CHECK(manager.activeTab().scrollState.offset == savedScrollA);

    manager.m_tabs.shutdownAll();
  }

  static void testLifecycleDuplicateReorderAndBoundedClosedHistory(const std::filesystem::path& root) {
    const auto startDir = root / "life_start";
    const auto midDir = root / "life_mid";
    const auto fwdDir = root / "life_fwd";
    std::filesystem::create_directories(startDir);
    std::filesystem::create_directories(midDir);
    std::filesystem::create_directories(fwdDir);

    FileManager manager(startDir.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    // Open New Tab: uses defaultTabPath() (home), inherits originating view settings, empty history, foreground.
    manager.setGridMode(true);
    manager.setSortField(FileDialogSortField::Modified);
    manager.persistFileViewState();
    const filemanager::TabId tab1 = manager.activeTab().id;
    const filemanager::TabId newTabId = manager.openNewTab();
    TEST_CHECK(newTabId != tab1 && newTabId != filemanager::kInvalidTabId);
    TEST_CHECK(manager.m_tabs.activeTabId() == newTabId);
    TEST_CHECK(manager.activeSession().path() == filemanager::normalizeTabPath(FileManager::defaultTabPath()));
    TEST_CHECK(manager.activeTab().viewState.gridMode);
    TEST_CHECK(manager.activeTab().viewState.sortField == FileDialogSortField::Modified);
    TEST_CHECK(manager.activeSession().backHistory().empty());
    TEST_CHECK(manager.activeSession().forwardHistory().empty());
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    // Duplicate Tab: inherits directory, view settings, and back/forward history, inserted right after source.
    TEST_CHECK(manager.activateTab(tab1));
    manager.navigate(midDir, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    manager.navigate(fwdDir, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    // Simulate Back once so both back and forward history are populated.
    const auto backTarget = manager.activeSession().backHistory().back();
    manager.activeSession().backHistory().pop_back();
    manager.activeSession().forwardHistory().push_back(manager.activeSession().path());
    manager.navigate(backTarget, false);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    TEST_CHECK(manager.activeSession().path() == midDir);
    TEST_CHECK(manager.activeSession().backHistory().size() == 1);
    TEST_CHECK(manager.activeSession().forwardHistory().size() == 1);

    const filemanager::TabId dupId = manager.duplicateTab(tab1);
    TEST_CHECK(dupId != filemanager::kInvalidTabId && dupId != tab1);
    TEST_CHECK(manager.m_tabs.activeTabId() == dupId);
    TEST_CHECK(manager.m_tabs.indexOf(dupId) == 1);
    TEST_CHECK(manager.activeSession().path() == midDir);
    TEST_CHECK(manager.activeSession().backHistory().size() == 1);
    TEST_CHECK(manager.activeSession().forwardHistory().size() == 1);
    TEST_CHECK(manager.activeTab().viewState.gridMode);
    TEST_CHECK(manager.activeTab().viewState.sortField == FileDialogSortField::Modified);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    // Reorder tabs and close tabs to the right / close others.
    TEST_CHECK(manager.m_tabs.reorderTab(newTabId, 0));
    TEST_CHECK(manager.m_tabs.indexOf(newTabId) == 0);
    // Order is [newTabId, tab1, dupId], with dupId active at index 2 -> +1 wraps to newTabId.
    TEST_CHECK(manager.activateRelativeTab(1));
    TEST_CHECK(manager.m_tabs.activeTabId() == newTabId);
    TEST_CHECK(manager.activateRelativeTab(1));
    TEST_CHECK(manager.m_tabs.activeTabId() == tab1);
    TEST_CHECK(manager.activateRelativeTab(-1));
    TEST_CHECK(manager.m_tabs.activeTabId() == newTabId);

    TEST_CHECK(manager.closeTabsToRight(newTabId));
    TEST_CHECK(manager.m_tabs.count() == 1);

    // Open & close 18 distinct tabs; verify bounded closed-tab history == 16 and LIFO reopen restores settings+history.
    std::vector<std::filesystem::path> closedDirs;
    for (int i = 0; i < 18; ++i) {
      const auto dir = root / ("closed_dir_" + std::to_string(i));
      const auto sub = dir / "inner";
      std::filesystem::create_directories(sub);
      filemanager::TabViewState vs;
      vs.gridMode = (i % 2 == 1);
      vs.showHidden = (i % 3 == 0);
      vs.viewScale = 1.0F + static_cast<float>(i % 4) * 0.1F;
      const auto id = manager.m_tabs.openTab(dir, vs, true);
      TEST_CHECK(id != filemanager::kInvalidTabId);
      pumpUntil([&] { return !manager.activeSession().m_loading; });
      manager.navigate(sub, true);
      pumpUntil([&] { return !manager.activeSession().m_loading; });
      closedDirs.push_back(sub);
    }
    while (manager.m_tabs.count() > 1) {
      const auto lastId = manager.m_tabs.tabs().back()->id;
      TEST_CHECK(manager.closeTab(lastId));
    }
    TEST_CHECK(manager.m_tabs.closedTabs().size() == filemanager::TabManager::kMaxClosedTabs);

    // Closing from back() closed i=17..0 in order; LIFO top has i=0..15 while 17 and 16 were evicted.
    TEST_CHECK(manager.m_tabs.closedTabs().size() == 16);
    for (int i = 0; i < 16; ++i) {
      const auto reopenedId = manager.reopenClosedTab();
      TEST_CHECK(reopenedId != filemanager::kInvalidTabId);
      TEST_CHECK(manager.m_tabs.activeTabId() == reopenedId);
      const auto* reopened = manager.m_tabs.findTab(reopenedId);
      TEST_CHECK(reopened != nullptr);
      TEST_CHECK(reopened->session.path() == closedDirs[static_cast<std::size_t>(i)]);
      TEST_CHECK(reopened->session.backHistory().size() == 1);
      TEST_CHECK(reopened->viewState.gridMode == (i % 2 == 1));
      TEST_CHECK(reopened->viewState.showHidden == (i % 3 == 0));
      pumpUntil([&] { return !manager.activeSession().m_loading; });
    }
    TEST_CHECK(!manager.m_tabs.canReopenClosedTab());
    TEST_CHECK(manager.reopenClosedTab() == filemanager::kInvalidTabId);

    // Close Other Tabs keeps only pivot.
    const auto keepId = manager.m_tabs.tabs()[5]->id;
    TEST_CHECK(manager.closeOtherTabs(keepId));
    TEST_CHECK(manager.m_tabs.count() == 1);
    TEST_CHECK(manager.m_tabs.activeTabId() == keepId);

    manager.m_tabs.shutdownAll();
  }

  static void testWindowTitleAndSanitization(const std::filesystem::path& root) {
    const std::filesystem::path home = FileManager::defaultTabPath();
    const auto downloads = root / "Downloads";
    const auto dirty = root / "bad\x07\nfolder\rname";
    std::filesystem::create_directories(downloads);
    std::filesystem::create_directories(dirty);

    FileManager manager(home.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    TEST_CHECK(manager.m_windowTitle == "Home \xE2\x80\x94 Nythera Files");

    manager.navigate(downloads, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    TEST_CHECK(manager.m_windowTitle == "Downloads \xE2\x80\x94 Nythera Files");

    manager.navigateTrash();
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    TEST_CHECK(manager.m_windowTitle == "Wastebin \xE2\x80\x94 Nythera Files");

    manager.navigate(std::filesystem::path("/"), true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    TEST_CHECK(manager.m_windowTitle == "/ \xE2\x80\x94 Nythera Files");

    manager.navigate(dirty, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    TEST_CHECK(manager.m_windowTitle.find('\x07') == std::string::npos);
    TEST_CHECK(manager.m_windowTitle.find('\n') == std::string::npos);
    TEST_CHECK(manager.m_windowTitle.find('\r') == std::string::npos);
    TEST_CHECK(manager.m_windowTitle.ends_with(" \xE2\x80\x94 Nythera Files"));

    manager.m_tabs.shutdownAll();
  }

  static void testStaleAsyncCallbackIsolationSameGeneration(const std::filesystem::path& root) {
    const auto dirA = root / "stale_A";
    const auto dirB = root / "stale_B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    writeFile(dirA / "a.txt", "alpha");
    writeFile(dirB / "b.txt", "beta");

    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabAId = manager.activeTab().id;
    auto& tabA = manager.activeTab();

    std::function<void(bool)> pendingWritableA;
    std::function<void(std::optional<std::uint64_t>)> pendingFreeA;
    std::function<void(std::optional<filemanager::SelectionInfo>)> pendingSelInfoA;

    tabA.session.m_directoryWritableQueryHook = [&](const std::filesystem::path&, std::function<void(bool)> deliver) {
      pendingWritableA = std::move(deliver);
      return true;
    };
    tabA.m_freeSpaceQueryHook = [&](const std::filesystem::path&,
                                    std::function<void(std::optional<std::uint64_t>)> deliver) {
      pendingFreeA = std::move(deliver);
      return true;
    };
    tabA.m_selectionInfoQueryHook = [&](const std::filesystem::path&,
                                        std::function<void(std::optional<filemanager::SelectionInfo>)> deliver) {
      pendingSelInfoA = std::move(deliver);
      return true;
    };

    tabA.selectedPaths = {(dirA / "a.txt").string()};
    tabA.session.queryDirectoryWritableAsync(dirA);
    tabA.queryStatusFreeSpaceAsync(dirA);
    tabA.querySelectionInfoAsync(dirA / "a.txt");
    TEST_CHECK(pendingWritableA && pendingFreeA && pendingSelInfoA);

    const std::uint64_t genA = tabA.session.navigationGeneration();
    const filemanager::TabId tabBId = manager.openFolderInNewTab(dirB, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    auto& tabB = *manager.m_tabs.findTab(tabBId);
    tabB.session.m_navigationGeneration = genA; // Explicitly test matching generation across distinct TabIds.
    const bool initialWritableB = tabB.session.currentDirectoryWritable();
    const auto initialFreeB = tabB.freeSpaceBytes();
    const auto initialTitle = manager.m_windowTitle;

    // Fire Tab A's deferred callbacks with toggled values while Tab B is active.
    pendingWritableA(!initialWritableB);
    pendingFreeA(std::uint64_t{987654321});
    pendingSelInfoA(
        filemanager::SelectionInfo{.path = dirA / "a.txt", .type = "Plain text", .hasSize = true, .size = 5}
    );

    TEST_CHECK(manager.m_tabs.activeTabId() == tabBId);
    TEST_CHECK(tabB.session.currentDirectoryWritable() == initialWritableB);
    TEST_CHECK(tabB.freeSpaceBytes() == initialFreeB);
    TEST_CHECK(!tabB.selectionInfo().has_value());
    TEST_CHECK(manager.m_windowTitle == initialTitle);

    TEST_CHECK(tabA.session.currentDirectoryWritable() == !initialWritableB);
    TEST_CHECK(tabA.freeSpaceBytes() == std::optional<std::uint64_t>{987654321});
    TEST_CHECK(tabA.selectionInfo() && tabA.selectionInfo()->path == dirA / "a.txt");

    // Stale free-space callback with different navigationGeneration is ignored on Tab A itself.
    std::function<void(std::optional<std::uint64_t>)> staleFreeA;
    tabA.m_freeSpaceQueryHook = [&](const std::filesystem::path&,
                                    std::function<void(std::optional<std::uint64_t>)> deliver) {
      staleFreeA = std::move(deliver);
      return true;
    };
    tabA.queryStatusFreeSpaceAsync(dirA);
    TEST_CHECK(static_cast<bool>(staleFreeA));
    tabA.cancelStatusFreeSpaceQuery();
    staleFreeA(std::uint64_t{111});
    TEST_CHECK(tabA.freeSpaceBytes() != std::optional<std::uint64_t>{111});

    manager.m_tabs.shutdownAll();
  }

  static void testCloseDuringEnumAndActiveOperationCutConsumption(const std::filesystem::path& root) {
    const auto srcDir = root / "close_op_src";
    const auto dstDir = root / "close_op_dst";
    const auto bigEnumDir = root / "close_enum";
    std::filesystem::create_directories(srcDir);
    std::filesystem::create_directories(dstDir);
    std::filesystem::create_directories(bigEnumDir);
    for (int i = 0; i < 60; ++i)
      writeFile(bigEnumDir / ("e_" + std::to_string(i) + ".txt"), "x");
    const auto moveFile = srcDir / "cut_item.txt";
    writeFile(moveFile, "precious payload");

    FileManager manager(dstDir.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    // Open a tab whose enumeration starts, then close it immediately before pumping.
    const filemanager::TabId enumTabId = manager.openFolderInNewTab(bigEnumDir, true);
    const filemanager::TabId stayTabId = manager.m_tabs.tabs().front()->id;
    TEST_CHECK(manager.activateTab(enumTabId));
    manager.enumerate(bigEnumDir);
    TEST_CHECK(manager.closeTab(enumTabId));
    TEST_CHECK(manager.m_tabs.activeTabId() == stayTabId);
    pumpMs(40);

    // Start an async Move from an originating tab, record cut selection, close originating tab while move is active.
    const filemanager::TabId opOriginId = manager.openFolderInNewTab(dstDir, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const std::uint64_t cutSelectionGen = manager.m_transfer.selectionGeneration();
    const auto navGen = manager.m_tabs.findTab(opOriginId)->session.navigationGeneration();
    const auto opId = manager.m_operations.move({moveFile.string()}, dstDir.string());
    TEST_CHECK(opId != 0);
    manager.m_transfers.m_cutOperations[opId] = cutSelectionGen;
    manager.m_transfers.registerPasteRequest(opId, filemanager::PasteRequest{opOriginId, dstDir, navGen});

    TEST_CHECK(manager.closeTab(opOriginId));
    TEST_CHECK(manager.m_tabs.findTab(opOriginId) == nullptr);
    pumpUntil([&] { return !manager.m_operations.active(); });

    TEST_CHECK(!std::filesystem::exists(moveFile));
    TEST_CHECK(std::filesystem::exists(dstDir / "cut_item.txt"));
    TEST_CHECK(manager.m_transfers.m_consumedCutSelection == std::optional<std::uint64_t>{cutSelectionGen});
    TEST_CHECK(manager.m_transfers.m_cutOperations.empty());
    TEST_CHECK(manager.m_transfers.m_pasteRequests.empty());

    manager.m_tabs.shutdownAll();
  }

  static void testSwitchCancelsTransientGestures(const std::filesystem::path& root) {
    const auto dirA = root / "gesture_A";
    const auto dirB = root / "gesture_B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    writeFile(dirA / "rename_me.txt", "abc");

    FileManager manager(dirA.string());
    GridHarness harness(manager);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabAId = manager.activeTab().id;
    const filemanager::TabId tabBId = manager.openFolderInNewTab(dirB, false);
    pumpUntil([&] { return !manager.m_tabs.findTab(tabBId)->session.m_loading; });

    Input locInput;
    manager.m_locationInput = &locInput;
    manager.setLocationEditMode(true);
    TEST_CHECK(manager.m_editingLocation);

    manager.m_renamePath = dirA / "rename_me.txt";
    manager.m_pendingInlineRenamePath = dirA / "rename_me.txt";
    manager.m_inlineRenameTarget = FileManager::InlineRenameTarget{0, tabAId, dirA / "rename_me.txt"};
    manager.m_refreshAfterRename = true;

    TEST_CHECK(harness.grid.beginRubberBand(20.0F, 350.0F));
    TEST_CHECK(harness.grid.rubberBandActive());

    // Start animated scroll on ScrollView and confirm binding new tab state stops animations and cancels gestures.
    manager.activeTab().scrollState.offset = 10.0F;
    harness.grid.scrollView().scrollBy(40.0F);

    TEST_CHECK(manager.activateTab(tabBId));
    TEST_CHECK(!manager.m_editingLocation);
    TEST_CHECK(!manager.m_renamePath.has_value());
    TEST_CHECK(!manager.m_pendingInlineRenamePath.has_value());
    TEST_CHECK(!manager.m_inlineRenameTarget.has_value());
    TEST_CHECK(!manager.m_refreshAfterRename);
    TEST_CHECK(manager.m_tabs.findTab(tabAId)->needsRefresh);
    TEST_CHECK(!harness.grid.rubberBandActive());

    manager.m_locationInput = nullptr;
    manager.m_tabs.shutdownAll();
  }

  static void testCrossTabCopyPasteAndReveal(const std::filesystem::path& root) {
    const auto dirA = root / "xfer_A";
    const auto dirB = root / "xfer_B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    const auto src1 = dirA / "copy1.txt";
    const auto src2 = dirA / "copy2.txt";
    writeFile(src1, "111");
    writeFile(src2, "222");
    for (int i = 0; i < 40; ++i)
      writeFile(dirB / ("aa_" + std::to_string(i) + ".txt"), "z");

    // Verify file transfer clipboard payload codec preserves cut/copy URI lists across tabs.
    const auto encodedCopy = filemanager::encodeFileTransfer({src1.string()}, false);
    TEST_CHECK(encodedCopy.contains("text/uri-list"));
    const auto decodedCopy = filemanager::decodeFileTransfer(encodedCopy.at("text/uri-list"), false, false);
    TEST_CHECK(!decodedCopy.cut && decodedCopy.paths == std::vector<std::string>{src1.string()});

    FileManager manager(dirA.string());
    GridHarness harness(manager, 800.0F, 180.0F);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabAId = manager.activeTab().id;
    const filemanager::TabId tabBId = manager.openFolderInNewTab(dirB, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const auto navB = manager.activeTab().session.navigationGeneration();

    // Case (a): Start copy into Tab B, switch back to Tab A before completion.
    const auto opId1 = manager.m_operations.copy({src1.string()}, dirB.string());
    TEST_CHECK(opId1 != 0);
    manager.m_transfers.registerPasteRequest(opId1, filemanager::PasteRequest{tabBId, dirB, navB});
    TEST_CHECK(manager.activateTab(tabAId));

    pumpUntil([&] { return !manager.m_operations.active(); });
    // Tab A stays untouched while Tab B has pending paste selection + reveal ready for activation.
    TEST_CHECK(!manager.activeTab().selectedPath.has_value());
    const auto& tabB = *manager.m_tabs.findTab(tabBId);
    TEST_CHECK(tabB.pendingPasteSelection.has_value() || tabB.selectedPath == dirB / "copy1.txt");

    TEST_CHECK(manager.activateTab(tabBId));
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    harness.layout();
    TEST_CHECK(manager.activeTab().selectedPath == dirB / "copy1.txt");
    TEST_CHECK(manager.activeTab().selectedPaths.contains((dirB / "copy1.txt").string()));
    TEST_CHECK(manager.activeTab().scrollState.offset > 0.0F);

    // Case (b): With dropScrollGeneration set, user scroll in Tab B keeps selection but suppresses reveal.
    manager.activeTab().scrollState.offset = 0.0F;
    harness.grid.scrollView().setScrollOffset(0.0F);
    harness.layout();
    const auto dropScrollGen = manager.activeTab().scrollState.userScrollGeneration;
    const auto opId2 = manager.m_operations.copy({src2.string()}, dirB.string());
    TEST_CHECK(opId2 != 0);
    manager.m_transfers.registerPasteRequest(
        opId2,
        filemanager::PasteRequest{tabBId, dirB, manager.activeTab().session.navigationGeneration(), dropScrollGen}
    );
    ++manager.activeTab().scrollState.userScrollGeneration;

    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().m_loading; });
    harness.layout();
    TEST_CHECK(manager.activeTab().selectedPath == dirB / "copy2.txt");
    TEST_CHECK(manager.activeTab().scrollState.offset == 0.0F);

    manager.m_tabs.shutdownAll();
  }

  static void testSameDirectoryTabsMonitorDirtyAndCreateIsolation(const std::filesystem::path& root) {
    const auto sharedDir = root / "same_dir";
    std::filesystem::create_directories(sharedDir);
    for (int i = 0; i < 30; ++i)
      writeFile(sharedDir / ("f_" + std::to_string(i) + ".txt"), "hi");

    FileManager manager(sharedDir.string());
    GridHarness harness(manager, 800.0F, 180.0F);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabAId = manager.activeTab().id;
    const filemanager::TabId tabBId = manager.openFolderInNewTab(sharedDir, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    harness.layout();

    const auto selB = sharedDir / "f_5.txt";
    manager.activeTab().selectedPath = selB;
    manager.activeTab().selectedPaths = {selB.string()};
    harness.grid.scrollView().setScrollOffset(90.0F);
    harness.layout();
    const float scrollB = manager.activeTab().scrollState.offset;
    TEST_CHECK(scrollB > 0.0F);

    // Switch to Tab A and test createFolder auto-select + background Tab B dirty notification.
    TEST_CHECK(manager.activateTab(tabAId));
    const auto newFolderPath = sharedDir / "CreatedInA";
    const auto createOp = manager.m_operations.createFolder(newFolderPath.string());
    TEST_CHECK(createOp != 0);
    manager.m_createRequests[createOp] =
        FileManager::CreateRequest{tabAId, manager.activeTab().session.navigationGeneration(), newFolderPath};
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().m_loading; });

    TEST_CHECK(std::filesystem::is_directory(newFolderPath));
    TEST_CHECK(manager.activeTab().selectedPath == newFolderPath);
    TEST_CHECK(manager.m_pendingInlineRenamePath == newFolderPath);
    auto& tabB = *manager.m_tabs.findTab(tabBId);
    TEST_CHECK(tabB.needsRefresh);
    TEST_CHECK(tabB.selectedPath == selB);

    // Reactivate Tab B: clears needsRefresh, refreshes preserving selection and scroll offset, no rename in B.
    manager.m_pendingInlineRenamePath.reset();
    TEST_CHECK(manager.activateTab(tabBId));
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    harness.layout();
    TEST_CHECK(!tabB.needsRefresh);
    TEST_CHECK(tabB.selectedPath == selB);
    TEST_CHECK(tabB.scrollState.offset == scrollB);
    TEST_CHECK(!manager.m_pendingInlineRenamePath.has_value());

    // Simulate background monitor event on inactive Tab A.
    const auto selA = sharedDir / "f_2.txt";
    manager.m_tabs.findTab(tabAId)->selectedPath = selA;
    manager.m_tabs.findTab(tabAId)->selectedPaths = {selA.string()};
    writeFile(sharedDir / "external_touch.txt", "ext");
    if (manager.m_tabs.findTab(tabAId)->session.m_callbacks.onBackgroundMonitorDirty)
      manager.m_tabs.findTab(tabAId)->session.m_callbacks.onBackgroundMonitorDirty();
    TEST_CHECK(manager.m_tabs.findTab(tabAId)->needsRefresh);

    manager.m_tabs.shutdownAll();
  }

  static void testWastebinDirtyCoherence(const nythera::test::ScopedTestEnv& env) {
    const auto& root = env.work();
    const auto workDir = root / "trash_work";
    std::filesystem::create_directories(workDir);
    const auto trashTarget = workDir / "discard.txt";
    writeFile(trashTarget, "bye");

    FileManager manager("trash:/");
    // Ensure all operations in this test use an isolated WastebinBackend immediately.
    FileOperationManagerTestAccess::useIsolatedWastebinBackend(manager.m_operations);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId trashTabId = manager.activeTab().id;
    const filemanager::TabId workTabId = manager.openFolderInNewTab(workDir, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    auto& trashTab = *manager.m_tabs.findTab(trashTabId);
    TEST_CHECK(!trashTab.needsRefresh);

    const auto opId = manager.m_operations.trash({trashTarget.string()});
    TEST_CHECK(opId != 0);
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().m_loading; });
    TEST_CHECK(trashTab.needsRefresh);
    TEST_CHECK(!std::filesystem::exists(trashTarget));
    TEST_CHECK(std::filesystem::exists(env.data() / "Trash" / "files" / "discard.txt"));

    // Reactivate Wastebin tab so pending refresh clears, then switch back to workTabId.
    TEST_CHECK(manager.activateTab(trashTabId));
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    TEST_CHECK(!trashTab.needsRefresh);
    TEST_CHECK(manager.activateTab(workTabId));
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    if (manager.m_operations.canUndo()) {
      const auto undoOp = manager.m_operations.undo();
      TEST_CHECK(undoOp != 0);
      pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().m_loading; });
      TEST_CHECK(trashTab.needsRefresh);
      TEST_CHECK(std::filesystem::exists(trashTarget));
    }

    // Restore operation completion marks inactive Wastebin tab dirty.
    trashTab.needsRefresh = false;
    const auto restoreOp = manager.m_operations.restore({"trash:///nonexistent-test-item"});
    TEST_CHECK(restoreOp != 0);
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().m_loading; });
    TEST_CHECK(trashTab.needsRefresh);

    // EmptyWastebin operation completion marks inactive Wastebin tab dirty.
    trashTab.needsRefresh = false;
    const auto emptyOp = manager.m_operations.emptyWastebin(true);
    TEST_CHECK(emptyOp != 0);
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().m_loading; });
    TEST_CHECK(trashTab.needsRefresh);

    manager.m_tabs.shutdownAll();
  }

  static void testInactiveMountRemovalRedirectsMatchingTabs(const std::filesystem::path& root) {
    const auto localDir = root / "mnt_local";
    const auto mountRoot = root / "usb_mount";
    const auto mountSub = mountRoot / "photos";
    std::filesystem::create_directories(localDir);
    std::filesystem::create_directories(mountSub);

    FileManager manager(localDir.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId localTabId = manager.activeTab().id;
    const filemanager::TabId subTabId = manager.openFolderInNewTab(mountSub, false);
    const filemanager::TabId rootTabId = manager.openFolderInNewTab(mountRoot, false);
    pumpUntil([&] {
      return !manager.m_tabs.findTab(subTabId)->session.m_loading
          && !manager.m_tabs.findTab(rootTabId)->session.m_loading;
    });

    const auto fallback = filemanager::normalizeTabPath(FileManager::defaultTabPath());
    if (manager.m_places.m_callbacks.onMountRemoved)
      manager.m_places.m_callbacks.onMountRemoved(mountRoot);

    TEST_CHECK(manager.m_tabs.findTab(localTabId)->session.path() == localDir);
    TEST_CHECK(filemanager::normalizeTabPath(manager.m_tabs.findTab(subTabId)->session.path()) == fallback);
    TEST_CHECK(filemanager::normalizeTabPath(manager.m_tabs.findTab(rootTabId)->session.path()) == fallback);
    TEST_CHECK(manager.m_tabs.findTab(subTabId)->session.backHistory().empty());
    TEST_CHECK(manager.m_tabs.findTab(subTabId)->session.forwardHistory().empty());

    manager.m_tabs.shutdownAll();
  }

  static void testBackgroundFolderCountPausePreservesCompletedCache(const std::filesystem::path& root) {
    const auto parent = root / "fcount_parent";
    const auto doneSub = parent / "done_sub";
    const auto pendingSub = parent / "pending_sub";
    const auto other = root / "fcount_other";
    std::filesystem::create_directories(doneSub);
    std::filesystem::create_directories(pendingSub);
    std::filesystem::create_directories(other);
    writeFile(doneSub / "one.txt", "1");
    writeFile(pendingSub / "two.txt", "2");

    FileManager manager(parent.string());
    manager.navigate(parent, false);
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabAId = manager.activeTab().id;
    auto& sessionA = manager.activeTab().session;

    manager.m_listHeader.columnLayout().visible[filemanager::columnIndex(filemanager::FileColumn::Size)] = true;
    const auto findDirEntry = [&](const std::filesystem::path& p) -> FileEntry {
      const auto it = std::ranges::find(sessionA.entries(), p, &FileEntry::absPath);
      TEST_CHECK(it != sessionA.entries().end());
      return *it;
    };
    const auto doneEntry = findDirEntry(doneSub);
    const auto pendingEntry = findDirEntry(pendingSub);
    TEST_CHECK(!sessionA.folderItemCount(doneEntry).has_value());
    pumpUntil([&] {
      const auto it = sessionA.m_folderCounts.find(doneSub.string());
      return it != sessionA.m_folderCounts.end() && it->second.items.has_value();
    });
    TEST_CHECK(*sessionA.m_folderCounts.at(doneSub.string()).items == 1);

    // Queue pendingSub then deactivate immediately while folder count is incomplete/running.
    TEST_CHECK(!sessionA.folderItemCount(pendingEntry).has_value());
    TEST_CHECK(sessionA.m_folderCounts.contains(pendingSub.string()));

    const filemanager::TabId tabBId = manager.openFolderInNewTab(other, true);
    TEST_CHECK(tabBId != tabAId);
    TEST_CHECK(!sessionA.m_active);
    TEST_CHECK(!sessionA.m_folderCountRunning);
    TEST_CHECK(sessionA.m_folderCounts.contains(doneSub.string()));
    TEST_CHECK(sessionA.m_folderCounts.at(doneSub.string()).items == std::optional<std::uint64_t>{1});
    TEST_CHECK(!sessionA.m_folderCounts.contains(pendingSub.string()));

    // Switching back preserves completed cache cleanly.
    TEST_CHECK(manager.activateTab(tabAId));
    TEST_CHECK(sessionA.m_active);
    TEST_CHECK(sessionA.m_folderCounts.at(doneSub.string()).items == std::optional<std::uint64_t>{1});

    manager.m_tabs.shutdownAll();
  }

  static void testLastTabCloseQuitSafeguard(const std::filesystem::path& root) {
    const auto dirA = root / "quit_A";
    const auto dirB = root / "quit_B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    const auto payloadFile = dirA / "payload.bin";
    writeFile(payloadFile, std::string(1 << 20, 'K'));

    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabAId = manager.activeTab().id;
    const filemanager::TabId tabBId = manager.openFolderInNewTab(dirB, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    // Closing non-final tab closes normally without quitting.
    TEST_CHECK(manager.closeTab(tabBId));
    TEST_CHECK(manager.m_tabs.count() == 1);
    TEST_CHECK(manager.m_tabs.activeTabId() == tabAId);
    TEST_CHECK(!manager.m_quitting && manager.m_running);

    // Pause active operation so m_operations.active() is true when final tab is closed.
    const auto opId = manager.m_operations.copy({payloadFile.string()}, dirB.string());
    TEST_CHECK(opId != 0);
    manager.m_operations.pause(opId);
    TEST_CHECK(manager.m_operations.active());

    TEST_CHECK(!manager.closeTab(tabAId));
    TEST_CHECK(manager.m_tabs.count() == 1);
    TEST_CHECK(manager.m_operations.active());
    TEST_CHECK(!manager.m_quitting);
    TEST_CHECK(manager.m_running);

    manager.m_operations.resume(opId);
    pumpUntil([&] { return !manager.m_operations.active(); });

    // With no active operations, closing the final tab initiates quit.
    TEST_CHECK(!manager.closeTab(tabAId));
    TEST_CHECK(manager.m_quitting);
    TEST_CHECK(!manager.m_running);

    manager.m_tabs.shutdownAll();
  }

  static void testMultiTabWindowShutdownDrainsActiveTransfer(const std::filesystem::path& root) {
    const auto srcDir = root / "shutdown_src";
    const auto dstDir = root / "shutdown_dst";
    std::filesystem::create_directories(srcDir);
    std::filesystem::create_directories(dstDir);
    std::vector<std::string> sources;
    for (int i = 0; i < 8; ++i) {
      const auto p = srcDir / ("chunk_" + std::to_string(i) + ".bin");
      writeFile(p, std::string(131072, static_cast<char>('A' + i)));
      sources.push_back(p.string());
    }

    FileManager manager(srcDir.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    manager.openFolderInNewTab(dstDir, false);
    manager.openFolderInNewTab(root, false);
    pumpMs(20);

    const auto opId = manager.m_operations.copy(std::move(sources), dstDir.string());
    TEST_CHECK(opId != 0);
    TEST_CHECK(manager.m_operations.active());

    manager.m_windowOpen = false;
    manager.m_quitting = true;
    manager.m_tabs.shutdownAll();
    // Active operation drains to completion and flips m_running = false cleanly.
    pumpUntil([&] { return !manager.m_operations.active() && !manager.m_running; });
    for (int i = 0; i < 8; ++i)
      TEST_CHECK(std::filesystem::exists(dstDir / ("chunk_" + std::to_string(i) + ".bin")));
  }

  static void testTabBarVisibilityAndAlwaysShow(const std::filesystem::path& root) {
    const auto dirA = root / "ui_vis_A";
    const auto dirB = root / "ui_vis_B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);

    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    {
      TabScene scene(manager, 1120.0F, 600.0F);
      TEST_CHECK(!manager.m_alwaysShowTabBar);
      TEST_CHECK(!scene.tabBarNode->visible());
      TEST_CHECK(!scene.tabBarNode->participatesInLayout());
      const float reclaimedBottomY = scene.bottomPane->y();
      TEST_CHECK(std::abs(reclaimedBottomY - 32.0F) < 0.5F);

      const filemanager::TabId tabBId = manager.openFolderInNewTab(dirB, true);
      pumpUntil([&] { return !manager.activeSession().m_loading; });
      scene.relayout(manager, 1120.0F, 600.0F);
      TEST_CHECK(scene.tabBarNode->visible());
      TEST_CHECK(scene.tabBarNode->participatesInLayout());
      TEST_CHECK(manager.m_tabBar.m_slots.size() == 2);
      TEST_CHECK(scene.bottomPane->y() > reclaimedBottomY + 30.0F);

      TEST_CHECK(manager.closeTab(tabBId));
      scene.relayout(manager, 1120.0F, 600.0F);
      TEST_CHECK(!scene.tabBarNode->visible());
      TEST_CHECK(!scene.tabBarNode->participatesInLayout());
      TEST_CHECK(std::abs(scene.bottomPane->y() - reclaimedBottomY) < 0.5F);

      // Toggle Always Show Tab Bar via View menu action.
      manager.activateContextMenuEntry({.id = filemanager::menu_id::kAlwaysShowTabBar});
      TEST_CHECK(manager.m_alwaysShowTabBar);
      TEST_CHECK(manager.m_settings.alwaysShowTabBar());
      scene.relayout(manager, 1120.0F, 600.0F);
      TEST_CHECK(scene.tabBarNode->visible());
      TEST_CHECK(scene.tabBarNode->participatesInLayout());
      TEST_CHECK(manager.m_tabBar.m_slots.size() == 1);
      TEST_CHECK(
          manager.m_tabBar.m_slots[0].closeButton != nullptr && manager.m_tabBar.m_slots[0].closeButton->visible()
      );
      TEST_CHECK(manager.m_tabBar.m_newTabButton != nullptr && manager.m_tabBar.m_newTabButton->visible());

      // Click '+' button to open new tab.
      dispatchClick(manager.m_dispatcher, manager.m_tabBar.m_newTabButton);
      TEST_CHECK(manager.m_tabs.count() == 2);
      scene.relayout(manager, 1120.0F, 600.0F);
      pumpUntil([&] { return !manager.activeSession().m_loading; });

      // Close 2nd tab and confirm final tab stays closable (triggers quit on last tab).
      TEST_CHECK(manager.closeTab(manager.m_tabs.activeTabId()));
      scene.relayout(manager, 1120.0F, 600.0F);
      TEST_CHECK(manager.m_tabs.count() == 1);
      TEST_CHECK(!manager.m_quitting);
      dispatchClick(manager.m_dispatcher, manager.m_tabBar.m_slots[0].closeButton);
      TEST_CHECK(manager.m_quitting);

      manager.setAlwaysShowTabBar(false);
      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_tabBar.detachUi();
    }
    manager.m_tabs.shutdownAll();
  }

  static void testTabBarNarrowOverflowAndScrollIntoView(const std::filesystem::path& root) {
    const auto base = root / "ui_overflow";
    std::filesystem::create_directories(base);
    std::vector<std::filesystem::path> dirs;
    for (int i = 0; i < 12; ++i) {
      const auto d = base / ("overflow_long_directory_name_" + std::to_string(i));
      std::filesystem::create_directories(d);
      dirs.push_back(d);
    }

    FileManager manager(dirs[0].string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    std::vector<filemanager::TabId> tabIds{manager.activeTab().id};
    for (int i = 1; i < 12; ++i)
      tabIds.push_back(manager.openFolderInNewTab(dirs[static_cast<std::size_t>(i)], false));
    pumpMs(30);

    {
      // Simulate FileManager minWidth 560 px with 8 px root padding => 544 px mainPane (sidebar hidden < 760).
      TabScene scene(manager, 560.0F, 400.0F, 8.0F);
      TEST_CHECK(manager.m_tabBar.m_slots.size() == 12);
      const float expectedMaxPane = 544.0F;
      TEST_CHECK(scene.navRow->width() <= expectedMaxPane + 0.5F);
      TEST_CHECK(scene.bottomPane->width() <= expectedMaxPane + 0.5F);
      TEST_CHECK(scene.tabBarNode->width() <= expectedMaxPane + 0.5F);

      for (const auto& slot : manager.m_tabBar.m_slots)
        TEST_CHECK(std::abs(slot.container->width() - 112.0F) < 0.5F);

      float newBtnX = 0.0F;
      float newBtnY = 0.0F;
      Node::absolutePosition(manager.m_tabBar.m_newTabButton, newBtnX, newBtnY);
      float stripX = 0.0F;
      float stripY = 0.0F;
      Node::absolutePosition(manager.m_tabBar.m_stripRow, stripX, stripY);
      TEST_CHECK(
          newBtnX + manager.m_tabBar.m_newTabButton->width() <= stripX + manager.m_tabBar.m_stripRow->width() + 0.5F
      );
      TEST_CHECK(manager.m_tabBar.m_scroll->scrollable());

      // Switching to the last tab scrolls it into view; switching back to first scrolls back.
      TEST_CHECK(manager.activateTab(tabIds.back()));
      scene.relayout(manager, 560.0F, 400.0F);
      const float viewportW = manager.m_tabBar.m_scroll->contentViewportWidth();
      const float scrollOffLast = manager.m_tabBar.m_scroll->scrollOffset();
      const auto* lastSlot = manager.m_tabBar.m_slots.back().container;
      TEST_CHECK(scrollOffLast > 0.0F);
      TEST_CHECK(lastSlot->x() - scrollOffLast >= -0.5F);
      TEST_CHECK(lastSlot->x() + lastSlot->width() - scrollOffLast <= viewportW + 0.5F);

      TEST_CHECK(manager.activateTab(tabIds.front()));
      scene.relayout(manager, 560.0F, 400.0F);
      TEST_CHECK(manager.m_tabBar.m_scroll->scrollOffset() <= 0.5F);

      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_tabBar.detachUi();
    }
    manager.m_tabs.shutdownAll();
  }

  static void testTabBarThemeUpdateAndStationaryTypography(const std::filesystem::path& root) {
    const auto dirA = root / "ui_theme_A";
    const auto dirB = root / "ui_theme_B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);

    const Palette savedPalette = palette;
    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    manager.openFolderInNewTab(dirB, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    {
      TabScene scene(manager, 900.0F, 500.0F);
      Palette cyanTheme = savedPalette;
      cyanTheme.primary = Color{0.16F, 0.78F, 0.88F, 1.0F};
      cyanTheme.onSurface = Color{0.93F, 0.96F, 0.99F, 1.0F};
      cyanTheme.onSurfaceVariant = Color{0.62F, 0.68F, 0.74F, 1.0F};
      cyanTheme.outlineVariant = Color{0.25F, 0.30F, 0.36F, 1.0F};
      setPalette(cyanTheme);
      manager.m_tabBar.updateTheme();

      TEST_CHECK(manager.m_tabBar.m_underline != nullptr);
      TEST_CHECK(manager.m_tabBar.m_underline->visible());
      TEST_CHECK(std::abs(manager.m_tabBar.m_underline->style().fill.r - cyanTheme.primary.r) < 0.01F);
      TEST_CHECK(std::abs(manager.m_tabBar.m_underline->style().fill.g - cyanTheme.primary.g) < 0.01F);
      TEST_CHECK(std::abs(manager.m_tabBar.m_underline->style().fill.b - cyanTheme.primary.b) < 0.01F);

      TEST_CHECK(manager.m_tabBar.m_slots.size() == 2);
      const auto& inactiveSlot = manager.m_tabBar.m_slots[0];
      const auto& activeSlot = manager.m_tabBar.m_slots[1];
      TEST_CHECK(!inactiveSlot.active && activeSlot.active);
      TEST_CHECK(std::abs(activeSlot.label->color().r - cyanTheme.onSurface.r) < 0.01F);
      TEST_CHECK(std::abs(inactiveSlot.label->color().r - cyanTheme.onSurfaceVariant.r) < 0.01F);

      const auto* activeBg = backgroundRect(activeSlot.container);
      TEST_CHECK(activeBg != nullptr);
      TEST_CHECK(std::abs(activeBg->style().fill.r - cyanTheme.primary.r) < 0.01F);
      TEST_CHECK(std::abs(activeBg->style().fill.b - cyanTheme.primary.b) < 0.01F);

      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_tabBar.detachUi();
    }
    setPalette(savedPalette);
    manager.m_tabs.shutdownAll();
  }

  static void testTabKeyboardShortcutsAndEditingCancellation(const std::filesystem::path& root) {
    const auto dirA = root / "keys_A";
    const auto dirB = root / "keys_B";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);

    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabAId = manager.activeTab().id;
    const auto homeNormalized = filemanager::normalizeTabPath(FileManager::defaultTabPath());

    // Ctrl+T opens new tab in foreground.
    manager.handleWindowKeyEvent(KeyboardEvent{.sym = XKB_KEY_t, .modifiers = KeyMod::Ctrl, .pressed = true});
    TEST_CHECK(manager.m_tabs.count() == 2);
    const filemanager::TabId tabBId = manager.m_tabs.activeTabId();
    TEST_CHECK(tabBId != tabAId);
    TEST_CHECK(manager.activeSession().path() == homeNormalized);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    // Open a 3rd tab on dirB so order is [tabA, tabB, tabC] with tabC active.
    const filemanager::TabId tabCId = manager.openFolderInNewTab(dirB, true);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    // Ctrl+Tab cycles forward [tabC -> tabA -> tabB].
    manager.handleWindowKeyEvent(KeyboardEvent{.sym = XKB_KEY_Tab, .modifiers = KeyMod::Ctrl, .pressed = true});
    TEST_CHECK(manager.m_tabs.activeTabId() == tabAId);
    manager.handleWindowKeyEvent(KeyboardEvent{.sym = XKB_KEY_Tab, .modifiers = KeyMod::Ctrl, .pressed = true});
    TEST_CHECK(manager.m_tabs.activeTabId() == tabBId);

    // Ctrl+Shift+Tab backward via Shift+Tab, Ctrl+Shift+ISO_Left_Tab, and Ctrl+ISO_Left_Tab.
    manager.handleWindowKeyEvent(
        KeyboardEvent{.sym = XKB_KEY_Tab, .modifiers = KeyMod::Ctrl | KeyMod::Shift, .pressed = true}
    );
    TEST_CHECK(manager.m_tabs.activeTabId() == tabAId);
    manager.handleWindowKeyEvent(
        KeyboardEvent{.sym = XKB_KEY_ISO_Left_Tab, .modifiers = KeyMod::Ctrl | KeyMod::Shift, .pressed = true}
    );
    TEST_CHECK(manager.m_tabs.activeTabId() == tabCId);
    manager.handleWindowKeyEvent(
        KeyboardEvent{.sym = XKB_KEY_ISO_Left_Tab, .modifiers = KeyMod::Ctrl, .pressed = true}
    );
    TEST_CHECK(manager.m_tabs.activeTabId() == tabBId);

    // Location edit active & focused still responds to Ctrl+Tab and cancels location editing cleanly.
    auto locContainer = std::make_unique<Flex>();
    auto locInput = std::make_unique<Input>();
    manager.m_locationInput = locInput.get();
    locContainer->addChild(std::move(locInput));
    manager.m_dispatcher.setSceneRoot(locContainer.get());
    manager.setLocationEditMode(true);
    manager.m_dispatcher.setFocus(manager.m_locationInput->inputArea());
    TEST_CHECK(manager.m_editingLocation);

    manager.handleWindowKeyEvent(KeyboardEvent{.sym = XKB_KEY_Tab, .modifiers = KeyMod::Ctrl, .pressed = true});
    TEST_CHECK(manager.m_tabs.activeTabId() == tabCId);
    TEST_CHECK(!manager.m_editingLocation);
    manager.m_dispatcher.setSceneRoot(nullptr);
    manager.m_locationInput = nullptr;

    // Ctrl+W closes tabC, Ctrl+Shift+T reopens tabC at dirB.
    manager.handleWindowKeyEvent(KeyboardEvent{.sym = XKB_KEY_w, .modifiers = KeyMod::Ctrl, .pressed = true});
    TEST_CHECK(manager.m_tabs.count() == 2);
    TEST_CHECK(manager.m_tabs.findTab(tabCId) == nullptr);

    manager.handleWindowKeyEvent(
        KeyboardEvent{.sym = XKB_KEY_T, .modifiers = KeyMod::Ctrl | KeyMod::Shift, .pressed = true}
    );
    TEST_CHECK(manager.m_tabs.count() == 3);
    TEST_CHECK(manager.activeSession().path() == dirB);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    manager.m_tabs.shutdownAll();
  }

  static void testTabMenusFolderPlacesMiddleClickAndBackgroundSetting(const std::filesystem::path& root) {
    const auto dirA = root / "menus_A";
    const auto subFolder = dirA / "sub_folder";
    const auto regularFile = dirA / "plain.txt";
    const auto extraPlaceDir = root / "place_dir";
    std::filesystem::create_directories(subFolder);
    std::filesystem::create_directories(extraPlaceDir);
    writeFile(regularFile, "abc");

    FileManager manager(dirA.string());
    manager.navigate(dirA, false);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    const auto findMenuEntry = [](const std::vector<ContextMenuControlEntry>& entries, int id) {
      return std::ranges::find(entries, id, &ContextMenuControlEntry::id);
    };

    // File menu entries reflect selection and closed-tab availability.
    manager.activeTab().selectedPath = regularFile;
    manager.activeTab().selectedPaths = {regularFile.string()};
    const auto fileMenuRegular = manager.fileMenuEntries();
    TEST_CHECK(findMenuEntry(fileMenuRegular, filemanager::menu_id::kNewTab) != fileMenuRegular.end());
    TEST_CHECK(findMenuEntry(fileMenuRegular, filemanager::menu_id::kCloseTab) != fileMenuRegular.end());
    const auto fileOpenTabReg = findMenuEntry(fileMenuRegular, filemanager::menu_id::kFileOpenInNewTab);
    const auto reopenDisabled = findMenuEntry(fileMenuRegular, filemanager::menu_id::kReopenClosedTab);
    TEST_CHECK(fileOpenTabReg != fileMenuRegular.end() && !fileOpenTabReg->enabled);
    TEST_CHECK(reopenDisabled != fileMenuRegular.end() && !reopenDisabled->enabled);

    manager.activeTab().selectedPath = subFolder;
    manager.activeTab().selectedPaths = {subFolder.string()};
    const auto fileMenuFolder = manager.fileMenuEntries();
    const auto fileOpenTabFolder = findMenuEntry(fileMenuFolder, filemanager::menu_id::kFileOpenInNewTab);
    TEST_CHECK(fileOpenTabFolder != fileMenuFolder.end() && fileOpenTabFolder->enabled);

    // View menu exposes Always Show Tab Bar and Open New Tabs in Background toggles.
    const auto viewBefore = manager.viewMenuEntries();
    const auto alwaysShowView = findMenuEntry(viewBefore, filemanager::menu_id::kAlwaysShowTabBar);
    const auto bgViewBefore = findMenuEntry(viewBefore, filemanager::menu_id::kOpenTabsInBackground);
    TEST_CHECK(alwaysShowView != viewBefore.end() && alwaysShowView->checkmark && alwaysShowView->toggleState == 0);
    TEST_CHECK(bgViewBefore != viewBefore.end() && bgViewBefore->checkmark && bgViewBefore->toggleState == 0);

    // Folder menu group order puts kOpenFolderInNewTab directly after Open (10) and before Open in New Window (60).
    std::vector<ContextMenuControlEntry> folderEntries{
        {.id = 10, .label = "Open"},
        {.id = 60, .label = "Open in New Window"},
        {.id = filemanager::menu_id::kOpenFolderInNewTab, .label = "Open in New Tab"},
        {.id = 20, .label = "Add to Places"},
    };
    filemanager::groupFileMenuEntries(folderEntries, filemanager::FileMenuContext::Folder);
    TEST_CHECK(folderEntries.size() >= 3);
    TEST_CHECK(folderEntries[0].id == 10);
    TEST_CHECK(folderEntries[1].id == filemanager::menu_id::kOpenFolderInNewTab);
    TEST_CHECK(folderEntries[2].id == 60);

    // Places menu includes Open in New Tab right after Open for mounted Places & Wastebin, excludes unmounted.
    std::vector<ContextMenuControlEntry> capturedPlaceMenu;
    manager.m_places.m_callbacks.onShowMenu = [&](std::vector<ContextMenuControlEntry> entries, float, float) {
      capturedPlaceMenu = std::move(entries);
    };
    filemanager::Place mountedPlace{
        .label = "Extra", .identity = "saved:extra", .path = extraPlaceDir, .mounted = true
    };
    manager.m_places.openPlaceMenu(mountedPlace, 10.0F, 10.0F);
    TEST_CHECK(capturedPlaceMenu.size() >= 2);
    TEST_CHECK(capturedPlaceMenu[0].id == 30);
    TEST_CHECK(capturedPlaceMenu[1].id == filemanager::menu_id::kPlaceOpenInNewTab);

    filemanager::Place trashPlace{
        .label = "Wastebin", .identity = "personal:trash", .path = "trash:/", .mounted = true, .personal = true
    };
    manager.m_places.openPlaceMenu(trashPlace, 10.0F, 10.0F);
    TEST_CHECK(findMenuEntry(capturedPlaceMenu, filemanager::menu_id::kPlaceOpenInNewTab) != capturedPlaceMenu.end());

    filemanager::Place unmountedPlace{.label = "Unmounted", .identity = "drive:u", .path = {}, .mounted = false};
    manager.m_places.openPlaceMenu(unmountedPlace, 10.0F, 10.0F);
    TEST_CHECK(findMenuEntry(capturedPlaceMenu, filemanager::menu_id::kPlaceOpenInNewTab) == capturedPlaceMenu.end());

    // Open in New Tab opens in foreground by default, and in background when kOpenTabsInBackground is enabled.
    const filemanager::TabId firstId = manager.activeTab().id;
    manager.m_menus.setContextMenuTarget(subFolder);
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kOpenFolderInNewTab});
    TEST_CHECK(manager.m_tabs.count() == 2);
    TEST_CHECK(manager.m_tabs.activeTabId() != firstId);
    TEST_CHECK(manager.activeSession().path() == subFolder);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    manager.activateContextMenuEntry({.id = filemanager::menu_id::kOpenTabsInBackground});
    TEST_CHECK(manager.m_openTabsInBackground);
    TEST_CHECK(manager.m_settings.openTabsInBackground());
    const auto viewAfter = manager.viewMenuEntries();
    TEST_CHECK(findMenuEntry(viewAfter, filemanager::menu_id::kOpenTabsInBackground)->toggleState == 1);

    TEST_CHECK(manager.activateTab(firstId));
    manager.m_places.m_placeMenuTarget = mountedPlace;
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kPlaceOpenInNewTab});
    TEST_CHECK(manager.m_tabs.count() == 3);
    TEST_CHECK(manager.m_tabs.activeTabId() == firstId);
    TEST_CHECK(manager.m_tabs.tabs().back()->session.path() == extraPlaceDir);
    pumpMs(20);

    // Tab context menu enablement & actions.
    const auto menuForLast = manager.m_tabBar.tabContextMenuEntries(manager.m_tabs.tabs().back()->id);
    TEST_CHECK(findMenuEntry(menuForLast, filemanager::menu_id::kTabContextCloseOthers)->enabled);
    TEST_CHECK(!findMenuEntry(menuForLast, filemanager::menu_id::kTabContextCloseRight)->enabled);
    const auto menuForFirst = manager.m_tabBar.tabContextMenuEntries(firstId);
    TEST_CHECK(findMenuEntry(menuForFirst, filemanager::menu_id::kTabContextCloseRight)->enabled);

    manager.m_tabBar.m_contextMenuTabId = firstId;
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kTabContextDuplicate});
    TEST_CHECK(manager.m_tabs.count() == 4);
    TEST_CHECK(manager.m_tabs.indexOf(manager.m_tabs.activeTabId()) == 1);
    pumpUntil([&] { return !manager.activeSession().m_loading; });

    manager.m_tabBar.m_contextMenuTabId = firstId;
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kTabContextCloseRight});
    TEST_CHECK(manager.m_tabs.count() == 1);
    TEST_CHECK(!findMenuEntry(manager.fileMenuEntries(), filemanager::menu_id::kReopenClosedTab)->enabled == false);

    // Middle-click folder entry, Places row callback, and Tab hitArea (via dispatcher).
    const auto subIt = std::ranges::find(manager.activeSession().entries(), subFolder, &FileEntry::absPath);
    TEST_CHECK(subIt != manager.activeSession().entries().end());
    manager.activatePointerMiddleItem(static_cast<std::size_t>(subIt - manager.activeSession().entries().begin()));
    TEST_CHECK(manager.m_tabs.count() == 2);
    TEST_CHECK(manager.m_tabs.activeTabId() == firstId);

    const auto regIt = std::ranges::find(manager.activeSession().entries(), regularFile, &FileEntry::absPath);
    TEST_CHECK(regIt != manager.activeSession().entries().end());
    manager.activatePointerMiddleItem(static_cast<std::size_t>(regIt - manager.activeSession().entries().begin()));
    TEST_CHECK(manager.m_tabs.count() == 2); // Regular files do not open tabs on middle-click.

    if (manager.m_places.m_callbacks.onOpenPlaceInBackgroundTab) {
      manager.m_places.m_callbacks.onOpenPlaceInBackgroundTab(trashPlace);
      TEST_CHECK(manager.m_tabs.count() == 3);
      TEST_CHECK(manager.m_tabs.activeTabId() == firstId);
      TEST_CHECK(manager.m_tabs.tabs().back()->session.browsingTrash());
    }
    pumpMs(20);

    {
      TabScene scene(manager, 900.0F, 500.0F);
      TEST_CHECK(manager.m_tabBar.m_slots.size() == 3);
      const filemanager::TabId middleVictimId = manager.m_tabBar.m_slots[2].tabId;
      dispatchClick(manager.m_dispatcher, manager.m_tabBar.m_slots[2].hitArea, BTN_MIDDLE);
      TEST_CHECK(manager.m_tabs.count() == 2);
      TEST_CHECK(manager.m_tabs.findTab(middleVictimId) == nullptr);
      scene.relayout(manager, 900.0F, 500.0F);
      TEST_CHECK(manager.m_tabBar.m_slots.size() == 2);

      manager.setOpenTabsInBackground(false);
      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_tabBar.detachUi();
    }
    manager.m_tabs.shutdownAll();
  }

  static void testTabBarDragReorderAndEscapeCancel(const std::filesystem::path& root) {
    const auto dirA = root / "drag_A";
    const auto dirB = root / "drag_B";
    const auto dirC = root / "drag_C";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    std::filesystem::create_directories(dirC);

    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().m_loading; });
    const filemanager::TabId tabA = manager.activeTab().id;
    const filemanager::TabId tabB = manager.openFolderInNewTab(dirB, false);
    const filemanager::TabId tabC = manager.openFolderInNewTab(dirC, false);
    pumpMs(20);

    {
      TabScene scene(manager, 900.0F, 500.0F);
      TEST_CHECK(manager.m_tabBar.m_slots.size() == 3);
      const auto* slotA = manager.m_tabBar.m_slots[0].container;
      const auto* slotB = manager.m_tabBar.m_slots[1].container;
      const auto* slotC = manager.m_tabBar.m_slots[2].container;

      float ax = 0.0F, ay = 0.0F;
      Node::absolutePosition(slotA, ax, ay);
      const float startX = ax + 24.0F;
      const float startY = ay + slotA->height() * 0.5F;

      manager.m_dispatcher.pointerMotion(startX, startY, 1);
      manager.m_dispatcher.pointerButton(startX, startY, BTN_LEFT, true, 2, 10, false);
      TEST_CHECK(manager.m_tabBar.m_dragArmed);
      TEST_CHECK(!manager.m_tabBar.m_dragging);

      // Sub-threshold motion does not initiate reorder drag.
      TEST_CHECK(!manager.m_tabBar.handlePointer(
          PointerEvent{.type = PointerEvent::Type::Motion, .sx = startX + 3.0F, .sy = startY}, startX + 3.0F, startY
      ));
      TEST_CHECK(!manager.m_tabBar.m_dragging);

      // Crossing 5 px threshold starts drag and shows insertion marker.
      float cx = 0.0F, cy = 0.0F;
      Node::absolutePosition(slotC, cx, cy);
      const float pastC = cx + slotC->width() * 0.75F;
      TEST_CHECK(manager.m_tabBar.handlePointer(
          PointerEvent{.type = PointerEvent::Type::Motion, .sx = pastC, .sy = startY}, pastC, startY
      ));
      TEST_CHECK(manager.m_tabBar.m_dragging);
      TEST_CHECK(manager.m_tabBar.m_dropIndicator != nullptr && manager.m_tabBar.m_dropIndicator->visible());

      // Pressing Escape cancels drag without reordering.
      manager.handleWindowKeyEvent(KeyboardEvent{.sym = XKB_KEY_Escape, .pressed = true});
      TEST_CHECK(!manager.m_tabBar.gestureActive());
      TEST_CHECK(!manager.m_tabBar.m_dropIndicator->visible());
      TEST_CHECK(manager.m_tabs.tabs()[0]->id == tabA);
      TEST_CHECK(manager.m_tabs.tabs()[1]->id == tabB);
      TEST_CHECK(manager.m_tabs.tabs()[2]->id == tabC);

      // Re-drag tabA past tabB's midpoint and release to commit [tabB, tabA, tabC].
      manager.m_dispatcher.pointerButton(startX, startY, BTN_LEFT, false, 3, 15, false);
      manager.m_dispatcher.pointerMotion(startX, startY, 4);
      manager.m_dispatcher.pointerButton(startX, startY, BTN_LEFT, true, 5, 20, false);
      TEST_CHECK(manager.m_tabBar.m_dragArmed);
      float bx = 0.0F, by = 0.0F;
      Node::absolutePosition(slotB, bx, by);
      const float pastB = bx + slotB->width() * 0.75F;
      TEST_CHECK(manager.m_tabBar.handlePointer(
          PointerEvent{.type = PointerEvent::Type::Motion, .sx = pastB, .sy = startY}, pastB, startY
      ));
      TEST_CHECK(manager.m_tabBar.m_dragging);
      TEST_CHECK(manager.m_tabBar.handlePointer(
          PointerEvent{
              .type = PointerEvent::Type::Button, .sx = pastB, .sy = startY, .button = BTN_LEFT, .pressed = false
          },
          pastB, startY
      ));
      TEST_CHECK(!manager.m_tabBar.gestureActive());
      TEST_CHECK(manager.m_tabs.tabs()[0]->id == tabB);
      TEST_CHECK(manager.m_tabs.tabs()[1]->id == tabA);
      TEST_CHECK(manager.m_tabs.tabs()[2]->id == tabC);

      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_tabBar.detachUi();
    }
    manager.m_tabs.shutdownAll();
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-filemanager-tabs"});
    const auto& work = env.work();

    FileManagerTestAccess::testIndependentTabStateAndSharedColumns(work);
    FileManagerTestAccess::testLifecycleDuplicateReorderAndBoundedClosedHistory(work);
    FileManagerTestAccess::testWindowTitleAndSanitization(work);
    FileManagerTestAccess::testStaleAsyncCallbackIsolationSameGeneration(work);
    FileManagerTestAccess::testCloseDuringEnumAndActiveOperationCutConsumption(work);
    FileManagerTestAccess::testSwitchCancelsTransientGestures(work);
    FileManagerTestAccess::testCrossTabCopyPasteAndReveal(work);
    FileManagerTestAccess::testSameDirectoryTabsMonitorDirtyAndCreateIsolation(work);
    FileManagerTestAccess::testWastebinDirtyCoherence(env);
    FileManagerTestAccess::testInactiveMountRemovalRedirectsMatchingTabs(work);
    FileManagerTestAccess::testBackgroundFolderCountPausePreservesCompletedCache(work);
    FileManagerTestAccess::testLastTabCloseQuitSafeguard(work);
    FileManagerTestAccess::testMultiTabWindowShutdownDrainsActiveTransfer(work);
    FileManagerTestAccess::testTabBarVisibilityAndAlwaysShow(work);
    FileManagerTestAccess::testTabBarNarrowOverflowAndScrollIntoView(work);
    FileManagerTestAccess::testTabBarThemeUpdateAndStationaryTypography(work);
    FileManagerTestAccess::testTabKeyboardShortcutsAndEditingCancellation(work);
    FileManagerTestAccess::testTabMenusFolderPlacesMiddleClickAndBackgroundSetting(work);
    FileManagerTestAccess::testTabBarDragReorderAndEscapeCancel(work);
  });
}
