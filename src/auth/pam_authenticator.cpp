#include "auth/pam_authenticator.h"

#include "auth/pam_helper_protocol.h"
#include "core/log.h"

#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#ifndef NOCTALIA_PAM_HELPER
#error "NOCTALIA_PAM_HELPER must be defined"
#endif

namespace {

  constexpr Logger kLog("pam");

  constexpr const char* kHelperName = "noctalia-pam-helper";

  void closeFd(int& fd) {
    if (fd >= 0) {
      (void)::close(fd);
      fd = -1;
    }
  }

  void closePipe(int (&pipeFds)[2]) {
    closeFd(pipeFds[0]);
    closeFd(pipeFds[1]);
  }

  [[nodiscard]] bool moveFdAboveStdio(int& fd) {
    if (fd > STDERR_FILENO) {
      return true;
    }

    int movedFd = -1;
    do {
      movedFd = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    } while (movedFd < 0 && errno == EINTR);
    if (movedFd < 0) {
      return false;
    }

    closeFd(fd);
    fd = movedFd;
    return true;
  }

  [[nodiscard]] bool createPipe(int (&pipeFds)[2]) {
    if (::pipe2(pipeFds, O_CLOEXEC) != 0 || !moveFdAboveStdio(pipeFds[0]) || !moveFdAboveStdio(pipeFds[1])) {
      closePipe(pipeFds);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool sendPassword(int fd, std::string_view password) {
    sigset_t pipeMask;
    sigset_t previousMask;
    sigset_t pendingMask;
    if (::sigemptyset(&pipeMask) != 0
        || ::sigaddset(&pipeMask, SIGPIPE) != 0
        || ::pthread_sigmask(SIG_BLOCK, &pipeMask, &previousMask) != 0) {
      return false;
    }

    if (::sigpending(&pendingMask) != 0) {
      (void)::pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
      return false;
    }
    const int wasPending = ::sigismember(&pendingMask, SIGPIPE);
    if (wasPending < 0) {
      (void)::pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
      return false;
    }

    const auto len = static_cast<std::uint32_t>(password.size());
    errno = 0;
    const bool sent = pam_helper::writeAll(fd, &pam_helper::kProtocolVersion, sizeof(pam_helper::kProtocolVersion))
        && pam_helper::writeAll(fd, &len, sizeof(len))
        && (len == 0 || pam_helper::writeAll(fd, password.data(), len));
    const int writeError = errno;

    bool restoreSafe = true;
    if (!sent && writeError == EPIPE && wasPending == 0) {
      const timespec timeout{};
      int receivedSignal = -1;
      do {
        receivedSignal = ::sigtimedwait(&pipeMask, nullptr, &timeout);
      } while (receivedSignal < 0 && errno == EINTR);
      restoreSafe = receivedSignal == SIGPIPE || (receivedSignal < 0 && errno == EAGAIN);
    }

    if (!restoreSafe || ::pthread_sigmask(SIG_SETMASK, &previousMask, nullptr) != 0) {
      return false;
    }
    return sent;
  }

  struct HelperReply {
    std::uint8_t version = 0;
    pam_helper::Status status = pam_helper::Status::Failed;
    int pamRc = PAM_SUCCESS;
  };

  // Reads the version first; the rest only when it matches.
  [[nodiscard]] bool readReply(int fd, HelperReply& reply) {
    if (!pam_helper::readAll(fd, &reply.version, sizeof(reply.version))
        || reply.version != pam_helper::kProtocolVersion) {
      return false;
    }
    std::uint8_t status = 0;
    std::int32_t pamRc = 0;
    if (!pam_helper::readAll(fd, &status, sizeof(status))
        || status > static_cast<std::uint8_t>(pam_helper::kLastStatus)
        || !pam_helper::readAll(fd, &pamRc, sizeof(pamRc))) {
      return false;
    }
    reply.status = static_cast<pam_helper::Status>(status);
    reply.pamRc = pamRc;
    return true;
  }

  // A helper next to the running binary (build tree, portable bundle) wins over
  // the installed one, so a dev build never talks to an older installed helper.
  // Installed binaries have no helper beside them in bindir.
  [[nodiscard]] std::string resolveHelperPath() {
    std::array<char, PATH_MAX> exe{};
    const ssize_t n = ::readlink("/proc/self/exe", exe.data(), exe.size() - 1);
    if (n > 0) {
      std::string path(exe.data(), static_cast<std::size_t>(n));
      const auto slash = path.rfind('/');
      if (slash != std::string::npos) {
        path.resize(slash + 1);
        path += kHelperName;
        if (::access(path.c_str(), X_OK) == 0) {
          return path;
        }
      }
    }
    return ::access(NOCTALIA_PAM_HELPER, X_OK) == 0 ? NOCTALIA_PAM_HELPER : std::string{};
  }

} // namespace

PamAuthenticator::Result
PamAuthenticator::authenticateCurrentUser(std::string_view password, const Messages& messages) const {
  // PAM runs in a separate helper executable: the helper cannot inherit locked
  // library state from the shell's other threads, and distributions can grant
  // it the privileges their PAM stack needs without extending them to the shell.
  const auto fail = [&messages]() { return Result{.success = false, .message = messages.startFailed}; };

  if (password.size() > pam_helper::kMaxPasswordBytes) {
    return fail();
  }

  const std::string helperPath = resolveHelperPath();
  if (helperPath.empty()) {
    kLog.error("{} not found (expected at {})", kHelperName, NOCTALIA_PAM_HELPER);
    return fail();
  }

  int inPipe[2] = {-1, -1};
  int outPipe[2] = {-1, -1};
  if (!createPipe(inPipe)) {
    return fail();
  }
  if (!createPipe(outPipe)) {
    closePipe(inPipe);
    return fail();
  }

  const char* helperArgv[] = {kHelperName, nullptr};

  const pid_t pid = ::fork();
  if (pid < 0) {
    closePipe(inPipe);
    closePipe(outPipe);
    return fail();
  }

  if (pid == 0) {
    // Keep this path async-signal-safe until execv().
    if (::dup2(inPipe[0], STDIN_FILENO) < 0 || ::dup2(outPipe[1], STDOUT_FILENO) < 0) {
      ::_exit(127);
    }
    (void)::close(inPipe[0]);
    (void)::close(inPipe[1]);
    (void)::close(outPipe[0]);
    (void)::close(outPipe[1]);
    ::execv(helperPath.c_str(), const_cast<char* const*>(helperArgv));
    ::_exit(127);
  }

  closeFd(inPipe[0]);
  closeFd(outPipe[1]);

  const bool sentOk = sendPassword(inPipe[1], password);
  closeFd(inPipe[1]);

  // Read even if sending failed: a helper that rejects our protocol version
  // replies with its own before closing stdin.
  HelperReply reply;
  const bool readOk = readReply(outPipe[0], reply);
  closeFd(outPipe[0]);

  int status = 0;
  pid_t waitResult = -1;
  do {
    waitResult = ::waitpid(pid, &status, 0);
  } while (waitResult < 0 && errno == EINTR);

  const bool exited = waitResult == pid && WIFEXITED(status);
  const int exitCode = exited ? WEXITSTATUS(status) : -1;
  const bool succeeded = reply.status == pam_helper::Status::Success;
  const bool statusOk = readOk
      && (exitCode == pam_helper::kExitSuccess || exitCode == pam_helper::kExitFailure)
      && ((exitCode == pam_helper::kExitSuccess) == succeeded);
  if (reply.version != 0 && reply.version != pam_helper::kProtocolVersion) {
    kLog.error(
        "pam helper {} speaks protocol {}, expected {}; restart noctalia after an upgrade", helperPath, reply.version,
        pam_helper::kProtocolVersion
    );
    return fail();
  }
  if (!sentOk || !statusOk) {
    kLog.warn(
        "pam helper {} failed (sent={} read={} waited={} exited={} status={})", helperPath, sentOk, readOk,
        waitResult == pid, exited, exitCode
    );
    return fail();
  }

  const char* pamError = pam_strerror(nullptr, reply.pamRc);
  switch (reply.status) {
  case pam_helper::Status::Success:
    kLog.debug("authentication succeeded");
    return Result{.success = true, .message = {}};
  case pam_helper::Status::UserUnavailable:
    kLog.error("pam helper could not resolve the current user");
    return Result{.success = false, .message = messages.userUnavailable};
  case pam_helper::Status::StartFailed:
    kLog.error("pam_start failed rc={} ({})", reply.pamRc, pamError != nullptr ? pamError : "");
    return Result{.success = false, .message = messages.startFailed};
  case pam_helper::Status::Failed:
    break;
  }

  kLog.warn("authentication failed rc={} ({})", reply.pamRc, pamError != nullptr ? pamError : "");
  return Result{.success = false, .message = pamError != nullptr ? pamError : messages.authenticationFailed};
}

std::string PamAuthenticator::currentUsername() {
  const uid_t uid = getuid();
  passwd pwd{};
  passwd* result = nullptr;
  std::vector<char> buf(4096);

  while (true) {
    const int rc = getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result);
    if (rc == 0 && result != nullptr) {
      return std::string(result->pw_name != nullptr ? result->pw_name : "");
    }
    if (rc != ERANGE) {
      return {};
    }
    buf.resize(buf.size() * 2);
    if (buf.size() > 1 << 20) {
      return {};
    }
  }
}
