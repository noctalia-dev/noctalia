#include "filemanager/admin_session_client.h"

#include "filemanager/tab_presentation.h"

#include "admin_helper_paths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace filemanager {
  namespace {

    constexpr PolkitHelperTarget kAdminHelperTarget{
        .helperPath = NYTHERA_ADMIN_HELPER_PATH,
        .policyPath = NYTHERA_ADMIN_POLICY_PATH,
        .actionId = "dev.nythera.Files.admin-session",
        .displayName = "Administrator",
    };

    void invokeOnMainContext(std::function<void()> fn) {
      auto* heapFn = new std::function<void()>(std::move(fn));
      // Always dispatch on the UI loop, even before it has acquired the default context.
      GSource* source = g_idle_source_new();
      g_source_set_priority(source, G_PRIORITY_DEFAULT);
      g_source_set_callback(
          source,
          [](gpointer data) -> gboolean {
            auto* callback = static_cast<std::function<void()>*>(data);
            (*callback)();
            return G_SOURCE_REMOVE;
          },
          heapFn,
          [](gpointer data) { delete static_cast<std::function<void()>*>(data); }
      );
      g_source_attach(source, g_main_context_default());
      g_source_unref(source);
    }

    std::string makeSessionNonce(TabId tabId, std::uint64_t generation) {
      const guint32 randA = g_random_int();
      const guint32 randB = g_random_int();
      return "tab-"
          + std::to_string(tabId)
          + "-gen-"
          + std::to_string(generation)
          + "-"
          + std::to_string(randA)
          + "-"
          + std::to_string(randB);
    }

  } // namespace

  PolkitHelperTarget defaultAdminPolkitTarget() noexcept { return kAdminHelperTarget; }

  AdminTransportLaunchResult launchPolkitAdminTransport(GCancellable* cancellable) {
    AdminTransportLaunchResult out;
    if (!verifyInstalledPolkitTarget(kAdminHelperTarget, out.error)) {
      out.failureReason = AdminSessionFailureReason::NotInstalled;
      return out;
    }
    if (!verifyRegisteredPolkitAction(kAdminHelperTarget, cancellable, out.cancelled, out.error)) {
      out.failureReason = out.cancelled ? AdminSessionFailureReason::AuthCancelled
                                        : AdminSessionFailureReason::ActionNotRegistered;
      return out;
    }

    GSubprocess* process = spawnPolkitHelperProcess(kAdminHelperTarget.helperPath, {}, out.error);
    if (process == nullptr) {
      out.failureReason = AdminSessionFailureReason::TransportError;
      return out;
    }

    GOutputStream* stdinStream = g_subprocess_get_stdin_pipe(process);
    GInputStream* stdoutStream = g_subprocess_get_stdout_pipe(process);
    if (!G_IS_UNIX_OUTPUT_STREAM(stdinStream) || !G_IS_UNIX_INPUT_STREAM(stdoutStream)) {
      g_subprocess_force_exit(process);
      g_object_unref(process);
      out.failureReason = AdminSessionFailureReason::TransportError;
      out.error = "Helper pipes do not expose pollable Unix file descriptors.";
      return out;
    }

    const int writeFd = g_unix_output_stream_get_fd(G_UNIX_OUTPUT_STREAM(stdinStream));
    const int readFd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stdoutStream));
    struct SubprocessHolder {
      GSubprocess* proc;
      GOutputStream* input;
      GCancellable* cancellable;

      SubprocessHolder(GSubprocess* process, GOutputStream* in, GCancellable* cancel)
          : proc(process), input(in), cancellable(cancel ? G_CANCELLABLE(g_object_ref(cancel)) : nullptr) {}
      ~SubprocessHolder() {
        closeWrite();
        g_object_unref(proc);
        g_clear_object(&cancellable);
      }

      void closeWrite() { (void)g_output_stream_close(input, nullptr, nullptr); }

      int waitExit() {
        closeWrite();
        // Cancellation closes stdin and lets the UI retire the worker even while a desktop agent
        // is still presenting the pkexec prompt or the helper is blocked inside the kernel.
        if (!g_subprocess_wait(proc, cancellable, nullptr))
          return -1;
        return g_subprocess_get_if_exited(proc) ? g_subprocess_get_exit_status(proc) : -1;
      }
    };

    auto holder = std::make_shared<SubprocessHolder>(process, stdinStream, cancellable);
    auto channel = std::make_unique<AdminTransportChannel>();
    channel->readFd = readFd;
    channel->writeFd = writeFd;
    channel->closeWrite = [holder] { holder->closeWrite(); };
    channel->abortTransport = [holder] { holder->closeWrite(); };
    channel->waitForExit = [holder] { return holder->waitExit(); };
    out.channel = std::move(channel);
    return out;
  }

  namespace {
    struct OperationMailbox {
      std::mutex mutex;
      std::vector<AdminServerFrame> frames;
      bool finished = false;

      void push(AdminServerFrame frame) {
        std::lock_guard lock(mutex);
        if (finished)
          return;
        finished = frame.kind == AdminServerFrameKind::OpResult;
        if (!frames.empty() && frame.kind == AdminServerFrameKind::OpProgress && frames.back().kind == frame.kind)
          frames.back() = std::move(frame);
        else
          frames.push_back(std::move(frame));
      }

      std::vector<AdminServerFrame> take() {
        std::lock_guard lock(mutex);
        return std::exchange(frames, {});
      }
    };

    bool isOperationFrame(AdminServerFrameKind kind) {
      return kind >= AdminServerFrameKind::OpConflict && kind <= AdminServerFrameKind::OpResult;
    }
  } // namespace

  struct AdminSessionClient::WorkerContext {
    struct OutgoingItem {
      AdminRequestId requestId = kInvalidAdminRequestId;
      AdminClientCommand command = AdminClientCommand::Ping;
      std::string encodedFrame;
      PingCallback onPing;
      RequestCallback onRequest;
      DirBatchCallback onDirBatch;
      DirCompleteCallback onDirComplete;
      StatCallback onStat;
      StatVfsCallback onStatVfs;
      CountDirCallback onCountDir;
      MutationCallback onMutation;
      std::vector<std::string> copyNames;
      CopyFrameCallback onCopyFrame;
      std::shared_ptr<OperationMailbox> operation;
      std::size_t operationSources = 0;
    };

    TabId tabId = kInvalidTabId;
    std::uint64_t generation = 0;
    std::uint32_t protocolVersion = kAdminProtocolVersion;
    std::uint32_t inactivityTimeoutMs = static_cast<std::uint32_t>(kDefaultAdminInactivityTimeoutSeconds * 1000);
    std::string nonce;
    GCancellable* cancellable = nullptr;
    int wakeupFd = -1;

    std::mutex mutex;
    std::deque<OutgoingItem> outgoingQueue;
    AdminRequestId activeOperation = kInvalidAdminRequestId;
    bool stopRequested = false;
    bool gracefulStop = false;
    bool cancelledAuth = false;
    AdminRequestId nextRequestId = 2; // 1 is reserved for HELLO.
    std::atomic<bool> finished{false};
    std::thread thread;

    WorkerContext() : cancellable(g_cancellable_new()), wakeupFd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

    ~WorkerContext() {
      if (wakeupFd >= 0)
        close(wakeupFd);
      if (cancellable != nullptr)
        g_object_unref(cancellable);
    }

    void signalWakeup() const noexcept {
      if (wakeupFd >= 0) {
        const std::uint64_t one = 1;
        (void)::write(wakeupFd, &one, sizeof(one));
      }
    }

    void drainWakeup() const noexcept {
      if (wakeupFd >= 0) {
        std::uint64_t value = 0;
        while (::read(wakeupFd, &value, sizeof(value)) > 0) {
        }
      }
    }

    void requestStop(bool graceful, bool isCancelAuth) {
      {
        std::lock_guard lock(mutex);
        stopRequested = true;
        gracefulStop = graceful && !isCancelAuth;
        if (isCancelAuth)
          cancelledAuth = true;
      }
      g_cancellable_cancel(cancellable);
      signalWakeup();
    }
  };

  AdminSessionClient::AdminSessionClient(
      TabId tabId, std::shared_ptr<bool> tabAlive, StateChangedCallback onStateChanged
  )
      : m_tabId(tabId),
        m_tabAlive(std::move(tabAlive)),
        m_onStateChanged(std::move(onStateChanged)),
        m_launcher(launchPolkitAdminTransport) {}

  AdminSessionClient::~AdminSessionClient() {
    shutdown();
    waitForSettled();
  }

  AdminSessionStatus AdminSessionClient::status() const {
    return AdminSessionStatus{
        .tabId = m_tabId,
        .state = m_state,
        .failureReason = m_failureReason,
        .generation = m_sessionGeneration,
        .protocolVersion = m_negotiatedProtocolVersion,
        .helperPid = m_helperPid,
        .invokingUid = m_invokingUid,
        .message = m_lastMessage,
    };
  }

  void AdminSessionClient::setStateChangedCallback(StateChangedCallback callback) {
    m_onStateChanged = std::move(callback);
  }

  void AdminSessionClient::setTransportLauncher(AdminTransportLauncher launcher) {
    m_launcher = launcher ? std::move(launcher) : AdminTransportLauncher(launchPolkitAdminTransport);
  }

  void AdminSessionClient::setInactivityTimeoutMs(std::uint32_t timeoutMs, bool isTestOverride) noexcept {
    constexpr std::uint32_t kMinMs = 10U;
    constexpr std::uint32_t kMaxMs = static_cast<std::uint32_t>(kMaxAdminInactivityTimeoutSeconds * 1000);
    m_inactivityTimeoutMs = std::clamp(timeoutMs, kMinMs, kMaxMs);
    if (isTestOverride)
      m_customTimeoutForTesting = true;
  }

  void AdminSessionClient::transitionTo(
      AdminSessionState newState, AdminSessionFailureReason reason, std::string message, bool notify
  ) {
    m_state = newState;
    m_failureReason = reason;
    m_lastMessage = std::move(message);
    if (newState != AdminSessionState::Active) {
      m_negotiatedProtocolVersion = 0;
      m_helperPid = 0;
      m_invokingUid = 0;
    }
    if (notify && !m_shutDown && m_onStateChanged) {
      const auto lockedTab = m_tabAlive ? m_tabAlive : std::shared_ptr<bool>{};
      if (!lockedTab || *lockedTab)
        m_onStateChanged(status());
    }
  }

  void AdminSessionClient::reapCompletedWorkersLocked() {
    for (auto it = m_allWorkers.begin(); it != m_allWorkers.end();) {
      if ((*it)->finished.load(std::memory_order_acquire)) {
        if ((*it)->thread.joinable())
          (*it)->thread.join();
        it = m_allWorkers.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool AdminSessionClient::startSessionAsync(StartCallback onComplete) {
    if (m_shutDown || (m_tabAlive && !*m_tabAlive))
      return false;
    if (m_state == AdminSessionState::Authenticating || m_state == AdminSessionState::Active)
      return false;

    if (m_activeWorker) {
      m_activeWorker->requestStop(false, false);
      m_activeWorker.reset();
    }

    const std::uint64_t generation = ++m_sessionGeneration;
    auto worker = std::make_shared<WorkerContext>();
    worker->tabId = m_tabId;
    worker->generation = generation;
    worker->protocolVersion = m_clientProtocolVersion;
    worker->inactivityTimeoutMs = m_inactivityTimeoutMs;
    worker->nonce = makeSessionNonce(m_tabId, generation);

    if (worker->wakeupFd < 0) {
      transitionTo(
          AdminSessionState::Failed, AdminSessionFailureReason::TransportError,
          "Could not allocate session event descriptor."
      );
      if (onComplete)
        onComplete(false, status());
      return false;
    }

    m_activeWorker = worker;
    {
      std::lock_guard lock(m_workersMutex);
      reapCompletedWorkersLocked();
      m_allWorkers.push_back(worker);
    }

    transitionTo(
        AdminSessionState::Authenticating, AdminSessionFailureReason::None, "Authenticating Administrator session…"
    );

    const AdminTransportLauncher launcher = m_launcher ? m_launcher : AdminTransportLauncher(launchPolkitAdminTransport);
    const TabId expectedTabId = m_tabId;
    std::weak_ptr<bool> weakTabAlive = m_tabAlive;
    std::weak_ptr<bool> weakClientAlive = m_clientAlive;

    worker->thread = std::thread([this, worker, launcher, expectedTabId, generation, weakTabAlive, weakClientAlive,
                                  startCb = std::move(onComplete)]() mutable {
      auto postToClient = [this, expectedTabId, generation, weakTabAlive,
                           weakClientAlive](std::function<void(AdminSessionClient&)> fn) {
        invokeOnMainContext([this, expectedTabId, generation, weakTabAlive, weakClientAlive, fn = std::move(fn)] {
          const auto tabLocked = weakTabAlive.lock();
          const auto clientLocked = weakClientAlive.lock();
          if ((weakTabAlive.owner_before(std::weak_ptr<bool>{}) || std::weak_ptr<bool>{}.owner_before(weakTabAlive))
              && (!tabLocked || !*tabLocked)) {
            return;
          }
          if (!clientLocked || !*clientLocked || m_shutDown)
            return;
          if (m_tabId != expectedTabId || m_sessionGeneration != generation)
            return;
          fn(*this);
        });
      };

      auto finishStartFailure = [&](AdminSessionFailureReason reason, std::string message) {
        postToClient([reason, msg = std::move(message), cb = std::move(startCb)](AdminSessionClient& self) mutable {
          self.m_activeWorker.reset();
          if (reason == AdminSessionFailureReason::AuthCancelled) {
            self.transitionTo(AdminSessionState::Inactive, AdminSessionFailureReason::AuthCancelled, std::move(msg));
          } else {
            self.transitionTo(AdminSessionState::Failed, reason, std::move(msg));
          }
          if (cb)
            cb(false, self.status());
        });
        worker->finished.store(true, std::memory_order_release);
      };

      // 1. Launch transport (Polkit checks + pkexec spawn or test fixture).
      AdminTransportLaunchResult launched = launcher(worker->cancellable);
      if (!launched.channel) {
        bool wasCancelled = launched.cancelled || (g_cancellable_is_cancelled(worker->cancellable) != FALSE);
        {
          std::lock_guard lock(worker->mutex);
          wasCancelled = wasCancelled || worker->cancelledAuth || worker->stopRequested;
        }
        const AdminSessionFailureReason reason = wasCancelled ? AdminSessionFailureReason::AuthCancelled
            : launched.failureReason != AdminSessionFailureReason::None ? launched.failureReason
                                                                        : AdminSessionFailureReason::TransportError;
        std::string err = launched.error;
        if (err.empty()) {
          err = reason == AdminSessionFailureReason::AuthCancelled
              ? "Administrator authentication was cancelled."
              : "Could not start Administrator helper.";
        }
        finishStartFailure(reason, std::move(err));
        return;
      }

      auto channel = std::move(launched.channel);
      // The worker owns the transport descriptors. The UI only requests cancellation and wakes it.
      for (const int fd : {channel->readFd, channel->writeFd}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
          if (channel->closeWrite)
            channel->closeWrite();
          if (channel->waitForExit)
            (void)channel->waitForExit();
          finishStartFailure(AdminSessionFailureReason::TransportError, "Could not configure helper transport.");
          return;
        }
      }

      // 2. Send HELLO handshake frame.
      AdminClientRequest helloReq;
      helloReq.command = AdminClientCommand::Hello;
      helloReq.requestId = 1;
      helloReq.protocolVersion = worker->protocolVersion;
      helloReq.inactivityTimeoutMs = worker->inactivityTimeoutMs;
      helloReq.nonce = worker->nonce;
      const auto encodedHello = encodeAdminClientRequest(helloReq);
      int writeErrno = 0;
      if (!encodedHello || !writeAdminFrameFd(channel->writeFd, *encodedHello, writeErrno)) {
        if (channel->closeWrite)
          channel->closeWrite();
        const int exitCode = channel->waitForExit ? channel->waitForExit() : -1;
        bool cancelled = g_cancellable_is_cancelled(worker->cancellable) != FALSE;
        {
          std::lock_guard lock(worker->mutex);
          cancelled = cancelled || worker->cancelledAuth || worker->stopRequested;
        }
        const auto classified = classifyPolkitExitStatus(exitCode, cancelled);
        const AdminSessionFailureReason reason = classified.cancelled ? AdminSessionFailureReason::AuthCancelled
            : classified.denied                                       ? AdminSessionFailureReason::AuthDenied
                                                                      : AdminSessionFailureReason::TransportError;
        finishStartFailure(reason, classified.error);
        return;
      }

      // 3. Wait for READY response (or cancellation / Polkit denial / version mismatch).
      AdminLineBuffer lineBuffer(kAdminMaxFrameBytes);
      bool handshakeOk = false;
      std::uint32_t negotiatedVersion = 0;
      std::uint32_t helperPid = 0;
      std::uint32_t invokingUid = 0;

      while (!handshakeOk) {
        bool stopRequested;
        {
          std::lock_guard lock(worker->mutex);
          stopRequested = worker->stopRequested;
        }
        if (stopRequested) {
          if (channel->abortTransport)
            channel->abortTransport();
          if (channel->closeWrite)
            channel->closeWrite();
          if (channel->waitForExit)
            (void)channel->waitForExit();
          finishStartFailure(
              AdminSessionFailureReason::AuthCancelled, "Administrator authentication was cancelled."
          );
          return;
        }

        std::string frameLine;
        const auto popStatus = lineBuffer.popFrame(frameLine);
        if (popStatus == AdminFrameReadStatus::OversizedFrame || popStatus == AdminFrameReadStatus::MalformedFrame) {
          if (channel->closeWrite)
            channel->closeWrite();
          if (channel->waitForExit)
            (void)channel->waitForExit();
          finishStartFailure(
              AdminSessionFailureReason::ProtocolError, "Received invalid handshake frame from Administrator helper."
          );
          return;
        }
        if (popStatus == AdminFrameReadStatus::FrameReady) {
          AdminServerFrame frame;
          AdminParseError parseErr;
          if (!decodeAdminServerFrame(frameLine, frame, &parseErr) || frame.requestId != 1) {
            if (channel->closeWrite)
              channel->closeWrite();
            if (channel->waitForExit)
              (void)channel->waitForExit();
            finishStartFailure(
                AdminSessionFailureReason::ProtocolError, "Malformed handshake response from Administrator helper."
            );
            return;
          }
          if (frame.kind == AdminServerFrameKind::Response) {
            if (channel->closeWrite)
              channel->closeWrite();
            if (channel->waitForExit)
              (void)channel->waitForExit();
            const AdminSessionFailureReason reason = frame.status == AdminStatusCode::VersionMismatch
                ? AdminSessionFailureReason::VersionMismatch
                : AdminSessionFailureReason::ProtocolError;
            finishStartFailure(
                reason,
                frame.detail.empty() ? "Administrator helper rejected protocol handshake." : frame.detail
            );
            return;
          }
          if (frame.kind != AdminServerFrameKind::Ready
              || frame.protocolVersion != kAdminProtocolVersion
              || frame.nonce != worker->nonce) {
            if (channel->closeWrite)
              channel->closeWrite();
            if (channel->waitForExit)
              (void)channel->waitForExit();
            const AdminSessionFailureReason reason = frame.protocolVersion != kAdminProtocolVersion
                ? AdminSessionFailureReason::VersionMismatch
                : AdminSessionFailureReason::ProtocolError;
            finishStartFailure(reason, "Administrator helper protocol version or session token mismatch.");
            return;
          }
          handshakeOk = true;
          negotiatedVersion = frame.protocolVersion;
          helperPid = frame.helperPid;
          invokingUid = frame.invokingUid;
          break;
        }

        std::array<pollfd, 2> pfds{{
            {channel->readFd, POLLIN | POLLHUP, 0},
            {worker->wakeupFd, POLLIN, 0},
        }};
        const int pollRc = ::poll(pfds.data(), pfds.size(), -1);
        if (pollRc < 0) {
          if (errno == EINTR)
            continue;
          if (channel->closeWrite)
            channel->closeWrite();
          if (channel->waitForExit)
            (void)channel->waitForExit();
          finishStartFailure(AdminSessionFailureReason::TransportError, std::strerror(errno));
          return;
        }

        if ((pfds[1].revents & POLLIN) != 0)
          worker->drainWakeup();

        bool sawEof = false;
        if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
          std::array<char, 4096> chunk{};
          while (true) {
            const ssize_t n = ::read(channel->readFd, chunk.data(), chunk.size());
            if (n > 0) {
              lineBuffer.append(std::string_view(chunk.data(), static_cast<std::size_t>(n)));
              break;
            }
            if (n == 0) {
              sawEof = true;
              break;
            }
            if (errno == EINTR)
              continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;
            sawEof = true;
            break;
          }
        }

        if (sawEof) {
          if (channel->closeWrite)
            channel->closeWrite();
          const int exitCode = channel->waitForExit ? channel->waitForExit() : -1;
          bool cancelled = g_cancellable_is_cancelled(worker->cancellable) != FALSE;
          {
            std::lock_guard lock(worker->mutex);
            cancelled = cancelled || worker->cancelledAuth || worker->stopRequested;
          }
          const auto classified = classifyPolkitExitStatus(exitCode, cancelled);
          const AdminSessionFailureReason reason = classified.cancelled ? AdminSessionFailureReason::AuthCancelled
              : classified.denied                                       ? AdminSessionFailureReason::AuthDenied
                                                                        : AdminSessionFailureReason::HelperTerminated;
          if (lineBuffer.hasBufferedBytes() && !cancelled)
            finishStartFailure(AdminSessionFailureReason::ProtocolError, "Truncated helper handshake frame.");
          else
            finishStartFailure(reason, classified.error);
          return;
        }
      }

      // 4. Handshake succeeded — transition client to Active on main thread.
      postToClient([negotiatedVersion, helperPid, invokingUid,
                    cb = std::move(startCb)](AdminSessionClient& self) mutable {
        self.m_negotiatedProtocolVersion = negotiatedVersion;
        self.m_helperPid = helperPid;
        self.m_invokingUid = invokingUid;
        self.m_lastRenewSentAt = std::chrono::steady_clock::now();
        self.transitionTo(
            AdminSessionState::Active, AdminSessionFailureReason::None, "Administrator session active."
        );
        if (cb)
          cb(true, self.status());
      });

      // 5. Active session loop: multiplex outgoing commands and incoming frames/EOF.
      struct PendingRequest {
        AdminClientCommand command = AdminClientCommand::Ping;
        std::uint64_t receivedEntries = 0;
        std::optional<std::uint64_t> copyTotalBytes;
        std::uint64_t copyCompletedBytes = 0;
        std::uint64_t copyTotalItems = 0;
        std::uint64_t copyCompletedItems = 0;
        std::size_t copyPublished = 0;
        bool copySent = false;
        PingCallback onPing;
        RequestCallback onRequest;
        DirBatchCallback onDirBatch;
        DirCompleteCallback onDirComplete;
        StatCallback onStat;
        StatVfsCallback onStatVfs;
        CountDirCallback onCountDir;
        MutationCallback onMutation;
        std::vector<std::string> copyNames;
        CopyFrameCallback onCopyFrame;
        std::shared_ptr<OperationMailbox> operation;
        std::size_t operationSources = 0;
        std::size_t operationCompleted = 0;
        std::size_t operationMoved = 0;
        bool operationSubmitted = false;
        std::uint64_t lastConflict = 0;
        bool operationReportingResult = false;
      };
      std::unordered_map<AdminRequestId, PendingRequest> pendingRequests;
      std::optional<WorkerContext::OutgoingItem> outgoing;

      auto failAllPending = [&](AdminStatusCode code, const std::string& detail) {
        std::vector<PendingRequest> callbacks;
        callbacks.reserve(pendingRequests.size());
        for (auto& [id, req] : pendingRequests) {
          if (req.operation) {
            req.operation->push(
                {.kind = AdminServerFrameKind::OpResult,
                 .requestId = id,
                 .status = req.operationSubmitted ? AdminStatusCode::InternalError : code,
                 .detail = req.operationSubmitted
                     ? detail + " The final operation outcome is uncertain."
                     : detail,
                 .mutated = req.operationCompleted != 0 || req.operationMoved != 0,
                 .outcomeUncertain = req.operationSubmitted}
            );
          }
          callbacks.push_back(std::move(req));
        }
        pendingRequests.clear();

        auto pushQueued = [&](WorkerContext::OutgoingItem&& item) {
          if (item.operation)
            item.operation->push(
                {.kind = AdminServerFrameKind::OpResult, .requestId = item.requestId, .status = code, .detail = detail}
            );
          if (item.onPing
              || item.onRequest
              || item.onDirComplete
              || item.onStat
              || item.onStatVfs
              || item.onCountDir
              || item.onMutation || item.onCopyFrame) {
            callbacks.push_back(PendingRequest{
                .command = item.command,
                .onPing = std::move(item.onPing),
                .onRequest = std::move(item.onRequest),
                .onDirBatch = std::move(item.onDirBatch),
                .onDirComplete = std::move(item.onDirComplete),
                .onStat = std::move(item.onStat),
                .onStatVfs = std::move(item.onStatVfs),
                .onCountDir = std::move(item.onCountDir),
                .onMutation = std::move(item.onMutation),
                .copyNames = std::move(item.copyNames),
                .onCopyFrame = std::move(item.onCopyFrame),
            });
          }
        };
        if (outgoing) {
          pushQueued(std::move(*outgoing));
          outgoing.reset();
        }
        {
          std::lock_guard lock(worker->mutex);
          worker->stopRequested = true;
          while (!worker->outgoingQueue.empty()) {
            pushQueued(std::move(worker->outgoingQueue.front()));
            worker->outgoingQueue.pop_front();
          }
        }
        for (auto& req : callbacks) {
          if (req.onCopyFrame) {
            req.onCopyFrame(AdminServerFrame{
                .status = code,
                .detail = req.copySent && req.copyPublished < req.copyNames.size()
                    ? detail + " Check the destination for any item published before the connection closed."
                    : detail,
            });
            req.onCopyFrame = {};
          }
        }
        if (callbacks.empty())
          return;

        // Deliver failure callbacks even after stopSession() advances m_sessionGeneration;
        // still guard against client/tab destruction, explicit shutdown, or tab reassignment.
        invokeOnMainContext([this, expectedTabId, weakTabAlive, weakClientAlive, callbacks = std::move(callbacks), code,
                             detail]() mutable {
          const auto tabLocked = weakTabAlive.lock();
          const auto clientLocked = weakClientAlive.lock();
          if ((weakTabAlive.owner_before(std::weak_ptr<bool>{}) || std::weak_ptr<bool>{}.owner_before(weakTabAlive))
              && (!tabLocked || !*tabLocked)) {
            return;
          }
          if (!clientLocked || !*clientLocked || m_shutDown)
            return;
          if (m_tabId != expectedTabId)
            return;

          for (auto& req : callbacks) {
            if (req.onPing) {
              req.onPing(
                  AdminHealthStatus{
                      .ok = false,
                      .statusCode = code,
                      .detail = detail,
                  }
              );
            }
            if (req.onRequest)
              req.onRequest(false, code, detail);
            if (req.onDirComplete)
              req.onDirComplete(false, false, 0, code, detail);
            if (req.onStat)
              req.onStat(false, AdminStatRecord{}, code, detail);
            if (req.onStatVfs)
              req.onStatVfs(false, AdminStatVfsRecord{}, code, detail);
            if (req.onCountDir)
              req.onCountDir(false, 0, code, detail);
            if (req.onMutation)
              req.onMutation(false, AdminMutationRecord{}, code, detail);
          }
        });
      };

      bool unexpectedExit = false;
      AdminSessionFailureReason exitFailureReason = AdminSessionFailureReason::HelperTerminated;
      std::string exitMessage = "The Administrator helper session ended unexpectedly.";

      std::size_t written = 0;
      bool writeClosed = false;
      while (true) {
        bool stopNow = false;
        bool graceful = false;
        {
          std::lock_guard lock(worker->mutex);
          if (!outgoing && !worker->outgoingQueue.empty()) {
            outgoing.emplace(std::move(worker->outgoingQueue.front()));
            worker->outgoingQueue.pop_front();
          }
          stopNow = worker->stopRequested || writeClosed;
          graceful = worker->gracefulStop;
        }

        if (outgoing && !stopNow) {
          if (outgoing->operation) {
            pendingRequests[outgoing->requestId] = PendingRequest{
                .command = AdminClientCommand::OpExec,
                .operation = std::move(outgoing->operation),
                .operationSources = outgoing->operationSources
            };
          }
          const auto remaining = std::string_view(outgoing->encodedFrame).substr(written);
          const ssize_t n = ::write(channel->writeFd, remaining.data(), remaining.size());
          if (n > 0) {
            written += static_cast<std::size_t>(n);
          } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            unexpectedExit = true;
            exitFailureReason = AdminSessionFailureReason::TransportError;
            exitMessage = "Failed to send request to Administrator helper.";
            stopNow = true;
            graceful = false;
          }
        }
        if (outgoing && written == outgoing->encodedFrame.size()) {
          auto item = std::move(*outgoing);
          outgoing.reset();
          written = 0;
          if (const auto pending = pendingRequests.find(item.requestId);
              pending != pendingRequests.end() && pending->second.operation) {
            pending->second.operationSubmitted = true;
          }
          if (item.requestId != kInvalidAdminRequestId
              && (item.onPing
                  || item.onRequest
                  || item.onDirBatch
                  || item.onDirComplete
                  || item.onStat
                  || item.onStatVfs
                  || item.onCountDir
                  || item.onMutation || item.onCopyFrame)) {
            pendingRequests[item.requestId] = PendingRequest{
                .command = item.command,
                .copySent = static_cast<bool>(item.onCopyFrame),
                .onPing = std::move(item.onPing),
                .onRequest = std::move(item.onRequest),
                .onDirBatch = std::move(item.onDirBatch),
                .onDirComplete = std::move(item.onDirComplete),
                .onStat = std::move(item.onStat),
                .onStatVfs = std::move(item.onStatVfs),
                .onCountDir = std::move(item.onCountDir),
                .onMutation = std::move(item.onMutation),
                .copyNames = std::move(item.copyNames),
                .onCopyFrame = std::move(item.onCopyFrame),
            };
          }
        }

        if (stopNow && !writeClosed) {
          if (graceful && written == 0) {
            AdminClientRequest shutdownReq;
            shutdownReq.command = AdminClientCommand::Shutdown;
            shutdownReq.requestId = 999999999ULL;
            if (const auto encoded = encodeAdminClientRequest(shutdownReq))
              (void)::write(channel->writeFd, encoded->data(), encoded->size());
          }
          if (channel->closeWrite)
            channel->closeWrite();
          const bool sentCopy = std::ranges::any_of(pendingRequests, [](const auto& pending) {
            return pending.second.onCopyFrame || pending.second.operation;
          });
          if (!sentCopy) {
            if (channel->waitForExit)
              (void)channel->waitForExit();
            failAllPending(
                unexpectedExit ? AdminStatusCode::InternalError : AdminStatusCode::Cancelled,
                unexpectedExit ? exitMessage : "Administrator session stopped."
            );
            break;
          }
          // Drain terminal replies before reaping the transport. A Copy may
          // already have committed when Finish or tab closure arrives.
          writeClosed = true;
        }

        bool frameError = false;
        std::string frameLine;
        while (true) {
          const auto popStatus = lineBuffer.popFrame(frameLine);
          if (popStatus == AdminFrameReadStatus::NeedMoreData)
            break;
          if (popStatus == AdminFrameReadStatus::OversizedFrame || popStatus == AdminFrameReadStatus::MalformedFrame) {
            unexpectedExit = true;
            exitFailureReason = AdminSessionFailureReason::ProtocolError;
            exitMessage = popStatus == AdminFrameReadStatus::OversizedFrame
                ? "Administrator helper sent an oversized frame."
                : "Administrator helper sent a malformed frame.";
            frameError = true;
            break;
          }

          AdminServerFrame frame;
          AdminParseError parseErr;
          if (!decodeAdminServerFrame(frameLine, frame, &parseErr)) {
            unexpectedExit = true;
            exitFailureReason = AdminSessionFailureReason::ProtocolError;
            exitMessage = parseErr.message.empty() ? "Invalid frame from Administrator helper." : parseErr.message;
            frameError = true;
            break;
          }

          auto it = pendingRequests.find(frame.requestId);
          if (it != pendingRequests.end() && it->second.operation) {
            auto& pending = it->second;
            using Kind = AdminServerFrameKind;
            const bool terminal =
                frame.kind == Kind::OpResult || (frame.kind == Kind::Response && frame.status != AdminStatusCode::Ok);
            bool valid = terminal;
            if (frame.kind == Kind::OpCompletedEntry || frame.kind == Kind::OpMovedSource) {
              pending.operationReportingResult = true;
              auto& count = frame.kind == Kind::OpCompletedEntry ? pending.operationCompleted : pending.operationMoved;
              valid = ++count <= pending.operationSources;
            } else if (!pending.operationReportingResult && frame.kind == Kind::OpConflict) {
              valid = frame.conflictId > pending.lastConflict;
              pending.lastConflict = frame.conflictId;
            } else if (
                !pending.operationReportingResult && (frame.kind == Kind::OpProgress || frame.kind == Kind::OpPaused)
            ) {
              valid = true; // Merged subtrees may change the measured totals.
            }
            if (!valid) {
              unexpectedExit = frameError = true;
              exitFailureReason = AdminSessionFailureReason::ProtocolError;
              exitMessage = "Invalid shared operation response.";
              break;
            }
            if (terminal) {
              frame.kind = Kind::OpResult;
              std::lock_guard lock(worker->mutex);
              if (worker->activeOperation == frame.requestId)
                worker->activeOperation = kInvalidAdminRequestId;
            }
            pending.operation->push(std::move(frame));
            if (terminal)
              pendingRequests.erase(it);
            continue;
          }
          if (isOperationFrame(frame.kind)) {
            unexpectedExit = frameError = true;
            exitFailureReason = AdminSessionFailureReason::ProtocolError;
            exitMessage = "Unexpected shared operation response.";
            break;
          }
          if (it != pendingRequests.end()) {
            if (it->second.onCopyFrame) {
              auto& pending = it->second;
              const bool progress = frame.kind == AdminServerFrameKind::Progress;
              const bool published = frame.kind == AdminServerFrameKind::CopyItem;
              const bool validProgress = progress
                  && frame.totalItems >= pending.copyNames.size()
                  && frame.totalItems <= kAdminMaxDirectoryEntries
                  && (pending.copyTotalItems == 0 || pending.copyTotalItems == frame.totalItems)
                  && frame.completedItems >= pending.copyCompletedItems
                  && frame.completedItems <= frame.totalItems
                  && frame.completedBytes >= pending.copyCompletedBytes
                  && frame.completedBytes <= frame.totalBytes
                  && pending.copyTotalBytes.value_or(frame.totalBytes) == frame.totalBytes;
              const bool validPublication = published && pending.copyTotalItems != 0
                  && pending.copyPublished < pending.copyNames.size()
                  && frame.mutationInfo.inode != 0
                  && frame.mutationInfo.name == pending.copyNames[pending.copyPublished];
              const bool terminal = frame.kind == AdminServerFrameKind::Response
                  && (frame.status != AdminStatusCode::Ok
                      || (pending.copyPublished == pending.copyNames.size()
                          && pending.copyCompletedItems == pending.copyTotalItems
                          && pending.copyTotalBytes == pending.copyCompletedBytes));
              if (!validProgress && !validPublication && !terminal) {
                unexpectedExit = frameError = true;
                exitFailureReason = AdminSessionFailureReason::ProtocolError;
                exitMessage = "Invalid Administrator copy response.";
                break;
              }
              if (progress) {
                pending.copyTotalBytes = frame.totalBytes;
                pending.copyCompletedBytes = frame.completedBytes;
                pending.copyTotalItems = frame.totalItems;
                pending.copyCompletedItems = frame.completedItems;
              }
              if (published)
                ++pending.copyPublished;
              pending.onCopyFrame(frame);
              if (terminal)
                pendingRequests.erase(it);
              continue;
            }
            if (writeClosed)
              continue; // Existing browsing and mutation callbacks are cancelled when their tab stops.

            if (frame.kind == AdminServerFrameKind::Progress || frame.kind == AdminServerFrameKind::CopyItem) {
              unexpectedExit = frameError = true;
              exitFailureReason = AdminSessionFailureReason::ProtocolError;
              exitMessage = "Unexpected Administrator copy frame.";
              break;
            }

            if (frame.kind == AdminServerFrameKind::DirBatch) {
              it->second.receivedEntries += frame.entries.size();
              if (it->second.command != AdminClientCommand::ListDir
                  || it->second.receivedEntries > kAdminMaxDirectoryEntries) {
                unexpectedExit = frameError = true;
                exitFailureReason = AdminSessionFailureReason::ProtocolError;
                exitMessage = "Unexpected or excessive Administrator directory batches.";
                break;
              }
              auto batchCb = it->second.onDirBatch;
              if (batchCb) {
                auto entriesCopy = frame.entries;
                postToClient([batchCb = std::move(batchCb),
                              entriesCopy = std::move(entriesCopy)](AdminSessionClient&) mutable {
                  batchCb(entriesCopy);
                });
              }
              continue;
            }

            if (frame.kind == AdminServerFrameKind::DirEnd
                && (it->second.command != AdminClientCommand::ListDir || frame.totalEntries != it->second.receivedEntries)) {
              unexpectedExit = frameError = true;
              exitFailureReason = AdminSessionFailureReason::ProtocolError;
              exitMessage = "Invalid Administrator directory batch total.";
              break;
            }
            if (frame.kind == AdminServerFrameKind::MutationResult
                && it->second.command != AdminClientCommand::CreateFile
                && it->second.command != AdminClientCommand::CreateDir
                && it->second.command != AdminClientCommand::RenameEntry) {
              unexpectedExit = frameError = true;
              exitFailureReason = AdminSessionFailureReason::ProtocolError;
              exitMessage = "Unexpected Administrator mutation result frame.";
              break;
            }

            PendingRequest matched = std::move(it->second);
            pendingRequests.erase(it);
            postToClient([matched = std::move(matched), frame = std::move(frame)](AdminSessionClient&) mutable {
              if (matched.onPing) {
                if (frame.kind == AdminServerFrameKind::Status) {
                  matched.onPing(AdminHealthStatus{
                      .ok = true,
                      .statusCode = AdminStatusCode::Ok,
                      .protocolVersion = frame.protocolVersion,
                      .helperPid = frame.helperPid,
                      .invokingUid = frame.invokingUid,
                      .uptimeMs = frame.uptimeMs,
                      .activeRequestId = frame.activeRequestId,
                      .detail = {},
                  });
                } else {
                  matched.onPing(AdminHealthStatus{
                      .ok = false,
                      .statusCode = frame.status,
                      .detail = frame.detail,
                  });
                }
              }
              if (matched.onRequest) {
                const bool ok = frame.kind == AdminServerFrameKind::Response && frame.status == AdminStatusCode::Ok;
                matched.onRequest(ok, frame.status, frame.detail);
              }
              if (matched.onDirComplete) {
                if (frame.kind == AdminServerFrameKind::DirEnd) {
                  matched.onDirComplete(true, frame.directoryWritable, frame.totalEntries, AdminStatusCode::Ok, {});
                } else {
                  matched.onDirComplete(false, false, 0, frame.status, frame.detail);
                }
              }
              if (matched.onStat) {
                if (frame.kind == AdminServerFrameKind::StatResult) {
                  matched.onStat(true, frame.statInfo, AdminStatusCode::Ok, {});
                } else {
                  matched.onStat(false, AdminStatRecord{}, frame.status, frame.detail);
                }
              }
              if (matched.onStatVfs) {
                if (frame.kind == AdminServerFrameKind::StatVfsResult) {
                  matched.onStatVfs(true, frame.statVfsInfo, AdminStatusCode::Ok, {});
                } else {
                  matched.onStatVfs(false, AdminStatVfsRecord{}, frame.status, frame.detail);
                }
              }
              if (matched.onCountDir) {
                if (frame.kind == AdminServerFrameKind::CountResult) {
                  matched.onCountDir(true, frame.itemCount, AdminStatusCode::Ok, {});
                } else {
                  matched.onCountDir(false, 0, frame.status, frame.detail);
                }
              }
              if (matched.onMutation) {
                if (frame.kind == AdminServerFrameKind::MutationResult) {
                  matched.onMutation(true, frame.mutationInfo, AdminStatusCode::Ok, {});
                } else {
                  matched.onMutation(false, AdminMutationRecord{}, frame.status, frame.detail);
                }
              }
            });
          }
        }

        if (frameError) {
          if (channel->closeWrite)
            channel->closeWrite();
          if (channel->waitForExit)
            (void)channel->waitForExit();
          failAllPending(AdminStatusCode::InternalError, exitMessage);
          break;
        }

        bool hasOutgoing = outgoing.has_value();
        {
          std::lock_guard lock(worker->mutex);
          hasOutgoing = hasOutgoing || !worker->outgoingQueue.empty();
        }
        std::array<pollfd, 3> pfds{{
            {channel->readFd, POLLIN | POLLHUP, 0},
            {worker->wakeupFd, POLLIN, 0},
            {hasOutgoing && !writeClosed ? channel->writeFd : -1, POLLOUT, 0},
        }};
        const int pollRc = ::poll(pfds.data(), pfds.size(), -1);
        if (pollRc < 0) {
          if (errno == EINTR)
            continue;
          unexpectedExit = true;
          exitFailureReason = AdminSessionFailureReason::TransportError;
          exitMessage = std::strerror(errno);
          if (channel->closeWrite)
            channel->closeWrite();
          if (channel->waitForExit)
            (void)channel->waitForExit();
          failAllPending(AdminStatusCode::InternalError, exitMessage);
          break;
        }

        if ((pfds[1].revents & POLLIN) != 0)
          worker->drainWakeup();

        bool sawEof = false;
        if ((pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
          std::array<char, 4096> chunk{};
          while (true) {
            const ssize_t n = ::read(channel->readFd, chunk.data(), chunk.size());
            if (n > 0) {
              lineBuffer.append(std::string_view(chunk.data(), static_cast<std::size_t>(n)));
              break;
            }
            if (n == 0) {
              sawEof = true;
              break;
            }
            if (errno == EINTR)
              continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;
            sawEof = true;
            break;
          }
        }

        if (sawEof) {
          bool requestedStop = false;
          {
            std::lock_guard lock(worker->mutex);
            requestedStop = worker->stopRequested;
          }
          if (channel->closeWrite)
            channel->closeWrite();
          if (channel->waitForExit)
            (void)channel->waitForExit();
          if (!requestedStop) {
            unexpectedExit = true;
            const bool truncated = lineBuffer.hasBufferedBytes();
            exitFailureReason = truncated ? AdminSessionFailureReason::ProtocolError
                                          : AdminSessionFailureReason::HelperTerminated;
            exitMessage = truncated ? "The Administrator helper sent a truncated frame."
                                    : "The Administrator helper process terminated.";
            failAllPending(AdminStatusCode::InternalError, exitMessage);
          } else {
            failAllPending(AdminStatusCode::Cancelled, "Administrator session stopped.");
          }
          break;
        }
      }

      if (unexpectedExit) {
        postToClient([exitFailureReason, msg = std::move(exitMessage)](AdminSessionClient& self) mutable {
          self.m_activeWorker.reset();
          self.transitionTo(AdminSessionState::Failed, exitFailureReason, std::move(msg));
        });
      }
      worker->finished.store(true, std::memory_order_release);
    });

    return true;
  }

  bool AdminSessionClient::cancelAuthentication() {
    if (m_shutDown || m_state != AdminSessionState::Authenticating)
      return false;
    ++m_sessionGeneration;
    if (m_activeWorker) {
      m_activeWorker->requestStop(false, true);
      m_activeWorker.reset();
    }
    transitionTo(
        AdminSessionState::Inactive, AdminSessionFailureReason::AuthCancelled,
        "Administrator authentication was cancelled."
    );
    return true;
  }

  AdminRequestId AdminSessionClient::pingAsync(PingCallback onComplete) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker) {
      if (onComplete) {
        invokeOnMainContext([cb = std::move(onComplete)] {
          cb(AdminHealthStatus{
              .ok = false,
              .statusCode = AdminStatusCode::HandshakeRequired,
              .detail = "Administrator session is not active.",
          });
        });
      }
      return kInvalidAdminRequestId;
    }

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return kInvalidAdminRequestId;
      reqId = worker->nextRequestId++;
      AdminClientRequest pingReq;
      pingReq.command = AdminClientCommand::Ping;
      pingReq.requestId = reqId;
      const auto encoded = encodeAdminClientRequest(pingReq);
      if (!encoded)
        return kInvalidAdminRequestId;
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = AdminClientCommand::Ping,
          .encodedFrame = *encoded,
          .onPing = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  AdminRequestId AdminSessionClient::renewActivityAsync(bool forceImmediate, RequestCallback onComplete) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker)
      return kInvalidAdminRequestId;
    const auto now = std::chrono::steady_clock::now();
    if (!forceImmediate
        && m_lastRenewSentAt.time_since_epoch().count() != 0
        && now - m_lastRenewSentAt < std::chrono::milliseconds(500)) {
      return kInvalidAdminRequestId;
    }
    m_lastRenewSentAt = now;

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return kInvalidAdminRequestId;
      reqId = worker->nextRequestId++;
      AdminClientRequest renewReq;
      renewReq.command = AdminClientCommand::RenewActivity;
      renewReq.requestId = reqId;
      const auto encoded = encodeAdminClientRequest(renewReq);
      if (!encoded)
        return kInvalidAdminRequestId;
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = AdminClientCommand::RenewActivity,
          .encodedFrame = *encoded,
          .onRequest = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  AdminRequestId AdminSessionClient::listDirectoryAsync(
      const std::filesystem::path& path,
      bool userInitiated,
      std::uint32_t maxBatchEntries,
      DirBatchCallback onBatch,
      DirCompleteCallback onComplete
  ) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker) {
      if (onComplete) {
        invokeOnMainContext([cb = std::move(onComplete)] {
          cb(false, false, 0, AdminStatusCode::HandshakeRequired, "Administrator session is not active.");
        });
      }
      return kInvalidAdminRequestId;
    }
    if (userInitiated)
      m_lastRenewSentAt = std::chrono::steady_clock::now();

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return kInvalidAdminRequestId;
      reqId = worker->nextRequestId++;
      AdminClientRequest listReq;
      listReq.command = AdminClientCommand::ListDir;
      listReq.requestId = reqId;
      listReq.maxBatchEntries = std::clamp(maxBatchEntries, 1U, kAdminMaxDirBatchEntries);
      listReq.userInitiated = userInitiated;
      listReq.path = normalizeTabPath(path).string();
      const auto encoded = encodeAdminClientRequest(listReq);
      if (!encoded) {
        if (onComplete) {
          invokeOnMainContext([cb = std::move(onComplete)] {
            cb(false, false, 0, AdminStatusCode::BadRequest, "Invalid directory path.");
          });
        }
        return kInvalidAdminRequestId;
      }
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = AdminClientCommand::ListDir,
          .encodedFrame = *encoded,
          .onDirBatch = std::move(onBatch),
          .onDirComplete = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  AdminRequestId
  AdminSessionClient::statInfoAsync(const std::filesystem::path& path, bool userInitiated, StatCallback onComplete) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker) {
      if (onComplete) {
        invokeOnMainContext([cb = std::move(onComplete)] {
          cb(false, AdminStatRecord{}, AdminStatusCode::HandshakeRequired, "Administrator session is not active.");
        });
      }
      return kInvalidAdminRequestId;
    }
    if (userInitiated)
      m_lastRenewSentAt = std::chrono::steady_clock::now();

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return kInvalidAdminRequestId;
      reqId = worker->nextRequestId++;
      AdminClientRequest statReq;
      statReq.command = AdminClientCommand::StatInfo;
      statReq.requestId = reqId;
      statReq.userInitiated = userInitiated;
      statReq.path = normalizeTabPath(path).string();
      const auto encoded = encodeAdminClientRequest(statReq);
      if (!encoded) {
        if (onComplete) {
          invokeOnMainContext([cb = std::move(onComplete)] {
            cb(false, AdminStatRecord{}, AdminStatusCode::BadRequest, "Invalid target path.");
          });
        }
        return kInvalidAdminRequestId;
      }
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = AdminClientCommand::StatInfo,
          .encodedFrame = *encoded,
          .onStat = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  AdminRequestId AdminSessionClient::statVfsAsync(const std::filesystem::path& path, StatVfsCallback onComplete) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker) {
      if (onComplete) {
        invokeOnMainContext([cb = std::move(onComplete)] {
          cb(false, AdminStatVfsRecord{}, AdminStatusCode::HandshakeRequired, "Administrator session is not active.");
        });
      }
      return kInvalidAdminRequestId;
    }

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return kInvalidAdminRequestId;
      reqId = worker->nextRequestId++;
      AdminClientRequest vfsReq;
      vfsReq.command = AdminClientCommand::StatVfs;
      vfsReq.requestId = reqId;
      vfsReq.path = normalizeTabPath(path).string();
      const auto encoded = encodeAdminClientRequest(vfsReq);
      if (!encoded) {
        if (onComplete) {
          invokeOnMainContext([cb = std::move(onComplete)] {
            cb(false, AdminStatVfsRecord{}, AdminStatusCode::BadRequest, "Invalid filesystem path.");
          });
        }
        return kInvalidAdminRequestId;
      }
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = AdminClientCommand::StatVfs,
          .encodedFrame = *encoded,
          .onStatVfs = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  AdminRequestId
  AdminSessionClient::countDirectoryItemsAsync(const std::filesystem::path& path, CountDirCallback onComplete) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker) {
      if (onComplete) {
        invokeOnMainContext([cb = std::move(onComplete)] {
          cb(false, 0, AdminStatusCode::HandshakeRequired, "Administrator session is not active.");
        });
      }
      return kInvalidAdminRequestId;
    }

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return kInvalidAdminRequestId;
      reqId = worker->nextRequestId++;
      AdminClientRequest countReq;
      countReq.command = AdminClientCommand::CountDir;
      countReq.requestId = reqId;
      countReq.path = normalizeTabPath(path).string();
      const auto encoded = encodeAdminClientRequest(countReq);
      if (!encoded) {
        if (onComplete) {
          invokeOnMainContext([cb = std::move(onComplete)] {
            cb(false, 0, AdminStatusCode::BadRequest, "Invalid directory path.");
          });
        }
        return kInvalidAdminRequestId;
      }
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = AdminClientCommand::CountDir,
          .encodedFrame = *encoded,
          .onCountDir = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  AdminRequestId AdminSessionClient::enqueueMutationAsync(
      AdminClientRequest request, const char* invalidDetail, MutationCallback onComplete
  ) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker) {
      if (onComplete) {
        invokeOnMainContext([cb = std::move(onComplete)] {
          cb(false, AdminMutationRecord{}, AdminStatusCode::HandshakeRequired, "Administrator session is not active.");
        });
      }
      return kInvalidAdminRequestId;
    }

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested) {
        if (onComplete) {
          invokeOnMainContext([cb = std::move(onComplete)] {
            cb(false, AdminMutationRecord{}, AdminStatusCode::Cancelled, "Administrator session stopped.");
          });
        }
        return kInvalidAdminRequestId;
      }
      reqId = worker->nextRequestId++;
      request.requestId = reqId;
      const auto encoded = encodeAdminClientRequest(request);
      if (!encoded) {
        if (onComplete) {
          invokeOnMainContext([cb = std::move(onComplete), msg = std::string(invalidDetail)] {
            cb(false, AdminMutationRecord{}, AdminStatusCode::BadRequest, msg);
          });
        }
        return kInvalidAdminRequestId;
      }
      m_lastRenewSentAt = std::chrono::steady_clock::now();
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = request.command,
          .encodedFrame = *encoded,
          .onMutation = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  AdminRequestId AdminSessionClient::createFileAsync(
      const std::filesystem::path& directoryPath, const std::string& name, bool autoNumber,
      MutationCallback onComplete
  ) {
    AdminClientRequest req;
    req.command = AdminClientCommand::CreateFile;
    req.path = normalizeTabPath(directoryPath).string();
    req.entryName = name;
    req.autoNumber = autoNumber;
    return enqueueMutationAsync(std::move(req), "Invalid target directory or file name.", std::move(onComplete));
  }

  AdminRequestId AdminSessionClient::createDirectoryAsync(
      const std::filesystem::path& directoryPath, const std::string& name, bool autoNumber,
      MutationCallback onComplete
  ) {
    AdminClientRequest req;
    req.command = AdminClientCommand::CreateDir;
    req.path = normalizeTabPath(directoryPath).string();
    req.entryName = name;
    req.autoNumber = autoNumber;
    return enqueueMutationAsync(std::move(req), "Invalid target directory or folder name.", std::move(onComplete));
  }

  AdminRequestId AdminSessionClient::renameEntryAsync(
      const std::filesystem::path& directoryPath, const std::string& sourceName, const std::string& destName,
      std::uint64_t expectedDevice, std::uint64_t expectedInode, bool expectedIsDir, bool expectedIsSymlink,
      MutationCallback onComplete
  ) {
    AdminClientRequest req;
    req.command = AdminClientCommand::RenameEntry;
    req.path = normalizeTabPath(directoryPath).string();
    req.sourceName = sourceName;
    req.destName = destName;
    req.expectedDevice = expectedDevice;
    req.expectedInode = expectedInode;
    req.expectedIsDir = expectedIsDir;
    req.expectedIsSymlink = expectedIsSymlink;
    return enqueueMutationAsync(std::move(req), "Invalid rename path, filename, or item identity.", std::move(onComplete));
  }

  AdminSessionClient::CopyControl AdminSessionClient::copyItems(
      const std::vector<std::string>& sources, const std::filesystem::path& directory, AdminRequestId listingId,
      std::uint64_t expectedGeneration, CopyFrameCallback onFrame
  ) {
    const auto fail = [&](AdminStatusCode code, std::string message) -> CopyControl {
      onFrame(AdminServerFrame{.status = code, .detail = std::move(message)});
      return {};
    };
    if (!isActive() || !m_activeWorker || expectedGeneration != m_sessionGeneration)
      return fail(AdminStatusCode::Unauthorized, "The originating Administrator session is no longer active.");
    if (sources.empty() || sources.size() > kAdminMaxCopySources)
      return fail(AdminStatusCode::BadRequest, "Administrator Paste supports up to 256 selected items.");

    std::vector<AdminCopySource> copySources;
    std::vector<std::string> copyNames;
    copySources.reserve(sources.size());
    copyNames.reserve(sources.size());
    for (const auto& source : sources) {
      const std::filesystem::path path(source);
      struct stat info{};
      if (!path.is_absolute() || ::lstat(path.c_str(), &info) != 0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
        return fail(AdminStatusCode::BadRequest, "Administrator Paste requires accessible regular files or directories.");
      copySources.push_back({
          source, static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), S_ISDIR(info.st_mode),
      });
      copyNames.push_back(path.filename().string());
    }

    auto worker = m_activeWorker;
    AdminRequestId id;
    {
      std::unique_lock lock(worker->mutex);
      if (worker->stopRequested) {
        lock.unlock();
        return fail(AdminStatusCode::Cancelled, "Administrator session stopped.");
      }
      id = worker->nextRequestId++;
      const auto encoded = encodeAdminClientRequest({
          .command = AdminClientCommand::CopyItems,
          .requestId = id,
          .path = directory.string(),
          .directoryListingId = listingId,
          .copySources = std::move(copySources),
      });
      if (!encoded) {
        lock.unlock();
        return fail(AdminStatusCode::BadRequest, "Invalid or oversized Administrator Paste request.");
      }
      worker->outgoingQueue.push_back(
          WorkerContext::OutgoingItem{
              .requestId = id,
              .command = AdminClientCommand::CopyItems,
              .encodedFrame = *encoded,
              .copyNames = std::move(copyNames),
              .onCopyFrame = std::move(onFrame),
          }
      );
    }
    worker->signalWakeup();
    return [weakWorker = std::weak_ptr(worker), id, cancelQueued = false](bool stopTransport) mutable {
      const auto captured = weakWorker.lock();
      if (!captured)
        return;
      if (stopTransport) {
        captured->requestStop(false, false);
        return;
      }
      if (std::exchange(cancelQueued, true))
        return;
      {
        std::lock_guard lock(captured->mutex);
        if (captured->stopRequested)
          return;
        const auto cancelId = captured->nextRequestId++;
        const auto encoded = encodeAdminClientRequest({
            .command = AdminClientCommand::Cancel,
            .requestId = cancelId,
            .targetRequestId = id,
        });
        if (!encoded)
          return;
        captured->outgoingQueue.push_back(
            WorkerContext::OutgoingItem{
                .requestId = cancelId,
                .command = AdminClientCommand::Cancel,
                .encodedFrame = *encoded,
            }
        );
      }
      captured->signalWakeup();
    };
  }

  AdminSessionClient::OperationTransport
  AdminSessionClient::runOperation(const file_operation_engine::Request& request, std::uint64_t expectedGeneration) {
    auto mailbox = std::make_shared<OperationMailbox>();
    OperationTransport operation;
    operation.takeFrames = [mailbox] { return mailbox->take(); };
    const auto failure = [&](AdminStatusCode code, const char* detail) {
      mailbox->push({.kind = AdminServerFrameKind::OpResult, .status = code, .detail = detail});
      return operation;
    };
    if (!isActive() || !m_activeWorker || expectedGeneration != m_sessionGeneration)
      return failure(AdminStatusCode::Unauthorized, "The originating Administrator session is no longer active.");

    auto worker = m_activeWorker;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return failure(AdminStatusCode::Cancelled, "Administrator session stopped.");
      if (worker->activeOperation != kInvalidAdminRequestId)
        return failure(AdminStatusCode::BadRequest, "Another shared operation is active.");
      const auto id = worker->nextRequestId++;
      auto encoded = encodeAdminClientRequest({
          .command = AdminClientCommand::OpBegin,
          .requestId = id,
          .path = request.destination,
          .expectedDevice = request.renameIdentity ? request.renameIdentity->first : 0,
          .expectedInode = request.renameIdentity ? request.renameIdentity->second : 0,
          .operationKind = request.kind,
          .sourceCount = request.sources.size(),
          .keepBoth = request.keepBoth,
      });
      if (!encoded)
        return failure(AdminStatusCode::BadRequest, "Invalid shared operation request.");
      for (const auto& source : request.sources) {
        auto chunk =
            encodeAdminClientRequest({.command = AdminClientCommand::OpSource, .requestId = id, .path = source});
        if (!chunk)
          return failure(AdminStatusCode::BadRequest, "Invalid shared operation source.");
        *encoded += *chunk;
      }
      *encoded += *encodeAdminClientRequest({.command = AdminClientCommand::OpExec, .requestId = id});
      worker->outgoingQueue.push_back({
          .requestId = id,
          .command = AdminClientCommand::OpExec,
          .encodedFrame = std::move(*encoded),
          .operation = mailbox,
          .operationSources = request.sources.size(),
      });
      worker->activeOperation = operation.requestId = id;
    }
    worker->signalWakeup();

    const auto send = [weak = std::weak_ptr(worker), id = operation.requestId](AdminClientRequest control) {
      const auto session = weak.lock();
      if (!session)
        return false;
      {
        std::lock_guard lock(session->mutex);
        if (session->stopRequested || session->activeOperation != id)
          return false;
        control.requestId = session->nextRequestId++;
        control.targetRequestId = id;
        auto encoded = encodeAdminClientRequest(control);
        if (!encoded)
          return false;
        session->outgoingQueue.push_back(
            {.requestId = control.requestId, .command = control.command, .encodedFrame = std::move(*encoded)}
        );
      }
      session->signalWakeup();
      return true;
    };
    operation.sendConflictReply = [send](std::uint64_t conflict, const auto& decision) {
      return send(
          {.command = AdminClientCommand::OpConflictReply,
           .conflictId = conflict,
           .conflictDecision = static_cast<std::uint32_t>(decision.action),
           .alternativeName = decision.alternativeName,
           .applyToAll = decision.applyToAll}
      );
    };
    operation.setPaused = [send](bool paused) {
      return send({.command = paused ? AdminClientCommand::OpPause : AdminClientCommand::OpResume});
    };
    operation.cancel = [send] { return send({.command = AdminClientCommand::Cancel}); };
    return operation;
  }

  AdminRequestId AdminSessionClient::cancelRequestAsync(AdminRequestId targetRequestId, RequestCallback onComplete) {
    if (m_shutDown
        || m_state != AdminSessionState::Active
        || !m_activeWorker
        || targetRequestId == kInvalidAdminRequestId) {
      if (onComplete) {
        invokeOnMainContext([cb = std::move(onComplete)] {
          cb(false, AdminStatusCode::BadRequest, "Administrator session is not active or target request is invalid.");
        });
      }
      return kInvalidAdminRequestId;
    }

    auto worker = m_activeWorker;
    AdminRequestId reqId = kInvalidAdminRequestId;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return kInvalidAdminRequestId;
      reqId = worker->nextRequestId++;
      AdminClientRequest cancelReq;
      cancelReq.command = AdminClientCommand::Cancel;
      cancelReq.requestId = reqId;
      cancelReq.targetRequestId = targetRequestId;
      const auto encoded = encodeAdminClientRequest(cancelReq);
      if (!encoded)
        return kInvalidAdminRequestId;
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = reqId,
          .command = AdminClientCommand::Cancel,
          .encodedFrame = *encoded,
          .onRequest = std::move(onComplete),
      });
    }
    worker->signalWakeup();
    return reqId;
  }

  bool AdminSessionClient::sendRawFrameForTesting(std::string rawBytes) {
    if (m_shutDown || m_state != AdminSessionState::Active || !m_activeWorker || rawBytes.empty())
      return false;
    auto worker = m_activeWorker;
    {
      std::lock_guard lock(worker->mutex);
      if (worker->stopRequested)
        return false;
      worker->outgoingQueue.push_back(WorkerContext::OutgoingItem{
          .requestId = kInvalidAdminRequestId,
          .command = AdminClientCommand::Ping,
          .encodedFrame = std::move(rawBytes),
      });
    }
    worker->signalWakeup();
    return true;
  }

  void AdminSessionClient::stopSession(bool graceful) {
    if (m_shutDown)
      return;
    const bool wasActiveOrAuth =
        m_state == AdminSessionState::Active || m_state == AdminSessionState::Authenticating;
    const bool wasAuth = m_state == AdminSessionState::Authenticating;
    ++m_sessionGeneration;
    if (m_activeWorker) {
      m_activeWorker->requestStop(graceful && !wasAuth, wasAuth);
      m_activeWorker.reset();
    }
    if (wasActiveOrAuth || m_state != AdminSessionState::Inactive) {
      transitionTo(AdminSessionState::Inactive, AdminSessionFailureReason::None, {});
    }
  }

  void AdminSessionClient::shutdown() {
    if (m_shutDown)
      return;
    m_shutDown = true;
    if (m_clientAlive)
      *m_clientAlive = false;
    ++m_sessionGeneration;
    if (m_activeWorker) {
      m_activeWorker->requestStop(false, m_state == AdminSessionState::Authenticating);
      m_activeWorker.reset();
    }
    m_state = AdminSessionState::Inactive;
    m_failureReason = AdminSessionFailureReason::None;
    m_negotiatedProtocolVersion = 0;
    m_helperPid = 0;
    m_invokingUid = 0;
    m_lastMessage.clear();
    m_onStateChanged = {};
  }

  void AdminSessionClient::waitForSettled() {
    std::vector<std::shared_ptr<WorkerContext>> workers;
    {
      std::lock_guard lock(m_workersMutex);
      workers = m_allWorkers;
    }
    for (const auto& worker : workers) {
      if (worker->thread.joinable())
        worker->thread.join();
    }
    {
      std::lock_guard lock(m_workersMutex);
      reapCompletedWorkersLocked();
    }
  }

} // namespace filemanager
