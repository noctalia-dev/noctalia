#pragma once

#include "filemanager/admin_protocol.h"
#include "filemanager/polkit_helper_launch.h"
#include "filemanager/tab_presentation.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace filemanager {

  enum class AdminSessionState : std::uint8_t {
    Inactive,
    Authenticating,
    Active,
    Terminating,
    Failed,
  };

  enum class AdminSessionFailureReason : std::uint8_t {
    None,
    NotInstalled,
    ActionNotRegistered,
    AuthCancelled,
    AuthDenied,
    VersionMismatch,
    ProtocolError,
    HelperTerminated,
    TransportError,
  };

  struct AdminSessionStatus {
    TabId tabId = kInvalidTabId;
    AdminSessionState state = AdminSessionState::Inactive;
    AdminSessionFailureReason failureReason = AdminSessionFailureReason::None;
    std::uint64_t generation = 0;
    std::uint32_t protocolVersion = 0;
    std::uint32_t helperPid = 0;
    std::uint32_t invokingUid = 0;
    std::string message;
  };

  struct AdminHealthStatus {
    bool ok = false;
    AdminStatusCode statusCode = AdminStatusCode::Ok;
    std::uint32_t protocolVersion = 0;
    std::uint32_t helperPid = 0;
    std::uint32_t invokingUid = 0;
    std::uint64_t uptimeMs = 0;
    AdminRequestId activeRequestId = kInvalidAdminRequestId;
    std::string detail;
  };

  // Low-level bidirectional pipe channel connecting AdminSessionClient to a helper process
  // or an unprivileged in-process test fixture.
  struct AdminTransportChannel {
    int readFd = -1;
    int writeFd = -1;
    std::function<void()> closeWrite;
    std::function<void()> abortTransport;
    std::function<int()> waitForExit;
  };

  struct AdminTransportLaunchResult {
    std::unique_ptr<AdminTransportChannel> channel;
    AdminSessionFailureReason failureReason = AdminSessionFailureReason::None;
    bool cancelled = false;
    std::string error;
  };

  using AdminTransportLauncher = std::function<AdminTransportLaunchResult(GCancellable* cancellable)>;

  // Returns the production Polkit helper target for nythera-admin-helper.
  [[nodiscard]] PolkitHelperTarget defaultAdminPolkitTarget() noexcept;

  // Default production launcher: verifies root-owned helper and policy installation,
  // verifies the dedicated dev.nythera.Files.admin-session Polkit action, and spawns
  // /usr/bin/pkexec --disable-internal-agent.
  [[nodiscard]] AdminTransportLaunchResult launchPolkitAdminTransport(GCancellable* cancellable);

  class AdminSessionClient {
  public:
    using StateChangedCallback = std::function<void(const AdminSessionStatus&)>;
    using StartCallback = std::function<void(bool ok, const AdminSessionStatus&)>;
    using PingCallback = std::function<void(const AdminHealthStatus&)>;
    using RequestCallback = std::function<void(bool ok, AdminStatusCode code, const std::string& detail)>;
    using DirBatchCallback = std::function<void(const std::vector<AdminDirEntryRecord>& entries)>;
    using DirCompleteCallback = std::function<
        void(bool ok, bool directoryWritable, std::uint64_t totalEntries, AdminStatusCode code, const std::string& detail)>;
    using StatCallback =
        std::function<void(bool ok, const AdminStatRecord& record, AdminStatusCode code, const std::string& detail)>;
    using StatVfsCallback = std::function<
        void(bool ok, const AdminStatVfsRecord& record, AdminStatusCode code, const std::string& detail)>;
    using CountDirCallback =
        std::function<void(bool ok, std::uint64_t count, AdminStatusCode code, const std::string& detail)>;
    using MutationCallback = std::function<
        void(bool ok, const AdminMutationRecord& record, AdminStatusCode code, const std::string& detail)>;

    AdminSessionClient(
        TabId tabId, std::shared_ptr<bool> tabAlive, StateChangedCallback onStateChanged = {}
    );
    ~AdminSessionClient();

    AdminSessionClient(const AdminSessionClient&) = delete;
    AdminSessionClient& operator=(const AdminSessionClient&) = delete;
    AdminSessionClient(AdminSessionClient&&) = delete;
    AdminSessionClient& operator=(AdminSessionClient&&) = delete;

    [[nodiscard]] TabId tabId() const noexcept { return m_tabId; }
    [[nodiscard]] AdminSessionState state() const noexcept { return m_state; }
    [[nodiscard]] bool isActive() const noexcept { return m_state == AdminSessionState::Active; }
    [[nodiscard]] bool isAuthenticating() const noexcept { return m_state == AdminSessionState::Authenticating; }
    [[nodiscard]] AdminSessionFailureReason failureReason() const noexcept { return m_failureReason; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastMessage; }
    [[nodiscard]] std::uint64_t sessionGeneration() const noexcept { return m_sessionGeneration; }
    [[nodiscard]] std::uint32_t helperPid() const noexcept { return m_helperPid; }
    [[nodiscard]] std::uint32_t invokingUid() const noexcept { return m_invokingUid; }
    [[nodiscard]] std::uint32_t inactivityTimeoutMs() const noexcept { return m_inactivityTimeoutMs; }
    [[nodiscard]] AdminSessionStatus status() const;

    void setStateChangedCallback(StateChangedCallback callback);
    void setTransportLauncher(AdminTransportLauncher launcher);
    void setInactivityTimeoutMs(std::uint32_t timeoutMs, bool isTestOverride = false) noexcept;
    [[nodiscard]] bool hasCustomTimeoutForTesting() const noexcept { return m_customTimeoutForTesting; }
    void setProtocolVersionForTesting(std::uint32_t version) noexcept { m_clientProtocolVersion = version; }

    // Starts Polkit authentication and the HELLO/READY protocol handshake asynchronously.
    bool startSessionAsync(StartCallback onComplete = {});

    // Cancels an in-flight authentication or handshake attempt.
    bool cancelAuthentication();

    // Sends a PING health/status request over an active session (does not renew inactivity timer).
    AdminRequestId pingAsync(PingCallback onComplete);

    // Explicitly renews the helper's inactivity timer on meaningful user interaction.
    AdminRequestId renewActivityAsync(bool forceImmediate = false, RequestCallback onComplete = {});

    // Enumerates a directory in bounded batches through the privileged helper.
    AdminRequestId listDirectoryAsync(
        const std::filesystem::path& path,
        bool userInitiated,
        std::uint32_t maxBatchEntries,
        DirBatchCallback onBatch,
        DirCompleteCallback onComplete
    );

    // Queries metadata and writability/symlink target for a path through the privileged helper.
    AdminRequestId
    statInfoAsync(const std::filesystem::path& path, bool userInitiated, StatCallback onComplete);

    // Queries filesystem capacity and free space through the privileged helper.
    AdminRequestId statVfsAsync(const std::filesystem::path& path, StatVfsCallback onComplete);

    // Counts immediate child items of a directory through the privileged helper.
    AdminRequestId countDirectoryItemsAsync(const std::filesystem::path& path, CountDirCallback onComplete);

    // Creates an empty regular file inside an already-authorised directory through the privileged helper.
    AdminRequestId createFileAsync(
        const std::filesystem::path& directoryPath, const std::string& name, bool autoNumber,
        MutationCallback onComplete
    );

    // Creates a directory inside an already-authorised directory through the privileged helper.
    AdminRequestId createDirectoryAsync(
        const std::filesystem::path& directoryPath, const std::string& name, bool autoNumber,
        MutationCallback onComplete
    );

    // Renames an entry within the same already-authorised directory through the privileged helper.
    AdminRequestId renameEntryAsync(
        const std::filesystem::path& directoryPath, const std::string& sourceName, const std::string& destName,
        std::uint64_t expectedDevice, std::uint64_t expectedInode, bool expectedIsDir, bool expectedIsSymlink,
        MutationCallback onComplete
    );

    // Copy frames are delivered on the transport worker (or synchronously on
    // submission failure), including one terminal frame even after tab shutdown.
    // The returned control is bound to this worker, never a replacement session.
    using CopyFrameCallback = std::function<void(const AdminServerFrame&)>;
    using CopyControl = std::function<void(bool stopTransport)>;
    CopyControl copyItems(
        const std::vector<std::string>& sources, const std::filesystem::path& directory, AdminRequestId listingId,
        std::uint64_t expectedGeneration, CopyFrameCallback onFrame
    );

    // Session-bound operation mailbox. The transport fills it independently of
    // the consumer; the caller owns conflict presentation and decision timing.
    struct OperationTransport {
      AdminRequestId requestId = kInvalidAdminRequestId;
      std::function<std::vector<AdminServerFrame>()> takeFrames;
      std::function<bool(std::uint64_t, const file_operation_engine::ConflictDecision&)> sendConflictReply;
      std::function<bool(bool paused)> setPaused;
      std::function<bool()> cancel;
    };
    // Submit on the client-owning thread. The mailbox and controls can be used by a worker.
    OperationTransport runOperation(const file_operation_engine::Request& request, std::uint64_t expectedGeneration);

    // Sends a CANCEL request for an in-flight request ID over an active session.
    AdminRequestId cancelRequestAsync(AdminRequestId targetRequestId, RequestCallback onComplete = {});

    // Sends raw bytes on the active transport channel (for protocol resilience tests).
    bool sendRawFrameForTesting(std::string rawBytes);

    // Terminates an active or authenticating session without blocking the UI thread.
    void stopSession(bool graceful = true);

    // Permanently shuts down this client when its owning BrowserTab closes.
    void shutdown();

    // Waits for all background worker threads spawned by this client to finish reaping
    // their transports. Safe to call in tests or final teardown.
    void waitForSettled();

  private:
    struct WorkerContext;

    AdminRequestId
    enqueueMutationAsync(AdminClientRequest request, const char* invalidDetail, MutationCallback onComplete);
    void transitionTo(
        AdminSessionState newState, AdminSessionFailureReason reason, std::string message, bool notify = true
    );
    void reapCompletedWorkersLocked();

    TabId m_tabId = kInvalidTabId;
    std::shared_ptr<bool> m_tabAlive;
    std::shared_ptr<bool> m_clientAlive = std::make_shared<bool>(true);
    bool m_shutDown = false;

    AdminSessionState m_state = AdminSessionState::Inactive;
    AdminSessionFailureReason m_failureReason = AdminSessionFailureReason::None;
    std::uint64_t m_sessionGeneration = 0;
    std::uint32_t m_clientProtocolVersion = kAdminProtocolVersion;
    std::uint32_t m_inactivityTimeoutMs = static_cast<std::uint32_t>(kDefaultAdminInactivityTimeoutSeconds * 1000);
    bool m_customTimeoutForTesting = false;
    std::uint32_t m_negotiatedProtocolVersion = 0;
    std::uint32_t m_helperPid = 0;
    std::uint32_t m_invokingUid = 0;
    std::string m_lastMessage;
    std::chrono::steady_clock::time_point m_lastRenewSentAt{};

    StateChangedCallback m_onStateChanged;
    AdminTransportLauncher m_launcher;

    std::shared_ptr<WorkerContext> m_activeWorker;
    std::mutex m_workersMutex;
    std::vector<std::shared_ptr<WorkerContext>> m_allWorkers;
  };

} // namespace filemanager
