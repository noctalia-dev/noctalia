#include "core/files/directory_scanner.h"
#include "core/files/display_text.h"
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
#include <linux/fs.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <ranges>
#include <source_location>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
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

  struct RawHelperClient {
    AdminTransportLaunchResult transport;
    AdminLineBuffer buffer;

    explicit RawHelperClient(AdminTransportLauncher launcher) : transport(launcher(nullptr)) {
      TEST_CHECK(transport.channel != nullptr);
    }

    void writeRaw(std::string_view frame) {
      int err = 0;
      TEST_CHECK(writeAdminFrameFd(transport.channel->writeFd, frame, err));
    }

    void send(const AdminClientRequest& req) {
      const auto enc = encodeAdminClientRequest(req);
      TEST_CHECK(enc.has_value());
      writeRaw(*enc);
    }

    AdminServerFrame readFrame() {
      int err = 0;
      std::string line;
      while (true) {
        const auto st = readAdminFrameFd(transport.channel->readFd, buffer, line, err);
        if (st == AdminFrameReadStatus::NeedMoreData) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        TEST_CHECK(st == AdminFrameReadStatus::FrameReady);
        AdminServerFrame frame;
        TEST_CHECK(decodeAdminServerFrame(line, frame));
        return frame;
      }
    }

    void hello(std::string nonce = "stage3") {
      send({.command = AdminClientCommand::Hello, .requestId = 1, .nonce = std::move(nonce)});
      const auto ready = readFrame();
      TEST_CHECK(ready.kind == AdminServerFrameKind::Ready);
    }

    AdminServerFrame listDir(AdminRequestId reqId, const std::filesystem::path& dir, bool userInitiated = true) {
      send({
          .command = AdminClientCommand::ListDir,
          .requestId = reqId,
          .path = dir.string(),
          .maxBatchEntries = kAdminDefaultDirBatchEntries,
          .userInitiated = userInitiated,
      });
      while (true) {
        const auto frame = readFrame();
        if (frame.kind == AdminServerFrameKind::DirBatch)
          continue;
        return frame;
      }
    }

    void closeAndWait() {
      if (transport.channel && transport.channel->waitForExit)
        (void)transport.channel->waitForExit();
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
    testProtocolStage3ValidationAndUnsupportedCommands();
    testPrivilegedCreateFileAndPermissionsAndCollisions();
    testPrivilegedCreateDirAndPermissionsAndCollisions();
    testPrivilegedSameDirectoryRenameAndCollisions();
    testSymlinkTraversalAndConcurrentRaceProtections();
    testUnauthorizedDirectoryAndNavigationInvalidation();
    testSessionExpiryActivityRenewalAndMidFlightDisconnect();
    testNormalTabPrivilegeIsolation();
    testUiWorkflowsCreateInlineRenameCollisionsAndMenuEnablement();
    testCopyFileProtocolAndFilesystem();
    testExtendedAdministratorCopy();
    testExtendedAdministratorCopyFailures();
    testAdministratorPasteAndNormalTabIsolation();
    testAdministratorCopyLifecycle();
    testLocationClipboardPaste();
  }

private:
  static void testCopyFileProtocolAndFilesystem() {
    TempDir tmp("nythera-stage41-copy");
    const auto source = tmp.path / "from" / "payload";
    const auto target = tmp.path / "restricted";
    const auto resultPath = target / source.filename();
    const std::string contents(512 * 1024, 'c');
    writeFile(source, contents);
    TEST_CHECK(::mkdir(target.c_str(), 0700) == 0);
    TEST_CHECK(::chmod(source.c_str(), 04777) == 0);
    const timespec requestedTimes[]{{1500000000, 123}, {1500000000, 456}};
    TEST_CHECK(::utimensat(AT_FDCWD, source.c_str(), requestedTimes, 0) == 0);

    const auto identity = [](const std::filesystem::path& path) {
      struct stat st{};
      TEST_CHECK(::lstat(path.c_str(), &st) == 0);
      return st;
    };
    AdminClientRequest request{
        .command = AdminClientCommand::CopyFile,
        .requestId = 3,
        .path = target.string(),
        .sourcePath = source.string(),
        .directoryListingId = 2,
        .expectedDevice = static_cast<std::uint64_t>(identity(source).st_dev),
        .expectedInode = static_cast<std::uint64_t>(identity(source).st_ino),
    };
    AdminClientRequest decoded;
    const auto encoded = encodeAdminClientRequest(request);
    TEST_CHECK(encoded && decodeAdminClientRequest(*encoded, decoded));
    TEST_CHECK(
        decoded.command == request.command
        && decoded.directoryListingId == request.directoryListingId
        && decoded.sourcePath == request.sourcePath
        && decoded.expectedInode == request.expectedInode
    );

    InProcessHelperFixture fixture;
    AdminHelperSessionConfig config{.invokingUid = static_cast<std::uint32_t>(::geteuid())};
    {
      RawHelperClient unauthenticated(fixture.makeLauncher(config));
      unauthenticated.send(request);
      TEST_CHECK(unauthenticated.readFrame().status == AdminStatusCode::HandshakeRequired);
      unauthenticated.closeAndWait();
    }
    RawHelperClient raw(fixture.makeLauncher(config));
    raw.hello();
    raw.writeRaw("COPY_FILE 2 L3RtcA==\n");
    TEST_CHECK(raw.readFrame().status == AdminStatusCode::BadRequest);
    raw.send(request);
    TEST_CHECK(raw.readFrame().status == AdminStatusCode::Unauthorized);
    TEST_CHECK(raw.listDir(4, target).directoryWritable);
    request.requestId = 5;
    raw.send(request); // Stale listing identifier.
    TEST_CHECK(raw.readFrame().status == AdminStatusCode::Unauthorized);
    request.directoryListingId = 4;

    const auto sendCopy = [&](const std::filesystem::path& file, struct stat expected) {
      ++request.requestId;
      request.sourcePath = file.string();
      request.expectedDevice = static_cast<std::uint64_t>(expected.st_dev);
      request.expectedInode = static_cast<std::uint64_t>(expected.st_ino);
      raw.send(request);
    };
    sendCopy(source, identity(source));
    std::uint64_t lastBytes = 0;
    bool sawProgress = false;
    auto frame = raw.readFrame();
    while (frame.kind == AdminServerFrameKind::Progress) {
      sawProgress = true;
      TEST_CHECK(frame.requestId == request.requestId && frame.totalBytes == contents.size());
      TEST_CHECK(frame.completedBytes >= lastBytes && frame.completedBytes <= frame.totalBytes);
      lastBytes = frame.completedBytes;
      frame = raw.readFrame();
    }
    const auto copied = identity(resultPath);
    TEST_CHECK(sawProgress && lastBytes == contents.size());
    TEST_CHECK(frame.kind == AdminServerFrameKind::MutationResult && frame.requestId == request.requestId);
    TEST_CHECK(frame.mutationInfo.name == source.filename() && frame.mutationInfo.inode == copied.st_ino);
    TEST_CHECK((copied.st_mode & 07777) == 0755);
    TEST_CHECK(
        copied.st_mtim.tv_sec == requestedTimes[1].tv_sec && copied.st_mtim.tv_nsec == requestedTimes[1].tv_nsec
    );
    TEST_CHECK(readFile(resultPath) == contents && readFile(source) == contents);
    TEST_CHECK((identity(source).st_mode & 07777) == 04777);

    sendCopy(source, identity(source));
    TEST_CHECK(
        raw.readFrame().status == AdminStatusCode::AlreadyExists && identity(resultPath).st_ino == copied.st_ino
    );

    const auto empty = source.parent_path() / "empty";
    writeFile(empty, "");
    const auto occupied = target / empty.filename();
    std::filesystem::create_symlink(resultPath, occupied);
    sendCopy(empty, identity(empty));
    TEST_CHECK(raw.readFrame().status == AdminStatusCode::AlreadyExists);
    TEST_CHECK(std::filesystem::is_symlink(occupied) && readFile(resultPath) == contents);
    TEST_CHECK(std::filesystem::remove(occupied));
    sendCopy(empty, identity(empty));
    TEST_CHECK(raw.readFrame().kind == AdminServerFrameKind::Progress);
    TEST_CHECK(raw.readFrame().kind == AdminServerFrameKind::Progress);
    TEST_CHECK(
        raw.readFrame().kind == AdminServerFrameKind::MutationResult && std::filesystem::file_size(occupied) == 0
    );

    const auto pipe = source.parent_path() / "pipe";
    TEST_CHECK(::mkfifo(pipe.c_str(), 0600) == 0);
    const auto link = source.parent_path() / "link";
    std::filesystem::create_symlink(source, link);
    const auto folderLink = tmp.path / "folder-link";
    std::filesystem::create_directory_symlink(source.parent_path(), folderLink);
    // Separate name so the destination collision does not mask the parent-symlink check.
    const auto throughLink = source.parent_path() / "through";
    writeFile(throughLink, "link traversal");
    for (const auto& bad : {source.parent_path(), pipe, link, folderLink / throughLink.filename()}) {
      sendCopy(bad, identity(bad));
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::BadRequest);
      TEST_CHECK(!std::filesystem::exists(target / bad.filename()));
    }

    const auto unreadable = source.parent_path() / "unreadable";
    writeFile(unreadable, "private");
    TEST_CHECK(::chmod(unreadable.c_str(), 0000) == 0);
    sendCopy(unreadable, identity(unreadable));
    TEST_CHECK(raw.readFrame().status == AdminStatusCode::AccessDenied);
    const auto oldSource = identity(throughLink);
    std::filesystem::rename(throughLink, source.parent_path() / "old");
    writeFile(throughLink, "replaced");
    sendCopy(throughLink, oldSource);
    TEST_CHECK(raw.readFrame().status == AdminStatusCode::IdentityMismatch);

    const auto moved = tmp.path / "moved";
    std::filesystem::rename(target, moved);
    TEST_CHECK(::mkdir(target.c_str(), 0700) == 0);
    sendCopy(throughLink, identity(throughLink));
    TEST_CHECK(raw.readFrame().status == AdminStatusCode::IdentityMismatch);
    TEST_CHECK(std::filesystem::is_empty(target) && !std::filesystem::exists(moved / throughLink.filename()));
    raw.closeAndWait();

    enum class Race { DestinationDirectory, DestinationName, SourceParent };
    Race change = Race::DestinationDirectory;
    bool swapped = false;
    std::error_code hookError;
    const auto copyTimeMoved = tmp.path / "copy-time-moved";
    const auto originalSourceParent = tmp.path / "original-source-parent";
    config.copyCheckpointForTesting = [&](std::uint64_t bytes) {
      if (bytes == 0 || swapped)
        return;
      swapped = true;
      switch (change) {
      case Race::DestinationDirectory:
        std::filesystem::rename(target, copyTimeMoved, hookError);
        if (!hookError)
          std::filesystem::create_directory(target, hookError);
        break;
      case Race::DestinationName: {
        std::ofstream winner(resultPath);
        winner << "race winner";
        winner.close();
        if (!winner)
          hookError = std::make_error_code(std::errc::io_error);
        break;
      }
      case Race::SourceParent:
        std::filesystem::rename(source.parent_path(), originalSourceParent, hookError);
        if (!hookError)
          std::filesystem::create_directory(source.parent_path(), hookError);
        break;
      }
    };
    for (const auto raceKind : {Race::DestinationDirectory, Race::DestinationName, Race::SourceParent}) {
      change = raceKind;
      swapped = false;
      RawHelperClient race(fixture.makeLauncher(config));
      race.hello();
      TEST_CHECK(race.listDir(2, target).directoryWritable);
      request.requestId = 3;
      request.directoryListingId = 2;
      request.sourcePath = source.string();
      request.expectedDevice = static_cast<std::uint64_t>(identity(source).st_dev);
      request.expectedInode = static_cast<std::uint64_t>(identity(source).st_ino);
      race.send(request);
      do {
        frame = race.readFrame();
      } while (frame.kind == AdminServerFrameKind::Progress);
      race.closeAndWait();
      TEST_CHECK(swapped && !hookError);
      if (raceKind == Race::DestinationName) {
        TEST_CHECK(frame.status == AdminStatusCode::AlreadyExists && readFile(resultPath) == "race winner");
        TEST_CHECK(std::filesystem::remove(resultPath));
      } else {
        TEST_CHECK(frame.status == AdminStatusCode::IdentityMismatch);
      }
      TEST_CHECK(std::filesystem::is_empty(target));
    }
    TEST_CHECK(std::filesystem::is_empty(copyTimeMoved));
    TEST_CHECK(readFile(originalSourceParent / source.filename()) == contents);
  }

  static AdminCopySource copySource(const std::filesystem::path& path) {
    struct stat st{};
    TEST_CHECK(::lstat(path.c_str(), &st) == 0);
    return {path.string(), static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino), S_ISDIR(st.st_mode)};
  }

  static void testExtendedAdministratorCopy() {
    TempDir tmp("nythera-stage42");
    const auto from = tmp.path / "from";
    const auto target = tmp.path / "target";
    const auto tree = from / "tree";
    writeFile(from / "first", "abc");
    writeFile(from / "second", "defg");
    writeFile(tree / "one" / "two" / "data", "deeper");
    writeFile(tree / "empty-file", "");
    std::filesystem::create_directory(tree / "empty-dir");
    TEST_CHECK(::mkdir(target.c_str(), 0700) == 0);
    writeFile(target / "unrelated", "keep");

    AdminClientRequest request{
        .command = AdminClientCommand::CopyItems,
        .requestId = 3,
        .path = target.string(),
        .directoryListingId = 2,
        .copySources = {copySource(from / "first"), copySource(from / "second"), copySource(tree)},
    };
    const auto encoded = encodeAdminClientRequest(request);
    AdminClientRequest decoded;
    TEST_CHECK(encoded && decodeAdminClientRequest(*encoded, decoded));
    TEST_CHECK(decoded.copySources.size() == 3 && decoded.copySources.back().directory);
    TEST_CHECK(decoded.copySources.back().path == tree.string());
    TEST_CHECK(!decodeAdminClientRequest("COPY_ITEMS 3 2 L3RtcA== 1 2 2 L2FiYw==\n", decoded));
    TEST_CHECK(!decodeAdminClientRequest("COPY_ITEMS 3 2 L3RtcA== 1 2 0\n", decoded));
    InProcessHelperFixture fixture;
    RawHelperClient raw(fixture.makeLauncher({.invokingUid = static_cast<std::uint32_t>(::geteuid())}));
    raw.hello();
    TEST_CHECK(raw.listDir(2, target).directoryWritable);
    raw.send(request);
    const auto readSuccess = [&](std::uint64_t items, std::uint64_t bytes, std::vector<std::string> names) {
      std::uint64_t lastItems = 0;
      std::uint64_t lastBytes = 0;
      std::vector<std::string> published;
      while (true) {
        const auto frame = raw.readFrame();
        TEST_CHECK(frame.requestId == request.requestId);
        if (frame.kind == AdminServerFrameKind::Response) {
          TEST_CHECK(frame.status == AdminStatusCode::Ok);
          break;
        }
        if (frame.kind == AdminServerFrameKind::Progress) {
          TEST_CHECK(frame.totalItems == items && frame.totalBytes == bytes);
          TEST_CHECK(frame.completedItems >= lastItems && frame.completedItems <= items);
          TEST_CHECK(frame.completedBytes >= lastBytes && frame.completedBytes <= bytes);
          lastItems = frame.completedItems;
          lastBytes = frame.completedBytes;
        } else {
          TEST_CHECK(frame.kind == AdminServerFrameKind::CopyItem);
          TEST_CHECK(frame.mutationInfo.inode == copySource(target / frame.mutationInfo.name).inode);
          published.push_back(frame.mutationInfo.name);
        }
      }
      TEST_CHECK(lastItems == items && lastBytes == bytes && published == names);
    };
    readSuccess(8, 13, {"first", "second", "tree"});
    TEST_CHECK(readFile(target / "first") == "abc" && readFile(target / "second") == "defg");
    TEST_CHECK(readFile(target / "tree/one/two/data") == "deeper" && readFile(tree / "one/two/data") == "deeper");
    TEST_CHECK(std::filesystem::is_empty(target / "tree/empty-dir"));
    TEST_CHECK(std::filesystem::file_size(target / "tree/empty-file") == 0);
    TEST_CHECK(readFile(target / "unrelated") == "keep");
    TEST_CHECK(std::ranges::distance(std::filesystem::directory_iterator(target)) == 4);

    writeFile(from / "third", "");
    std::filesystem::create_directory(from / "fourth");
    request.requestId++;
    request.copySources = {copySource(from / "third"), copySource(from / "fourth")};
    raw.send(request);
    readSuccess(2, 0, {"third", "fourth"});
    raw.closeAndWait();
  }

  static void testExtendedAdministratorCopyFailures() {
    TempDir tmp("nythera-stage42-failure");
    const auto tree = tmp.path / "from" / "tree";
    const auto data = tree / "child" / "payload";
    const auto first = tmp.path / "first";
    const auto target = tmp.path / "target";
    const auto oldTarget = tmp.path / "old-target";
    constexpr std::size_t length = 512 * 1024;
    writeFile(data, std::string(length, 'b'));
    writeFile(first, "first");
    TEST_CHECK(::mkdir(target.c_str(), 0700) == 0);
    const auto send = [&](RawHelperClient& raw, std::vector<AdminCopySource> sources, auto destination, AdminRequestId id) {
      raw.send({
          .command = AdminClientCommand::CopyItems, .requestId = id, .path = destination.string(),
          .directoryListingId = 2, .copySources = std::move(sources),
      });
    };
    InProcessHelperFixture fixture;
    {
      RawHelperClient raw(fixture.makeLauncher({.invokingUid = static_cast<std::uint32_t>(::geteuid())}));
      raw.hello();
      raw.listDir(2, target);
      std::filesystem::create_symlink(data, tree / "link");
      send(raw, {copySource(first), copySource(tree)}, target, 3);
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::BadRequest && std::filesystem::is_empty(target));
      TEST_CHECK(std::filesystem::remove(tree / "link"));
      TEST_CHECK(::mkfifo((tree / "fifo").c_str(), 0600) == 0);
      send(raw, {copySource(tree)}, target, 4);
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::BadRequest && std::filesystem::is_empty(target));
      TEST_CHECK(std::filesystem::remove(tree / "fifo"));
      writeFile(target / "tree" / "old", "existing");
      send(raw, {copySource(first), copySource(tree)}, target, 5);
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::AlreadyExists);
      TEST_CHECK(!std::filesystem::exists(target / "first") && readFile(target / "tree/old") == "existing");
      TEST_CHECK(::chmod(target.c_str(), 0777) == 0);
      send(raw, {copySource(tree / "child")}, target, 6);
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::NotSupported);
      TEST_CHECK(::chmod(target.c_str(), 0700) == 0);
      raw.closeAndWait();
    }
    for (const auto& descendant : {tree, data.parent_path()}) {
      RawHelperClient raw(fixture.makeLauncher({.invokingUid = static_cast<std::uint32_t>(::geteuid())}));
      raw.hello();
      raw.listDir(2, descendant);
      send(raw, {copySource(tree)}, descendant, 3);
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::BadRequest);
      TEST_CHECK(!std::filesystem::exists(descendant / tree.filename()));
      raw.closeAndWait();
    }
    TEST_CHECK(std::filesystem::remove(target / "tree/old") && std::filesystem::remove(target / "tree"));

    enum class Race { EditFile, ReplaceFile, ReplaceDestination, Collide };
    for (const auto race : {Race::EditFile, Race::ReplaceFile, Race::ReplaceDestination, Race::Collide}) {
      bool injected = false;
      AdminHelperSessionConfig config{.invokingUid = static_cast<std::uint32_t>(::geteuid())};
      config.copyCheckpointForTesting = [&](std::uint64_t bytes) {
        if (bytes <= 5 || injected)
          return;
        injected = true;
        switch (race) {
        case Race::EditFile:
          writeFile(data, std::string(length, 'e'));
          break;
        case Race::ReplaceFile:
          std::filesystem::rename(data, data.parent_path() / "old");
          writeFile(data, std::string(length, 'r'));
          break;
        case Race::ReplaceDestination:
          std::filesystem::rename(target, oldTarget);
          std::filesystem::create_directory(target);
          break;
        case Race::Collide:
          writeFile(target / "tree" / "winner", "existing");
          break;
        }
      };
      RawHelperClient raw(fixture.makeLauncher(config));
      raw.hello();
      raw.listDir(2, target);
      send(raw, {copySource(first), copySource(tree)}, target, 3);
      std::vector<std::string> published;
      while (true) {
        const auto frame = raw.readFrame();
        if (frame.kind == AdminServerFrameKind::CopyItem)
          published.push_back(frame.mutationInfo.name);
        if (frame.kind == AdminServerFrameKind::Response) {
          TEST_CHECK(frame.status == (race == Race::Collide ? AdminStatusCode::AlreadyExists : AdminStatusCode::IdentityMismatch));
          break;
        }
      }
      raw.closeAndWait();
      const auto& retained = race == Race::ReplaceDestination ? oldTarget : target;
      TEST_CHECK(injected && published.size() == 1 && published.front() == "first");
      TEST_CHECK(readFile(retained / "first") == "first" && readFile(first) == "first");
      TEST_CHECK(std::filesystem::remove(retained / "first"));
      if (race == Race::Collide) {
        TEST_CHECK(readFile(target / "tree/winner") == "existing");
        TEST_CHECK(std::filesystem::remove(target / "tree/winner") && std::filesystem::remove(target / "tree"));
      }
      TEST_CHECK(std::filesystem::is_empty(retained) && std::filesystem::is_empty(target));
      TEST_CHECK(readFile(data).size() == length);
    }
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
    pumpUntil([&] { return manager.activeTab().capabilities().canPasteAsAdministrator; });

    NativeDataTransfer::ReadCallback pending;
    manager.m_transfers.m_readPasteSelectionForTesting = [&](auto cb) { pending = std::move(cb); };
    const auto paste = [&](std::vector<std::string> paths, bool cut) {
      manager.pasteClipboard();
      TEST_CHECK(pending);
      std::exchange(pending, {})(encodeFileTransfer(std::move(paths), cut));
    };
    const auto& operations = manager.m_operations.operations();
    paste({source.string()}, true);
    TEST_CHECK(operations.empty() && manager.m_statusMessage.find("requires copied files or directories") != std::string::npos);
    manager.pasteClipboard();
    auto conflictingClipboard = encodeFileTransfer({source.string()}, true);
    conflictingClipboard["application/x-kde-cutselection"] = "0";
    std::exchange(pending, {})(std::move(conflictingClipboard));
    TEST_CHECK(operations.empty());

    // A delayed clipboard read cannot start a privileged job once the originating tab is inactive.
    manager.pasteClipboard();
    TEST_CHECK(pending && manager.activateTab(normalId));
    std::exchange(pending, {})(encodeFileTransfer({source.string()}, false));
    TEST_CHECK(operations.empty() && !std::filesystem::exists(target / source.filename()));
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.m_operations.copyAsAdministrator(manager.activeTab(), {source.string()}) == 0);

    TEST_CHECK(manager.activateTab(adminId));
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canPasteAsAdministrator; });
    paste({source.string()}, false);
    pumpUntil([&] {
      return operations.size() == 1 && operations.back().finished && !manager.activeSession().loading();
    });
    TEST_CHECK(operations.back().succeeded && operations.back().adminCopy && operations.back().undoUnavailable);
    TEST_CHECK(operations.back().processedBytes == 14 && operations.back().processedFiles == 1);
    TEST_CHECK(!manager.m_operations.canUndo() && readFile(target / source.filename()) == "paste contents");
    TEST_CHECK(manager.activeTab().selectedPath == target / source.filename());

    paste({source.string()}, false);
    pumpUntil([&] { return operations.size() == 2 && operations.back().finished; });
    TEST_CHECK(!operations.back().succeeded && operations.back().status.find("already exists") != std::string::npos);
    TEST_CHECK(readFile(target / source.filename()) == "paste contents" && readFile(source) == "paste contents");

    TEST_CHECK(manager.activateTab(normalId));
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canMutate; });
    paste({source.string()}, false);
    pumpUntil([&] { return operations.size() == 3 && operations.back().finished; });
    TEST_CHECK(operations.back().succeeded && !operations.back().adminCopy && manager.m_operations.canUndo());
    TEST_CHECK(readFile(normal / source.filename()) == "paste contents");
    TEST_CHECK(manager.m_tabs.findTab(adminId)->hasActiveAdminSession());

    const auto tree = normal / "selected-tree";
    const auto empty = normal / "selected-empty";
    writeFile(tree / "sub" / "child", "mixed paste");
    writeFile(empty, "");
    TEST_CHECK(manager.activateTab(adminId));
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeTab().capabilities().canPasteAsAdministrator; });
    paste({empty.string(), tree.string()}, false);
    pumpUntil([&] { return operations.size() == 4 && operations.back().finished && !manager.activeSession().loading(); });
    TEST_CHECK(operations.back().succeeded && operations.back().requestedItemCount == 2);
    TEST_CHECK(operations.back().processedFiles == 4 && operations.back().completedEntries.size() == 2);
    TEST_CHECK(manager.activeTab().selectedPaths.contains((target / tree.filename()).string()));
    TEST_CHECK(manager.activeTab().selectedPaths.contains((target / empty.filename()).string()));
    TEST_CHECK(readFile(target / tree.filename() / "sub" / "child") == "mixed paste");
    TEST_CHECK(std::filesystem::file_size(target / empty.filename()) == 0);
  }

  static void testAdministratorCopyLifecycle() {
    using namespace std::chrono_literals;
    enum class End { Cancel, Finish, CloseTab, Expire, Disconnect, Shutdown };
    const auto checkLifecycle = [](bool recursive, End end) {
      TempDir tmp("nythera-stage41-lifecycle");
      const auto root = tmp.path / "clipboard" / "tree";
      const auto source = recursive ? root / "nested" / "payload" : tmp.path / "clipboard" / "payload";
      const bool partial = recursive && end == End::Cancel;
      const auto first = tmp.path / "first";
      const auto target = tmp.path / "restricted";
      const auto normal = tmp.path / "normal";
      const std::string contents(512 * 1024, 'a');
      writeFile(source, contents);
      if (partial)
        writeFile(first, "");
      TEST_CHECK(::mkdir(target.c_str(), 0700) == 0);
      TEST_CHECK(::mkdir(normal.c_str(), 0700) == 0);

      std::mutex gateMutex;
      std::condition_variable gate;
      std::atomic<bool> atCheckpoint{false};
      bool release = false;
      auto releaseCopy = [&] {
        {
          std::lock_guard lock(gateMutex);
          release = true;
        }
        gate.notify_all();
      };
      InProcessHelperFixture fixture;
      AdminHelperSessionConfig config{.invokingUid = static_cast<std::uint32_t>(::geteuid())};
      if (end == End::Expire)
        config.inactivityTimeoutMs = 400;
      config.copyCheckpointForTesting = [&](std::uint64_t bytes) {
        if (bytes == 0) {
          std::this_thread::sleep_for(110ms); // Make one intermediate byte count observable.
        } else if (!atCheckpoint.exchange(true)) {
          std::unique_lock lock(gateMutex);
          gate.wait_for(lock, 3s, [&] { return release; });
        }
      };

      FileManager manager(target.string());
      const auto unblock = nythera::test::makeScopeExit(releaseCopy);
      manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher(config));
      pumpUntil([&] { return !manager.activeSession().loading(); });
      const auto adminId = manager.activeTab().id;
      const auto normalId = manager.openFolderInNewTab(normal, false);
      TEST_CHECK(manager.enterAdministratorMode(adminId));
      pumpUntil([&] { return manager.activeTab().capabilities().canPasteAsAdministrator; });
      std::vector<std::string> selection;
      if (partial)
        selection.push_back(first.string());
      selection.push_back((recursive ? root : source).string());
      const auto id = manager.m_operations.copyAsAdministrator(manager.activeTab(), std::move(selection));
      TEST_CHECK(id != 0);
      const auto& operation = manager.m_operations.operations().back();
      pumpUntil([&] { return atCheckpoint && operation.processedBytes > 0; });
      TEST_CHECK(!operation.finished && operation.processedBytes < contents.size());
      TEST_CHECK(manager.m_operations.usesMount(target) && manager.m_operations.usesMount(source.parent_path()));
      if (recursive) {
        TEST_CHECK(!std::filesystem::exists(target / root.filename()));
        const auto entries = std::filesystem::directory_iterator(target);
        const auto stage = std::ranges::find_if(entries, [](const auto& entry) {
          return entry.path().filename().string().starts_with(".nythera-copy-");
        });
        TEST_CHECK(stage != std::filesystem::directory_iterator{});
        TEST_CHECK((stage->status().permissions() & std::filesystem::perms::all) == std::filesystem::perms::owner_all);
      } else {
        TEST_CHECK(std::filesystem::is_empty(target));
      }

      const auto channel = fixture.activeStates.front();
      const auto waitForWriteClosure = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
          {
            std::lock_guard lock(channel->fdMutex);
            if (channel->clientWriteFd < 0)
              return;
          }
          std::this_thread::sleep_for(1ms);
        }
      };
      switch (end) {
      case End::Cancel: {
        manager.m_operations.cancel(id);
        TEST_CHECK(!operation.finished && manager.m_operations.usesMount(target));
        // Wait for the worker to deliver the request while the helper is paused.
        pollfd request{channel->helperReadFd, POLLIN, 0};
        TEST_CHECK(::poll(&request, 1, 2000) == 1 && (request.revents & POLLIN) != 0);
        releaseCopy();
        break;
      }
      case End::Finish: {
        manager.finishAdministratorMode(adminId, "Administrator Mode ended.");
        waitForWriteClosure();
        releaseCopy();
        break;
      }
      case End::CloseTab: {
        std::jthread unblockCloser([&] {
          waitForWriteClosure();
          releaseCopy();
        });
        TEST_CHECK(manager.closeTab(adminId) && manager.activeTab().id == normalId);
        break;
      }
      case End::Expire:
        std::this_thread::sleep_for(410ms);
        releaseCopy();
        break;
      case End::Disconnect:
        fixture.killAllActiveHelpersUnexpectedly();
        releaseCopy();
        break;
      case End::Shutdown: {
        std::jthread unblockShutdown([&] {
          waitForWriteClosure();
          releaseCopy();
        });
        manager.m_operations.shutdown();
        TEST_CHECK(fixture.activeHelpers.load() == 0);
        break;
      }
      }
      if (end != End::Shutdown) {
        pumpUntil([&] { return operation.finished; });
        TEST_CHECK(!operation.succeeded && operation.mutated == partial && !manager.m_operations.usesMount(target));
        TEST_CHECK(operation.cancelled == (end == End::Cancel || end == End::Finish || end == End::CloseTab));
        if (end == End::Cancel)
          TEST_CHECK(manager.activeTab().hasActiveAdminSession());
        if (end == End::CloseTab)
          TEST_CHECK(manager.m_statusMessage != operation.status);
      }
      if (partial) {
        TEST_CHECK(operation.completedEntries.size() == 1);
        TEST_CHECK(operation.completedEntries.front().path == (target / first.filename()).string());
        TEST_CHECK(operation.resultDescription.find("1 of 2 selected items copied") != std::string::npos);
        TEST_CHECK(std::filesystem::file_size(target / first.filename()) == 0);
        TEST_CHECK(std::filesystem::remove(target / first.filename()));
      }
      TEST_CHECK(std::filesystem::is_empty(target) && readFile(source) == contents);
    };
    for (const bool recursive : {false, true}) {
      for (const auto end : {End::Cancel, End::Finish, End::CloseTab, End::Expire, End::Disconnect, End::Shutdown})
        checkLifecycle(recursive, end);
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
    TEST_CHECK(copied.empty());
    pasteKey(XKB_KEY_Insert, KeyMod::Shift);
    deliver("/tmp/administrator folder");
    TEST_CHECK(manager.m_locationInput->value() == "/tmp/administrator folder");
    TEST_CHECK(manager.m_operations.operations().empty());
  }

  static void testProtocolStage3ValidationAndUnsupportedCommands() {
    TEST_CHECK(kAdminProtocolVersion == 6);

    AdminParseError err;
    for (const auto code : {
             AdminStatusCode::AlreadyExists,
             AdminStatusCode::ReadOnlyFilesystem,
             AdminStatusCode::IdentityMismatch,
             AdminStatusCode::NotSupported,
         }) {
      AdminStatusCode parsed = AdminStatusCode::Ok;
      TEST_CHECK(parseAdminStatusCode(static_cast<std::uint32_t>(code), parsed) && parsed == code);
      TEST_CHECK(adminStatusCodeLabel(code) != "Unknown");
    }

    // 1. CREATE_FILE / CREATE_DIR / RENAME_ENTRY / MUTATION_RESULT round-trip.
    {
      const auto encFile = encodeAdminClientRequest({
          .command = AdminClientCommand::CreateFile,
          .requestId = 20,
          .path = "/etc/nythera",
          .entryName = "new item.txt",
          .autoNumber = true,
      });
      TEST_CHECK(encFile.has_value());
      AdminClientRequest decFile;
      TEST_CHECK(decodeAdminClientRequest(*encFile, decFile, &err));
      TEST_CHECK(
          decFile.command == AdminClientCommand::CreateFile
          && decFile.requestId == 20
          && decFile.autoNumber
          && decFile.path == "/etc/nythera"
          && decFile.entryName == "new item.txt"
      );

      const auto encDir = encodeAdminClientRequest({
          .command = AdminClientCommand::CreateDir,
          .requestId = 21,
          .path = "/etc/nythera",
          .entryName = "New Folder",
          .autoNumber = false,
      });
      TEST_CHECK(encDir.has_value());
      AdminClientRequest decDir;
      TEST_CHECK(decodeAdminClientRequest(*encDir, decDir, &err));
      TEST_CHECK(
          decDir.command == AdminClientCommand::CreateDir
          && decDir.requestId == 21
          && !decDir.autoNumber
          && decDir.path == "/etc/nythera"
          && decDir.entryName == "New Folder"
      );

      const auto encRen = encodeAdminClientRequest({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 22,
          .path = "/etc/nythera",
          .sourceName = "old.conf",
          .destName = "new.conf",
          .expectedDevice = 2049,
          .expectedInode = 987654,
          .expectedIsDir = false,
          .expectedIsSymlink = true,
      });
      TEST_CHECK(encRen.has_value());
      AdminClientRequest decRen;
      TEST_CHECK(decodeAdminClientRequest(*encRen, decRen, &err));
      TEST_CHECK(
          decRen.command == AdminClientCommand::RenameEntry
          && decRen.requestId == 22
          && decRen.path == "/etc/nythera"
          && decRen.sourceName == "old.conf"
          && decRen.destName == "new.conf"
          && decRen.expectedDevice == 2049
          && decRen.expectedInode == 987654
          && !decRen.expectedIsDir
          && decRen.expectedIsSymlink
      );

      const auto encMut = encodeAdminServerFrame({
          .kind = AdminServerFrameKind::MutationResult,
          .requestId = 22,
          .mutationInfo = {.name = "new.conf", .device = 2049, .inode = 987654},
      });
      TEST_CHECK(encMut.has_value());
      AdminServerFrame decMut;
      TEST_CHECK(decodeAdminServerFrame(*encMut, decMut, &err));
      TEST_CHECK(
          decMut.kind == AdminServerFrameKind::MutationResult
          && decMut.requestId == 22
          && decMut.mutationInfo.name == "new.conf"
          && decMut.mutationInfo.device == 2049
          && decMut.mutationInfo.inode == 987654
      );
    }

    // 2. Reject invalid names, traversal, relative paths, cross-dir names, same-name rename, zero identity.
    for (const auto badName : {"", ".", "..", "a/b", "/abs", "../escape"}) {
      TEST_CHECK(!encodeAdminClientRequest({
          .command = AdminClientCommand::CreateFile,
          .requestId = 1,
          .path = "/tmp",
          .entryName = badName,
      }));
      TEST_CHECK(!encodeAdminClientRequest({
          .command = AdminClientCommand::CreateDir,
          .requestId = 1,
          .path = "/tmp",
          .entryName = badName,
      }));
      TEST_CHECK(!encodeAdminClientRequest({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 1,
          .path = "/tmp",
          .sourceName = "ok",
          .destName = badName,
          .expectedDevice = 1,
          .expectedInode = 1,
      }));
      TEST_CHECK(!encodeAdminClientRequest({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 1,
          .path = "/tmp",
          .sourceName = badName,
          .destName = "ok",
          .expectedDevice = 1,
          .expectedInode = 1,
      }));
    }
    TEST_CHECK(!encodeAdminClientRequest({
        .command = AdminClientCommand::RenameEntry,
        .requestId = 1,
        .path = "/tmp",
        .sourceName = "same",
        .destName = "same",
        .expectedDevice = 1,
        .expectedInode = 1,
    }));
    TEST_CHECK(!encodeAdminClientRequest({
        .command = AdminClientCommand::RenameEntry,
        .requestId = 1,
        .path = "/tmp",
        .sourceName = "a",
        .destName = "b",
        .expectedDevice = 0,
        .expectedInode = 1,
    }));
    TEST_CHECK(!encodeAdminClientRequest({
        .command = AdminClientCommand::RenameEntry,
        .requestId = 1,
        .path = "/tmp",
        .sourceName = "a",
        .destName = "b",
        .expectedDevice = 1,
        .expectedInode = 0,
    }));

    // 3. Helper rejects raw malformed frames and Stage 4+ / unsupported commands cleanly.
    TempDir tmp("nythera-stage3-proto");
    InProcessHelperFixture fixture;
    RawHelperClient raw(fixture.makeLauncher());
    raw.hello();
    TEST_CHECK(raw.listDir(2, tmp.path).kind == AdminServerFrameKind::DirEnd);

    // Raw CREATE_FILE with ".." (base64 "Li4=") on authorised directory -> BadRequest.
    raw.writeRaw("CREATE_FILE 3 0 L3RtcA== Li4=\n");
    const auto dotdotResp = raw.readFrame();
    TEST_CHECK(dotdotResp.requestId == 3 && dotdotResp.status == AdminStatusCode::BadRequest);

    // Raw RENAME_ENTRY with "sub/file" (base64 "c3ViL2ZpbGU=") -> BadRequest.
    raw.writeRaw("RENAME_ENTRY 4 1 1 0 0 L3RtcA== YQ== c3ViL2ZpbGU=\n");
    const auto slashResp = raw.readFrame();
    TEST_CHECK(slashResp.requestId == 4 && slashResp.status == AdminStatusCode::BadRequest);

    // Reset consecutive bad-request streak with a valid PING before probing unsupported commands.
    AdminRequestId nextId = 10;
    for (const auto unsupportedCmd : {
             "DELETE_FILE " + std::to_string(nextId++) + " L3RtcA==\n",
             "MOVE_FILE " + std::to_string(nextId++) + " L3RtcA==\n",
             "CREATE_SYMLINK " + std::to_string(nextId++) + " L3RtcA==\n",
             "CHMOD " + std::to_string(nextId++) + " L3RtcA==\n",
         }) {
      const auto pingId = nextId++;
      raw.send({.command = AdminClientCommand::Ping, .requestId = pingId});
      const auto statusFrame = raw.readFrame();
      TEST_CHECK(statusFrame.kind == AdminServerFrameKind::Status && statusFrame.requestId == pingId);

      raw.writeRaw(unsupportedCmd);
      const auto resp = raw.readFrame();
      TEST_CHECK(resp.kind == AdminServerFrameKind::Response && resp.status == AdminStatusCode::Unsupported);
    }

    raw.send({
        .command = AdminClientCommand::ConflictReply,
        .requestId = nextId++,
        .conflictId = 1,
        .conflictDecision = 1,
        .alternativeName = "alt",
    });
    const auto conflictResp = raw.readFrame();
    TEST_CHECK(
        conflictResp.kind == AdminServerFrameKind::Response && conflictResp.status == AdminStatusCode::Unsupported
    );

    raw.closeAndWait();
  }

  static void testPrivilegedCreateFileAndPermissionsAndCollisions() {
    TempDir tmp("nythera-stage3-cfile");
    const auto workDir = tmp.path / "dir";
    std::filesystem::create_directories(workDir);
    writeFile(workDir / "existing.txt", "keep-me");
    std::filesystem::create_directory(workDir / "subdir_collide");
    std::filesystem::create_symlink(workDir / "existing.txt", workDir / "valid_link");
    std::filesystem::create_symlink(workDir / "does_not_exist_target", workDir / "dangling_link");

    // 1. Auto-numbered & exact file creation through AdminSessionClient.
    {
      auto alive = std::make_shared<bool>(true);
      InProcessHelperFixture fixture;
      AdminSessionClient client(1, alive);
      client.setTransportLauncher(fixture.makeLauncher());

      bool started = false;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
      pumpUntil([&] { return started; });

      bool listed = false;
      client.listDirectoryAsync(
          workDir, true, kAdminDefaultDirBatchEntries, [](const auto&) {},
          [&](bool ok, bool writable, std::uint64_t, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok && writable);
            listed = true;
          }
      );
      pumpUntil([&] { return listed; });

      // Create "New File", then "New File 2", then "New File 3".
      for (const std::string expected : {"New File", "New File 2", "New File 3"}) {
        bool done = false;
        AdminMutationRecord rec;
        client.createFileAsync(
            workDir, "New File", true,
            [&](bool ok, const AdminMutationRecord& r, AdminStatusCode code, const std::string&) {
              TEST_CHECK(ok && code == AdminStatusCode::Ok);
              rec = r;
              done = true;
            }
        );
        pumpUntil([&] { return done; });
        TEST_CHECK(rec.name == expected);
        TEST_CHECK(std::filesystem::is_regular_file(workDir / expected));
        TEST_CHECK(std::filesystem::file_size(workDir / expected) == 0);
      }

      // Extension-preserving auto-numbering ("report.txt" -> "report 2.txt").
      writeFile(workDir / "report.txt", "first");
      bool extDone = false;
      AdminMutationRecord extRec;
      client.createFileAsync(
          workDir, "report.txt", true, [&](bool ok, const AdminMutationRecord& r, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok);
            extRec = r;
            extDone = true;
          }
      );
      pumpUntil([&] { return extDone; });
      TEST_CHECK(extRec.name == "report 2.txt");
      TEST_CHECK(readFile(workDir / "report.txt") == "first");

      // Exact-name collisions with existing file, directory, valid symlink, and dangling symlink must all fail
      // with AlreadyExists without overwriting or creating the dangling symlink target!
      for (const auto existingName : {"existing.txt", "subdir_collide", "valid_link", "dangling_link"}) {
        bool done = false;
        AdminStatusCode codeOut = AdminStatusCode::Ok;
        client.createFileAsync(
            workDir, existingName, false,
            [&](bool ok, const AdminMutationRecord&, AdminStatusCode code, const std::string&) {
              TEST_CHECK(!ok);
              codeOut = code;
              done = true;
            }
        );
        pumpUntil([&] { return done; });
        TEST_CHECK(codeOut == AdminStatusCode::AlreadyExists);
      }
      TEST_CHECK(readFile(workDir / "existing.txt") == "keep-me");
      TEST_CHECK(!std::filesystem::exists(workDir / "does_not_exist_target"));

      // Auto-numbering skips over a dangling symlink rather than following it.
      bool linkAutoDone = false;
      AdminMutationRecord linkAutoRec;
      client.createFileAsync(
          workDir, "dangling_link", true,
          [&](bool ok, const AdminMutationRecord& r, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok);
            linkAutoRec = r;
            linkAutoDone = true;
          }
      );
      pumpUntil([&] { return linkAutoDone; });
      TEST_CHECK(linkAutoRec.name == "dangling_link 2");
      TEST_CHECK(!std::filesystem::exists(workDir / "does_not_exist_target"));

      client.stopSession(true);
      client.waitForSettled();
    }

    // 2. Hardened umask(0077) + creationUmask(0022) produces normal 0644 files
    //    and restores process umask to 0077 immediately afterward.
    {
      const mode_t prevUmask = ::umask(0077);
      const auto restoreUmask = nythera::test::makeScopeExit([prevUmask] { ::umask(prevUmask); });

      InProcessHelperFixture fixture;
      RawHelperClient raw(fixture.makeLauncher({.helperPid = 700, .invokingUid = 1000, .creationUmask = mode_t{0022}}));
      raw.hello("umask");
      TEST_CHECK(raw.listDir(2, workDir).kind == AdminServerFrameKind::DirEnd);
      raw.send({
          .command = AdminClientCommand::CreateFile,
          .requestId = 3,
          .path = workDir.string(),
          .entryName = "umask_check.txt",
          .autoNumber = false,
      });
      const auto mutFrame = raw.readFrame();
      TEST_CHECK(
          mutFrame.kind == AdminServerFrameKind::MutationResult && mutFrame.mutationInfo.name == "umask_check.txt"
      );
      raw.send({.command = AdminClientCommand::Shutdown, .requestId = 4});
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::ShuttingDown);
      raw.closeAndWait();

      TEST_CHECK(
          !fixture.results.empty() && fixture.results.back().reason == AdminHelperSessionExitReason::CleanShutdown
      );
      // Verify helper restored its process umask back to 0077 after creation.
      TEST_CHECK(::umask(0077) == 0077);

      struct stat st{};
      TEST_CHECK(::lstat((workDir / "umask_check.txt").c_str(), &st) == 0);
      TEST_CHECK((st.st_mode & 0777) == 0644);
    }
  }

  static void testPrivilegedCreateDirAndPermissionsAndCollisions() {
    TempDir tmp("nythera-stage3-cdir");
    const auto workDir = tmp.path / "dir";
    std::filesystem::create_directories(workDir);
    writeFile(workDir / "file_collide", "abc");
    std::filesystem::create_directory(workDir / "dir_collide");
    std::filesystem::create_symlink(workDir / "dir_collide", workDir / "valid_dir_link");
    std::filesystem::create_symlink(workDir / "missing_dir_target", workDir / "dangling_dir_link");

    // 1. Auto-numbered folder creation + collision rejections.
    {
      auto alive = std::make_shared<bool>(true);
      InProcessHelperFixture fixture;
      AdminSessionClient client(2, alive);
      client.setTransportLauncher(fixture.makeLauncher());

      bool started = false;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
      pumpUntil([&] { return started; });

      bool listed = false;
      client.listDirectoryAsync(
          workDir, true, kAdminDefaultDirBatchEntries, [](const auto&) {},
          [&](bool ok, bool writable, std::uint64_t, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok && writable);
            listed = true;
          }
      );
      pumpUntil([&] { return listed; });

      for (const std::string expected : {"New Folder", "New Folder 2", "New Folder 3"}) {
        bool done = false;
        AdminMutationRecord rec;
        client.createDirectoryAsync(
            workDir, "New Folder", true,
            [&](bool ok, const AdminMutationRecord& r, AdminStatusCode code, const std::string&) {
              TEST_CHECK(ok && code == AdminStatusCode::Ok);
              rec = r;
              done = true;
            }
        );
        pumpUntil([&] { return done; });
        TEST_CHECK(rec.name == expected);
        TEST_CHECK(std::filesystem::is_directory(workDir / expected));
      }

      for (const auto existing : {"file_collide", "dir_collide", "valid_dir_link", "dangling_dir_link"}) {
        bool done = false;
        AdminStatusCode codeOut = AdminStatusCode::Ok;
        client.createDirectoryAsync(
            workDir, existing, false,
            [&](bool ok, const AdminMutationRecord&, AdminStatusCode code, const std::string&) {
              TEST_CHECK(!ok);
              codeOut = code;
              done = true;
            }
        );
        pumpUntil([&] { return done; });
        TEST_CHECK(codeOut == AdminStatusCode::AlreadyExists);
      }
      TEST_CHECK(!std::filesystem::exists(workDir / "missing_dir_target"));

      client.stopSession(true);
      client.waitForSettled();
    }

    // 2. Hardened umask(0077) + creationUmask(0022) creates 0755 directories and restores umask to 0077.
    {
      const mode_t prevUmask = ::umask(0077);
      const auto restoreUmask = nythera::test::makeScopeExit([prevUmask] { ::umask(prevUmask); });

      InProcessHelperFixture fixture;
      RawHelperClient raw(fixture.makeLauncher({.helperPid = 701, .invokingUid = 1000, .creationUmask = mode_t{0022}}));
      raw.hello("dir-umask");
      TEST_CHECK(raw.listDir(2, workDir).kind == AdminServerFrameKind::DirEnd);
      raw.send({
          .command = AdminClientCommand::CreateDir,
          .requestId = 3,
          .path = workDir.string(),
          .entryName = "umask_dir",
          .autoNumber = false,
      });
      const auto mutFrame = raw.readFrame();
      TEST_CHECK(mutFrame.kind == AdminServerFrameKind::MutationResult && mutFrame.mutationInfo.name == "umask_dir");
      raw.send({.command = AdminClientCommand::Shutdown, .requestId = 4});
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::ShuttingDown);
      raw.closeAndWait();

      TEST_CHECK(
          !fixture.results.empty() && fixture.results.back().reason == AdminHelperSessionExitReason::CleanShutdown
      );
      TEST_CHECK(::umask(0077) == 0077);

      struct stat st{};
      TEST_CHECK(::lstat((workDir / "umask_dir").c_str(), &st) == 0);
      TEST_CHECK(S_ISDIR(st.st_mode));
      TEST_CHECK((st.st_mode & 0777) == 0755);
    }
  }

  static void testPrivilegedSameDirectoryRenameAndCollisions() {
    TempDir tmp("nythera-stage3-rename");
    const auto workDir = tmp.path / "dir";
    const auto extDir = tmp.path / "external";
    std::filesystem::create_directories(workDir / "folder_src");
    std::filesystem::create_directories(workDir / "empty_dir_dst");
    std::filesystem::create_directories(workDir / "nonempty_dir_dst");
    std::filesystem::create_directories(extDir);

    writeFile(workDir / "file_src.txt", "original-bytes");
    writeFile(workDir / "folder_src" / "inside.txt", "child-bytes");
    writeFile(workDir / "file_dst.txt", "do-not-replace");
    writeFile(workDir / "nonempty_dir_dst" / "keep.txt", "keep");
    writeFile(extDir / "target.txt", "ext-target");

    std::filesystem::create_symlink(extDir / "target.txt", workDir / "valid_link_src");
    std::filesystem::create_symlink(extDir / "missing.txt", workDir / "broken_link_src");
    std::filesystem::create_symlink(extDir / "missing_dst.txt", workDir / "dangling_dst");

    auto alive = std::make_shared<bool>(true);
    InProcessHelperFixture fixture;
    AdminSessionClient client(3, alive);
    client.setTransportLauncher(fixture.makeLauncher());

    bool started = false;
    client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
    pumpUntil([&] { return started; });

    std::vector<AdminDirEntryRecord> entries;
    bool listed = false;
    client.listDirectoryAsync(
        workDir, true, kAdminDefaultDirBatchEntries,
        [&](const std::vector<AdminDirEntryRecord>& batch) {
          entries.insert(entries.end(), batch.begin(), batch.end());
        },
        [&](bool ok, bool writable, std::uint64_t, AdminStatusCode, const std::string&) {
          TEST_CHECK(ok && writable);
          listed = true;
        }
    );
    pumpUntil([&] { return listed; });

    const auto findRec = [&](std::string_view name) {
      const auto it = std::ranges::find(entries, name, &AdminDirEntryRecord::name);
      TEST_CHECK(it != entries.end());
      TEST_CHECK(it->hasIdentity && it->device != 0 && it->inode != 0);
      return *it;
    };

    const auto fileRec = findRec("file_src.txt");
    const auto dirRec = findRec("folder_src");
    const auto validLinkRec = findRec("valid_link_src");
    const auto brokenLinkRec = findRec("broken_link_src");

    // Verify LIST_DIR device/inode matches lstat (including makedev fix and symlink identity).
    struct stat lstatFile{};
    struct stat lstatLink{};
    TEST_CHECK(::lstat((workDir / "file_src.txt").c_str(), &lstatFile) == 0);
    TEST_CHECK(::lstat((workDir / "valid_link_src").c_str(), &lstatLink) == 0);
    TEST_CHECK(fileRec.device == static_cast<std::uint64_t>(lstatFile.st_dev));
    TEST_CHECK(fileRec.inode == static_cast<std::uint64_t>(lstatFile.st_ino));
    TEST_CHECK(validLinkRec.device == static_cast<std::uint64_t>(lstatLink.st_dev));
    TEST_CHECK(validLinkRec.inode == static_cast<std::uint64_t>(lstatLink.st_ino));

    // 1. Successful file rename preserves contents and inode.
    {
      bool done = false;
      AdminMutationRecord mut;
      client.renameEntryAsync(
          workDir, "file_src.txt", "file_renamed.txt", fileRec.device, fileRec.inode, false, false,
          [&](bool ok, const AdminMutationRecord& r, AdminStatusCode code, const std::string&) {
            TEST_CHECK(ok && code == AdminStatusCode::Ok);
            mut = r;
            done = true;
          }
      );
      pumpUntil([&] { return done; });
      TEST_CHECK(mut.name == "file_renamed.txt" && mut.device == fileRec.device && mut.inode == fileRec.inode);
      TEST_CHECK(!std::filesystem::exists(workDir / "file_src.txt"));
      TEST_CHECK(readFile(workDir / "file_renamed.txt") == "original-bytes");
    }

    // 2. Successful folder rename preserves child entries and inode.
    {
      bool done = false;
      AdminMutationRecord mut;
      client.renameEntryAsync(
          workDir, "folder_src", "folder_renamed", dirRec.device, dirRec.inode, true, false,
          [&](bool ok, const AdminMutationRecord& r, AdminStatusCode code, const std::string&) {
            TEST_CHECK(ok && code == AdminStatusCode::Ok);
            mut = r;
            done = true;
          }
      );
      pumpUntil([&] { return done; });
      TEST_CHECK(mut.name == "folder_renamed" && mut.device == dirRec.device && mut.inode == dirRec.inode);
      TEST_CHECK(!std::filesystem::exists(workDir / "folder_src"));
      TEST_CHECK(readFile(workDir / "folder_renamed" / "inside.txt") == "child-bytes");
    }

    // 3. Valid and broken symlinks rename the link itself without touching targets.
    {
      bool done = false;
      client.renameEntryAsync(
          workDir, "valid_link_src", "valid_link_renamed", validLinkRec.device, validLinkRec.inode, false, true,
          [&](bool ok, const AdminMutationRecord& r, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok && r.name == "valid_link_renamed" && r.inode == validLinkRec.inode);
            done = true;
          }
      );
      pumpUntil([&] { return done; });
      TEST_CHECK(std::filesystem::is_symlink(workDir / "valid_link_renamed"));
      TEST_CHECK(readFile(extDir / "target.txt") == "ext-target");

      done = false;
      client.renameEntryAsync(
          workDir, "broken_link_src", "broken_link_renamed", brokenLinkRec.device, brokenLinkRec.inode, false, true,
          [&](bool ok, const AdminMutationRecord& r, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok && r.name == "broken_link_renamed" && r.inode == brokenLinkRec.inode);
            done = true;
          }
      );
      pumpUntil([&] { return done; });
      std::error_code ec;
      const auto st = std::filesystem::symlink_status(workDir / "broken_link_renamed", ec);
      TEST_CHECK(!ec && std::filesystem::is_symlink(st));
    }

    // 4. Collisions with file, empty dir, non-empty dir, valid symlink, and dangling symlink must ALL fail cleanly.
    for (const auto existingDst :
         {"file_dst.txt", "empty_dir_dst", "nonempty_dir_dst", "valid_link_renamed", "dangling_dst"}) {
      bool done = false;
      AdminStatusCode codeOut = AdminStatusCode::Ok;
      client.renameEntryAsync(
          workDir, "file_renamed.txt", existingDst, fileRec.device, fileRec.inode, false, false,
          [&](bool ok, const AdminMutationRecord&, AdminStatusCode code, const std::string&) {
            TEST_CHECK(!ok);
            codeOut = code;
            done = true;
          }
      );
      pumpUntil([&] { return done; });
      TEST_CHECK(codeOut == AdminStatusCode::AlreadyExists);
    }
    TEST_CHECK(readFile(workDir / "file_renamed.txt") == "original-bytes");
    TEST_CHECK(readFile(workDir / "file_dst.txt") == "do-not-replace");
    TEST_CHECK(readFile(workDir / "nonempty_dir_dst" / "keep.txt") == "keep");

    client.stopSession(true);
    client.waitForSettled();
  }

  static void testSymlinkTraversalAndConcurrentRaceProtections() {
    // 1. Parent directory replaced with symlink to outside directory after LIST_DIR -> IdentityMismatch.
    {
      TempDir tmp("nythera-stage3-swap-parent");
      const auto realDir = tmp.path / "real_dir";
      const auto outsideDir = tmp.path / "outside_dir";
      std::filesystem::create_directories(realDir);
      std::filesystem::create_directories(outsideDir);
      writeFile(realDir / "item.txt", "inside");

      InProcessHelperFixture fixture;
      RawHelperClient raw(fixture.makeLauncher());
      raw.hello();
      TEST_CHECK(raw.listDir(2, realDir).kind == AdminServerFrameKind::DirEnd);

      // Swap realDir with a symlink pointing to outsideDir.
      const auto movedReal = tmp.path / "moved_real_dir";
      std::filesystem::rename(realDir, movedReal);
      std::filesystem::create_symlink(outsideDir, realDir);

      raw.send({
          .command = AdminClientCommand::CreateFile,
          .requestId = 3,
          .path = realDir.string(),
          .entryName = "escape.txt",
      });
      const auto resp = raw.readFrame();
      TEST_CHECK(resp.kind == AdminServerFrameKind::Response && resp.status == AdminStatusCode::IdentityMismatch);
      TEST_CHECK(!std::filesystem::exists(outsideDir / "escape.txt"));
      TEST_CHECK(!std::filesystem::exists(movedReal / "escape.txt"));
      raw.closeAndWait();
    }

    // 2. Source replaced before rename & source type mismatch -> IdentityMismatch.
    {
      TempDir tmp("nythera-stage3-src-swap");
      const auto dir = tmp.path / "dir";
      std::filesystem::create_directories(dir);
      writeFile(dir / "target.txt", "v1");
      struct stat st1{};
      TEST_CHECK(::lstat((dir / "target.txt").c_str(), &st1) == 0);

      InProcessHelperFixture fixture;
      RawHelperClient raw(fixture.makeLauncher());
      raw.hello();
      TEST_CHECK(raw.listDir(2, dir).kind == AdminServerFrameKind::DirEnd);

      // Wrong type flag with real device/inode -> IdentityMismatch.
      raw.send({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 3,
          .path = dir.string(),
          .sourceName = "target.txt",
          .destName = "renamed.txt",
          .expectedDevice = static_cast<std::uint64_t>(st1.st_dev),
          .expectedInode = static_cast<std::uint64_t>(st1.st_ino),
          .expectedIsDir = true,
          .expectedIsSymlink = false,
      });
      const auto typeMismatch = raw.readFrame();
      TEST_CHECK(typeMismatch.status == AdminStatusCode::IdentityMismatch);
      TEST_CHECK(std::filesystem::exists(dir / "target.txt"));

      // Replace source with a different file before sending rename -> IdentityMismatch.
      std::filesystem::remove(dir / "target.txt");
      writeFile(dir / "target.txt", "v2");
      raw.send({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 4,
          .path = dir.string(),
          .sourceName = "target.txt",
          .destName = "renamed.txt",
          .expectedDevice = static_cast<std::uint64_t>(st1.st_dev),
          .expectedInode = static_cast<std::uint64_t>(st1.st_ino),
          .expectedIsDir = false,
          .expectedIsSymlink = false,
      });
      const auto idMismatch = raw.readFrame();
      TEST_CHECK(idMismatch.status == AdminStatusCode::IdentityMismatch);
      TEST_CHECK(readFile(dir / "target.txt") == "v2");
      TEST_CHECK(!std::filesystem::exists(dir / "renamed.txt"));
      raw.closeAndWait();
    }

    // 3. Race right before renameat2: destination appears -> AlreadyExists and destination preserved.
    {
      TempDir tmp("nythera-stage3-dst-race");
      const auto dir = tmp.path / "dir";
      std::filesystem::create_directories(dir);
      writeFile(dir / "src.txt", "src-data");
      struct stat srcSt{};
      TEST_CHECK(::lstat((dir / "src.txt").c_str(), &srcSt) == 0);

      InProcessHelperFixture fixture;
      AdminHelperSessionConfig cfg{.helperPid = 7100, .invokingUid = 1000};
      cfg.beforeRenameHookForTesting = [&](const std::string& p, const std::string&, const std::string& dstName) {
        writeFile(std::filesystem::path(p) / dstName, "raced-dst");
      };
      RawHelperClient raw(fixture.makeLauncher(cfg));
      raw.hello();
      TEST_CHECK(raw.listDir(2, dir).kind == AdminServerFrameKind::DirEnd);

      raw.send({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 3,
          .path = dir.string(),
          .sourceName = "src.txt",
          .destName = "dst.txt",
          .expectedDevice = static_cast<std::uint64_t>(srcSt.st_dev),
          .expectedInode = static_cast<std::uint64_t>(srcSt.st_ino),
          .expectedIsDir = false,
          .expectedIsSymlink = false,
      });
      const auto resp = raw.readFrame();
      TEST_CHECK(resp.kind == AdminServerFrameKind::Response && resp.status == AdminStatusCode::AlreadyExists);
      TEST_CHECK(readFile(dir / "src.txt") == "src-data");
      TEST_CHECK(readFile(dir / "dst.txt") == "raced-dst");
      raw.closeAndWait();
    }

    // 4. Race right before renameat2: source swapped -> post-check rolls back and returns IdentityMismatch.
    {
      TempDir tmp("nythera-stage3-src-race");
      const auto dir = tmp.path / "dir";
      std::filesystem::create_directories(dir);
      writeFile(dir / "src.txt", "original-src");
      struct stat srcSt{};
      TEST_CHECK(::lstat((dir / "src.txt").c_str(), &srcSt) == 0);

      InProcessHelperFixture fixture;
      AdminHelperSessionConfig cfg{.helperPid = 7101, .invokingUid = 1000};
      cfg.beforeRenameHookForTesting = [&](const std::string& p, const std::string& srcName, const std::string&) {
        const auto target = std::filesystem::path(p) / srcName;
        std::filesystem::remove(target);
        writeFile(target, "swapped-attacker");
      };
      RawHelperClient raw(fixture.makeLauncher(cfg));
      raw.hello();
      TEST_CHECK(raw.listDir(2, dir).kind == AdminServerFrameKind::DirEnd);

      raw.send({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 3,
          .path = dir.string(),
          .sourceName = "src.txt",
          .destName = "dst.txt",
          .expectedDevice = static_cast<std::uint64_t>(srcSt.st_dev),
          .expectedInode = static_cast<std::uint64_t>(srcSt.st_ino),
          .expectedIsDir = false,
          .expectedIsSymlink = false,
      });
      const auto resp = raw.readFrame();
      TEST_CHECK(resp.kind == AdminServerFrameKind::Response && resp.status == AdminStatusCode::IdentityMismatch);
      TEST_CHECK(readFile(dir / "src.txt") == "swapped-attacker");
      TEST_CHECK(!std::filesystem::exists(dir / "dst.txt"));
      raw.closeAndWait();
    }

    // 5. RENAME_NOREPLACE unsupported (EOPNOTSUPP / ENOSYS) fails safely with NotSupported (no fallback).
    for (const int unsupportedErr : {EOPNOTSUPP, ENOSYS}) {
      TempDir tmp("nythera-stage3-nosup");
      const auto dir = tmp.path / "dir";
      std::filesystem::create_directories(dir);
      writeFile(dir / "src.txt", "keep");
      struct stat srcSt{};
      TEST_CHECK(::lstat((dir / "src.txt").c_str(), &srcSt) == 0);

      InProcessHelperFixture fixture;
      AdminHelperSessionConfig cfg{.helperPid = 7102, .invokingUid = 1000};
      cfg.renameAt2HookForTesting = [unsupportedErr](int, const char*, int, const char*, unsigned int) -> int {
        errno = unsupportedErr;
        return -1;
      };
      RawHelperClient raw(fixture.makeLauncher(cfg));
      raw.hello();
      TEST_CHECK(raw.listDir(2, dir).kind == AdminServerFrameKind::DirEnd);

      raw.send({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 3,
          .path = dir.string(),
          .sourceName = "src.txt",
          .destName = "dst.txt",
          .expectedDevice = static_cast<std::uint64_t>(srcSt.st_dev),
          .expectedInode = static_cast<std::uint64_t>(srcSt.st_ino),
          .expectedIsDir = false,
          .expectedIsSymlink = false,
      });
      const auto resp = raw.readFrame();
      TEST_CHECK(resp.kind == AdminServerFrameKind::Response && resp.status == AdminStatusCode::NotSupported);
      TEST_CHECK(readFile(dir / "src.txt") == "keep");
      TEST_CHECK(!std::filesystem::exists(dir / "dst.txt"));
      raw.closeAndWait();
    }
  }

  static void testUnauthorizedDirectoryAndNavigationInvalidation() {
    TempDir tmp("nythera-stage3-unauth");
    const auto dirA = tmp.path / "dirA";
    const auto dirB = tmp.path / "dirB";
    const auto missingDir = tmp.path / "missing";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);
    writeFile(dirA / "a.txt", "a");
    struct stat aSt{};
    TEST_CHECK(::lstat((dirA / "a.txt").c_str(), &aSt) == 0);

    // 1. Write before HELLO -> HandshakeRequired & ProtocolViolation.
    {
      InProcessHelperFixture fixture;
      RawHelperClient raw(fixture.makeLauncher());
      raw.send({
          .command = AdminClientCommand::CreateFile,
          .requestId = 1,
          .path = dirA.string(),
          .entryName = "pre_hello.txt",
      });
      const auto resp = raw.readFrame();
      TEST_CHECK(resp.kind == AdminServerFrameKind::Response && resp.status == AdminStatusCode::HandshakeRequired);
      raw.closeAndWait();
      TEST_CHECK(!std::filesystem::exists(dirA / "pre_hello.txt"));
      TEST_CHECK(
          !fixture.results.empty() && fixture.results.back().reason == AdminHelperSessionExitReason::ProtocolViolation
      );
    }

    // 2. Write before LIST_DIR, write to un-authorised dirB, and old dirA invalidated after navigating away.
    {
      InProcessHelperFixture fixture;
      RawHelperClient raw(fixture.makeLauncher());
      raw.hello();

      raw.send({
          .command = AdminClientCommand::CreateFile,
          .requestId = 2,
          .path = dirA.string(),
          .entryName = "before_list.txt",
      });
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::Unauthorized);
      TEST_CHECK(!std::filesystem::exists(dirA / "before_list.txt"));

      TEST_CHECK(raw.listDir(3, dirA).kind == AdminServerFrameKind::DirEnd);

      // Targeting dirB while dirA is authorised -> Unauthorized.
      raw.send({
          .command = AdminClientCommand::CreateDir,
          .requestId = 4,
          .path = dirB.string(),
          .entryName = "sneaky_dir",
      });
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::Unauthorized);
      TEST_CHECK(!std::filesystem::exists(dirB / "sneaky_dir"));

      // Navigate to missingDir (fails LIST_DIR) -> dirA authorization must be cleared!
      TEST_CHECK(raw.listDir(5, missingDir).status == AdminStatusCode::NotFound);
      raw.send({
          .command = AdminClientCommand::RenameEntry,
          .requestId = 6,
          .path = dirA.string(),
          .sourceName = "a.txt",
          .destName = "b.txt",
          .expectedDevice = static_cast<std::uint64_t>(aSt.st_dev),
          .expectedInode = static_cast<std::uint64_t>(aSt.st_ino),
          .expectedIsDir = false,
          .expectedIsSymlink = false,
      });
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::Unauthorized);
      TEST_CHECK(std::filesystem::exists(dirA / "a.txt"));

      // Navigate to dirB -> dirB is authorised, dirA is not.
      TEST_CHECK(raw.listDir(7, dirB).kind == AdminServerFrameKind::DirEnd);
      raw.send({
          .command = AdminClientCommand::CreateFile,
          .requestId = 8,
          .path = dirA.string(),
          .entryName = "stale_dir.txt",
      });
      TEST_CHECK(raw.readFrame().status == AdminStatusCode::Unauthorized);
      TEST_CHECK(!std::filesystem::exists(dirA / "stale_dir.txt"));

      raw.send({
          .command = AdminClientCommand::CreateFile,
          .requestId = 9,
          .path = dirB.string(),
          .entryName = "ok_in_b.txt",
      });
      TEST_CHECK(raw.readFrame().kind == AdminServerFrameKind::MutationResult);
      TEST_CHECK(std::filesystem::exists(dirB / "ok_in_b.txt"));

      raw.closeAndWait();
    }
  }

  static void testSessionExpiryActivityRenewalAndMidFlightDisconnect() {
    TempDir tmp("nythera-stage3-session");
    const auto dir = tmp.path / "dir";
    std::filesystem::create_directories(dir);
    writeFile(dir / "initial.txt", "hello");

    // 1. Inactive client rejects mutations immediately.
    {
      auto alive = std::make_shared<bool>(true);
      AdminSessionClient client(1, alive);
      bool rejected = false;
      const auto reqId = client.createFileAsync(
          dir, "nope.txt", false, [&](bool ok, const AdminMutationRecord&, AdminStatusCode code, const std::string&) {
            TEST_CHECK(!ok && code == AdminStatusCode::HandshakeRequired);
            rejected = true;
          }
      );
      TEST_CHECK(reqId == kInvalidAdminRequestId);
      pumpUntil([&] { return rejected; });
      TEST_CHECK(!std::filesystem::exists(dir / "nope.txt"));
    }

    // 2. Write commands renew inactivity timer, whereas background LIST_DIR / PING / STATVFS / COUNT_DIR do not.
    {
      auto alive = std::make_shared<bool>(true);
      InProcessHelperFixture fixture;
      AdminSessionClient client(2, alive);
      client.setTransportLauncher(
          fixture.makeLauncher({.helperPid = 7200, .invokingUid = 1000, .inactivityTimeoutMs = 110})
      );
      client.setInactivityTimeoutMs(110, true);

      bool started = false;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
      pumpUntil([&] { return started; });

      bool listed = false;
      client.listDirectoryAsync(
          dir, true, kAdminDefaultDirBatchEntries, [](const auto&) {},
          [&](bool ok, bool, std::uint64_t, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok);
            listed = true;
          }
      );
      pumpUntil([&] { return listed; });

      // Sleep 55ms -> createFile (renews) -> sleep 55ms -> createDir (renews) -> sleep 55ms -> rename (renews).
      // Total > 165ms > 110ms, so session stays active ONLY because writes renewed the timer.
      std::this_thread::sleep_for(std::chrono::milliseconds(55));
      bool fileDone = false;
      AdminMutationRecord fileMut;
      client.createFileAsync(
          dir, "renew.txt", false, [&](bool ok, const AdminMutationRecord& r, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok);
            fileMut = r;
            fileDone = true;
          }
      );
      pumpUntil([&] { return fileDone; });

      std::this_thread::sleep_for(std::chrono::milliseconds(55));
      bool dirDone = false;
      client.createDirectoryAsync(
          dir, "renew_dir", false, [&](bool ok, const AdminMutationRecord&, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok);
            dirDone = true;
          }
      );
      pumpUntil([&] { return dirDone; });

      std::this_thread::sleep_for(std::chrono::milliseconds(55));
      bool renDone = false;
      client.renameEntryAsync(
          dir, "renew.txt", "renewed.txt", fileMut.device, fileMut.inode, false, false,
          [&](bool ok, const AdminMutationRecord&, AdminStatusCode, const std::string&) {
            TEST_CHECK(ok);
            renDone = true;
          }
      );
      pumpUntil([&] { return renDone; });
      TEST_CHECK(client.isActive());

      // Now send only background LIST_DIR(userInitiated=false), PING, STATVFS_INFO -> session MUST expire.
      const auto bgStart = std::chrono::steady_clock::now();
      while (client.state() == AdminSessionState::Active
             && std::chrono::steady_clock::now() - bgStart < std::chrono::milliseconds(500)) {
        (void)client.listDirectoryAsync(
            dir, false, kAdminDefaultDirBatchEntries, [](const auto&) {},
            [](bool, bool, std::uint64_t, AdminStatusCode, const std::string&) {}
        );
        (void)client.pingAsync([](const AdminHealthStatus&) {});
        (void)client.statVfsAsync(dir, [](bool, const AdminStatVfsRecord&, AdminStatusCode, const std::string&) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
      }
      TEST_CHECK(client.state() == AdminSessionState::Failed);
      client.waitForSettled();
      TEST_CHECK(
          !fixture.results.empty() && fixture.results.back().reason == AdminHelperSessionExitReason::InactivityTimeout
      );

      // Expired session rejects new mutations immediately.
      bool postExpireRejected = false;
      TEST_CHECK(
          client.createFileAsync(
              dir, "after_expire.txt", false,
              [&](bool ok, const AdminMutationRecord&, AdminStatusCode code, const std::string&) {
                TEST_CHECK(!ok && code == AdminStatusCode::HandshakeRequired);
                postExpireRejected = true;
              }
          )
          == kInvalidAdminRequestId
      );
      pumpUntil([&] { return postExpireRejected; });
    }

    // 3. Helper disconnect mid-flight fails pending mutation callback and recovers UI without wedged state.
    {
      InProcessHelperFixture fixture;
      FileManager manager(dir.string());
      pumpUntil([&] { return !manager.activeSession().loading(); });
      manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher({.inactivityTimeoutMs = 150}));
      TEST_CHECK(manager.enterAdministratorMode(manager.activeTab().id));
      pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });

      // Start an inline rename via real scene, hold helper by killing transport unexpectedly mid-operation.
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
      TEST_CHECK(!manager.m_adminInlineRenameTarget.has_value());
      TEST_CHECK(manager.m_adminMutationsInFlight.empty());

      // Also test pending mutation callback drained cleanly when AdminSessionClient stops mid-flight.
      writeFile(dir / "hold_inflight.txt", "hold");
      struct stat holdSt{};
      TEST_CHECK(::stat((dir / "hold_inflight.txt").c_str(), &holdSt) == 0);

      std::mutex holdMu;
      std::condition_variable holdCv;
      bool holdEntered = false;
      bool stopIssued = false;
      AdminHelperSessionConfig holdCfg{.helperPid = 7300, .invokingUid = 1000};
      holdCfg.beforeRenameHookForTesting = [&](const std::string&, const std::string&, const std::string&) {
        {
          std::lock_guard lock(holdMu);
          holdEntered = true;
        }
        holdCv.notify_all();
        std::unique_lock lock(holdMu);
        holdCv.wait(lock, [&] { return stopIssued; });
        lock.unlock();
        // Let the worker thread wake on stopRequested before the helper writes MUTATION_RESULT.
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
      };

      auto alive = std::make_shared<bool>(true);
      AdminSessionClient rawClient(44, alive);
      rawClient.setTransportLauncher(fixture.makeLauncher(std::move(holdCfg)));
      bool rawStarted = false;
      rawClient.startSessionAsync([&](bool ok, const AdminSessionStatus&) { rawStarted = ok; });
      pumpUntil([&] { return rawStarted; });

      bool rawListed = false;
      rawClient.listDirectoryAsync(
          dir, false, kAdminDefaultDirBatchEntries, [](const auto&) {},
          [&](bool ok, bool, std::uint64_t, AdminStatusCode, const std::string&) { rawListed = ok; }
      );
      pumpUntil([&] { return rawListed; });

      bool failedPending = false;
      AdminStatusCode failedCode = AdminStatusCode::Ok;
      (void)rawClient.renameEntryAsync(
          dir, "hold_inflight.txt", "hold_inflight_renamed.txt", static_cast<std::uint64_t>(holdSt.st_dev),
          static_cast<std::uint64_t>(holdSt.st_ino), false, false,
          [&](bool ok, const AdminMutationRecord&, AdminStatusCode code, const std::string&) {
            TEST_CHECK(!ok);
            failedCode = code;
            failedPending = true;
          }
      );
      {
        std::unique_lock lock(holdMu);
        const bool entered = holdCv.wait_for(lock, std::chrono::seconds(5), [&] { return holdEntered; });
        if (!entered) {
          stopIssued = true;
          holdCv.notify_all();
        }
        TEST_CHECK(entered);
      }
      rawClient.stopSession(true);
      {
        std::lock_guard lock(holdMu);
        stopIssued = true;
      }
      holdCv.notify_all();
      pumpUntil([&] { return failedPending; });
      TEST_CHECK(failedCode == AdminStatusCode::Cancelled);
      rawClient.waitForSettled();

      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_grid = nullptr;
      manager.m_tabs.shutdownAll();
      for (const auto& t : manager.m_tabs.tabs())
        t->adminSession.waitForSettled();
    }
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

    // Create file & folder in Admin tab: must NOT record anything in FileOperationManager or undo stack.
    const std::size_t opsBefore = manager.m_operations.operations().size();
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 112});
    pumpUntil([&] {
      return std::filesystem::exists(adminDir / "New File")
          && manager.m_adminMutationsInFlight.empty()
          && !manager.activeSession().loading();
    });
    TEST_CHECK(manager.m_operations.operations().size() == opsBefore);
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
    TEST_CHECK(manager.m_operations.operations().size() == opsBefore + 1);
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

    // 2. Verify File/Edit/Tools menus and Create New submenu enable ONLY Folder… (111), Empty File… (112),
    //    Rename (4) and Administrator Paste (3) while blocking the other transfers.
    {
      manager.activeTab().selectedPath = writableDir / "alpha.txt";
      manager.activeTab().selectedPaths = {(writableDir / "alpha.txt").string()};
      const auto fileMenu = manager.fileMenuEntries();
      const auto* createMenu = findEntryById(fileMenu, 110);
      TEST_CHECK(createMenu != nullptr && createMenu->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, 111)->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, 112)->enabled);
      TEST_CHECK(!findEntryById(createMenu->submenuEntries, 113)->enabled);
      TEST_CHECK(!findEntryById(createMenu->submenuEntries, kManualSymlink)->enabled);
      TEST_CHECK(findEntryById(createMenu->submenuEntries, kSelectionSymlink) == nullptr);
      for (int tplId : {114, 115, 116}) {
        if (const auto* tpl = findEntryById(createMenu->submenuEntries, tplId))
          TEST_CHECK(!tpl->enabled);
      }
      const auto editMenu = manager.editMenuEntries();
      TEST_CHECK(findEntryById(editMenu, 4)->enabled);
      TEST_CHECK(findEntryById(editMenu, 3)->enabled);
      for (int blockedId :
           {1, 2, 5, 6, 59, static_cast<int>(kDuplicate), static_cast<int>(kUndo), static_cast<int>(kRedo),
            static_cast<int>(kManualSymlink)}) {
        const auto* e = findEntryById(editMenu, blockedId);
        TEST_CHECK(e != nullptr && !e->enabled);
      }
    }

    // 3. Empty File… (112) -> creates "New File", selects it, opens inline rename after load finishes,
    //    and inline rename commits "created_doc.txt".
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 112});
    pumpUntil([&] { return manager.m_adminMutationsInFlight.empty(); });
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
          && manager.m_adminMutationsInFlight.empty()
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
          && manager.m_adminMutationsInFlight.empty()
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
          && manager.m_adminMutationsInFlight.empty()
          && !manager.activeSession().loading();
    });
    TEST_CHECK(manager.activeTab().selectedPath == writableDir / "New Folder 2");
    scene.relayout(manager, 1120.0F, 760.0F);
    TEST_CHECK(manager.m_renamePath == writableDir / "New Folder 2");
    typeInlineTextAndSubmit(manager.m_dispatcher, "renamed_folder");
    pumpUntil([&] {
      return std::filesystem::is_directory(writableDir / "renamed_folder")
          && manager.m_adminMutationsInFlight.empty()
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
    TEST_CHECK(manager.m_statusMessage == "Choose a valid filename");

    // Existing destination collision ("existing.txt") -> rejected by helper, editor stays open, files intact.
    typeInlineTextAndSubmit(manager.m_dispatcher, "existing.txt");
    pumpUntil([&] {
      return !manager.m_adminInlineRenameTarget.has_value()
          && manager.m_statusMessage == "An item with that name already exists";
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

    // 6. Blocked actions in Administrator Mode remain rejected even when invoked directly.
    const std::size_t opsBeforeBlocked = manager.m_operations.operations().size();
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 113}); // Text File…
    TEST_CHECK(manager.m_statusMessage == "This action is unavailable in Administrator Mode");
    TEST_CHECK(!std::filesystem::exists(writableDir / "New File.txt"));
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 114}); // Document…
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kManualSymlink});
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kDuplicate});
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 5});     // Wastebin
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 6});     // Permanent delete
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
