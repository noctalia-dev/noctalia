#include "filemanager/admin_helper_session.h"
#include "filemanager/admin_protocol.h"
#include "filemanager/admin_session_client.h"
#include "filemanager/browser_tab.h"
#include "filemanager/folder_size.h"
#include "filemanager/polkit_helper_launch.h"
#include "filemanager/tab_manager.h"
#include "test_isolation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glib.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace filemanager;

namespace {

  struct TempDir {
    nythera::test::ScopedTempDir inner;
    std::filesystem::path path = inner.path();
    explicit TempDir(const char* prefix) : inner(prefix) {}
  };

  template <typename Predicate> void pumpUntil(Predicate&& predicate, int timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      if (!g_main_context_iteration(nullptr, FALSE))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    TEST_CHECK(predicate());
  }

  void drainMainContext(int iterations = 8) {
    for (int i = 0; i < iterations; ++i) {
      while (g_main_context_iteration(nullptr, FALSE)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  // In-process unprivileged fixture running the real `runAdminHelperSession` loop over a
  // private pipe pair. Tracks live helper instances so tests can assert zero leaked helpers.
  struct InProcessHelperFixture {
    std::atomic<int> activeHelpers{0};
    std::atomic<int> totalSpawned{0};
    std::atomic<int> totalExited{0};
    std::mutex resultsMutex;
    std::vector<AdminHelperSessionResult> results;

    AdminTransportLauncher
    makeLauncher(AdminHelperSessionConfig config = {.helperPid = 4242, .invokingUid = 1000}, int preStartDelayMs = 0) {
      return [this, config, preStartDelayMs](GCancellable* cancellable) -> AdminTransportLaunchResult {
        AdminTransportLaunchResult out;
        if (g_cancellable_is_cancelled(cancellable) != FALSE) {
          out.cancelled = true;
          out.failureReason = AdminSessionFailureReason::AuthCancelled;
          return out;
        }

        int clientToHelper[2]{-1, -1};
        int helperToClient[2]{-1, -1};
        if (::pipe2(clientToHelper, O_CLOEXEC) != 0) {
          out.failureReason = AdminSessionFailureReason::TransportError;
          out.error = "pipe2 failed";
          return out;
        }
        if (::pipe2(helperToClient, O_CLOEXEC) != 0) {
          ::close(clientToHelper[0]);
          ::close(clientToHelper[1]);
          out.failureReason = AdminSessionFailureReason::TransportError;
          out.error = "pipe2 failed";
          return out;
        }

        struct PipeSessionState {
          InProcessHelperFixture* owner = nullptr;
          int clientWriteFd = -1;
          int clientReadFd = -1;
          int helperInFd = -1;
          int helperOutFd = -1;
          std::mutex mutex;
          std::atomic<bool> helperRunning{true};
          std::atomic<bool> aborted{false};
          std::thread helperThread;
          int exitCode = 0;

          ~PipeSessionState() {
            closeClientWrite();
            if (helperThread.joinable())
              helperThread.join();
            std::lock_guard lock(mutex);
            if (clientReadFd >= 0)
              ::close(clientReadFd);
            if (helperInFd >= 0)
              ::close(helperInFd);
            if (helperOutFd >= 0)
              ::close(helperOutFd);
          }

          void closeClientWrite() {
            std::lock_guard lock(mutex);
            if (clientWriteFd >= 0) {
              ::close(clientWriteFd);
              clientWriteFd = -1;
            }
          }

          void abortNow() {
            aborted.store(true, std::memory_order_release);
            closeClientWrite();
          }

          int waitExit() {
            closeClientWrite();
            if (helperThread.joinable())
              helperThread.join();
            return exitCode;
          }
        };

        auto state = std::make_shared<PipeSessionState>();
        state->owner = this;
        state->helperInFd = clientToHelper[0];
        state->clientWriteFd = clientToHelper[1];
        state->clientReadFd = helperToClient[0];
        state->helperOutFd = helperToClient[1];

        ++activeHelpers;
        const int instanceNum = ++totalSpawned;
        AdminHelperSessionConfig instanceConfig = config;
        if (instanceConfig.helperPid == 4242)
          instanceConfig.helperPid = static_cast<std::uint32_t>(4200 + instanceNum);

        state->helperThread = std::thread([this, state, instanceConfig, preStartDelayMs]() {
          if (preStartDelayMs > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(preStartDelayMs);
            while (std::chrono::steady_clock::now() < deadline && !state->aborted.load(std::memory_order_acquire))
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }

          AdminHelperSessionResult sessionRes;
          if (state->aborted.load(std::memory_order_acquire)) {
            state->exitCode = 126;
            sessionRes.reason = AdminHelperSessionExitReason::ClientEof;
          } else {
            sessionRes = runAdminHelperSession(state->helperInFd, state->helperOutFd, instanceConfig);
            state->exitCode = (sessionRes.handshakeCompleted
                               && (sessionRes.reason == AdminHelperSessionExitReason::CleanShutdown
                                   || sessionRes.reason == AdminHelperSessionExitReason::ClientEof))
                ? 0
                : 1;
          }

          {
            std::lock_guard lock(state->mutex);
            if (state->helperOutFd >= 0) {
              ::close(state->helperOutFd);
              state->helperOutFd = -1;
            }
            if (state->helperInFd >= 0) {
              ::close(state->helperInFd);
              state->helperInFd = -1;
            }
          }
          {
            std::lock_guard lock(resultsMutex);
            results.push_back(std::move(sessionRes));
          }
          state->helperRunning.store(false, std::memory_order_release);
          --activeHelpers;
          ++totalExited;
        });

        auto channel = std::make_unique<AdminTransportChannel>();
        channel->readFd = state->clientReadFd;
        channel->writeFd = state->clientWriteFd;
        channel->closeWrite = [state] { state->closeClientWrite(); };
        channel->abortTransport = [state] { state->abortNow(); };
        channel->waitForExit = [state] { return state->waitExit(); };
        out.channel = std::move(channel);
        return out;
      };
    }
  };

  void testSharedPolkitValidationAndInvocationChecks() {
    TempDir tmp("nythera-admin-polkit-test");
    const auto userFile = tmp.path / "fake-helper";
    {
      std::ofstream out(userFile);
      out << "#!/bin/sh\n";
    }
    TEST_CHECK(::chmod(userFile.c_str(), 0755) == 0);

    // User-owned files, relative paths, missing paths, and symlinks must be rejected by verifyInstalledPolkitFile.
    TEST_CHECK(!verifyInstalledPolkitFile({}));
    TEST_CHECK(!verifyInstalledPolkitFile("relative/path"));
    TEST_CHECK(!verifyInstalledPolkitFile(tmp.path / "nonexistent"));
    TEST_CHECK(!verifyInstalledPolkitFile(userFile));

    // Root-owned system binaries like /bin/sh or /usr/bin/true (if root-owned non-symlink) vs symlink:
    const auto symlinkPath = tmp.path / "symlink-helper";
    std::filesystem::create_symlink("/bin/sh", symlinkPath);
    TEST_CHECK(!verifyInstalledPolkitFile(symlinkPath));

    PolkitHelperTarget fakeTarget{
        .helperPath = userFile.c_str(),
        .policyPath = userFile.c_str(),
        .actionId = "dev.nythera.Files.admin-session",
        .displayName = "Administrator",
    };
    std::string error;
    TEST_CHECK(!verifyInstalledPolkitTarget(fakeTarget, error));
    TEST_CHECK(error.find("root-owned Administrator helper and Polkit policy must be installed") != std::string::npos);

    // Verify pkexec exit code classification.
    const auto dismissed = classifyPolkitExitStatus(126, false);
    TEST_CHECK(dismissed.cancelled && !dismissed.denied);
    const auto explicitCancel = classifyPolkitExitStatus(1, true);
    TEST_CHECK(explicitCancel.cancelled && !explicitCancel.denied);
    const auto denied = classifyPolkitExitStatus(127, false);
    TEST_CHECK(!denied.cancelled && denied.denied);
    const auto crash = classifyPolkitExitStatus(1, false);
    TEST_CHECK(!crash.cancelled && !crash.denied);

    // Verify helper invocation validator rejects non-root, missing/zero/invalid PKEXEC_UID, and extra argv.
    std::uint32_t invokingUid = 0;
    TEST_CHECK(!validateAdminHelperInvocation(1, 1000, "1000", invokingUid, error));
    TEST_CHECK(!validateAdminHelperInvocation(1, 0, nullptr, invokingUid, error));
    TEST_CHECK(!validateAdminHelperInvocation(1, 0, "", invokingUid, error));
    TEST_CHECK(!validateAdminHelperInvocation(1, 0, "0", invokingUid, error));
    TEST_CHECK(!validateAdminHelperInvocation(1, 0, "-1", invokingUid, error));
    TEST_CHECK(!validateAdminHelperInvocation(1, 0, "+1000", invokingUid, error));
    TEST_CHECK(!validateAdminHelperInvocation(1, 0, "1000;id", invokingUid, error));
    TEST_CHECK(!validateAdminHelperInvocation(2, 0, "1000", invokingUid, error));
    TEST_CHECK(validateAdminHelperInvocation(1, 0, "1000", invokingUid, error));
    TEST_CHECK(invokingUid == 1000U);

    // Verify default production launcher does not bypass trusted installation checks in development builds.
    const auto prodTarget = defaultAdminPolkitTarget();
    if (!verifyInstalledPolkitFile(prodTarget.helperPath) || !verifyInstalledPolkitFile(prodTarget.policyPath)) {
      GCancellable* cancellable = g_cancellable_new();
      const auto prodLaunch = launchPolkitAdminTransport(cancellable);
      TEST_CHECK(!prodLaunch.channel);
      TEST_CHECK(prodLaunch.failureReason == AdminSessionFailureReason::NotInstalled);
      g_object_unref(cancellable);
    }
  }

  void testProtocolEncodingValidationAndPartialFraming() {
    // 1. Valid client requests round-trip cleanly.
    AdminClientRequest hello{
        .command = AdminClientCommand::Hello,
        .requestId = 1,
        .protocolVersion = kAdminProtocolVersion,
        .nonce = "nonce-123",
    };
    const auto encHello = encodeAdminClientRequest(hello);
    TEST_CHECK(encHello.has_value());
    AdminClientRequest decHello;
    AdminParseError err;
    TEST_CHECK(decodeAdminClientRequest(*encHello, decHello, &err));
    TEST_CHECK(decHello.command == AdminClientCommand::Hello);
    TEST_CHECK(decHello.requestId == 1);
    TEST_CHECK(decHello.protocolVersion == kAdminProtocolVersion);
    TEST_CHECK(decHello.nonce == "nonce-123");

    AdminClientRequest ping{.command = AdminClientCommand::Ping, .requestId = 7};
    const auto encPing = encodeAdminClientRequest(ping);
    TEST_CHECK(encPing.has_value());
    AdminClientRequest decPing;
    TEST_CHECK(decodeAdminClientRequest(*encPing, decPing, &err));
    TEST_CHECK(decPing.command == AdminClientCommand::Ping && decPing.requestId == 7);

    AdminClientRequest cancel{.command = AdminClientCommand::Cancel, .requestId = 8, .targetRequestId = 7};
    const auto encCancel = encodeAdminClientRequest(cancel);
    TEST_CHECK(encCancel.has_value());
    AdminClientRequest decCancel;
    TEST_CHECK(decodeAdminClientRequest(*encCancel, decCancel, &err));
    TEST_CHECK(decCancel.command == AdminClientCommand::Cancel && decCancel.targetRequestId == 7);

    AdminClientRequest shutdownReq{.command = AdminClientCommand::Shutdown, .requestId = 9};
    const auto encShutdown = encodeAdminClientRequest(shutdownReq);
    TEST_CHECK(encShutdown.has_value());
    AdminClientRequest decShutdown;
    TEST_CHECK(decodeAdminClientRequest(*encShutdown, decShutdown, &err));
    TEST_CHECK(decShutdown.command == AdminClientCommand::Shutdown && decShutdown.requestId == 9);

    static_assert(kAdminProtocolVersion == 9);
    for (const std::string_view command : {
             "CREATE_FILE", "CREATE_DIR", "RENAME_ENTRY", "COPY_FILE", "COPY_ITEMS", "CONFLICT_REPLY",
         }) {
      AdminClientRequest legacyRequest;
      TEST_CHECK(!decodeAdminClientRequest(std::string(command) + " 10", legacyRequest, &err));
      TEST_CHECK(err.requestId == 10 && err.status == AdminParseStatus::UnsupportedCommand);
    }
    for (const std::string_view kind : {"MUTATION_RESULT", "COPY_ITEM", "PROG", "CONFLICT"}) {
      AdminServerFrame legacyFrame;
      TEST_CHECK(!decodeAdminServerFrame(std::string(kind) + " 10", legacyFrame, &err));
      TEST_CHECK(err.requestId == 10 && err.status == AdminParseStatus::UnknownFrameKind);
    }

    // 2. Reject malformed, control-char, non-canonical base64, zero requestId, and unknown commands.
    AdminClientRequest badReq;
    TEST_CHECK(!decodeAdminClientRequest("", badReq, &err) && err.status == AdminParseStatus::EmptyFrame);
    TEST_CHECK(!decodeAdminClientRequest("PING 0\n", badReq, &err));
    TEST_CHECK(!decodeAdminClientRequest("PING -1\n", badReq, &err));
    TEST_CHECK(!decodeAdminClientRequest("PING  1\n", badReq, &err));
    TEST_CHECK(!decodeAdminClientRequest(" PING 1\n", badReq, &err));
    TEST_CHECK(!decodeAdminClientRequest("PING 1 \n", badReq, &err));
    TEST_CHECK(
        !decodeAdminClientRequest("PING 1\r\n", badReq, &err) && err.status == AdminParseStatus::ControlCharacter
    );
    TEST_CHECK(!decodeAdminClientRequest("HELLO 1 1 not_base64!\n", badReq, &err));
    TEST_CHECK(
        !decodeAdminClientRequest("EXEC 1 /bin/sh\n", badReq, &err)
        && err.status == AdminParseStatus::UnsupportedCommand
    );

    // 3. Partial 1-byte reads, coalesced frames, and oversized frame rejection in AdminLineBuffer.
    AdminLineBuffer lineBuf(128);
    const std::string combined = *encHello + *encPing;
    std::string popped;
    for (std::size_t i = 0; i < combined.size(); ++i) {
      lineBuf.append(std::string_view(&combined[i], 1));
      if (i + 1 == encHello->size()) {
        TEST_CHECK(lineBuf.popFrame(popped) == AdminFrameReadStatus::FrameReady);
        TEST_CHECK(popped + "\n" == *encHello);
      }
    }
    TEST_CHECK(lineBuf.popFrame(popped) == AdminFrameReadStatus::FrameReady);
    TEST_CHECK(popped + "\n" == *encPing);
    TEST_CHECK(lineBuf.popFrame(popped) == AdminFrameReadStatus::NeedMoreData);

    // Oversized frame without newline latches OversizedFrame immediately.
    lineBuf.append(std::string(200, 'A'));
    TEST_CHECK(lineBuf.popFrame(popped) == AdminFrameReadStatus::OversizedFrame);
  }

  void testHelperSessionDirectProtocolAndEdgeCases() {
    // 1. Command sent before HELLO is rejected with HandshakeRequired and terminates helper.
    {
      int inPipe[2]{-1, -1};
      int outPipe[2]{-1, -1};
      TEST_CHECK(::pipe2(inPipe, O_CLOEXEC) == 0);
      TEST_CHECK(::pipe2(outPipe, O_CLOEXEC) == 0);

      int writeErr = 0;
      TEST_CHECK(writeAdminFrameFd(inPipe[1], "PING 1\n", writeErr));
      const auto res = runAdminHelperSession(inPipe[0], outPipe[1], {.helperPid = 99, .invokingUid = 1000});
      TEST_CHECK(!res.handshakeCompleted);
      TEST_CHECK(res.reason == AdminHelperSessionExitReason::ProtocolViolation);
      ::close(outPipe[1]);

      AdminLineBuffer buf;
      std::string replyLine;
      int readErr = 0;
      TEST_CHECK(readAdminFrameFd(outPipe[0], buf, replyLine, readErr) == AdminFrameReadStatus::FrameReady);
      AdminServerFrame frame;
      TEST_CHECK(decodeAdminServerFrame(replyLine, frame));
      TEST_CHECK(frame.kind == AdminServerFrameKind::Response);
      TEST_CHECK(frame.status == AdminStatusCode::HandshakeRequired);
      ::close(inPipe[0]);
      ::close(inPipe[1]);
      ::close(outPipe[0]);
    }

    // 2. Oversized frame terminates helper with ProtocolViolation.
    {
      int inPipe[2]{-1, -1};
      int outPipe[2]{-1, -1};
      TEST_CHECK(::pipe2(inPipe, O_CLOEXEC) == 0);
      TEST_CHECK(::pipe2(outPipe, O_CLOEXEC) == 0);

      std::string oversized(512, 'X');
      oversized.push_back('\n');
      int writeErr = 0;
      TEST_CHECK(writeAdminFrameFd(inPipe[1], oversized, writeErr));
      const auto res =
          runAdminHelperSession(inPipe[0], outPipe[1], {.helperPid = 99, .invokingUid = 1000, .maxFrameBytes = 256});
      TEST_CHECK(res.reason == AdminHelperSessionExitReason::ProtocolViolation);
      ::close(inPipe[0]);
      ::close(inPipe[1]);
      ::close(outPipe[0]);
      ::close(outPipe[1]);
    }

    // 3. Partial writes and partial reads across helper handshake + PING + CANCEL + unsupported command + EOF exit.
    {
      int inPipe[2]{-1, -1};
      int outPipe[2]{-1, -1};
      TEST_CHECK(::pipe2(inPipe, O_CLOEXEC) == 0);
      TEST_CHECK(::pipe2(outPipe, O_CLOEXEC) == 0);

      AdminHelperSessionResult helperRes;
      std::thread helperThread([&] {
        helperRes = runAdminHelperSession(inPipe[0], outPipe[1], {.helperPid = 777, .invokingUid = 1000});
        ::close(outPipe[1]);
      });

      const auto helloFrame = *encodeAdminClientRequest(
          AdminClientRequest{
              .command = AdminClientCommand::Hello,
              .requestId = 1,
              .protocolVersion = kAdminProtocolVersion,
              .nonce = "partial-test",
          }
      );
      // Drip-feed HELLO 3 bytes at a time.
      for (std::size_t i = 0; i < helloFrame.size(); i += 3) {
        const std::size_t chunkLen = std::min<std::size_t>(3, helloFrame.size() - i);
        TEST_CHECK(::write(inPipe[1], helloFrame.data() + i, chunkLen) == static_cast<ssize_t>(chunkLen));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      AdminLineBuffer clientBuf;
      std::string line;
      int readErr = 0;
      TEST_CHECK(readAdminFrameFd(outPipe[0], clientBuf, line, readErr) == AdminFrameReadStatus::FrameReady);
      AdminServerFrame ready;
      TEST_CHECK(decodeAdminServerFrame(line, ready));
      TEST_CHECK(ready.kind == AdminServerFrameKind::Ready && ready.helperPid == 777 && ready.nonce == "partial-test");

      // Send unsupported mutation command (must be rejected with Unsupported in Stage 1 & Stage 2).
      int writeErr = 0;
      TEST_CHECK(writeAdminFrameFd(inPipe[1], "DELETE_FILE 2 /root/secret\n", writeErr));
      TEST_CHECK(readAdminFrameFd(outPipe[0], clientBuf, line, readErr) == AdminFrameReadStatus::FrameReady);
      AdminServerFrame unsupportedResp;
      TEST_CHECK(decodeAdminServerFrame(line, unsupportedResp));
      TEST_CHECK(
          unsupportedResp.kind == AdminServerFrameKind::Response
          && unsupportedResp.requestId == 2
          && unsupportedResp.status == AdminStatusCode::Unsupported
      );

      // Close client stdin -> helper must exit immediately with ClientEof.
      ::close(inPipe[1]);
      helperThread.join();
      TEST_CHECK(helperRes.handshakeCompleted);
      TEST_CHECK(helperRes.reason == AdminHelperSessionExitReason::ClientEof);
      ::close(inPipe[0]);
      ::close(outPipe[0]);
    }

    // 4. Architectural support for helper-enforced inactivity timeout.
    {
      int inPipe[2]{-1, -1};
      int outPipe[2]{-1, -1};
      TEST_CHECK(::pipe2(inPipe, O_CLOEXEC) == 0);
      TEST_CHECK(::pipe2(outPipe, O_CLOEXEC) == 0);

      AdminHelperSessionResult helperRes;
      std::thread helperThread([&] {
        helperRes = runAdminHelperSession(
            inPipe[0], outPipe[1],
            {.helperPid = 888, .invokingUid = 1000, .handshakeTimeoutMs = 1000, .inactivityTimeoutMs = 40}
        );
        ::close(outPipe[1]);
      });

      const auto helloFrame = *encodeAdminClientRequest(
          AdminClientRequest{
              .command = AdminClientCommand::Hello,
              .requestId = 1,
              .protocolVersion = kAdminProtocolVersion,
              .nonce = "idle-timeout",
          }
      );
      int writeErr = 0;
      TEST_CHECK(writeAdminFrameFd(inPipe[1], helloFrame, writeErr));

      AdminLineBuffer clientBuf;
      std::string line;
      int readErr = 0;
      TEST_CHECK(readAdminFrameFd(outPipe[0], clientBuf, line, readErr) == AdminFrameReadStatus::FrameReady);

      // Do not send any further requests; helper must self-terminate after inactivityTimeoutMs.
      helperThread.join();
      TEST_CHECK(helperRes.handshakeCompleted);
      TEST_CHECK(helperRes.reason == AdminHelperSessionExitReason::InactivityTimeout);
      ::close(inPipe[0]);
      ::close(inPipe[1]);
      ::close(outPipe[0]);
    }
  }

  void testSharedOperationTransport(const std::filesystem::path& root) {
    namespace engine = file_operation_engine;
    using Kind = AdminServerFrameKind;
    const auto from = root / "source";
    const auto to = root / "destination";
    std::filesystem::create_directory(from);
    std::filesystem::create_directory(to);
    const auto source = from / "input";
    const auto destination = to / source.filename();
    std::ofstream(source) << "incoming payload";
    std::ofstream(destination) << "old";

    // A controllable I/O checkpoint lets the test deterministically pause before publish.
    std::mutex gateMutex;
    std::condition_variable gate;
    std::atomic<bool> hold{false};
    std::atomic<bool> reached{false};
    std::atomic<std::uint64_t> holdBytes{0};
    bool release = false;
    const auto releaseGate = [&] {
      std::lock_guard lock(gateMutex);
      release = true;
      gate.notify_all();
    };
    AdminHelperSessionConfig config{
        .helperPid = 801, .invokingUid = static_cast<std::uint32_t>(geteuid()), .creationUmask = mode_t{0022}
    };
    config.copyCheckpointForTesting = [&](std::uint64_t bytes) {
      if (!hold.load() || bytes < holdBytes.load() || reached.exchange(true))
        return;
      std::unique_lock lock(gateMutex);
      gate.wait_for(lock, std::chrono::seconds(4), [&] { return release; });
    };
    InProcessHelperFixture fixture;
    auto alive = std::make_shared<bool>(true);
    AdminSessionClient client(1, alive);
    AdminSessionClient other(2, alive);
    const auto unblock = nythera::test::makeScopeExit(releaseGate);
    client.setTransportLauncher(fixture.makeLauncher(config));
    config.helperPid = 802;
    config.copyCheckpointForTesting = {};
    other.setTransportLauncher(fixture.makeLauncher(config));
    const engine::Request emptyFile{.kind = engine::Kind::CreateFile, .destination = (to / "empty").string()};
    const auto inactive = client.runOperation(emptyFile, client.sessionGeneration());
    TEST_CHECK(inactive.requestId == 0 && inactive.takeFrames().front().status == AdminStatusCode::Unauthorized);
    TEST_CHECK(!std::filesystem::exists(emptyFile.destination));
    TEST_CHECK(client.startSessionAsync() && other.startSessionAsync());
    pumpUntil([&] { return client.isActive() && other.isActive(); });

    auto begin = [&](engine::Kind kind, std::vector<std::string> sources, const std::filesystem::path& dest) {
      auto op = client.runOperation(
          {.kind = kind, .sources = std::move(sources), .destination = dest.string()}, client.sessionGeneration()
      );
      TEST_CHECK(op.requestId != 0);
      return op;
    };
    auto waitFrame = [&](const AdminSessionClient::OperationTransport& op, Kind kind, bool paused = false) {
      std::optional<AdminServerFrame> matching;
      pumpUntil([&] {
        for (auto& frame : op.takeFrames()) {
          TEST_CHECK(frame.requestId == op.requestId);
          if (frame.kind == kind && (kind != Kind::OpPaused || frame.paused == paused))
            matching = std::move(frame);
        }
        return matching.has_value();
      });
      return *matching;
    };
    auto finish = [&](const AdminSessionClient::OperationTransport& op, AdminStatusCode status = AdminStatusCode::Ok) {
      // Completion messages in the mailbox precede the final result.
      std::vector<AdminServerFrame> frames;
      pumpUntil([&] {
        auto batch = op.takeFrames();
        for (auto& frame : batch) {
          TEST_CHECK(frame.requestId == op.requestId);
          frames.push_back(std::move(frame));
        }
        return !frames.empty() && frames.back().kind == Kind::OpResult;
      });
      TEST_CHECK(frames.back().status == status);
      return frames;
    };
    auto readText = [](const std::filesystem::path& path) {
      std::ifstream input(path);
      return std::string(std::istreambuf_iterator<char>(input), {});
    };

    auto copy = begin(engine::Kind::Copy, {source.string()}, to);
    const auto conflict = waitFrame(copy, Kind::OpConflict);
    TEST_CHECK(conflict.sourcePath == source && conflict.destinationPath == destination);
    TEST_CHECK(conflict.conflictMetadata.incoming.type == G_FILE_TYPE_REGULAR);
    TEST_CHECK(conflict.conflictMetadata.existing.size == 3);
    auto busy = client.runOperation(
        {.kind = engine::Kind::CreateFolder, .destination = (to / "busy").string()}, client.sessionGeneration()
    );
    TEST_CHECK(busy.requestId == 0 && busy.takeFrames().front().status == AdminStatusCode::BadRequest);
    // Bypass the client-side gate. The helper must also serialize mutations.
    TEST_CHECK(client.sendRawFrameForTesting(
        *encodeAdminClientRequest({
            .command = AdminClientCommand::OpBegin,
            .requestId = 901,
            .path = (to / "raw-busy").string(),
            .operationKind = engine::Kind::CreateFolder,
        })
        + *encodeAdminClientRequest({.command = AdminClientCommand::OpExec, .requestId = 901})
    ));
    const auto independent = other.runOperation(
        {.kind = engine::Kind::CreateFolder, .destination = (to / "independent").string()}, other.sessionGeneration()
    );
    TEST_CHECK(independent.requestId && finish(independent).back().mutated);
    TEST_CHECK(!std::filesystem::exists(to / "busy"));
    TEST_CHECK(!std::filesystem::exists(to / "raw-busy"));
    const auto digest = client.runOperation(
        {.kind = engine::Kind::ProtectedChecksum, .destination = source.string(), .contentRevision = "0"},
        client.sessionGeneration()
    );
    g_autofree char* expectedHash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "incoming payload", -1);
    TEST_CHECK(digest.requestId && finish(digest).back().customDescription == expectedHash);
    TEST_CHECK(copy.sendConflictReply(conflict.conflictId, {.action = engine::ConflictAction::Replace}));
    const auto copied = finish(copy);
    TEST_CHECK(std::ranges::any_of(copied, [&](const auto& frame) {
      return frame.kind == Kind::OpCompletedEntry && frame.completedEntry.path == destination;
    }));
    TEST_CHECK(readText(destination) == "incoming payload");

    const auto privateDir = root / "protected-edit";
    TEST_CHECK(mkdir(privateDir.c_str(), 0700) == 0);
    const auto temporary = privateDir / "input";
    std::ofstream(temporary) << "placeholder";
    TEST_CHECK(chmod(source.c_str(), 0640) == 0);
    const auto exported = finish(begin(engine::Kind::ExportProtectedFile, {source.string()}, temporary)).back();
    TEST_CHECK(!exported.mutated && !exported.customDescription.empty() && readText(temporary) == "incoming payload");
    std::ofstream(temporary) << "edited in temporary copy";
    TEST_CHECK(readText(source) == "incoming payload");
    const engine::Request save{
        .kind = engine::Kind::SaveProtectedFile,
        .sources = {temporary.string()},
        .destination = source.string(),
        .contentRevision = exported.customDescription,
    };
    const auto saved = finish(client.runOperation(save, client.sessionGeneration())).back();
    TEST_CHECK(saved.mutated && !saved.customDescription.empty() && readText(source) == "edited in temporary copy");
    struct stat sourceState{}, folderState{};
    TEST_CHECK(stat(source.c_str(), &sourceState) == 0 && (sourceState.st_mode & 0777) == 0640);
    TEST_CHECK(stat(from.c_str(), &folderState) == 0);
    const auto sized = client.runOperation(
        {.kind = engine::Kind::ProtectedFolderSize, .destination = from.string(),
         .renameIdentity = {{folderState.st_dev, folderState.st_ino}}},
        client.sessionGeneration()
    );
    const auto stats = decodeFolderSizeResult(finish(sized).back().customDescription);
    TEST_CHECK(stats && !stats->partial() && stats->files == 1
               && stats->bytes == static_cast<std::uint64_t>(sourceState.st_size));
    std::ofstream(source) << "the original changed";
    const auto staleSave = finish(client.runOperation(save, client.sessionGeneration()), AdminStatusCode::InternalError).back();
    TEST_CHECK(!staleSave.mutated && readText(source) == "the original changed");
    TEST_CHECK(readText(temporary) == "edited in temporary copy");
    std::ofstream(source) << "incoming payload";
    TEST_CHECK(chmod(source.c_str(), 0644) == 0);

    const auto numberedFile = to / "empty 2";
    const auto numberedFolder = to / "independent 2";
    {
      const auto prior = ::umask(0077);
      const auto restoreUmask = nythera::test::makeScopeExit([prior] { ::umask(prior); });
      TEST_CHECK(finish(client.runOperation(emptyFile, client.sessionGeneration())).back().mutated);
      TEST_CHECK(!finish(client.runOperation(emptyFile, client.sessionGeneration()), AdminStatusCode::InternalError)
                      .back().mutated);
      auto keepBoth = emptyFile;
      keepBoth.keepBoth = true;
      TEST_CHECK(finish(client.runOperation(keepBoth, client.sessionGeneration())).back().mutated);
      TEST_CHECK(finish(client.runOperation(
          {.kind = engine::Kind::CreateFolder, .destination = (to / "independent").string(), .keepBoth = true},
          client.sessionGeneration()
      )).back().mutated);
      TEST_CHECK(::umask(0077) == 0077);
    }
    struct stat createdState{};
    TEST_CHECK(::stat(numberedFile.c_str(), &createdState) == 0);
    TEST_CHECK(S_ISREG(createdState.st_mode) && (createdState.st_mode & 0777) == 0644 && createdState.st_size == 0);
    TEST_CHECK(::stat(numberedFolder.c_str(), &createdState) == 0);
    TEST_CHECK(S_ISDIR(createdState.st_mode) && (createdState.st_mode & 0777) == 0755);

    const auto moved = begin(engine::Kind::Move, {destination.string()}, to / "independent");
    const auto movedResult = finish(moved);
    TEST_CHECK(std::ranges::any_of(movedResult, [&](const auto& frame) {
      return frame.kind == Kind::OpMovedSource && frame.sourcePath == destination;
    }));
    const auto movedPath = to / "independent" / "input";
    TEST_CHECK(!std::filesystem::exists(destination) && readText(movedPath) == "incoming payload");
    const auto renamed = to / "independent" / "renamed";
    struct stat movedState{};
    TEST_CHECK(::lstat(movedPath.c_str(), &movedState) == 0);
    engine::Request rename{
        .kind = engine::Kind::Rename, .sources = {movedPath.string()}, .destination = renamed.string(),
        .renameIdentity = {{movedState.st_dev, movedState.st_ino == 1 ? 2 : 1}}
    };
    auto stale = client.runOperation(rename, client.sessionGeneration());
    TEST_CHECK(stale.requestId != 0 && !finish(stale, AdminStatusCode::InternalError).back().mutated);
    TEST_CHECK(std::filesystem::exists(movedPath) && !std::filesystem::exists(renamed));
    rename.renameIdentity = {{movedState.st_dev, movedState.st_ino}};
    TEST_CHECK(finish(client.runOperation(rename, client.sessionGeneration())).back().mutated);
    TEST_CHECK(!std::filesystem::exists(movedPath) && readText(renamed) == "incoming payload");
    TEST_CHECK(finish(begin(engine::Kind::Delete, {renamed.string()}, {})).back().mutated);
    TEST_CHECK(!std::filesystem::exists(renamed));
    const auto created = to / "created";
    TEST_CHECK(finish(begin(engine::Kind::CreateFile, {source.string()}, created)).back().mutated);
    TEST_CHECK(readText(created) == "incoming payload");
    const auto duplicate = finish(begin(engine::Kind::Duplicate, {created.string()}, to));
    const auto duplicateEntry = std::ranges::find(duplicate, Kind::OpCompletedEntry, &AdminServerFrame::kind);
    TEST_CHECK(duplicateEntry != duplicate.end() && duplicateEntry->completedEntry.path != created);
    TEST_CHECK(readText(duplicateEntry->completedEntry.path) == "incoming payload");

    const auto link = to / "link";
    const auto renamedLink = to / "renamed-link";
    TEST_CHECK(finish(begin(engine::Kind::CreateSymlink, {"missing-target"}, link)).back().mutated);
    TEST_CHECK(::lstat(link.c_str(), &movedState) == 0 && S_ISLNK(movedState.st_mode));
    rename.sources = {link.string()};
    rename.destination = renamedLink.string();
    rename.renameIdentity = {{movedState.st_dev, movedState.st_ino}};
    TEST_CHECK(finish(client.runOperation(rename, client.sessionGeneration())).back().mutated);
    TEST_CHECK(!std::filesystem::is_symlink(link) && std::filesystem::read_symlink(renamedLink) == "missing-target");

    const auto metadataSource = from / "metadata";
    const auto metadataTarget = to / "metadata-target";
    std::filesystem::create_directory(metadataTarget);
    std::ofstream(metadataSource) << "metadata fixture";
    std::ofstream(metadataTarget / "new-file") << "destination creation defaults";
    TEST_CHECK(::chmod(metadataSource.c_str(), 04750) == 0);
    const auto metadataOf = [](const std::filesystem::path& path) {
      struct stat state{};
      TEST_CHECK(::lstat(path.c_str(), &state) == 0);
      return state;
    };
    const auto original = metadataOf(metadataSource);
    const auto creationDefaults = metadataOf(metadataTarget / "new-file");
    const auto checkCopy = [&](const std::filesystem::path& path) {
      const auto copyState = metadataOf(path);
      TEST_CHECK(copyState.st_ino != original.st_ino && readText(path) == "metadata fixture");
      TEST_CHECK(copyState.st_uid == creationDefaults.st_uid && copyState.st_gid == creationDefaults.st_gid);
      TEST_CHECK((copyState.st_mode & 07777) == 0750);
    };
    TEST_CHECK(finish(begin(engine::Kind::Copy, {metadataSource.string()}, metadataTarget)).back().mutated);
    checkCopy(metadataTarget / "metadata");
    TEST_CHECK(finish(begin(engine::Kind::Duplicate, {metadataSource.string()}, from)).back().mutated);
    checkCopy(from / "metadata (copy)");
    const auto checkOriginal = [&](const std::filesystem::path& path) {
      const auto state = metadataOf(path);
      TEST_CHECK(state.st_ino == original.st_ino && state.st_uid == original.st_uid && state.st_gid == original.st_gid);
      TEST_CHECK(state.st_mode == original.st_mode);
    };
    TEST_CHECK(finish(begin(engine::Kind::Move, {metadataSource.string()}, to)).back().mutated);
    checkOriginal(to / "metadata");
    TEST_CHECK(finish(begin(engine::Kind::Rename, {(to / "metadata").string()}, to / "metadata-renamed"))
                   .back().mutated);
    checkOriginal(to / "metadata-renamed");

    std::vector<std::string> batch;
    for (unsigned i = 0; i < 260; ++i) {
      const auto file = from / ("batch-" + std::to_string(i));
      std::ofstream(file) << i;
      batch.push_back(file.string());
    }
    const auto published = finish(begin(engine::Kind::Copy, std::move(batch), to));
    TEST_CHECK(
        std::ranges::count_if(published, [](const auto& frame) { return frame.kind == Kind::OpCompletedEntry; }) == 260
    );
    TEST_CHECK(std::ranges::any_of(published, [](const auto& frame) { return frame.kind == Kind::OpProgress; }));
    TEST_CHECK(published.back().completedItems == 260 && published.back().totalItems == 260);
    TEST_CHECK(readText(to / "batch-259") == "259");

    hold = true;
    auto pauseCopy = begin(engine::Kind::Copy, {source.string()}, to);
    pumpUntil([&] { return reached.load(); });
    TEST_CHECK(pauseCopy.setPaused(true));
    bool responsive = false;
    TEST_CHECK(client.pingAsync([&](const auto& health) {
      responsive = health.activeRequestId == pauseCopy.requestId;
    }));
    pumpUntil([&] { return responsive; });
    releaseGate();
    (void)waitFrame(pauseCopy, Kind::OpPaused, true);
    TEST_CHECK(!std::filesystem::exists(destination));
    TEST_CHECK(pauseCopy.setPaused(false));
    const auto resumed = finish(pauseCopy);
    TEST_CHECK(std::ranges::any_of(resumed, [](const auto& frame) {
      return frame.kind == Kind::OpPaused && !frame.paused;
    }));
    TEST_CHECK(readText(destination) == "incoming payload");

    const auto large = from / "large";
    std::ofstream(large) << std::string(8U * 1024U * 1024U, 'X');
    {
      std::lock_guard lock(gateMutex);
      release = false;
    }
    holdBytes = 1;
    reached = false;
    auto midTransfer = begin(engine::Kind::Copy, {large.string()}, to);
    pumpUntil([&] { return reached.load(); });
    TEST_CHECK(midTransfer.cancel());
    responsive = false;
    TEST_CHECK(client.pingAsync([&](const auto& health) {
      responsive = health.activeRequestId == midTransfer.requestId;
    }));
    pumpUntil([&] { return responsive; });
    releaseGate();
    TEST_CHECK(!finish(midTransfer, AdminStatusCode::Cancelled).back().mutated);
    TEST_CHECK(std::filesystem::exists(large) && !std::filesystem::exists(to / "large"));
    TEST_CHECK(std::ranges::none_of(std::filesystem::directory_iterator(to), [](const auto& entry) {
      return entry.path().filename().string().starts_with(".nythera-partial-");
    }));
    hold = false;

    auto renamedConflict = begin(engine::Kind::Copy, {source.string()}, to);
    const auto keepBoth = waitFrame(renamedConflict, Kind::OpConflict);
    TEST_CHECK(renamedConflict.sendConflictReply(
        keepBoth.conflictId, {.action = engine::ConflictAction::Rename, .applyToAll = true, .alternativeName = "kept"}
    ));
    (void)finish(renamedConflict);
    TEST_CHECK(readText(to / "kept") == "incoming payload");

    auto cancelled = begin(engine::Kind::Copy, {source.string()}, to);
    (void)waitFrame(cancelled, Kind::OpConflict);
    TEST_CHECK(cancelled.cancel());
    TEST_CHECK(!finish(cancelled, AdminStatusCode::Cancelled).back().mutated);

    for (const bool graceful : {true, false}) {
      auto finishing = begin(engine::Kind::Copy, {source.string()}, to);
      (void)waitFrame(finishing, Kind::OpConflict);
      client.stopSession(graceful);
      const auto frames = finish(finishing, graceful ? AdminStatusCode::Cancelled : AdminStatusCode::InternalError);
      TEST_CHECK(frames.back().outcomeUncertain == !graceful);
      client.waitForSettled();
      TEST_CHECK(!finishing.setPaused(true));
      TEST_CHECK(client.startSessionAsync());
      pumpUntil([&] { return client.isActive(); });
    }
    client.stopSession(true);
    other.stopSession(true);
    client.waitForSettled();
    other.waitForSettled();
    TEST_CHECK(fixture.activeHelpers.load() == 0);

    AdminClientRequest invalid;
    TEST_CHECK(!decodeAdminClientRequest("OP_BEGIN 2 0 1025 0 0 0 L3RtcA==\n", invalid));
    TEST_CHECK(!decodeAdminClientRequest("OP_EXEC 2 unexpected\n", invalid));
    TEST_CHECK(!decodeAdminClientRequest("OP_CONFLICT_REPLY 3 2 1 99 0 -\n", invalid));
    TEST_CHECK(!decodeAdminClientRequest("OP_SOURCE 2 " + std::string(kAdminMaxFrameBytes, 'A'), invalid));
  }

  void testSharedOperationExpiry(const std::filesystem::path& root) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using Kind = AdminServerFrameKind;
    using Operation = file_operation_engine::Kind;

    const auto from = root / "expiry-source";
    const auto to = root / "expiry-destination";
    std::filesystem::create_directory(from);
    std::filesystem::create_directory(to);
    const auto first = from / "first";
    const auto middle = from / "middle";
    const auto second = from / "second";
    std::ofstream(first).close();
    std::ofstream(middle) << "more data";
    std::ofstream(second) << "incoming";
    std::ofstream(to / second.filename()) << "existing";

    const auto epoch = Clock::now();
    std::atomic<int> clockMinutes{0};
    std::atomic<bool> workerReached{false};
    std::atomic<bool> holdAfterFirstMove{false};
    std::atomic<bool> firstMoved{false};
    std::atomic<bool> holdClock{false};
    std::atomic<bool> helperReached{false};
    std::atomic<bool> resultQueued{false};
    std::thread::id helperThread;
    std::mutex mutex;
    std::condition_variable gate;
    bool releaseWorker = false;
    bool releaseAfterFirstMove = false;
    bool releaseHelper = false;
    const auto unblockWorker = [&] {
      std::lock_guard lock(mutex);
      releaseWorker = true;
      gate.notify_all();
    };
    AdminHelperSessionConfig config{.invokingUid = static_cast<std::uint32_t>(geteuid())};
    config.clockForTesting = [&] {
      if (helperThread == std::thread::id{})
        helperThread = std::this_thread::get_id();
      if (std::this_thread::get_id() == helperThread && holdClock.load() && !helperReached.exchange(true)) {
        std::unique_lock lock(mutex);
        gate.wait_for(lock, 4s, [&] { return releaseHelper; });
      }
      return epoch + std::chrono::minutes(clockMinutes.load());
    };
    config.copyCheckpointForTesting = [&](std::uint64_t) {
      if (!workerReached.exchange(true)) {
        std::unique_lock lock(mutex);
        gate.wait_for(lock, 4s, [&] { return releaseWorker; });
      }
      if (holdAfterFirstMove.load() && !std::filesystem::exists(first) && !firstMoved.exchange(true)) {
        std::unique_lock lock(mutex);
        gate.wait_for(lock, 4s, [&] { return releaseAfterFirstMove; });
      }
    };
    config.operationResultQueuedForTesting = [&] { resultQueued = true; };

    InProcessHelperFixture fixture;
    auto alive = std::make_shared<bool>(true);
    AdminSessionClient client(1, alive);
    const auto unblock = nythera::test::makeScopeExit([&] {
      std::lock_guard lock(mutex);
      releaseWorker = releaseAfterFirstMove = releaseHelper = true;
      gate.notify_all();
    });
    auto start = [&] {
      clockMinutes = 0;
      workerReached = holdClock = helperReached = resultQueued = false;
      holdAfterFirstMove = firstMoved = false;
      helperThread = {};
      releaseWorker = releaseAfterFirstMove = releaseHelper = false;
      client.setTransportLauncher(fixture.makeLauncher(config));
      TEST_CHECK(client.startSessionAsync());
      pumpUntil([&] { return client.isActive(); });
    };
    auto begin = [&](Operation kind, std::vector<std::string> sources) {
      auto operation = client.runOperation(
          {.kind = kind, .sources = std::move(sources), .destination = to.string()}, client.sessionGeneration()
      );
      TEST_CHECK(operation.requestId != kInvalidAdminRequestId);
      return operation;
    };
    auto waitFor = [&](const auto& operation, auto matches) {
      std::vector<AdminServerFrame> frames;
      pumpUntil([&] {
        for (auto& frame : operation.takeFrames())
          frames.push_back(std::move(frame));
        return std::ranges::any_of(frames, matches);
      });
      return frames;
    };
    auto finishAfterExpiry = [&](const auto& operation, AdminStatusCode status) {
      pumpUntil([&] { return client.state() == AdminSessionState::Failed; });
      TEST_CHECK(client.failureReason() == AdminSessionFailureReason::HelperTerminated);
      client.waitForSettled();
      TEST_CHECK(fixture.activeHelpers.load() == 0);
      TEST_CHECK(fixture.results.back().reason == AdminHelperSessionExitReason::InactivityTimeout);
      auto frames = operation.takeFrames();
      TEST_CHECK(std::ranges::count(frames, Kind::OpResult, &AdminServerFrame::kind) == 1);
      TEST_CHECK(frames.back().kind == Kind::OpResult && frames.back().status == status);
      TEST_CHECK(!frames.back().outcomeUncertain && operation.takeFrames().empty());
      return frames;
    };

    // A completed empty file advances only the item count. Its progress at minute nine
    // keeps the session alive at minute eleven, with no user input since submission.
    start();
    holdAfterFirstMove = true;
    auto moving = begin(Operation::Move, {first.string(), middle.string(), second.string()});
    pumpUntil([&] { return workerReached.load(); });
    clockMinutes = 9;
    unblockWorker();
    pumpUntil([&] { return firstMoved.load(); });
    (void)waitFor(moving, [](const auto& frame) {
      return frame.kind == Kind::OpProgress && frame.completedItems == 1 && frame.completedBytes == 0;
    });
    clockMinutes = 11;
    bool stillRunning = false;
    TEST_CHECK(client.pingAsync([&](const auto& health) {
      stillRunning = health.ok && health.activeRequestId == moving.requestId;
    }));
    pumpUntil([&] { return stillRunning; });
    {
      std::lock_guard lock(mutex);
      releaseAfterFirstMove = true;
      gate.notify_all();
    }
    const auto beforeConflict = waitFor(moving, [](const auto& frame) { return frame.kind == Kind::OpConflict; });
    TEST_CHECK(std::ranges::any_of(beforeConflict, [](const auto& frame) {
      return frame.kind == Kind::OpProgress && frame.completedItems == 2 && frame.completedBytes > 0;
    }));
    clockMinutes = 22;
    const auto partial = finishAfterExpiry(moving, AdminStatusCode::Cancelled);
    TEST_CHECK(partial.size() == 5);
    TEST_CHECK(partial[0].kind == Kind::OpCompletedEntry && partial[0].completedEntry.path == to / first.filename());
    TEST_CHECK(partial[1].kind == Kind::OpCompletedEntry && partial[1].completedEntry.path == to / middle.filename());
    TEST_CHECK(partial[2].kind == Kind::OpMovedSource && partial[2].sourcePath == first);
    TEST_CHECK(partial[3].kind == Kind::OpMovedSource && partial[3].sourcePath == middle);
    TEST_CHECK(partial.back().mutated && partial.back().detail.contains("inactivity timeout"));
    TEST_CHECK(!std::filesystem::exists(first) && std::filesystem::exists(second));

    // A pause acknowledgement does not keep a paused transfer alive indefinitely.
    std::ofstream(first) << "paused";
    std::filesystem::remove(to / first.filename());
    start();
    auto paused = begin(Operation::Copy, {first.string()});
    pumpUntil([&] { return workerReached.load(); });
    TEST_CHECK(paused.setPaused(true));
    stillRunning = false;
    TEST_CHECK(client.pingAsync([&](const auto& health) { stillRunning = health.ok; }));
    pumpUntil([&] { return stillRunning; });
    unblockWorker();
    (void)waitFor(paused, [](const auto& frame) { return frame.kind == Kind::OpPaused && frame.paused; });
    clockMinutes = 11;
    const auto cancelled = finishAfterExpiry(paused, AdminStatusCode::Cancelled);
    TEST_CHECK(!cancelled.back().mutated && !std::filesystem::exists(to / first.filename()));

    // Hold the event loop before its deadline check while the worker queues a complete
    // result. Crossing the idle deadline must preserve both the record and the success.
    start();
    auto completed = begin(Operation::Move, {first.string()});
    pumpUntil([&] { return workerReached.load(); });
    holdClock = true;
    TEST_CHECK(client.pingAsync([](const auto&) {}));
    pumpUntil([&] { return helperReached.load(); });
    unblockWorker();
    pumpUntil([&] { return resultQueued.load(); });
    clockMinutes = 11;
    {
      std::lock_guard lock(mutex);
      releaseHelper = true;
      gate.notify_all();
    }
    const auto committed = finishAfterExpiry(completed, AdminStatusCode::Ok);
    TEST_CHECK(committed.size() >= 3);
    TEST_CHECK(committed[committed.size() - 3].kind == Kind::OpCompletedEntry);
    TEST_CHECK(committed[committed.size() - 2].kind == Kind::OpMovedSource);
    TEST_CHECK(committed.back().mutated && std::filesystem::exists(to / first.filename()));

    // A session with no operation still expires. PING wakes the fake clock's poll
    // without renewing activity.
    start();
    clockMinutes = 11;
    TEST_CHECK(client.pingAsync([](const auto&) {}));
    pumpUntil([&] { return client.state() == AdminSessionState::Failed; });
    client.waitForSettled();
    TEST_CHECK(fixture.results.back().reason == AdminHelperSessionExitReason::InactivityTimeout);

    // Submitting a quick mutation renews activity even when it reports no transfer progress.
    start();
    clockMinutes = 8;
    const auto freshFile = to / "renewed";
    const auto created = client.runOperation(
        {.kind = Operation::CreateFile, .destination = freshFile.string()}, client.sessionGeneration()
    );
    TEST_CHECK(created.requestId != 0);
    const auto creation = waitFor(created, [](const auto& frame) { return frame.kind == Kind::OpResult; });
    TEST_CHECK(creation.back().status == AdminStatusCode::Ok && std::filesystem::exists(freshFile));
    clockMinutes = 11;
    stillRunning = false;
    TEST_CHECK(client.pingAsync([&](const auto& health) { stillRunning = health.ok; }));
    pumpUntil([&] { return stillRunning; });
    clockMinutes = 19;
    TEST_CHECK(client.pingAsync([](const auto&) {}));
    pumpUntil([&] { return client.state() == AdminSessionState::Failed; });
    client.waitForSettled();
    const auto expired = client.runOperation(
        {.kind = Operation::CreateFolder, .destination = (to / "expired").string()}, client.sessionGeneration()
    );
    TEST_CHECK(expired.requestId == 0 && expired.takeFrames().front().status == AdminStatusCode::Unauthorized);
    TEST_CHECK(!std::filesystem::exists(to / "expired"));
  }

  void testClientHandshakeAuthDenialCancellationAndVersionMismatch() {
    auto alive = std::make_shared<bool>(true);

    // 1. Successful handshake, PING health check, CANCEL, and clean stop.
    {
      InProcessHelperFixture fixture;
      AdminSessionClient client(1, alive);
      client.setTransportLauncher(fixture.makeLauncher({.helperPid = 5001, .invokingUid = 1000}));

      bool startDone = false;
      bool startOk = false;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus& st) {
        startDone = true;
        startOk = ok;
        TEST_CHECK(st.tabId == 1);
        TEST_CHECK(st.state == AdminSessionState::Active);
        TEST_CHECK(st.helperPid == 5001);
        TEST_CHECK(st.invokingUid == 1000);
      });
      pumpUntil([&] { return startDone; });
      TEST_CHECK(startOk && client.isActive());

      bool pingDone = false;
      AdminHealthStatus health;
      TEST_CHECK(client.pingAsync([&](const AdminHealthStatus& h) {
        pingDone = true;
        health = h;
      }) != kInvalidAdminRequestId);
      pumpUntil([&] { return pingDone; });
      TEST_CHECK(health.ok && health.helperPid == 5001 && health.protocolVersion == kAdminProtocolVersion);

      bool cancelDone = false;
      AdminStatusCode cancelCode = AdminStatusCode::InternalError;
      TEST_CHECK(client.cancelRequestAsync(42, [&](bool ok, AdminStatusCode code, const std::string&) {
        cancelDone = true;
        TEST_CHECK(ok);
        cancelCode = code;
      }) != kInvalidAdminRequestId);
      pumpUntil([&] { return cancelDone; });
      TEST_CHECK(cancelCode == AdminStatusCode::Ok);

      client.stopSession(true);
      TEST_CHECK(client.state() == AdminSessionState::Inactive);
      client.waitForSettled();
      TEST_CHECK(fixture.activeHelpers.load() == 0);
      TEST_CHECK(!fixture.results.empty() && fixture.results.back().lastCancelledTargetId == 42);
    }

    // 2. Authentication denial (pkexec exit 127) and dismissal (pkexec exit 126).
    for (int exitCode : {126, 127}) {
      AdminSessionClient client(2, alive);
      client.setTransportLauncher([exitCode](GCancellable*) -> AdminTransportLaunchResult {
        int c2h[2]{-1, -1};
        int h2c[2]{-1, -1};
        TEST_CHECK(::pipe2(c2h, O_CLOEXEC) == 0);
        TEST_CHECK(::pipe2(h2c, O_CLOEXEC) == 0);
        // Simulate pkexec exiting immediately without starting helper.
        ::close(c2h[0]);
        ::close(h2c[1]);

        struct ClosedPipeFds {
          int writeFd = -1;
          int readFd = -1;
          std::mutex mutex;
          ~ClosedPipeFds() {
            closeWrite();
            closeRead();
          }
          void closeWrite() {
            std::lock_guard lock(mutex);
            if (writeFd >= 0) {
              ::close(writeFd);
              writeFd = -1;
            }
          }
          void closeRead() {
            std::lock_guard lock(mutex);
            if (readFd >= 0) {
              ::close(readFd);
              readFd = -1;
            }
          }
        };
        auto fds = std::make_shared<ClosedPipeFds>();
        fds->writeFd = c2h[1];
        fds->readFd = h2c[0];

        auto channel = std::make_unique<AdminTransportChannel>();
        channel->readFd = h2c[0];
        channel->writeFd = c2h[1];
        channel->closeWrite = [fds] { fds->closeWrite(); };
        channel->waitForExit = [fds, exitCode] {
          fds->closeWrite();
          fds->closeRead();
          return exitCode;
        };
        return AdminTransportLaunchResult{.channel = std::move(channel)};
      });

      bool done = false;
      bool okResult = true;
      AdminSessionStatus finalStatus;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus& st) {
        done = true;
        okResult = ok;
        finalStatus = st;
      });
      pumpUntil([&] { return done; });
      TEST_CHECK(!okResult);
      if (exitCode == 126) {
        TEST_CHECK(client.state() == AdminSessionState::Inactive);
        TEST_CHECK(finalStatus.failureReason == AdminSessionFailureReason::AuthCancelled);
      } else {
        TEST_CHECK(client.state() == AdminSessionState::Failed);
        TEST_CHECK(finalStatus.failureReason == AdminSessionFailureReason::AuthDenied);
      }
      client.waitForSettled();
    }

    // 3. The previous wire version is rejected cleanly.
    {
      InProcessHelperFixture fixture;
      AdminSessionClient client(3, alive);
      client.setTransportLauncher(fixture.makeLauncher());
      client.setProtocolVersionForTesting(8);

      bool done = false;
      bool okResult = true;
      AdminSessionStatus finalStatus;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus& st) {
        done = true;
        okResult = ok;
        finalStatus = st;
      });
      pumpUntil([&] { return done; });
      TEST_CHECK(!okResult);
      TEST_CHECK(client.state() == AdminSessionState::Failed);
      TEST_CHECK(finalStatus.failureReason == AdminSessionFailureReason::VersionMismatch);
      client.waitForSettled();
      TEST_CHECK(fixture.activeHelpers.load() == 0);
      TEST_CHECK(
          !fixture.results.empty() && fixture.results.back().reason == AdminHelperSessionExitReason::VersionMismatch
      );
    }

    // 4. Client authentication cancellation while Polkit prompt is pending; late completion cannot activate.
    {
      InProcessHelperFixture fixture;
      AdminSessionClient client(4, alive);
      client.setTransportLauncher(fixture.makeLauncher({.helperPid = 6001, .invokingUid = 1000}, 200));

      bool startCallbackFired = false;
      client.startSessionAsync([&](bool, const AdminSessionStatus&) { startCallbackFired = true; });
      TEST_CHECK(client.isAuthenticating());
      TEST_CHECK(client.cancelAuthentication());
      TEST_CHECK(client.state() == AdminSessionState::Inactive);
      TEST_CHECK(client.failureReason() == AdminSessionFailureReason::AuthCancelled);

      client.waitForSettled();
      drainMainContext();
      TEST_CHECK(!startCallbackFired);
      TEST_CHECK(!client.isActive());
      TEST_CHECK(fixture.activeHelpers.load() == 0);
    }
  }

  void testUnexpectedHelperTerminationAndMalformedServerFrame() {
    auto alive = std::make_shared<bool>(true);

    // 1. Helper terminates unexpectedly mid-session (e.g. crash or inactivity timeout).
    {
      InProcessHelperFixture fixture;
      AdminSessionClient client(10, alive);
      client.setTransportLauncher(
          fixture.makeLauncher({.helperPid = 7001, .invokingUid = 1000, .inactivityTimeoutMs = 50})
      );

      bool started = false;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
      pumpUntil([&] { return started; });
      TEST_CHECK(client.isActive());

      // Wait for helper inactivity timeout to close the pipe; client must detect EOF and transition to Failed.
      pumpUntil([&] { return client.state() == AdminSessionState::Failed; });
      TEST_CHECK(client.failureReason() == AdminSessionFailureReason::HelperTerminated);
      client.waitForSettled();
      TEST_CHECK(fixture.activeHelpers.load() == 0);
    }

    // A malformed reply or a truncated terminal result is a protocol failure.
    for (const bool duringOperation : {false, true}) {
      int c2h[2]{-1, -1};
      int h2c[2]{-1, -1};
      TEST_CHECK(::pipe2(c2h, O_CLOEXEC) == 0);
      TEST_CHECK(::pipe2(h2c, O_CLOEXEC) == 0);

      std::thread rogueHelper([inFd = c2h[0], outFd = h2c[1], duringOperation]() {
        AdminLineBuffer buf;
        std::string line;
        int err = 0;
        if (readAdminFrameFd(inFd, buf, line, err) == AdminFrameReadStatus::FrameReady) {
          AdminClientRequest hello;
          if (decodeAdminClientRequest(line, hello)) {
            AdminServerFrame ready{
                .kind = AdminServerFrameKind::Ready,
                .requestId = hello.requestId,
                .protocolVersion = kAdminProtocolVersion,
                .helperPid = 9999,
                .invokingUid = 1000,
                .nonce = hello.nonce,
            };
            const auto enc = *encodeAdminServerFrame(ready);
            (void)writeAdminFrameFd(outFd, enc, err);
          }
        }
        while (readAdminFrameFd(inFd, buf, line, err) == AdminFrameReadStatus::FrameReady) {
          if (!duringOperation) {
            (void)writeAdminFrameFd(outFd, "CORRUPT_FRAME_FROM_HELPER 2 !!!\n", err);
            break;
          }
          AdminClientRequest request;
          if (decodeAdminClientRequest(line, request) && request.command == AdminClientCommand::OpExec) {
            const auto completed = encodeAdminServerFrame({
                .kind = AdminServerFrameKind::OpCompletedEntry,
                .requestId = request.requestId,
                .completedEntry = {"/fixture/destination/item", 1, 2},
            });
            const auto moved = encodeAdminServerFrame({
                .kind = AdminServerFrameKind::OpMovedSource,
                .requestId = request.requestId,
                .sourcePath = "/fixture/source/item",
            });
            (void)writeAdminFrameFd(outFd, *completed + *moved + "OP_RESULT ", err);
            break;
          }
        }
        ::close(outFd);
        ::close(inFd);
      });

      struct RogueChannelState {
        int writeFd = -1;
        int readFd = -1;
        std::mutex mutex;
        ~RogueChannelState() {
          closeWrite();
          closeRead();
        }
        void closeWrite() {
          std::lock_guard lock(mutex);
          if (writeFd >= 0) {
            ::close(writeFd);
            writeFd = -1;
          }
        }
        void closeRead() {
          std::lock_guard lock(mutex);
          if (readFd >= 0) {
            ::close(readFd);
            readFd = -1;
          }
        }
      };
      auto rogueFds = std::make_shared<RogueChannelState>();
      rogueFds->writeFd = c2h[1];
      rogueFds->readFd = h2c[0];

      AdminSessionClient client(11, alive);
      client.setTransportLauncher([&rogueHelper, rogueFds](GCancellable*) -> AdminTransportLaunchResult {
        auto channel = std::make_unique<AdminTransportChannel>();
        channel->readFd = rogueFds->readFd;
        channel->writeFd = rogueFds->writeFd;
        channel->closeWrite = [rogueFds] { rogueFds->closeWrite(); };
        channel->waitForExit = [&rogueHelper, rogueFds] {
          rogueFds->closeWrite();
          if (rogueHelper.joinable())
            rogueHelper.join();
          rogueFds->closeRead();
          return 1;
        };
        return AdminTransportLaunchResult{.channel = std::move(channel)};
      });

      bool started = false;
      client.startSessionAsync([&](bool ok, const AdminSessionStatus&) { started = ok; });
      pumpUntil([&] { return started; });
      TEST_CHECK(client.isActive());

      if (duringOperation) {
        const auto operation = client.runOperation(
            {.kind = file_operation_engine::Kind::Move,
             .sources = {"/fixture/source/item"},
             .destination = "/fixture/destination"},
            client.sessionGeneration()
        );
        TEST_CHECK(operation.requestId != kInvalidAdminRequestId);
        pumpUntil([&] { return client.state() == AdminSessionState::Failed; });
        const auto frames = operation.takeFrames();
        TEST_CHECK(frames.size() == 3);
        TEST_CHECK(frames[0].kind == AdminServerFrameKind::OpCompletedEntry);
        TEST_CHECK(frames[1].kind == AdminServerFrameKind::OpMovedSource);
        TEST_CHECK(frames[2].kind == AdminServerFrameKind::OpResult);
        TEST_CHECK(frames[2].mutated && frames[2].outcomeUncertain && frames[2].detail.contains("outcome is uncertain"));
        TEST_CHECK(operation.takeFrames().empty());
      } else {
        bool pingFailed = false;
        (void)client.pingAsync([&](const AdminHealthStatus& h) { pingFailed = !h.ok; });
        pumpUntil([&] { return client.state() == AdminSessionState::Failed && pingFailed; });
      }
      TEST_CHECK(client.failureReason() == AdminSessionFailureReason::ProtocolError);
      client.waitForSettled();
    }
  }

  void testPerTabOwnershipIsolationCloseDuringAuthAndNoInheritance() {
    TempDir tmp("nythera-admin-tabs-test");
    const auto dirA = tmp.path / "dirA";
    const auto dirB = tmp.path / "dirB";
    std::filesystem::create_directories(dirA);
    std::filesystem::create_directories(dirB);

    InProcessHelperFixture fixture;
    std::vector<std::pair<TabId, AdminSessionState>> stateNotifications;

    TabManager tabs(
        dirA, TabViewState{},
        TabManager::Callbacks{
            .onTabAdminSessionChanged = [&](BrowserTab& tab, const AdminSessionStatus& st) {
              TEST_CHECK(tab.id == st.tabId);
              stateNotifications.emplace_back(st.tabId, st.state);
            },
        }
    );
    tabs.setAdminTransportLauncherForTesting(fixture.makeLauncher({.helperPid = 4242, .invokingUid = 1000}));

    const TabId tabAId = tabs.activeTabId();
    const TabId tabBId = tabs.openTab(dirB, TabViewState{}, false, std::nullopt, false);
    TEST_CHECK(tabAId != tabBId);

    BrowserTab* tabA = tabs.findTab(tabAId);
    BrowserTab* tabB = tabs.findTab(tabBId);
    TEST_CHECK(tabA != nullptr && tabB != nullptr);
    TEST_CHECK(!tabA->hasActiveAdminSession() && !tabB->hasActiveAdminSession());

    // 1. Authenticate Tab A only; Tab B remains unprivileged.
    bool tabAActive = false;
    TEST_CHECK(tabA->adminSession.startSessionAsync([&](bool ok, const AdminSessionStatus& st) {
      tabAActive = ok && st.tabId == tabAId;
    }));
    pumpUntil([&] { return tabAActive; });
    TEST_CHECK(tabA->hasActiveAdminSession());
    TEST_CHECK(!tabB->hasActiveAdminSession());
    TEST_CHECK(fixture.activeHelpers.load() == 1);

    // 2. Duplicate Tab A -> duplicated tab must NOT inherit Administrator session.
    const TabId dupTabId = tabs.duplicateTab(tabAId);
    TEST_CHECK(dupTabId != kInvalidTabId && dupTabId != tabAId);
    BrowserTab* dupTab = tabs.findTab(dupTabId);
    TEST_CHECK(dupTab != nullptr);
    TEST_CHECK(dupTab->session.path() == tabA->session.path());
    TEST_CHECK(!dupTab->hasActiveAdminSession());
    TEST_CHECK(dupTab->adminSessionState() == AdminSessionState::Inactive);
    TEST_CHECK(tabA->hasActiveAdminSession());
    TEST_CHECK(fixture.activeHelpers.load() == 1);

    // 3. Authenticate Tab B independently; verify distinct helper sessions.
    bool tabBActive = false;
    TEST_CHECK(tabB->adminSession.startSessionAsync([&](bool ok, const AdminSessionStatus& st) {
      tabBActive = ok && st.tabId == tabBId;
    }));
    pumpUntil([&] { return tabBActive; });
    TEST_CHECK(tabA->hasActiveAdminSession() && tabB->hasActiveAdminSession());
    TEST_CHECK(tabA->adminSession.helperPid() != tabB->adminSession.helperPid());
    TEST_CHECK(fixture.activeHelpers.load() == 2);

    // 4. Close Tab A -> terminates Tab A's helper immediately while Tab B's helper remains active.
    TEST_CHECK(tabs.closeTab(tabAId));
    TEST_CHECK(tabs.findTab(tabAId) == nullptr);
    pumpUntil([&] { return fixture.activeHelpers.load() == 1; });
    TEST_CHECK(tabB->hasActiveAdminSession());

    bool pingBOk = false;
    (void)tabB->adminSession.pingAsync([&](const AdminHealthStatus& h) { pingBOk = h.ok; });
    pumpUntil([&] { return pingBOk; });

    // 5. Reopen closed Tab A -> reopened tab must NOT inherit Administrator privilege.
    const TabId reopenedId = tabs.reopenClosedTab();
    TEST_CHECK(reopenedId != kInvalidTabId);
    BrowserTab* reopenedTab = tabs.findTab(reopenedId);
    TEST_CHECK(reopenedTab != nullptr);
    TEST_CHECK(!reopenedTab->hasActiveAdminSession());
    TEST_CHECK(reopenedTab->adminSessionState() == AdminSessionState::Inactive);
    TEST_CHECK(fixture.activeHelpers.load() == 1);

    // 6. Open Tab C, start slow authentication, and close Tab C mid-authentication.
    //    Late callbacks from Tab C must never fire or activate privilege on any remaining tab.
    const TabId tabCId = tabs.openTab(dirA, TabViewState{}, true, std::nullopt, false);
    BrowserTab* tabC = tabs.findTab(tabCId);
    TEST_CHECK(tabC != nullptr);
    tabC->adminSession.setTransportLauncher(fixture.makeLauncher({.helperPid = 8888, .invokingUid = 1000}, 150));

    bool lateCallbackOnClosedTab = false;
    TEST_CHECK(tabC->adminSession.startSessionAsync([&](bool, const AdminSessionStatus&) {
      lateCallbackOnClosedTab = true;
    }));
    TEST_CHECK(tabC->adminSession.isAuthenticating());
    TEST_CHECK(tabs.closeTab(tabCId));
    TEST_CHECK(tabs.findTab(tabCId) == nullptr);

    drainMainContext(20);
    TEST_CHECK(!lateCallbackOnClosedTab);
    TEST_CHECK(!reopenedTab->hasActiveAdminSession());
    TEST_CHECK(!dupTab->hasActiveAdminSession());
    TEST_CHECK(tabB->hasActiveAdminSession());
    pumpUntil([&] { return fixture.activeHelpers.load() == 1; });

    // 7. Authenticate reopenedTab as well, then simulate window close (`shutdownAll()`) with multiple sessions.
    bool reopenedActive = false;
    TEST_CHECK(reopenedTab->adminSession.startSessionAsync([&](bool ok, const AdminSessionStatus&) {
      reopenedActive = ok;
    }));
    pumpUntil([&] { return reopenedActive; });
    TEST_CHECK(fixture.activeHelpers.load() == 2);

    tabs.shutdownAll();
    pumpUntil([&] { return fixture.activeHelpers.load() == 0; });
    for (const auto& t : tabs.tabs())
      t->adminSession.waitForSettled();
    TEST_CHECK(fixture.totalSpawned.load() == fixture.totalExited.load());
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-admin-stage1"});
    TEST_CHECK(geteuid() != 0);

    testSharedPolkitValidationAndInvocationChecks();
    testProtocolEncodingValidationAndPartialFraming();
    testHelperSessionDirectProtocolAndEdgeCases();
    testSharedOperationTransport(env.work());
    testSharedOperationExpiry(env.work());
    testClientHandshakeAuthDenialCancellationAndVersionMismatch();
    testUnexpectedHelperTerminationAndMalformedServerFrame();
    testPerTabOwnershipIsolationCloseDuringAuthAndNoInheritance();
  });
}
