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
#include "render/scene/rect_node.h"
#include "test_isolation.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

  // The real helper session loop runs on pipes without root. Tests can inject authentication
  // outcomes or independently model accessibility through the ordinary Files backend.
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
    std::atomic<std::uint32_t> nextPid{5000};
    std::mutex resultsMutex;
    std::vector<AdminHelperSessionResult> results;
    std::vector<std::shared_ptr<SharedChannelState>> activeStates;

    AdminTransportLauncher makeLauncher(
        AdminHelperSessionConfig baseOpts = {.helperPid = 5000, .invokingUid = 1000}, int authDelayMs = 0,
        AdminSessionFailureReason simulatedFailure = AdminSessionFailureReason::None, std::string simulatedError = {}
    ) {
      return [this, baseOpts, authDelayMs, simulatedFailure,
              simulatedError](GCancellable* cancellable) -> AdminTransportLaunchResult {
        if (authDelayMs > 0) {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(authDelayMs);
          while (std::chrono::steady_clock::now() < deadline) {
            if (g_cancellable_is_cancelled(cancellable))
              return AdminTransportLaunchResult{
                  .failureReason = AdminSessionFailureReason::AuthCancelled,
                  .cancelled = true,
                  .error = "Administrator authentication cancelled.",
              };
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
        }
        if (g_cancellable_is_cancelled(cancellable)) {
          return AdminTransportLaunchResult{
              .failureReason = AdminSessionFailureReason::AuthCancelled,
              .cancelled = true,
              .error = "Administrator authentication cancelled.",
          };
        }
        if (simulatedFailure != AdminSessionFailureReason::None) {
          return AdminTransportLaunchResult{
              .failureReason = simulatedFailure,
              .cancelled = (simulatedFailure == AdminSessionFailureReason::AuthCancelled),
              .error = simulatedError.empty() ? "Simulated Polkit failure" : simulatedError,
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

} // namespace

class FileManagerTestAccess {
public:
  struct HeaderAndTabScene {
    StubRenderer renderer;
    std::unique_ptr<Flex> root;
    Flex* appBar = nullptr;
    Node* tabBarNode = nullptr;

    HeaderAndTabScene(FileManager& manager, float width, float height) {
      root = std::make_unique<Flex>();
      root->setDirection(FlexDirection::Vertical);
      root->setAlign(FlexAlign::Stretch);
      root->setGap(4.0F);
      appBar = static_cast<Flex*>(root->addChild(manager.buildHeaderAppBar()));
      tabBarNode = root->addChild(manager.m_tabBar.buildUi());
      manager.m_dispatcher.setSceneRoot(root.get());
      relayout(manager, width, height);
    }

    void relayout(FileManager& manager, float width, float height) {
      manager.updateAdminHeaderUi(static_cast<int>(width));
      (void)manager.m_tabBar.rebuildIfDirty();
      root->setSize(width, height);
      root->layout(renderer);
      manager.m_tabBar.syncLayout(renderer);
    }

    ~HeaderAndTabScene() = default;
  };

  static void runAll() {
    testProtocolStage2ValidationAndMalformedRequests();
    testPrivilegedEnumerationBatchesAndMetadata();
    testStreamingCancellationAndBackpressure();
    testEnteringAdministratorModeViaToolsAndFolderContextMenu();
    testTabAndNavigationIsolation();
    testHeaderIntegratedStatusTabStylingAndWindowTitle();
    testCapabilityRestrictionsAndDisabledThumbnailsAndDotDirectory();
    testDirectoryActivationInListAndGrid();
    testInactivityTimeoutAndMeaningfulActivityRenewal();
    testInactiveTabExpiresAndRecovers();
    testFinishRecoveryAndInaccessibleFallback();
  }

private:
  static void testProtocolStage2ValidationAndMalformedRequests() {
    // 1. Round-trip encoding/decoding of Stage 2 commands and server frames.
    AdminParseError err;
    const auto encRenew =
        encodeAdminClientRequest(AdminClientRequest{.command = AdminClientCommand::RenewActivity, .requestId = 10});
    TEST_CHECK(encRenew.has_value());
    AdminClientRequest decRenew;
    TEST_CHECK(decodeAdminClientRequest(*encRenew, decRenew, &err));
    TEST_CHECK(decRenew.command == AdminClientCommand::RenewActivity && decRenew.requestId == 10);

    const auto encList = encodeAdminClientRequest(
        AdminClientRequest{
            .command = AdminClientCommand::ListDir,
            .requestId = 11,
            .path = "/var/log",
            .maxBatchEntries = 32,
            .userInitiated = true,
        }
    );
    TEST_CHECK(encList.has_value());
    AdminClientRequest decList;
    TEST_CHECK(decodeAdminClientRequest(*encList, decList, &err));
    TEST_CHECK(
        decList.command == AdminClientCommand::ListDir
        && decList.requestId == 11
        && decList.userInitiated
        && decList.maxBatchEntries == 32
        && decList.path == "/var/log"
    );

    const auto encStat = encodeAdminClientRequest(
        AdminClientRequest{
            .command = AdminClientCommand::StatInfo,
            .requestId = 12,
            .path = "/etc/shadow",
            .userInitiated = false,
        }
    );
    TEST_CHECK(encStat.has_value());
    AdminClientRequest decStat;
    TEST_CHECK(decodeAdminClientRequest(*encStat, decStat, &err));
    TEST_CHECK(
        decStat.command == AdminClientCommand::StatInfo
        && decStat.requestId == 12
        && !decStat.userInitiated
        && decStat.path == "/etc/shadow"
    );

    // 2. Reject relative paths, parent traversal (".."), virtual trash:/ paths, and out-of-range batch sizes.
    for (const auto path : {"", "relative/path", "/etc/../shadow", "/etc/..", "trash:/"}) {
      TEST_CHECK(!encodeAdminClientRequest({.command = AdminClientCommand::StatInfo, .requestId = 1, .path = path}));
    }
    for (const auto name : {"", ".", "..", "sub/name", "valid_file.txt"}) {
      AdminServerFrame frame{.kind = AdminServerFrameKind::DirBatch, .requestId = 1, .entries = {{.name = name}}};
      TEST_CHECK(encodeAdminServerFrame(frame).has_value() == (std::string_view(name) == "valid_file.txt"));
    }
    for (const auto size : {0U, kAdminMaxDirBatchEntries + 1U}) {
      TEST_CHECK(!encodeAdminClientRequest(
          {.command = AdminClientCommand::ListDir, .requestId = 1, .path = "/etc", .maxBatchEntries = size}
      ));
    }

    // 3. Direct helper rejection of malformed paths and non-directory LIST_DIR.
    TempDir tmp("nythera-stage2-proto");
    const auto regularFile = tmp.path / "plain.txt";
    writeFile(regularFile, "abc");

    auto alive = std::make_shared<bool>(true);
    InProcessHelperFixture fixture;
    AdminSessionClient client(1, alive);
    client.setTransportLauncher(fixture.makeLauncher());

    bool started = false;
    client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
    pumpUntil([&] { return started; });

    // LIST_DIR on a regular file -> NotDirectory
    bool listDone = false;
    AdminStatusCode listCode = AdminStatusCode::Ok;
    client.listDirectoryAsync(
        regularFile, true, 16, [](const auto&) {},
        [&](bool ok, bool, std::uint64_t, AdminStatusCode code, const std::string&) {
          TEST_CHECK(!ok);
          listCode = code;
          listDone = true;
        }
    );
    pumpUntil([&] { return listDone; });
    TEST_CHECK(listCode == AdminStatusCode::NotDirectory);

    // LIST_DIR on a nonexistent directory -> NotFound
    listDone = false;
    client.listDirectoryAsync(
        tmp.path / "does_not_exist", true, 16, [](const auto&) {},
        [&](bool ok, bool, std::uint64_t, AdminStatusCode code, const std::string&) {
          TEST_CHECK(!ok);
          listCode = code;
          listDone = true;
        }
    );
    pumpUntil([&] { return listDone; });
    TEST_CHECK(listCode == AdminStatusCode::NotFound);

    client.stopSession(true);
    client.waitForSettled();
  }

  static void testPrivilegedEnumerationBatchesAndMetadata() {
    TempDir tmp("nythera-stage2-enum");
    const auto largeDir = tmp.path / "large_dir";
    std::filesystem::create_directories(largeDir);

    // Populate 140 files, 1 subdirectory (with 3 items inside), 1 valid symlink, and 1 broken symlink,
    // plus 1 filename with control characters to verify filename sanitisation.
    for (int i = 0; i < 140; ++i)
      writeFile(largeDir / ("file_" + std::to_string(i) + ".txt"), "payload-" + std::to_string(i));
    const auto subDir = largeDir / "nested_folder";
    std::filesystem::create_directories(subDir);
    writeFile(subDir / "a.txt", "1");
    writeFile(subDir / "b.txt", "2");
    writeFile(subDir / ".hidden_in_sub", "3");

    std::filesystem::create_symlink(largeDir / "file_0.txt", largeDir / "valid_link");
    std::filesystem::create_symlink(largeDir / "missing_target.txt", largeDir / "broken_link");
    writeFile(largeDir / "ctrl_\x07_name.txt", "ctrl");

    auto alive = std::make_shared<bool>(true);
    InProcessHelperFixture fixture;
    AdminSessionClient client(2, alive);
    client.setTransportLauncher(fixture.makeLauncher());

    bool started = false;
    client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
    pumpUntil([&] { return started; });

    // 1. Bounded batch enumeration: request maxBatchEntries = 20 -> must arrive in multiple batches of <= 20.
    std::size_t batchCalls = 0;
    std::size_t totalReceived = 0;
    bool dirWritable = false;
    bool enumDone = false;
    bool foundValidLink = false;
    bool foundBrokenLink = false;
    client.listDirectoryAsync(
        largeDir, true, 20,
        [&](const std::vector<AdminDirEntryRecord>& batch) {
          TEST_CHECK(!batch.empty() && batch.size() <= 20);
          ++batchCalls;
          totalReceived += batch.size();
          for (const auto& entry : batch) {
            if (entry.name == "valid_link") {
              foundValidLink = entry.isSymlink && !entry.brokenSymlink;
            } else if (entry.name == "broken_link") {
              foundBrokenLink = entry.isSymlink && entry.brokenSymlink;
            }
          }
        },
        [&](bool ok, bool writable, std::uint64_t total, AdminStatusCode code, const std::string&) {
          TEST_CHECK(ok && code == AdminStatusCode::Ok);
          TEST_CHECK(total == totalReceived);
          dirWritable = writable;
          enumDone = true;
        }
    );
    pumpUntil([&] { return enumDone; });
    TEST_CHECK(batchCalls >= 7);
    TEST_CHECK(totalReceived == 144);
    TEST_CHECK(dirWritable);
    TEST_CHECK(foundValidLink);
    TEST_CHECK(foundBrokenLink);

    // STAT_INFO (file identity and symlink target), STATVFS_INFO, and COUNT_DIR.
    bool statDone = false;
    AdminStatRecord statRec;
    client.statInfoAsync(
        largeDir / "valid_link", true, [&](bool ok, const AdminStatRecord& rec, AdminStatusCode, const std::string&) {
          TEST_CHECK(ok);
          statRec = rec;
          statDone = true;
        }
    );
    pumpUntil([&] { return statDone; });
    TEST_CHECK(statRec.exists && statRec.isSymlink && !statRec.brokenSymlink);
    TEST_CHECK(statRec.device != 0 && statRec.inode != 0);
    TEST_CHECK(statRec.symlinkTarget.ends_with("file_0.txt"));

    bool vfsDone = false;
    AdminStatVfsRecord vfsRec;
    client.statVfsAsync(largeDir, [&](bool ok, const AdminStatVfsRecord& rec, AdminStatusCode, const std::string&) {
      TEST_CHECK(ok);
      vfsRec = rec;
      vfsDone = true;
    });
    pumpUntil([&] { return vfsDone; });
    TEST_CHECK(vfsRec.totalBytes > 0 && vfsRec.freeBytes > 0);

    bool countDone = false;
    std::uint64_t subCount = 0;
    client.countDirectoryItemsAsync(subDir, [&](bool ok, std::uint64_t count, AdminStatusCode, const std::string&) {
      TEST_CHECK(ok);
      subCount = count;
      countDone = true;
    });
    pumpUntil([&] { return countDone; });
    TEST_CHECK(subCount == 3);

    client.stopSession(true);
    client.waitForSettled();
  }

  static void testStreamingCancellationAndBackpressure() {
    TempDir tmp("nythera-stage2-stream");
    for (int i = 0; i < 140; ++i)
      writeFile(tmp.path / (std::to_string(i) + std::string(232, 'x')), "data");
    std::filesystem::create_directory_symlink("/proc/self/fd/0", tmp.path / "magic_link");

    InProcessHelperFixture fixture;
    auto transport = fixture.makeLauncher({.invokingUid = 1000, .inactivityTimeoutMs = 1000})(nullptr);
    TEST_CHECK(transport.channel != nullptr);
    auto& channel = *transport.channel;
    TEST_CHECK(fcntl(channel.readFd, F_SETPIPE_SZ, 4096) >= 4096);
    AdminLineBuffer buffer;
    auto writeFrame = [&](std::string_view frame) {
      int error = 0;
      TEST_CHECK(writeAdminFrameFd(channel.writeFd, frame, error));
    };
    auto request = [&](const AdminClientRequest& msg) {
      const auto encoded = encodeAdminClientRequest(msg);
      TEST_CHECK(encoded);
      writeFrame(*encoded);
    };
    auto response = [&] {
      int error = 0;
      std::string line;
      AdminServerFrame frame;
      TEST_CHECK(readAdminFrameFd(channel.readFd, buffer, line, error) == AdminFrameReadStatus::FrameReady);
      TEST_CHECK(decodeAdminServerFrame(line, frame));
      return frame;
    };
    request({.command = AdminClientCommand::Hello, .requestId = 1, .nonce = "test"});
    TEST_CHECK(response().kind == AdminServerFrameKind::Ready);
    writeFrame("LIST_DIR 2 1 0 Li4vZXRj\n"); // Relative ../etc, sent without the client encoder.
    const auto invalidPath = response();
    TEST_CHECK(invalidPath.requestId == 2 && invalidPath.status == AdminStatusCode::BadRequest);
    writeFrame("LIST_DIR 3 65 0 Lw==\n");
    const auto invalidBatch = response();
    TEST_CHECK(invalidBatch.requestId == 3 && invalidBatch.status == AdminStatusCode::BadRequest);
    request({.command = AdminClientCommand::StatInfo, .requestId = 4, .path = (tmp.path / "magic_link").string()});
    const auto magic = response();
    TEST_CHECK(magic.kind == AdminServerFrameKind::StatResult && magic.statInfo.brokenSymlink);
    request({.command = AdminClientCommand::ListDir, .requestId = 5, .path = tmp.path.string(), .maxBatchEntries = 4});
    TEST_CHECK(response().kind == AdminServerFrameKind::DirBatch);
    request({.command = AdminClientCommand::Cancel, .requestId = 6, .targetRequestId = 5});
    bool acknowledged = false;
    bool cancelled = false;
    std::size_t received = 4;
    for (int i = 0; i < 40 && !cancelled; ++i) {
      const auto frame = response();
      if (frame.kind == AdminServerFrameKind::DirBatch) {
        TEST_CHECK(frame.requestId == 5 && frame.entries.size() <= 4);
        received += frame.entries.size();
      } else if (frame.requestId == 6) {
        TEST_CHECK(frame.status == AdminStatusCode::Ok);
        acknowledged = true;
      } else {
        TEST_CHECK(frame.requestId == 5 && frame.status == AdminStatusCode::Cancelled);
        cancelled = true;
      }
    }
    TEST_CHECK(acknowledged && cancelled && received < 141);

    // Simulate a stalled client with a live stdin and full response pipe.
    request({.command = AdminClientCommand::ListDir, .requestId = 7, .path = tmp.path.string()});
    pumpUntil([&] { return fixture.totalExited == 1; }, 2500);
    channel.waitForExit();
    TEST_CHECK(fixture.results.size() == 1);
    TEST_CHECK(fixture.results.front().reason == AdminHelperSessionExitReason::InactivityTimeout);
  }

  static void testEnteringAdministratorModeViaToolsAndFolderContextMenu() {
    TempDir tmp("nythera-stage2-enter");
    const auto dirA = tmp.path / "dirA";
    const auto dirB = dirA / "subFolder";
    const auto dirC = tmp.path / "dirC";
    std::filesystem::create_directories(dirB);
    std::filesystem::create_directories(dirC);
    writeFile(dirA / "alpha.txt", "alpha");
    writeFile(dirB / "secret_in_sub.txt", "secret");
    writeFile(dirB / "ctrl_\x07_file.txt", "sanitise");
    writeFile(dirC / "charlie.txt", "charlie");

    InProcessHelperFixture fixture;
    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });

    const TabId tab1Id = manager.activeTab().id;
    const TabId tab2Id = manager.openFolderInNewTab(dirC, false);
    TEST_CHECK(tab1Id != tab2Id);
    pumpUntil([&] { return !manager.m_tabs.findTab(tab2Id)->session.loading(); });

    // 1. Denied authentication via Tools -> Act as Administrator leaves tab unprivileged.
    manager.m_tabs.setAdminTransportLauncherForTesting(
        fixture.makeLauncher({}, 0, AdminSessionFailureReason::AuthDenied, "Polkit authentication denied.")
    );
    const auto toolsBefore = manager.toolsMenuEntries();
    const auto* actEntryBefore = findEntryById(toolsBefore, kActAsAdministrator);
    TEST_CHECK(actEntryBefore != nullptr && actEntryBefore->enabled);

    const auto beforeDenied = manager.activeSession().enumerationGeneration();
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kActAsAdministrator});
    pumpUntil([&] { return manager.activeTab().adminSessionState() == AdminSessionState::Failed; });
    TEST_CHECK(manager.activeSession().enumerationGeneration() == beforeDenied);
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.activeSession().path() == dirA);
    TEST_CHECK(!manager.m_windowTitle.ends_with("[Administrator]"));

    // 2. Cancelled authentication leaves tab unprivileged.
    manager.m_tabs.setAdminTransportLauncherForTesting(
        fixture.makeLauncher({}, 0, AdminSessionFailureReason::AuthCancelled, "Cancelled")
    );
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kActAsAdministrator});
    pumpUntil([&] { return manager.activeTab().adminSessionState() == AdminSessionState::Inactive; });
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());

    // 3. Successful authentication via Tools -> Act as Administrator retains current tab & directory,
    //    re-enumerates through the privileged helper, and affects only Tab 1 (Tab 2 remains unprivileged).
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kActAsAdministrator});
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    TEST_CHECK(manager.activeTab().id == tab1Id);
    TEST_CHECK(manager.activeSession().path() == dirA);
    TEST_CHECK(manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(!manager.m_tabs.findTab(tab2Id)->hasActiveAdminSession());

    // Tools -> Act as Administrator is now disabled while already active in this tab.
    const auto toolsActive = manager.toolsMenuEntries();
    const auto* actEntryActive = findEntryById(toolsActive, kActAsAdministrator);
    TEST_CHECK(actEntryActive != nullptr && !actEntryActive->enabled);

    // 4. Switch to Tab 2 (unprivileged) and activate Folder context menu -> Open as Administrator on dirB.
    TEST_CHECK(manager.activateTab(tab2Id));
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
    manager.m_menus.setContextMenuTarget(dirB);
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = kOpenFolderAsAdministrator});
    pumpUntil([&] {
      return manager.activeTab().hasActiveAdminSession()
          && !manager.activeSession().loading()
          && manager.activeSession().path() == dirB;
    });
    TEST_CHECK(manager.activeSession().entries().size() == 2);
    // Verify filename sanitisation is preserved on privileged enumeration entries.
    bool sawSanitized = false;
    for (const auto& entry : manager.activeSession().entries()) {
      const auto disp = sanitizeFilenameForDisplay(entry.name);
      if (entry.name.starts_with("ctrl_"))
        sawSanitized = (disp.find('\x07') == std::string::npos);
    }
    TEST_CHECK(sawSanitized);

    // 5. Creating, duplicating, or reopening tabs does not inherit Administrator privilege.
    const TabId dupId = manager.duplicateTab(tab2Id);
    pumpUntil([&] { return !manager.activeSession().loading(); });
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.closeTab(dupId));
    const TabId reopenedId = manager.reopenClosedTab();
    pumpUntil([&] { return !manager.activeSession().loading(); });
    TEST_CHECK(!manager.m_tabs.findTab(reopenedId)->hasActiveAdminSession());

    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }

  static void testTabAndNavigationIsolation() {
    TempDir tmp("nythera-stage2-isolation");
    const auto shared = tmp.path / "shared";
    const auto other = tmp.path / "other";
    writeFile(shared / "shared.txt", "shared");
    writeFile(other / "other.txt", "other");
    InProcessHelperFixture fixture;
    FileManager manager(shared.string());
    const auto firstId = manager.activeTab().id;
    const auto normalId = manager.openFolderInNewTab(shared, true);
    pumpUntil([&] { return !manager.activeSession().loading(); });

    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher({}, 75));
    TEST_CHECK(manager.activateTab(firstId));
    TEST_CHECK(manager.enterAdministratorMode(firstId));
    TEST_CHECK(manager.m_tabs.findTab(firstId)->adminSession.isAuthenticating());
    TEST_CHECK(manager.activateTab(normalId));
    TEST_CHECK(!manager.m_windowTitle.ends_with("[Administrator]"));
    pumpUntil([&] {
      const auto& origin = *manager.m_tabs.findTab(firstId);
      return origin.hasActiveAdminSession() && !origin.session.loading();
    });
    const auto firstHelper = manager.m_tabs.findTab(firstId)->adminSession.status().helperPid;
    TEST_CHECK(manager.activeTab().id == normalId && !manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.activeTab().capabilities().canCopyItems);
    const auto oldEntryCount = manager.m_tabs.findTab(firstId)->session.entries().size();

    writeFile(shared / "new_normal_entry", "normal");
    pumpUntil([&] { return manager.activeSession().entries().size() == oldEntryCount + 1; });
    TEST_CHECK(manager.m_tabs.findTab(firstId)->session.entries().size() == oldEntryCount);

    const auto closingAuthId = manager.openFolderInNewTab(shared, false);
    TEST_CHECK(manager.enterAdministratorMode(closingAuthId, other));
    TEST_CHECK(manager.closeTab(closingAuthId));
    TEST_CHECK(manager.activeTab().id == normalId);
    TEST_CHECK(manager.m_tabs.findTab(firstId)->hasActiveAdminSession());

    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher({}, 0));
    const auto secondId = manager.openFolderInNewTab(other, false);
    TEST_CHECK(manager.enterAdministratorMode(secondId));
    pumpUntil([&] {
      const auto& second = *manager.m_tabs.findTab(secondId);
      return second.hasActiveAdminSession() && !second.session.loading();
    });
    TEST_CHECK(fixture.activeHelpers == 2);
    TEST_CHECK(manager.m_tabs.findTab(secondId)->adminSession.status().helperPid != firstHelper);

    TEST_CHECK(manager.activateTab(secondId));
    manager.navigate(shared);
    manager.navigate(other);
    TEST_CHECK(manager.activeSession().loading());
    TEST_CHECK(manager.activateTab(normalId));
    TEST_CHECK(manager.activeSession().path() == shared && !manager.activeTab().hasActiveAdminSession());
    pumpUntil([&] { return !manager.m_tabs.findTab(secondId)->session.loading(); });
    TEST_CHECK(manager.activateTab(secondId));
    TEST_CHECK(manager.activeSession().entries().size() == 1);
    TEST_CHECK(manager.activeSession().entries().front().name == "other.txt");

    manager.navigate(shared);
    TEST_CHECK(manager.activeSession().loading());
    TEST_CHECK(manager.closeTab(secondId));
    TEST_CHECK(manager.activateTab(normalId));
    pumpUntil([&] { return fixture.activeHelpers == 1; });
    TEST_CHECK(manager.m_tabs.findTab(firstId)->adminSession.status().helperPid == firstHelper);
    TEST_CHECK(manager.activeSession().path() == shared && manager.activeTab().capabilities().canCopyItems);
    TEST_CHECK(manager.activeSession().entries().size() == oldEntryCount + 1);

    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }

  static void testHeaderIntegratedStatusTabStylingAndWindowTitle() {
    TempDir tmp("nythera-stage2-ui");
    const auto dirA = tmp.path / "alpha";
    const auto dirB = tmp.path / "beta";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);

    InProcessHelperFixture fixture;
    FileManager manager(dirA.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());

    const TabId tabA = manager.activeTab().id;
    const TabId tabB = manager.openFolderInNewTab(dirB, false);
    pumpUntil([&] { return !manager.m_tabs.findTab(tabB)->session.loading(); });

    HeaderAndTabScene scene(manager, 1120.0F, 760.0F);

    // Initially unprivileged: header status hidden, branding visible, normal tab label color, normal title.
    TEST_CHECK(manager.m_adminHeaderContainer != nullptr && !manager.m_adminHeaderContainer->visible());
    TEST_CHECK(manager.m_appBrandContainer != nullptr && manager.m_appBrandContainer->visible());
    TEST_CHECK(manager.m_windowTitle == "alpha \xE2\x80\x94 Nythera Files");

    // Enter Administrator Mode on Tab A.
    TEST_CHECK(manager.enterAdministratorMode(tabA));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    scene.relayout(manager, 1120.0F, 760.0F);

    // Header status is now visible beside the NYTHERA FILES branding with shield, "Administrator Mode", and "Finish".
    TEST_CHECK(manager.m_adminHeaderContainer->visible());
    TEST_CHECK(manager.m_appBrandContainer->visible());
    TEST_CHECK(manager.m_adminHeaderSeparator->visible());
    TEST_CHECK(manager.m_adminHeaderShield != nullptr && manager.m_adminHeaderShield->visible());
    TEST_CHECK(manager.m_adminHeaderLabel != nullptr && manager.m_adminHeaderLabel->text() == "Administrator Mode");
    TEST_CHECK(manager.m_adminHeaderLabel->color() == colorForRole(kAdminWarningColorRole));
    TEST_CHECK(
        manager.m_adminFinishButton != nullptr
        && manager.m_adminFinishButton->label() != nullptr
        && manager.m_adminFinishButton->label()->text() == "Finish"
    );
    TEST_CHECK(manager.m_windowTitle == "alpha \xE2\x80\x94 Nythera Files [Administrator]");

    // Narrow window width (< 760): branding and vertical separator collapse so Administrator status + Finish remain
    // accessible.
    scene.relayout(manager, 560.0F, 760.0F);
    TEST_CHECK(manager.m_adminHeaderContainer->visible());
    TEST_CHECK(!manager.m_appBrandContainer->visible());
    TEST_CHECK(!manager.m_adminHeaderSeparator->visible());
    TEST_CHECK(manager.m_adminFinishButton->visible());
    float finishX = 0.0F;
    float finishY = 0.0F;
    Node::absolutePosition(manager.m_adminFinishButton, finishX, finishY);
    TEST_CHECK(finishX >= 0.0F && manager.m_adminFinishButton->width() > 0.0F);
    TEST_CHECK(finishX + manager.m_adminFinishButton->width() <= 560.0F);

    // Restore wide width -> branding returns.
    scene.relayout(manager, 1120.0F, 760.0F);
    TEST_CHECK(manager.m_appBrandContainer->visible());
    TEST_CHECK(manager.m_adminHeaderSeparator->visible());

    // Tab appearance: Tab A has warning-coloured directory label while active, and retains it when inactive!
    TEST_CHECK(manager.m_tabBar.m_slots.size() == 2);
    TEST_CHECK(manager.m_tabBar.m_slots[0].tabId == tabA && manager.m_tabBar.m_slots[0].administrator);
    TEST_CHECK(!manager.m_tabBar.m_slots[1].administrator);

    // Switch to unprivileged Tab B:
    // - Header administrator status hides immediately.
    // - Window title restores normal title without [Administrator].
    // - Inactive administrator Tab A remains open and still carries administrator styling.
    TEST_CHECK(manager.activateTab(tabB));
    scene.relayout(manager, 1120.0F, 760.0F);
    TEST_CHECK(!manager.m_adminHeaderContainer->visible());
    TEST_CHECK(manager.m_windowTitle == "beta \xE2\x80\x94 Nythera Files");
    TEST_CHECK(manager.m_tabBar.m_slots[0].administrator);
    TEST_CHECK(manager.m_tabBar.m_slots[0].label->color() == colorForRole(kAdminWarningColorRole));
    TEST_CHECK(manager.m_tabs.findTab(tabA)->hasActiveAdminSession());

    // Switch back to Tab A and click Finish in the header -> exits Administrator Mode and restores normal appearance.
    TEST_CHECK(manager.activateTab(tabA));
    scene.relayout(manager, 1120.0F, 760.0F);
    TEST_CHECK(manager.m_adminHeaderContainer->visible());
    TEST_CHECK(manager.m_windowTitle == "alpha \xE2\x80\x94 Nythera Files [Administrator]");

    dispatchClick(manager.m_dispatcher, manager.m_adminFinishButton);
    pumpUntil([&] { return !manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    scene.relayout(manager, 1120.0F, 760.0F);

    TEST_CHECK(!manager.m_adminHeaderContainer->visible());
    TEST_CHECK(manager.m_windowTitle == "alpha \xE2\x80\x94 Nythera Files");
    TEST_CHECK(!manager.m_tabBar.m_slots[0].administrator);

    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }

  static void testCapabilityRestrictionsAndDisabledThumbnailsAndDotDirectory() {
    TempDir tmp("nythera-stage2-caps");
    const auto dir = tmp.path / "restricted_work";
    const auto subFolder = dir / "child_dir";
    const auto regFile = dir / "notes.txt";
    std::filesystem::create_directories(subFolder);
    writeFile(regFile, "important");
    // Place a custom .directory file in subFolder that would normally override its icon to "custom-secret-icon".
    writeFile(subFolder / ".directory", "[Desktop Entry]\nIcon=custom-secret-icon\n");

    InProcessHelperFixture fixture;
    FileManager manager(dir.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());

    TEST_CHECK(manager.enterAdministratorMode(manager.activeTab().id));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });

    auto& tab = manager.activeTab();
    const auto caps = tab.capabilities();
    TEST_CHECK(!caps.canCreate);
    TEST_CHECK(caps.canCreateBasic);
    TEST_CHECK(caps.canRename);
    TEST_CHECK(!caps.canMutate);
    TEST_CHECK(!caps.canTrash);
    TEST_CHECK(!caps.canLaunchApps);
    TEST_CHECK(!caps.canRunExternalTools);
    TEST_CHECK(!caps.canGenerateThumbnails);
    TEST_CHECK(!caps.canChecksum);
    TEST_CHECK(!caps.canPinToPlaces);
    TEST_CHECK(!caps.canUndo);
    TEST_CHECK(!caps.canDragSource);
    TEST_CHECK(!caps.canAcceptDrop);
    TEST_CHECK(!caps.canOpenProperties);
    TEST_CHECK(!caps.canCopyItems);

    // Select regFile and check menu entries.
    tab.selectedPath = regFile;
    tab.selectedPaths = {regFile.string()};

    const auto fileMenu = manager.fileMenuEntries();
    for (int id : {110, 111, 112}) {
      const auto* entry = findEntryById(fileMenu, id);
      TEST_CHECK(entry != nullptr && entry->enabled);
    }
    for (int id : {113, static_cast<int>(kManualSymlink), 30002, 30003, 30005, 30006}) {
      const auto* entry = findEntryById(fileMenu, id);
      TEST_CHECK(entry != nullptr && !entry->enabled);
    }

    TEST_CHECK(findEntryById(fileMenu, kSelectionSymlink) == nullptr);
    for (int id : {114, 115, 116}) {
      if (const auto* entry = findEntryById(fileMenu, id))
        TEST_CHECK(!entry->enabled);
    }
    const auto editMenu = manager.editMenuEntries();
    {
      const auto* renameEntry = findEntryById(editMenu, 4);
      TEST_CHECK(renameEntry != nullptr && renameEntry->enabled);
    }
    TEST_CHECK(findEntryById(editMenu, 3)->enabled);
    for (int id : {1, 2, 5, 6, static_cast<int>(kDuplicate), static_cast<int>(kUndo), static_cast<int>(kRedo)}) {
      const auto* entry = findEntryById(editMenu, id);
      TEST_CHECK(entry != nullptr && !entry->enabled);
    }

    const auto toolsMenu = manager.toolsMenuEntries();
    for (int id : {static_cast<int>(kCalculateChecksum), static_cast<int>(kVerifyChecksum)}) {
      const auto* entry = findEntryById(toolsMenu, id);
      TEST_CHECK(entry != nullptr && !entry->enabled);
    }
    TEST_CHECK(!manager.checksumTarget().has_value());

    // Unsupported mutations, clipboard exports, properties and app launching remain blocked.
    // Paste with no clipboard offer must leave the directory unchanged.
    const std::size_t opsBefore = manager.m_operations.operations().size();
    manager.duplicateSelection();
    manager.createSelectionSymlink();
    manager.beginRename();
    manager.copyToDesktopClipboard(false);
    manager.copyToDesktopClipboard(true);
    manager.pasteClipboard();
    manager.openProperties(regFile);
    TEST_CHECK(!manager.m_propertiesWindow || !manager.m_propertiesWindow->isOpen());
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 113}); // Text File (unsupported in Stage 3)
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 5});   // Move to Wastebin
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 6});   // Delete Permanently
    manager.activateContextMenuEntry(ContextMenuControlEntry{.id = 59});  // Copy Location
    TEST_CHECK(manager.m_statusMessage == "Copying locations is unavailable in Administrator Mode");
    manager.navigate("trash:/");
    TEST_CHECK(manager.activeSession().path() == dir);
    for (const auto& shortcut : {
             KeyboardEvent{.sym = XKB_KEY_c, .modifiers = KeyMod::Ctrl, .pressed = true},
             KeyboardEvent{.sym = XKB_KEY_v, .modifiers = KeyMod::Ctrl, .pressed = true},
             KeyboardEvent{.sym = XKB_KEY_F2, .pressed = true},
             KeyboardEvent{.sym = XKB_KEY_Delete, .pressed = true},
         })
      manager.handleWindowKeyEvent(shortcut);
    TEST_CHECK(manager.m_operations.operations().size() == opsBefore);
    TEST_CHECK(manager.m_transfers.pendingClipboardReads() == 0);
    TEST_CHECK(!manager.m_renamePath);

    // Drag-and-drop source and target must both be blocked.
    manager.m_transfers.beginFileDragPress(0, 10.0F, 10.0F, 1);
    TEST_CHECK(!manager.m_transfers.isDragPending() && !manager.m_transfers.isDraggingFiles());
    TEST_CHECK(!manager.dropDestination(10.0F, 10.0F, true).has_value());
    TEST_CHECK(!manager.dropDestination(10.0F, 10.0F, false).has_value());

    // Activating a regular file must NOT launch an external application, whereas activating a folder MUST navigate into
    // it.
    manager.activatePath(regFile);
    TEST_CHECK(manager.activeSession().path() == dir);
    TEST_CHECK(manager.m_statusMessage.find("disabled in Administrator Mode") != std::string::npos);

    // Verify FileIcon with setAllowFilesystemQueries(false) does NOT load custom .directory icon.
    FileIcon restrictedIcon;
    restrictedIcon.setAllowFilesystemQueries(false);
    restrictedIcon.setFile(subFolder, true);
    drainMainContext(5);
    TEST_CHECK(!restrictedIcon.allowFilesystemQueries());

    // Activating subFolder navigates into it via the privileged helper.
    manager.activatePath(subFolder);
    pumpUntil([&] { return !manager.activeSession().loading() && manager.activeSession().path() == subFolder; });
    TEST_CHECK(manager.activeSession().path() == subFolder);

    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }

  static void testDirectoryActivationInListAndGrid() {
    struct ActivationAdapter final : VirtualGridAdapter {
      FileManager& manager;
      explicit ActivationAdapter(FileManager& owner) : manager(owner) {}
      std::size_t itemCount() const override { return manager.activeSession().entries().size(); }
      std::unique_ptr<Node> createTile() override { return std::make_unique<Box>(); }
      void bindTile(Node&, std::size_t, bool, bool) override {}
      void onActivate(std::size_t index) override { manager.activatePointerItem(index); }
    };

    TempDir tmp("nythera-stage2-activation");
    const auto folder = tmp.path / "folder";
    std::filesystem::create_directory(folder);
    writeFile(tmp.path / "ordinary.txt", "opening this file must be blocked in Administrator Mode");
    for (const bool gridMode : {false, true}) {
      InProcessHelperFixture fixture;
      FileManager manager(tmp.path.string());
      pumpUntil([&] { return !manager.activeSession().loading(); });
      manager.setGridMode(gridMode);

      StubRenderer renderer;
      ActivationAdapter adapter(manager);
      VirtualGridView grid;
      grid.setAdapter(&adapter);
      grid.setColumns(gridMode ? 0 : 1);
      grid.setSquareCells(false);
      grid.setMinCellWidth(190.0F);
      grid.setCellHeight(gridMode ? 100.0F : 32.0F);
      grid.setSize(480.0F, 320.0F);
      manager.m_grid = &grid;
      manager.m_dispatcher.setSceneRoot(&grid);
      // Use the real Files pointer routing with a disconnected surface.
      manager.m_surface = std::make_unique<ToplevelSurface>(manager.m_wayland);

      std::uint32_t time = 1000;
      const auto clickItem = [&](std::size_t index) {
        float x = 0.0F;
        float y = 0.0F;
        TEST_CHECK(grid.absoluteAnchorForIndex(index, x, y));
        TEST_CHECK(grid.indexAtSurface(x, y) == index);
        for (const bool pressed : {true, false}) {
          manager.dispatchPointer({
              .type = PointerEvent::Type::Button,
              .surface = manager.m_surface->wlSurface(),
              .sx = x,
              .sy = y,
              .time = time,
              .button = BTN_LEFT,
              .pressed = pressed,
          });
          time += 30;
        }
      };
      const auto showParent = [&] {
        manager.navigate(tmp.path);
        pumpUntil([&] { return !manager.activeSession().loading(); });
        grid.layout(renderer);
        TEST_CHECK(grid.layoutColumnCount() == (gridMode ? 2 : 1));
        TEST_CHECK(manager.activeSession().entries().size() == 2);
        TEST_CHECK(manager.activeSession().entries()[0].absPath == folder);
      };
      const auto pressEnter = [&](std::uint32_t key) { manager.handleKeyboard({.sym = key, .pressed = true}); };
      const auto verifyFolderOpened = [&](std::source_location caller = std::source_location::current()) {
        if (manager.activeSession().path() != folder)
          nythera::test::failCheck("the folder opened", caller);
        pumpUntil([&] { return !manager.activeSession().loading(); });
      };
      showParent();
      clickItem(0);
      TEST_CHECK(manager.activeSession().path() == tmp.path);
      clickItem(0);
      verifyFolderOpened();
      showParent();
      clickItem(0);
      pressEnter(XKB_KEY_Return);
      verifyFolderOpened();

      showParent();
      manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());
      TEST_CHECK(manager.enterAdministratorMode(manager.activeTab().id));
      pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
      grid.layout(renderer);
      clickItem(0);
      TEST_CHECK(manager.activeSession().path() == tmp.path);
      TEST_CHECK(!manager.m_transfers.isDragPending() && !manager.m_transfers.isDraggingFiles());
      manager.m_keyboardModifiers = KeyMod::Ctrl;
      clickItem(0);
      TEST_CHECK(manager.activeSession().path() == tmp.path);
      manager.m_keyboardModifiers = 0;
      clickItem(0);
      TEST_CHECK(manager.activeSession().path() == tmp.path);
      clickItem(0);
      verifyFolderOpened();
      showParent();
      clickItem(0);
      pressEnter(gridMode ? XKB_KEY_KP_Enter : XKB_KEY_Return);
      verifyFolderOpened();

      showParent();
      clickItem(1);
      clickItem(1);
      TEST_CHECK(manager.m_statusMessage.find("disabled in Administrator Mode") != std::string::npos);
      manager.setStatus({});
      pressEnter(XKB_KEY_Return);
      TEST_CHECK(manager.m_statusMessage.find("disabled in Administrator Mode") != std::string::npos);
      TEST_CHECK(manager.activeSession().path() == tmp.path);

      manager.m_dispatcher.setSceneRoot(nullptr);
      manager.m_grid = nullptr;
    }
  }

  static void testInactivityTimeoutAndMeaningfulActivityRenewal() {
    // 1. Validate FileManagerSettings finite bounds [60, 3600] and default 600s (no unlimited option).
    ConfigService cfg;
    FileManagerSettings settings(cfg);
    TEST_CHECK(settings.adminInactivityTimeoutSeconds() == 600);
    TEST_CHECK(!settings.setAdminInactivityTimeoutSeconds(0));
    TEST_CHECK(!settings.setAdminInactivityTimeoutSeconds(-1));
    TEST_CHECK(!settings.setAdminInactivityTimeoutSeconds(10));
    TEST_CHECK(!settings.setAdminInactivityTimeoutSeconds(86400));
    TEST_CHECK(settings.setAdminInactivityTimeoutSeconds(300));
    TEST_CHECK(settings.adminInactivityTimeoutSeconds() == 300);

    // 2. Helper-enforced inactivity expiry when only background PING / STATVFS_INFO / COUNT_DIR traffic occurs,
    //    vs. renewal when meaningful user activity (RENEW_ACTIVITY) occurs.
    TempDir tmp("nythera-stage2-timeout");
    writeFile(tmp.path / "item.txt", "hello");

    auto alive = std::make_shared<bool>(true);
    InProcessHelperFixture fixture;
    AdminSessionClient client(99, alive);
    client.setTransportLauncher(
        fixture.makeLauncher({.helperPid = 9100, .invokingUid = 1000, .inactivityTimeoutMs = 90})
    );
    client.setInactivityTimeoutMs(90, true);

    bool started = false;
    client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
    pumpUntil([&] { return started; });
    TEST_CHECK(client.isActive());

    // Meaningful user activity at +45ms and +90ms keeps the 90ms session alive past 120ms.
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    bool renewOk = false;
    client.renewActivityAsync(true, [&](bool ok, AdminStatusCode, const std::string&) { renewOk = ok; });
    pumpUntil([&] { return renewOk; });
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    renewOk = false;
    client.renewActivityAsync(true, [&](bool ok, AdminStatusCode, const std::string&) { renewOk = ok; });
    pumpUntil([&] { return renewOk; });
    TEST_CHECK(client.isActive());

    // Now send only background PING and STATVFS_INFO every 25ms — these must NOT renew the inactivity timer,
    // so the helper must still expire within ~150ms.
    const auto bgStart = std::chrono::steady_clock::now();
    while (client.state() == AdminSessionState::Active
           && std::chrono::steady_clock::now() - bgStart < std::chrono::milliseconds(400)) {
      (void)client.pingAsync([](const AdminHealthStatus&) {});
      (void)client.statVfsAsync(tmp.path, [](bool, const AdminStatVfsRecord&, AdminStatusCode, const std::string&) {});
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
    }
    TEST_CHECK(client.state() == AdminSessionState::Failed);
    client.waitForSettled();
    TEST_CHECK(
        !fixture.results.empty() && fixture.results.back().reason == AdminHelperSessionExitReason::InactivityTimeout
    );
  }

  static void testInactiveTabExpiresAndRecovers() {
    TempDir tmp("nythera-stage2-inactive");
    writeFile(tmp.path / "ordinary.txt", "content");
    InProcessHelperFixture fixture;
    FileManager manager(tmp.path.string());
    const auto administratorId = manager.activeTab().id;
    const auto normalId = manager.openFolderInNewTab(tmp.path, false);
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher({.inactivityTimeoutMs = 250}));
    TEST_CHECK(manager.enterAdministratorMode(administratorId));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    TEST_CHECK(manager.activateTab(normalId));
    auto& origin = *manager.m_tabs.findTab(administratorId);
    TEST_CHECK(origin.hasActiveAdminSession());
    pumpUntil([&] { return !origin.hasActiveAdminSession() && !origin.session.loading(); });
    TEST_CHECK(manager.activeTab().id == normalId && manager.activeTab().capabilities().canCopyItems);
    TEST_CHECK(origin.session.path() == tmp.path && origin.capabilities().canCopyItems);
    TEST_CHECK(origin.session.entries().size() == 1);
    TEST_CHECK(origin.adminSession.status().helperPid == 0);

    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }

  static void testFinishRecoveryAndInaccessibleFallback() {
    TempDir tmp("nythera-stage2-recovery");
    const auto accessibleDir = tmp.path / "public_dir";
    const auto restrictedDir = tmp.path / "root_vault";
    std::filesystem::create_directories(accessibleDir);
    std::filesystem::create_directories(restrictedDir);
    writeFile(accessibleDir / "pub.txt", "public");
    writeFile(restrictedDir / "top_secret.txt", "classified");

    InProcessHelperFixture fixture;
    FileManager manager(accessibleDir.string());
    pumpUntil([&] { return !manager.activeSession().loading(); });
    manager.m_tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher());

    // Simulate restrictedDir being inaccessible to the unprivileged backend while accessible to the privileged helper.
    manager.m_unprivilegedDirectoryAccessibleHook = [restrictedDir](const std::filesystem::path& candidate) {
      if (normalizeTabPath(candidate) == normalizeTabPath(restrictedDir))
        return std::optional<bool>(false);
      return std::optional<bool>(std::nullopt);
    };

    const TabId tabId = manager.activeTab().id;
    TEST_CHECK(manager.enterAdministratorMode(tabId, restrictedDir));
    pumpUntil([&] {
      return manager.activeTab().hasActiveAdminSession()
          && !manager.activeSession().loading()
          && manager.activeSession().path() == restrictedDir;
    });
    TEST_CHECK(manager.activeSession().entries().size() == 1);
    TEST_CHECK(manager.activeSession().entries().front().name == "top_secret.txt");

    // 1. Clicking Finish while inside an unprivileged-inaccessible directory must:
    //    - terminate the helper session
    //    - discard privileged entries
    //    - fall back to the default opening location (accessible through the normal backend).
    manager.finishAdministratorMode(tabId, "Exited Administrator Mode.");
    TEST_CHECK(manager.activeSession().entries().empty());
    TEST_CHECK(manager.activeTab().selectedPaths.empty() && !manager.activeTab().freeSpaceBytes());
    TEST_CHECK(manager.activeTab().adminSession.status().helperPid == 0);
    pumpUntil([&] { return !manager.activeSession().loading(); });
    TEST_CHECK(!manager.activeTab().hasActiveAdminSession());
    TEST_CHECK(manager.activeSession().path() != restrictedDir);
    TEST_CHECK(manager.activeSession().path() == normalizeTabPath(manager.defaultTabPath()));

    // 2. Navigate back to accessibleDir, enter Administrator Mode, and simulate unexpected helper termination:
    //    tab must recover automatically and retain accessibleDir through the normal unprivileged backend.
    manager.navigate(accessibleDir, true);
    pumpUntil([&] { return !manager.activeSession().loading(); });
    TEST_CHECK(manager.enterAdministratorMode(tabId));
    pumpUntil([&] { return manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });

    fixture.killAllActiveHelpersUnexpectedly();
    pumpUntil([&] { return !manager.activeTab().hasActiveAdminSession() && !manager.activeSession().loading(); });
    TEST_CHECK(manager.activeSession().path() == accessibleDir);
    TEST_CHECK(manager.activeSession().entries().size() == 1);
    TEST_CHECK(manager.activeSession().entries().front().name == "pub.txt");

    manager.m_tabs.shutdownAll();
    for (const auto& t : manager.m_tabs.tabs())
      t->adminSession.waitForSettled();
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-stage2-env"});
    FileManagerTestAccess::runAll();
  });
}
