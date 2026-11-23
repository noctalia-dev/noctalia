#include "core/files/directory_scanner.h"
#include "core/files/display_text.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "filemanager/admin_helper_session.h"
#include "filemanager/admin_protocol.h"
#include "filemanager/admin_session_client.h"
#include "filemanager/file_manager.h"
#include "filemanager/file_transfer_payload.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/tab_presentation.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "test_isolation.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"
#include "ui/palette.h"
#include "wayland/toplevel_surface.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <source_location>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  using namespace filemanager;
  using namespace filemanager::menu_id;

  struct TempDir {
    nythera::test::ScopedTempDir inner;
    std::filesystem::path path = inner.path();
    explicit TempDir(const char* prefix) : inner(prefix) {}
  };

  void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    TEST_CHECK(out.good());
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    TEST_CHECK(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  template <typename Predicate>
  void pumpUntil(
      Predicate&& predicate, int timeoutMs = 5000, std::source_location caller = std::source_location::current()
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      if (!g_main_context_iteration(nullptr, FALSE))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!predicate())
      g_printerr("Timed out at %s:%u\n", caller.file_name(), caller.line());
    TEST_CHECK(predicate());
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
  }

  void drainMainContext(int rounds = 10) {
    for (int i = 0; i < rounds; ++i) {
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
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

  struct InProcessHelperFixture {
    struct SharedChannelState {
      int clientReadFd = -1;
      int clientWriteFd = -1;
      int helperReadFd = -1;
      int helperWriteFd = -1;
      std::mutex fdMutex;
      std::thread helperThread;
      AdminHelperSessionResult sessionResult;
      std::atomic<bool> finished{false};

      ~SharedChannelState() {
        closeClientWrite();
        abortTransport();
        if (helperThread.joinable())
          helperThread.join();
        std::lock_guard lock(fdMutex);
        if (clientReadFd >= 0)
          ::close(clientReadFd);
        if (helperReadFd >= 0)
          ::close(helperReadFd);
      }

      void closeClientWrite() {
        std::lock_guard lock(fdMutex);
        if (clientWriteFd >= 0) {
          ::close(clientWriteFd);
          clientWriteFd = -1;
        }
      }

      void closeHelperWrite() {
        std::lock_guard lock(fdMutex);
        if (helperWriteFd >= 0) {
          ::close(helperWriteFd);
          helperWriteFd = -1;
        }
      }

      void abortTransport() {
        closeClientWrite();
        std::lock_guard lock(fdMutex);
        if (clientReadFd >= 0) {
          ::close(clientReadFd);
          clientReadFd = -1;
        }
      }
    };

    std::atomic<int> activeHelpers{0};
    std::atomic<int> totalSpawned{0};
    std::atomic<int> totalExited{0};
    std::atomic<std::uint32_t> nextPid{7000};
    std::mutex resultsMutex;
    std::vector<AdminHelperSessionResult> results;
    std::vector<std::shared_ptr<SharedChannelState>> activeStates;

    AdminTransportLauncher makeLauncher(AdminHelperSessionConfig baseOpts = {.helperPid = 7000, .invokingUid = 1000}) {
      return [this, baseOpts](GCancellable* cancellable) -> AdminTransportLaunchResult {
        if (g_cancellable_is_cancelled(cancellable)) {
          return AdminTransportLaunchResult{
              .failureReason = AdminSessionFailureReason::AuthCancelled,
              .cancelled = true,
              .error = "Administrator authentication cancelled.",
          };
        }

        int c2h[2]{-1, -1};
        int h2c[2]{-1, -1};
        if (::pipe2(c2h, O_CLOEXEC) != 0) {
          return AdminTransportLaunchResult{
              .failureReason = AdminSessionFailureReason::TransportError,
              .error = "pipe2 failed",
          };
        }
        if (::pipe2(h2c, O_CLOEXEC) != 0) {
          ::close(c2h[0]);
          ::close(c2h[1]);
          return AdminTransportLaunchResult{
              .failureReason = AdminSessionFailureReason::TransportError,
              .error = "pipe2 failed",
          };
        }

        auto state = std::make_shared<SharedChannelState>();
        state->helperReadFd = c2h[0];
        state->clientWriteFd = c2h[1];
        state->clientReadFd = h2c[0];
        state->helperWriteFd = h2c[1];

        AdminHelperSessionConfig opts = baseOpts;
        opts.helperPid = nextPid.fetch_add(1);

        activeHelpers.fetch_add(1);
        totalSpawned.fetch_add(1);
        {
          std::lock_guard lock(resultsMutex);
          activeStates.push_back(state);
        }

        state->helperThread = std::thread([this, state, opts]() {
          state->sessionResult = runAdminHelperSession(state->helperReadFd, state->helperWriteFd, opts);
          state->closeHelperWrite();
          state->finished.store(true);
          activeHelpers.fetch_sub(1);
          totalExited.fetch_add(1);
          std::lock_guard lock(resultsMutex);
          results.push_back(state->sessionResult);
        });

        auto channel = std::make_unique<AdminTransportChannel>();
        channel->readFd = state->clientReadFd;
        channel->writeFd = state->clientWriteFd;
        channel->closeWrite = [state] { state->closeClientWrite(); };
        channel->abortTransport = [state] { state->abortTransport(); };
        channel->waitForExit = [state]() -> int {
          state->closeClientWrite();
          if (state->helperThread.joinable())
            state->helperThread.join();
          return 0;
        };
        return AdminTransportLaunchResult{.channel = std::move(channel)};
      };
    }

    void killAllActiveHelpersUnexpectedly() {
      std::vector<std::shared_ptr<SharedChannelState>> copy;
      {
        std::lock_guard lock(resultsMutex);
        copy = activeStates;
      }
      for (const auto& st : copy) {
        st->closeHelperWrite();
        st->closeClientWrite();
      }
    }
  };

  const ContextMenuControlEntry* findEntryById(const std::vector<ContextMenuControlEntry>& entries, int id) noexcept {
    for (const auto& entry : entries) {
      if (entry.id == id)
        return &entry;
      if (const auto* sub = findEntryById(entry.submenuEntries, id))
        return sub;
    }
    return nullptr;
  }

  void typeInlineTextAndSubmit(InputDispatcher& dispatcher, std::string_view text) {
    dispatcher.keyEvent('a', 0, KeyMod::Ctrl, true);
    for (const unsigned char ch : text)
      dispatcher.keyEvent(static_cast<std::uint32_t>(ch), static_cast<std::uint32_t>(ch), 0, true);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
  }

} // namespace

class FileOperationManagerTestAccess {
public:
  static void useIsolatedEmptyWastebinBackend(FileOperationManager& operations, std::atomic_bool& called) {
    operations.m_wastebinBackend.enumerateTopLevel = [&called](GCancellable*, std::vector<std::string>&, std::string&) {
      called = true;
      return true;
    };
  }
};

class FileManagerTestAccess {
public:
  struct Stage3GridAdapter final : public VirtualGridAdapter {
    FileManager& manager;
    Renderer& renderer;

    Stage3GridAdapter(FileManager& owner, Renderer& r) : manager(owner), renderer(r) {}

    [[nodiscard]] std::size_t itemCount() const override { return manager.activeSession().entries().size(); }

    [[nodiscard]] std::optional<bool> selectedState(std::size_t index) const override {
      const auto& tab = manager.activeTab();
      if (index >= tab.session.entries().size())
        return false;
      return tab.selectedPaths.contains(tab.session.entries()[index].absPath.string());
    }

    [[nodiscard]] std::unique_ptr<Node> createTile() override {
      auto row = std::make_unique<FileEntryRow>(1.0F);
      row->setSelectionRoles(ColorRole::PrimaryContainer, ColorRole::OnPrimaryContainer);
      return row;
    }

    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
      const auto& tab = manager.activeTab();
      auto& session = manager.activeSession();
      if (index >= session.entries().size())
        return;
      auto& row = static_cast<FileEntryRow&>(tile);
      row.setAllowThumbnailsAndMetadata(tab.capabilities().canGenerateThumbnails);
      row.setColumns(manager.m_listHeader.visibleColumns(), manager.m_listHeader.listColumns());
      row.bind(renderer, session.entries()[index], index, tile.width(), selected, hovered, false);
    }

    InputArea* beginInlineEdit(
        Node& tile, const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
        std::function<void()> cancel
    ) override {
      return static_cast<FileEntryRow&>(tile).beginInlineEdit(path, name, std::move(submit), std::move(cancel));
    }

    void endInlineEdit(Node& tile, const std::string& path) override {
      static_cast<FileEntryRow&>(tile).endInlineEdit(path);
    }
  };

  struct Stage3Scene {
    StubRenderer renderer;
    std::unique_ptr<Flex> root;
    Flex* appBar = nullptr;
    Node* tabBarNode = nullptr;
    std::unique_ptr<Stage3GridAdapter> adapter;
    std::unique_ptr<VirtualGridView> ownedGrid;
    VirtualGridView* grid = nullptr;

    Stage3Scene(FileManager& manager, float width, float height) {
      root = std::make_unique<Flex>();
      root->setDirection(FlexDirection::Vertical);
      root->setAlign(FlexAlign::Stretch);
      root->setGap(4.0F);
      appBar = static_cast<Flex*>(root->addChild(manager.buildHeaderAppBar()));
      tabBarNode = root->addChild(manager.m_tabBar.buildUi());

      adapter = std::make_unique<Stage3GridAdapter>(manager, renderer);
      ownedGrid = std::make_unique<VirtualGridView>();
      grid = ownedGrid.get();
      grid->setFlexGrow(1.0F);
      grid->setColumns(1);
      grid->setSquareCells(false);
      grid->setMinCellWidth(190.0F);
      grid->setCellHeight(32.0F);
      grid->setRowGap(1.0F);
      grid->setAdapter(adapter.get());
      grid->bindScrollState(&manager.activeTab().scrollState);
      grid->setOnSelectionChanged([&manager](std::optional<std::size_t> index) {
        if (manager.m_restoringSelection)
          return;
        auto& tab = manager.activeTab();
        const auto& entries = tab.session.entries();
        if (index.has_value() && *index < entries.size()) {
          tab.selectedPath = entries[*index].absPath;
          tab.selectedPaths = {entries[*index].absPath.string()};
          tab.selectionAnchor = entries[*index].absPath;
        } else {
          tab.selectedPath.reset();
          tab.selectedPaths.clear();
          tab.selectionAnchor.reset();
        }
      });
      manager.m_grid = grid;
      manager.m_surface = std::make_unique<ToplevelSurface>(manager.m_wayland);
      root->addChild(std::move(ownedGrid));
      manager.m_dispatcher.setSceneRoot(root.get());
      manager.m_dispatcher.setFocus(grid->focusArea());
      relayout(manager, width, height);
    }

    ~Stage3Scene() {
      // Clear grid before FileManager destructor runs if Stage3Scene outlives nothing.
    }

    void relayout(FileManager& manager, float width, float height) {
      manager.updateAdminHeaderUi(static_cast<int>(width));
      (void)manager.m_tabBar.rebuildIfDirty();
      root->setSize(width, height);
      root->layout(renderer);
      manager.m_tabBar.syncLayout(renderer);
      if (manager.m_pendingInlineRenamePath && !manager.activeSession().loading()) {
        const auto pending = *manager.m_pendingInlineRenamePath;
        manager.m_pendingInlineRenamePath.reset();
        manager.beginRenamePath(pending);
        root->layout(renderer);
      }
    }
  };

  static void runAll() {
    testInlineRenameRecoveryAfterHelperDisconnect();
    testNormalTabPrivilegeIsolation();
    testUiWorkflowsCreateInlineRenameCollisionsAndMenuEnablement();
    testAdministratorPasteAndNormalTabIsolation();
    testAdministratorCopyLifecycle();
    testLocationClipboardPaste();
    testAdministratorPropertiesDropAndUserWastebin();
  }

private:
  static void testAdministratorPropertiesDropAndUserWastebin() {
    TempDir tmp("nythera-admin-properties");
    const auto directory = tmp.path / "administrator";
    const auto target = directory / "properties.txt";
    const auto normal = tmp.path / "normal";
    const auto payload = normal / "drop.txt";
    writeFile(target, "Properties test");
    writeFile(payload, "Dropped bytes");
    TEST_CHECK(::chmod(target.c_str(), 0600) == 0);
    std::atomic_bool wastebinEnumerated = false;
    InProcessHelperFixture fixture;
    FileManager manager(directory.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());
    const auto adminId = manager.activeTab().id;
    const auto normalId = manager.openFolderInNewTab(normal, false);
    TEST_CHECK(manager.enterAdministratorMode(adminId));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });

    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    TEST_CHECK(manager.activeTab().adminSession.statInfoAsync(
        target, true,
        [&](bool ok, const AdminStatRecord& record, auto, const std::string&) {
          TEST_CHECK(ok);
          g_autoptr(GFileInfo) info = decodePropertyInfo(record.properties);
          TEST_CHECK(info && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR);
          TEST_CHECK((g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 0777) == 0600);
          TEST_CHECK(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID) == geteuid());
          device = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
          inode = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_INODE);
        },
        AdminStatDetail::Properties
    ));
    pumpUntil([&] { return inode != 0; });
    auto* administrator = &manager.activeTab();
    TEST_CHECK(manager.activateTab(normalId));
    bool applied = false;
    TEST_CHECK(manager.m_operations.modifyProperties(
        {.path = target, .device = device, .inode = inode, .original = 0600, .edited = 0640},
        static_cast<std::uint32_t>(geteuid()),
        [&](const auto& result) {
          TEST_CHECK(result.ok() && result.succeeded == 1);
          applied = true;
        },
        administrator
    ));
    pumpUntil([&] { return applied; });
    struct stat state{};
    TEST_CHECK(::stat(target.c_str(), &state) == 0 && (state.st_mode & 0777) == 0640);
    TEST_CHECK(manager.activeTab().id == normalId && !manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.m_operations.operations().back().adminOrigin->tabId == adminId);

    TEST_CHECK(manager.activateTab(adminId));
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.activateContextMenuEntry({.id = 21});
    pumpUntil([&] {
      return std::ranges::any_of(manager.m_settings.loadSavedPlaces(), [&](const auto& place) {
        return place.path == directory;
      });
    });
    manager.m_transfers.m_protocolDropDestination = directory;
    manager.m_transfers.m_dragTargetValid = true;
    auto receive = manager.m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    TEST_CHECK(receive);
    std::optional<bool> finished;
    receive(encodeFileTransfer({payload.string()}, false), [&](bool success) { finished = success; });
    pumpUntil([&] { return finished.has_value() && !manager.activeSession().loading(); });
    TEST_CHECK(*finished && readFile(directory / payload.filename()) == "Dropped bytes");
    TEST_CHECK(manager.m_operations.operations().back().adminOrigin->tabId == adminId);

    const auto alias = tmp.path / "drop-alias";
    std::filesystem::create_directory_symlink(directory, alias);
    const auto operationsBeforeAlias = manager.m_operations.operations().size();
    manager.m_transfers.m_protocolDropDestination = alias;
    manager.m_transfers.m_dragTargetValid = true;
    receive = manager.m_transfers.receiveProtocolDrop(WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    TEST_CHECK(receive);
    finished.reset();
    receive(encodeFileTransfer({(directory / payload.filename()).string()}, false), [&](bool success) {
      finished = success;
    });
    pumpUntil([&] { return finished.has_value(); });
    TEST_CHECK(!*finished && manager.m_operations.operations().size() == operationsBeforeAlias);

    manager.activateContextMenuEntry({.id = 114});
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().loading(); });
    TEST_CHECK(
        readFile(directory / "New Document.odt") == readFile(paths::assetPath("templates/filemanager/office/blank.odt"))
    );
    TEST_CHECK(manager.m_operations.operations().back().adminOrigin->tabId == adminId);

    FileOperationManagerTestAccess::useIsolatedEmptyWastebinBackend(manager.m_operations, wastebinEnumerated);
    manager.emptyWastebinAfterConfirmation(adminId, 1);
    pumpUntil([&] { return !manager.m_operations.active(); });
    const auto& empty = manager.m_operations.operations().back();
    TEST_CHECK(wastebinEnumerated && empty.kind == FileOperationManager::Kind::EmptyWastebin && empty.succeeded);
    TEST_CHECK(!empty.adminOrigin && manager.activeTab().hasActiveAdminSession());
  }

  static void testAdministratorPasteAndNormalTabIsolation() {
    TempDir tmp("nythera-stage41-paste");
    const auto source = tmp.path / "clipboard" / "payload";
    const auto target = tmp.path / "restricted";
    const auto normal = tmp.path / "normal";
    writeFile(source, "paste contents");
    TEST_CHECK(::mkdir(target.c_str(), 0700) == 0);
    TEST_CHECK(::mkdir(normal.c_str(), 0700) == 0);

    InProcessHelperFixture fixture;
    FileManager manager(target.string());
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher({
        .invokingUid = static_cast<std::uint32_t>(::geteuid()),
    }));
    pumpUntil([&] { return !manager.activeSession().loading(); });
    const auto adminId = manager.activeTab().id;
    const auto normalId = manager.openFolderInNewTab(normal, false);
    TEST_CHECK(manager.enterAdministratorMode(adminId));
    pumpUntil([&] { return manager.activeTab().capabilities().canMutate; });

    NativeDataTransfer::ReadCallback pending;
    manager.m_transfers.m_readPasteSelectionForTesting = [&](auto cb) { pending = std::move(cb); };
    const auto paste = [&](std::vector<std::string> paths, bool cut) {
      manager.pasteClipboard();
      TEST_CHECK(pending);
      std::exchange(pending, {})(encodeFileTransfer(std::move(paths), cut));
    };
    const auto& operations = manager.m_operations.operations();
    manager.pasteClipboard();
    TEST_CHECK(pending);
    auto conflictingClipboard = encodeFileTransfer({source.string()}, true);
    conflictingClipboard["application/x-kde-cutselection"] = "0";
    std::exchange(pending, {})(std::move(conflictingClipboard));
    TEST_CHECK(operations.empty() && manager.m_statusMessage.contains("markers disagree"));

    // A delayed clipboard read still belongs to the Administrator tab that requested it.
    manager.pasteClipboard();
    TEST_CHECK(pending && manager.activateTab(normalId));
    std::exchange(pending, {})(encodeFileTransfer({source.string()}, false));
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.m_operations.copy({source.string()}, normal.string(), &manager.activeTab()) == 0);
    pumpUntil([&] { return operations.size() == 1 && operations.back().finished; });
    TEST_CHECK(!std::filesystem::exists(normal / source.filename()));

    TEST_CHECK(manager.activateTab(adminId));
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canMutate; });
    TEST_CHECK(operations.back().succeeded && operations.back().adminOrigin && operations.back().undoUnavailable);
    TEST_CHECK(operations.back().processedBytes == 14 && operations.back().processedFiles == 1);
    TEST_CHECK(!manager.m_operations.canUndo() && readFile(target / source.filename()) == "paste contents");
    TEST_CHECK(manager.activeTab().selectedPath == target / source.filename());

    using DialogKind = filemanager::ConflictDialogKind;
    manager.m_modalDialog.m_conflictDialogHook = [&](const auto& data, bool more, auto respond) {
      TEST_CHECK(data.pathAccess == ConflictPathAccess::ExecutorOnly && data.kind == DialogKind::File);
      TEST_CHECK(!more && data.incoming.path == source);
      respond(2, "alternative.txt");
      return true;
    };
    paste({source.string()}, false);
    pumpUntil([&] { return operations.size() == 2 && operations.back().finished; });
    TEST_CHECK(operations.back().succeeded && readFile(target / "alternative.txt") == "paste contents");
    TEST_CHECK(readFile(target / source.filename()) == "paste contents" && readFile(source) == "paste contents");

    TEST_CHECK(manager.activateTab(normalId));
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canMutate; });
    paste({source.string()}, false);
    pumpUntil([&] { return operations.size() == 3 && operations.back().finished; });
    TEST_CHECK(operations.back().succeeded && !operations.back().adminOrigin && manager.m_operations.canUndo());
    TEST_CHECK(readFile(normal / source.filename()) == "paste contents");
    TEST_CHECK(manager.m_tabs.findTab(adminId)->hasActiveAdminSession());

    const auto tree = normal / "selected-tree";
    const auto empty = normal / "selected-empty";
    writeFile(tree / "sub" / "child", "mixed paste");
    writeFile(empty, "");
    TEST_CHECK(manager.activateTab(adminId));
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canMutate; });
    paste({empty.string(), tree.string()}, false);
    pumpUntil([&] {
      return operations.size() == 4 && operations.back().finished && !manager.activeSession().loading();
    });
    TEST_CHECK(operations.back().succeeded && operations.back().requestedItemCount == 2);
    TEST_CHECK(operations.back().processedFiles == 2 && operations.back().completedEntries.size() == 2);
    TEST_CHECK(manager.activeTab().selectedPaths.contains((target / tree.filename()).string()));
    TEST_CHECK(manager.activeTab().selectedPaths.contains((target / empty.filename()).string()));
    TEST_CHECK(readFile(target / tree.filename() / "sub" / "child") == "mixed paste");
    TEST_CHECK(std::filesystem::file_size(target / empty.filename()) == 0);

    const auto cut = normal / "cut.txt";
    writeFile(cut, "cut payload");
    paste({cut.string()}, true);
    pumpUntil([&] { return operations.size() == 5 && operations.back().finished; });
    TEST_CHECK(operations.back().kind == FileOperationManager::Kind::Move && operations.back().succeeded);
    TEST_CHECK(operations.back().movedSources == std::vector{cut.string()});
    TEST_CHECK(!std::filesystem::exists(cut) && readFile(target / cut.filename()) == "cut payload");

    // Exercise the remaining existing conflict choices through ModalDialogHost.
    for (const int choice : {1, 3, 4, 5}) {
      pumpUntil([&] { return !manager.activeSession().loading(); });
      const auto count = operations.size();
      unsigned prompts = 0;
      manager.m_modalDialog.m_conflictDialogHook = [choice, &prompts](const auto& data, bool more, auto respond) {
        ++prompts;
        TEST_CHECK(data.pathAccess == ConflictPathAccess::ExecutorOnly);
        TEST_CHECK(data.kind == (choice == 5 ? DialogKind::Directory : DialogKind::File));
        TEST_CHECK(more == (choice == 1 || choice == 5));
        respond(choice == 1 ? 11 : choice, {});
        return true;
      };
      std::vector<std::string> paths{source.string()};
      if (choice == 5) {
        paths = {tree.string()};
        std::filesystem::remove_all(tree / "sub");
        writeFile(tree / "added.txt", "merge");
      } else if (choice == 1) {
        writeFile(source, "replacement");
        writeFile(empty, "also replaced");
        paths.push_back(empty.string());
      }
      manager.m_transfers.m_consumedCutSelection.reset(); // The injected clipboard payload is a new selection.
      paste(std::move(paths), false);
      pumpUntil([&] { return operations.size() == count + 1 && operations.back().finished; });
      TEST_CHECK(prompts == 1 && operations.back().cancelled == (choice == 4));
      TEST_CHECK(operations.back().succeeded == (choice != 4));
      if (choice == 3)
        TEST_CHECK(operations.back().completedEntries.empty() && operations.back().status.contains("0 of 1"));
    }
    TEST_CHECK(readFile(target / source.filename()) == "replacement");
    TEST_CHECK(readFile(target / empty.filename()) == "also replaced");
    TEST_CHECK(readFile(target / tree.filename() / "added.txt") == "merge");

    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_modalDialog.m_conflictDialogHook = [](const auto&, bool, auto respond) {
      respond(2, "alternative.txt");
      return true;
    };
    auto count = operations.size();
    paste({source.string()}, false);
    pumpUntil([&] { return operations.size() == count + 1 && operations.back().finished; });
    TEST_CHECK(!operations.back().succeeded && operations.back().status.contains("alternative name already exists"));

    bool waitingForDecision = false;
    manager.m_modalDialog.m_conflictDialogHook = [&](const auto&, bool, auto) {
      waitingForDecision = true;
      return true;
    };
    count = operations.size();
    paste({source.string()}, false);
    pumpUntil([&] { return waitingForDecision; });
    manager.finishAdministratorMode(adminId, "Administrator Mode ended.");
    pumpUntil([&] { return operations.size() == count + 1 && operations.back().finished; });
    TEST_CHECK(operations.back().cancelled && !operations.back().outcomeUncertain);
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
  }

  static void testAdministratorCopyLifecycle() {
    using namespace std::chrono_literals;
    enum class End { Resume, Cancel, Finish, CloseTab, Expire, Disconnect, Shutdown };
    for (const auto end :
         {End::Resume, End::Cancel, End::Finish, End::CloseTab, End::Expire, End::Disconnect, End::Shutdown}) {
      TempDir tmp("nythera-shared-admin-lifecycle");
      const auto source = tmp.path / "clipboard" / "payload";
      const auto first = tmp.path / "first";
      const auto target = tmp.path / "restricted";
      const auto normal = tmp.path / "normal";
      const std::string contents(512 * 1024, 'a');
      writeFile(source, contents);
      writeFile(first, "first");
      std::filesystem::create_directory(target);
      std::filesystem::create_directory(normal);

      std::mutex gateMutex;
      std::condition_variable gate;
      std::atomic<bool> atCheckpoint{false};
      bool release = false;
      const auto releaseCopy = [&] {
        std::lock_guard lock(gateMutex);
        release = true;
        gate.notify_all();
      };
      std::atomic<bool> expired{false};
      const auto epoch = std::chrono::steady_clock::now();
      AdminHelperSessionConfig config{.invokingUid = static_cast<std::uint32_t>(::geteuid())};
      config.clockForTesting = [&] { return epoch + (expired ? 11min : 0min); };
      // Hold the next checkpoint after the first item publishes. Filesystems may
      // complete native copies in a single callback, so rely on item boundaries.
      config.copyCheckpointForTesting = [&](std::uint64_t bytes) {
        if (bytes != 5 || !std::filesystem::exists(target / first.filename()) || atCheckpoint.exchange(true))
          return;
        std::unique_lock lock(gateMutex);
        gate.wait_for(lock, 3s, [&] { return release; });
      };

      InProcessHelperFixture fixture;
      FileManager manager(target.string());
      const auto unblock = nythera::test::makeScopeExit(releaseCopy);
      manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher(config));
      pumpUntil([&] { return !manager.activeSession().loading(); });
      const auto adminId = manager.activeTab().id;
      const auto normalId = manager.openFolderInNewTab(normal, false);
      TEST_CHECK(manager.enterAdministratorMode(adminId));
      pumpUntil([&] {
        return manager.activeTab().hasActiveAdminSession() && manager.activeTab().capabilities().canMutate;
      });
      const auto id =
          manager.m_operations.copy({first.string(), source.string()}, target.string(), &manager.activeTab());
      TEST_CHECK(id != 0);
      const auto& operation = manager.m_operations.operations().back();
      pumpUntil([&] { return atCheckpoint.load(); });
      TEST_CHECK(manager.m_operations.usesMount(target));
      manager.m_operations.pause(id);
      bool acknowledged = false;
      TEST_CHECK(manager.activeTab().adminSession.pingAsync([&](const auto& health) { acknowledged = health.ok; }));
      pumpUntil([&] { return acknowledged; });
      releaseCopy();
      pumpUntil([&] { return operation.paused; });
      TEST_CHECK(operation.processedBytes > 0 && operation.processedBytes < contents.size());
      TEST_CHECK(!std::filesystem::exists(target / source.filename()));

      switch (end) {
      case End::Resume:
        TEST_CHECK(manager.activateTab(normalId));
        manager.m_operations.resume(id);
        break;
      case End::Cancel:
        manager.m_operations.cancel(id);
        break;
      case End::Finish:
        manager.finishAdministratorMode(adminId, "Administrator Mode ended.");
        break;
      case End::CloseTab:
        TEST_CHECK(manager.closeTab(adminId) && manager.activeTab().id == normalId);
        break;
      case End::Expire:
        expired = true;
        break;
      case End::Disconnect:
        fixture.killAllActiveHelpersUnexpectedly();
        break;
      case End::Shutdown:
        manager.m_operations.shutdown();
        pumpUntil([&] { return fixture.activeHelpers.load() == 0; });
        break;
      }
      if (end != End::Shutdown) {
        pumpUntil([&] { return operation.finished; });
        TEST_CHECK(operation.succeeded == (end == End::Resume));
        TEST_CHECK(operation.outcomeUncertain == (end == End::Disconnect));
        TEST_CHECK(operation.cancelled == (end != End::Resume && end != End::Disconnect));
        TEST_CHECK(!operation.paused && !manager.m_operations.usesMount(target));
        if (end == End::Resume || end == End::Cancel) {
          TEST_CHECK(operation.completedEntries.size() == (end == End::Resume ? 2 : 1));
          TEST_CHECK(operation.mutated);
        }
        if (end == End::Cancel)
          TEST_CHECK(operation.resultDescription.contains("1 of 2 selected items copied"));
        if (end == End::CloseTab)
          TEST_CHECK(manager.m_statusMessage != operation.status);
      }
      TEST_CHECK(readFile(source) == contents);
      TEST_CHECK(std::filesystem::is_regular_file(target / first.filename()));
      TEST_CHECK(std::filesystem::exists(target / source.filename()) == (end == End::Resume));
    }
  }

  static void testLocationClipboardPaste() {
    TempDir tmp("nythera-location-clipboard");
    InProcessHelperFixture fixture;
    FileManager manager(tmp.path.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    Flex root;
    auto location = std::make_unique<Input>();
    manager.m_locationInput = location.get();
    root.addChild(std::move(location));
    manager.m_dispatcher.setSceneRoot(&root);
    const auto detach = nythera::test::makeScopeExit([&] {
      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_locationInput = nullptr;
    });
    std::string copied;
    manager.m_locationInput->setOnCopyText([&](std::string text) { copied = std::move(text); });
    const auto copyKey = [&] {
      manager.handleWindowKeyEvent({.sym = XKB_KEY_c, .modifiers = KeyMod::Ctrl, .pressed = true});
    };
    std::function<void(std::optional<std::string>)> clipboardReply;
    manager.m_readLocationClipboardForTesting = [&](auto reply) { clipboardReply = std::move(reply); };

    const auto pasteKey = [&](std::uint32_t sym = XKB_KEY_v, std::uint32_t modifiers = KeyMod::Ctrl) {
      manager.handleWindowKeyEvent({.sym = sym, .modifiers = modifiers, .pressed = true});
      TEST_CHECK(clipboardReply);
    };
    const auto deliver = [&](std::string text) { std::exchange(clipboardReply, {})(std::move(text)); };

    manager.handleWindowKeyEvent({.sym = XKB_KEY_l, .modifiers = KeyMod::Ctrl, .pressed = true});
    copyKey();
    TEST_CHECK(copied == tmp.path.string());
    pasteKey();
    deliver("/tmp/ordinary folder");
    TEST_CHECK(manager.m_locationInput->value() == "/tmp/ordinary folder");
    TEST_CHECK(manager.m_operations.operations().empty());

    manager.m_locationInput->selectRange(5, 13);
    manager.activateContextMenuEntry({.id = 92});
    TEST_CHECK(clipboardReply);
    deliver("another");
    TEST_CHECK(manager.m_locationInput->value() == "/tmp/another folder");

    pasteKey();
    manager.setLocationEditMode(false);
    manager.setLocationEditMode(true);
    deliver("/tmp/stale");
    TEST_CHECK(manager.m_locationInput->value() == tmp.path.string());

    manager.setLocationEditMode(false);
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());
    TEST_CHECK(manager.enterAdministratorMode(manager.activeTab().id));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    manager.setLocationEditMode(true);
    copied.clear();
    copyKey();
    TEST_CHECK(copied == tmp.path.string());
    manager.m_locationInput->selectAll();
    manager.activateContextMenuEntry({.id = 90});
    TEST_CHECK(manager.m_locationInput->value().empty() && copied == tmp.path.string());
    pasteKey(XKB_KEY_Insert, KeyMod::Shift);
    deliver("/tmp/administrator folder");
    TEST_CHECK(manager.m_locationInput->value() == "/tmp/administrator folder");
    TEST_CHECK(manager.m_operations.operations().empty());
  }

  static void testInlineRenameRecoveryAfterHelperDisconnect() {
    TempDir tmp("nythera-stage3-session");
    const auto dir = tmp.path / "dir";
    writeFile(dir / "initial.txt", "hello");

    InProcessHelperFixture fixture;
    FileManager manager(dir.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());
    TEST_CHECK(manager.enterAdministratorMode(manager.activeTab().id));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });

    // Start inline rename before simulating a helper disconnect.
    Stage3Scene scene(manager, 960.0F, 640.0F);
    Input::setValidateKeyMatcher({});
    manager.activeTab().selectedPath = dir / "initial.txt";
    manager.activeTab().selectedPaths = {(dir / "initial.txt").string()};
    manager.beginRename();
    TEST_CHECK(manager.m_renamePath == dir / "initial.txt");
    TEST_CHECK(manager.m_inlineEditArea != nullptr);

    // Kill helper before submitting rename -> disconnect triggers recovery and cancels inline editor.
    fixture.killAllActiveHelpersUnexpectedly();
    pumpUntil([&] { return !manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    TEST_CHECK(!manager.m_renamePath.has_value());
    TEST_CHECK(!manager.m_inlineRenameTarget.has_value());
    TEST_CHECK(!manager.m_operations.active());

    manager.m_dispatcher.setSceneRoot(nullptr);
    manager.m_grid = nullptr;
    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
    TEST_CHECK(readFile(dir / "initial.txt") == "hello");
  }

  static void testNormalTabPrivilegeIsolation() {
    TempDir tmp("nythera-stage3-isolation");
    const auto adminDir = tmp.path / "admin_dir";
    const auto normalDir = tmp.path / "normal_dir";
    std::filesystem::create_directories(adminDir);
    std::filesystem::create_directories(normalDir);
    writeFile(adminDir / "item.txt", "admin");
    writeFile(normalDir / "normal.txt", "normal");

    InProcessHelperFixture fixture;
    FileManager manager(adminDir.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());

    const TabId adminTabId = manager.activeTab().id;
    const TabId normalTabId = manager.openFolderInNewTab(normalDir, false);
    pumpUntil([&] { return !manager.m_tabs.findTab(normalTabId)->session.loading(); });

    TEST_CHECK(manager.enterAdministratorMode(adminTabId));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });

    // Administrator creation uses the manager and leaves the undo stack empty.
    const std::size_t opsBefore = manager.m_operations.operations().size();
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 112});
    pumpUntil([&] {
      return std::filesystem::exists(adminDir / "New File")
          && !manager.m_operations.active()
          && !manager.activeSession().loading();
    });
    TEST_CHECK(manager.m_operations.operations().size() == opsBefore + 1);
    TEST_CHECK(manager.m_operations.operations().back().adminOrigin->tabId == adminTabId);
    TEST_CHECK(!manager.m_operations.canUndo());

    // Normal tab stays unprivileged and uses normal FileOperationManager with undo.
    TEST_CHECK(manager.activateTab(normalTabId));
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.activeTab().capabilities().canCreate);
    TEST_CHECK(manager.activeTab().capabilities().canMutate);
    TEST_CHECK(manager.activeTab().capabilities().canUndo);

    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 112});
    pumpUntil([&] {
      return !manager.m_operations.active()
          && std::filesystem::exists(normalDir / "New File")
          && !manager.activeSession().loading();
    });
    TEST_CHECK(manager.m_operations.operations().size() == opsBefore + 2);
    TEST_CHECK(manager.m_operations.canUndo());

    // Admin tab still has its own privileged session intact.
    TEST_CHECK(manager.m_tabs.findTab(adminTabId)->hasActiveAdminSession());

    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }

  static void testUiWorkflowsCreateInlineRenameCollisionsAndMenuEnablement() {
    TempDir tmp("nythera-stage3-ui");
    const auto writableDir = tmp.path / "writable";
    const auto readOnlyDir = tmp.path / "readonly";
    std::filesystem::create_directories(writableDir);
    std::filesystem::create_directories(readOnlyDir);
    writeFile(writableDir / "alpha.txt", "alpha");
    writeFile(writableDir / "existing.txt", "existing");
    writeFile(readOnlyDir / "locked.txt", "locked");
    TEST_CHECK(::chmod(readOnlyDir.c_str(), 0555) == 0);
    const auto restoreRoPerms = nythera::test::makeScopeExit([&] { ::chmod(readOnlyDir.c_str(), 0755); });

    InProcessHelperFixture fixture;
    FileManager manager(writableDir.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher({.inactivityTimeoutMs = 2000}));
    manager.activeTab().adminSession.setInactivityTimeoutMs(2000, true);

    const TabId tabId = manager.activeTab().id;
    TEST_CHECK(manager.enterAdministratorMode(tabId));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });

    Stage3Scene scene(manager, 1120.0F, 760.0F);
    Input::setValidateKeyMatcher({});

    // 1. Verify Stage 2 header, warning-coloured tab label, and [Administrator] title remain intact.
    TEST_CHECK(manager.m_adminHeaderContainer != nullptr && manager.m_adminHeaderContainer->visible());
    TEST_CHECK(manager.m_adminHeaderLabel != nullptr && manager.m_adminHeaderLabel->text() == "Administrator Mode");
    TEST_CHECK(manager.m_adminFinishButton != nullptr && manager.m_adminFinishButton->visible());
    TEST_CHECK(manager.m_windowTitle == "writable \xE2\x80\x94 Nythera Files [Administrator]");
    TEST_CHECK(manager.m_tabBar.m_slots.size() == 1);
    TEST_CHECK(manager.m_tabBar.m_slots[0].administrator);
    TEST_CHECK(manager.m_tabBar.m_slots[0].label->color() == colorForRole(kAdminWarningColorRole));

    // 2. Verify the shared mutations are enabled alongside the existing restricted actions.
    {
      manager.activeTab().selectedPath = writableDir / "alpha.txt";
      manager.activeTab().selectedPaths = {(writableDir / "alpha.txt").string()};
      const auto fileMenu = manager.fileMenuEntries();
      const auto* createMenu = findEntryById(fileMenu, 110);
      TEST_CHECK(createMenu != nullptr && createMenu->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, 111)->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, 112)->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, 113)->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, kManualSymlink)->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, kSelectionSymlink)->enabled);
      for (int tplId : {114, 115, 116}) {
        if (const auto* tpl = findEntryById(createMenu->submenuEntries, tplId))
          TEST_CHECK(tpl->enabled);
      }
      const auto editMenu = manager.editMenuEntries();
      TEST_CHECK(findEntryById(editMenu, 4)->enabled);
      TEST_CHECK(findEntryById(editMenu, 3)->enabled);
      for (int id : {1, 2, 6, static_cast<int>(kDuplicate), static_cast<int>(kManualSymlink)})
        TEST_CHECK(findEntryById(editMenu, id)->enabled);
      TEST_CHECK(findEntryById(editMenu, 59)->enabled);
      for (int blockedId : {5, static_cast<int>(kUndo), static_cast<int>(kRedo)}) {
        const auto* e = findEntryById(editMenu, blockedId);
        TEST_CHECK(e != nullptr && !e->enabled);
      }
    }

    // 3. Empty File… (112) -> creates "New File", selects it, opens inline rename after load finishes,
    //    and inline rename commits "created_doc.txt".
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 112});
    pumpUntil([&] { return !manager.m_operations.active(); });
    TEST_CHECK(std::filesystem::exists(writableDir / "New File"));
    // Pending inline rename stays armed while the directory listing refresh is still loading,
    // and opens once loading completes.
    if (manager.activeSession().loading())
      TEST_CHECK(manager.m_pendingInlineRenamePath == writableDir / "New File");
    pumpUntil([&] { return !manager.activeSession().loading(); });
    TEST_CHECK(manager.activeTab().selectedPath == writableDir / "New File");
    scene.relayout(manager, 1120.0F, 760.0F);
    TEST_CHECK(manager.m_renamePath == writableDir / "New File");
    TEST_CHECK(manager.m_inlineEditArea != nullptr);

    typeInlineTextAndSubmit(manager.m_dispatcher, "created_doc.txt");
    pumpUntil([&] {
      return std::filesystem::exists(writableDir / "created_doc.txt")
          && !manager.m_operations.active()
          && !manager.m_renamePath.has_value()
          && !manager.activeSession().loading();
    });
    TEST_CHECK(!std::filesystem::exists(writableDir / "New File"));
    TEST_CHECK(manager.activeTab().selectedPath == writableDir / "created_doc.txt");
    TEST_CHECK(
        manager.activeTab().selectedPaths == std::unordered_set<std::string>{(writableDir / "created_doc.txt").string()}
    );
    scene.relayout(manager, 1120.0F, 760.0F);

    // 4. Folder… (111) and Ctrl+Shift+N -> creates "New Folder" and "New Folder 2" with selection & inline rename.
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 111});
    pumpUntil([&] {
      return std::filesystem::is_directory(writableDir / "New Folder")
          && !manager.m_operations.active()
          && !manager.activeSession().loading();
    });
    TEST_CHECK(manager.activeTab().selectedPath == writableDir / "New Folder");
    scene.relayout(manager, 1120.0F, 760.0F);
    TEST_CHECK(manager.m_renamePath == writableDir / "New Folder");
    manager.cancelRename();
    TEST_CHECK(!manager.m_renamePath.has_value());

    manager.handleWindowKeyEvent(
        KeyboardEvent{.sym = XKB_KEY_N, .modifiers = KeyMod::Ctrl | KeyMod::Shift, .pressed = true}
    );
    pumpUntil([&] {
      return std::filesystem::is_directory(writableDir / "New Folder 2")
          && !manager.m_operations.active()
          && !manager.activeSession().loading();
    });
    TEST_CHECK(manager.activeTab().selectedPath == writableDir / "New Folder 2");
    scene.relayout(manager, 1120.0F, 760.0F);
    TEST_CHECK(manager.m_renamePath == writableDir / "New Folder 2");
    typeInlineTextAndSubmit(manager.m_dispatcher, "renamed_folder");
    pumpUntil([&] {
      return std::filesystem::is_directory(writableDir / "renamed_folder")
          && !manager.m_operations.active()
          && !manager.m_renamePath.has_value()
          && !manager.activeSession().loading();
    });
    scene.relayout(manager, 1120.0F, 760.0F);

    // 5. F2 inline rename collision & invalid name keep editor open and preserve entries, then valid rename succeeds.
    const auto alphaPath = writableDir / "alpha.txt";
    const auto alphaIt = std::ranges::find(manager.activeSession().entries(), alphaPath, &FileEntry::absPath);
    TEST_CHECK(alphaIt != manager.activeSession().entries().end());
    scene.grid->setSelectedIndex(static_cast<std::size_t>(alphaIt - manager.activeSession().entries().begin()), false);
    TEST_CHECK(manager.activeTab().selectedPath == alphaPath);
    manager.handleWindowKeyEvent(KeyboardEvent{.sym = XKB_KEY_F2, .pressed = true});
    TEST_CHECK(manager.m_renamePath == alphaPath && manager.m_inlineEditArea != nullptr);

    // Invalid filename ("bad/name") -> rejected locally, editor stays open.
    typeInlineTextAndSubmit(manager.m_dispatcher, "bad/name");
    TEST_CHECK(manager.m_renamePath == alphaPath);
    TEST_CHECK(manager.m_statusMessage == "File and folder names cannot contain /.");

    // Existing destination collision ("existing.txt") -> rejected by helper, editor stays open, files intact.
    typeInlineTextAndSubmit(manager.m_dispatcher, "existing.txt");
    pumpUntil([&] {
      return !manager.m_inlineRenameTarget.has_value()
          && manager.m_statusMessage.contains("An item with that name already exists");
    });
    TEST_CHECK(manager.m_renamePath == alphaPath);
    TEST_CHECK(readFile(alphaPath) == "alpha");
    TEST_CHECK(readFile(writableDir / "existing.txt") == "existing");

    // Retry with valid non-colliding name ("beta.txt") -> succeeds, updates selection, closes editor.
    typeInlineTextAndSubmit(manager.m_dispatcher, "beta.txt");
    pumpUntil([&] {
      return std::filesystem::exists(writableDir / "beta.txt")
          && !manager.m_renamePath.has_value()
          && !manager.activeSession().loading();
    });
    TEST_CHECK(!std::filesystem::exists(alphaPath));
    TEST_CHECK(readFile(writableDir / "beta.txt") == "alpha");
    TEST_CHECK(manager.activeTab().selectedPath == writableDir / "beta.txt");

    // 6. Text creation, duplicate, selection symlink and multi-item delete use the shared transport.
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 113});
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().loading(); });
    TEST_CHECK(std::filesystem::is_regular_file(writableDir / "New File.txt"));
    manager.m_pendingInlineRenamePath.reset();
    manager.activeTab().selectedPaths = {(writableDir / "beta.txt").string()};
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kDuplicate});
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().loading(); });
    const auto duplicate = writableDir / "beta (copy).txt";
    TEST_CHECK(readFile(duplicate) == "alpha");

    manager.activeTab().selectedPaths = {(writableDir / "beta.txt").string()};
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kSelectionSymlink});
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().loading(); });
    const auto link = writableDir / "beta-link.txt";
    TEST_CHECK(std::filesystem::read_symlink(link) == writableDir / "beta.txt");

    const auto id = manager.m_operations.removePermanently({duplicate.string(), link.string()}, &manager.activeTab());
    TEST_CHECK(id != 0);
    pumpUntil([&] { return !manager.m_operations.active() && !manager.activeSession().loading(); });
    TEST_CHECK(!std::filesystem::exists(duplicate) && !std::filesystem::is_symlink(link));
    const auto& deletion = manager.m_operations.operations().back();
    TEST_CHECK(deletion.succeeded && deletion.adminOrigin && deletion.processedFiles == 2);

    const std::size_t opsBeforeBlocked = manager.m_operations.operations().size();
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 5});     // Wastebin
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 30006}); // Properties
    TEST_CHECK(manager.m_operations.operations().size() == opsBeforeBlocked);

    // 7. Navigate to a read-only directory -> Create New and Rename are disabled & blocked.
    manager.navigate(readOnlyDir);
    pumpUntil([&] { return manager.activeSession().path() == readOnlyDir && !manager.activeSession().loading(); });
    TEST_CHECK(!manager.activeTab().capabilities().canCreateBasic);
    TEST_CHECK(!manager.activeTab().capabilities().canRename);
    manager.activeTab().selectedPath = readOnlyDir / "locked.txt";
    manager.activeTab().selectedPaths = {(readOnlyDir / "locked.txt").string()};
    const auto roFileMenu = manager.fileMenuEntries();
    TEST_CHECK(!findEntryById(roFileMenu, 110)->enabled);
    const auto roEditMenu = manager.editMenuEntries();
    TEST_CHECK(!findEntryById(roEditMenu, 4)->enabled);
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 112});
    TEST_CHECK(!std::filesystem::exists(readOnlyDir / "New File"));
    manager.beginRename();
    TEST_CHECK(!manager.m_renamePath.has_value());

    // 8. Navigate back to writableDir, start inline rename, click Finish -> cancels inline rename and
    //    recovers unprivileged browsing cleanly.
    manager.navigate(writableDir);
    pumpUntil([&] { return manager.activeSession().path() == writableDir && !manager.activeSession().loading(); });
    scene.relayout(manager, 1120.0F, 760.0F);
    manager.activeTab().selectedPath = writableDir / "beta.txt";
    manager.activeTab().selectedPaths = {(writableDir / "beta.txt").string()};
    manager.beginRename();
    TEST_CHECK(manager.m_renamePath == writableDir / "beta.txt");
    dispatchClick(manager.m_dispatcher, manager.m_adminFinishButton);
    pumpUntil([&] { return !manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    TEST_CHECK(!manager.m_renamePath.has_value());
    TEST_CHECK(!manager.m_adminHeaderContainer->visible());
    TEST_CHECK(manager.m_windowTitle == "writable \xE2\x80\x94 Nythera Files");

    manager.m_dispatcher.setSceneRoot(nullptr);
    manager.m_grid = nullptr;
    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-stage3-env"});
    TEST_CHECK(geteuid() != 0);
    FileManagerTestAccess::runAll();
  });
}
