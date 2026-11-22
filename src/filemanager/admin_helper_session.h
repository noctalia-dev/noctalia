#pragma once

#include "filemanager/admin_protocol.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <sys/types.h>

namespace filemanager {

  inline constexpr int kDefaultAdminHelperInactivityTimeoutMs = kDefaultAdminInactivityTimeoutSeconds * 1000;
  inline constexpr int kMinAdminHelperInactivityTimeoutMs = 10;
  inline constexpr int kMaxAdminHelperInactivityTimeoutMs = kMaxAdminInactivityTimeoutSeconds * 1000;

  struct AdminHelperSessionConfig {
    std::uint32_t helperPid = 0;
    std::uint32_t invokingUid = 0;
    std::size_t maxFrameBytes = kAdminMaxFrameBytes;
    int handshakeTimeoutMs = 5000;
    // If set, overrides the client-requested inactivity timeout; otherwise the helper
    // enforces the validated timeout negotiated during HELLO (defaulting to 10 minutes).
    std::optional<int> inactivityTimeoutMs = std::nullopt;
    std::size_t maxConsecutiveBadRequests = 3;
    const volatile std::sig_atomic_t* terminate = nullptr;
    std::function<void(std::uint64_t)> copyCheckpointForTesting = {};
    std::optional<mode_t> creationUmask = std::nullopt;
    std::function<void(const std::string& dirPath, const std::string& srcName, const std::string& dstName)>
        beforeRenameHookForTesting = {};
    std::function<int(int oldDirFd, const char* oldName, int newDirFd, const char* newName, unsigned int flags)>
        renameAt2HookForTesting = {};
  };

  enum class AdminHelperSessionExitReason : std::uint8_t {
    CleanShutdown,
    ClientEof,
    HandshakeTimeout,
    InactivityTimeout,
    VersionMismatch,
    ProtocolViolation,
    IoError,
  };

  struct AdminHelperSessionResult {
    AdminHelperSessionExitReason reason = AdminHelperSessionExitReason::ClientEof;
    bool handshakeCompleted = false;
    std::uint64_t requestsHandled = 0;
    AdminRequestId lastCancelledTargetId = kInvalidAdminRequestId;
    int negotiatedInactivityTimeoutMs = kDefaultAdminHelperInactivityTimeoutMs;
    std::string detail;
  };

  // Validates that the helper was invoked with no extra arguments, euid == 0, and a valid
  // non-root PKEXEC_UID environment variable from pkexec.
  [[nodiscard]] bool validateAdminHelperInvocation(
      int argc, uid_t effectiveUid, const char* pkexecUidEnv, std::uint32_t& invokingUidOut, std::string& errorOut
  );

  // Applies process-level hardening for the privileged helper: restrictive umask(0077),
  // non-dumpable process state, SIGPIPE ignore, chdir("/"), closing fds > 2, and environment scrubbing.
  [[nodiscard]] bool applyAdminHelperProcessHardening(std::string& errorOut);

  // Runs the bounded stdin/stdout session loop on `inFd` and `outFd`.
  [[nodiscard]] AdminHelperSessionResult
  runAdminHelperSession(int inFd, int outFd, const AdminHelperSessionConfig& config = {});

} // namespace filemanager
