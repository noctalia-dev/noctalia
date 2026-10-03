#include "auth/pam_authenticator.h"

#include "core/log.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

namespace {

  constexpr Logger kLog("pam");

  constexpr std::size_t kMaxPamMessageBytes = 4096;
  constexpr std::size_t kMaxPasswordBytes = 64 * 1024;

  void secureClear(std::string& value) {
    volatile char* ptr = value.empty() ? nullptr : value.data();
    for (std::size_t i = 0; i < value.size(); ++i) {
      ptr[i] = '\0';
    }
    value.clear();
  }

  struct PamConversationData {
    const char* password = nullptr;
    bool passwordSent = false;
  };

  struct PamHandle {
    pam_handle_t* h = nullptr;
    int lastRc = PAM_SUCCESS;

    PamHandle() = default;
    PamHandle(const PamHandle&) = delete;
    PamHandle& operator=(const PamHandle&) = delete;

    ~PamHandle() {
      if (h != nullptr) {
        pam_end(h, lastRc);
      }
    }
  };

  int pamConversation(int numMsg, const pam_message** msg, pam_response** response, void* appdataPtr) {
    if (numMsg <= 0 || msg == nullptr || response == nullptr || appdataPtr == nullptr) {
      return PAM_CONV_ERR;
    }

    auto* data = static_cast<PamConversationData*>(appdataPtr);
    auto* replies = static_cast<pam_response*>(std::calloc(static_cast<std::size_t>(numMsg), sizeof(pam_response)));
    if (replies == nullptr) {
      return PAM_BUF_ERR;
    }

    for (int i = 0; i < numMsg; ++i) {
      if (msg[i] == nullptr) {
        std::free(replies);
        return PAM_CONV_ERR;
      }

      switch (msg[i]->msg_style) {
      case PAM_PROMPT_ECHO_OFF:
        if (data->passwordSent) {
          for (int j = 0; j < i; ++j) {
            if (replies[j].resp != nullptr) {
              std::free(replies[j].resp);
            }
          }
          std::free(replies);
          return PAM_CONV_ERR;
        }
        replies[i].resp = ::strdup(data->password != nullptr ? data->password : "");
        data->passwordSent = true;
        break;
      case PAM_PROMPT_ECHO_ON:
        replies[i].resp = ::strdup("");
        break;
      case PAM_ERROR_MSG:
      case PAM_TEXT_INFO:
        replies[i].resp = nullptr;
        break;
      default:
        for (int j = 0; j <= i; ++j) {
          if (replies[j].resp != nullptr) {
            std::free(replies[j].resp);
          }
        }
        std::free(replies);
        return PAM_CONV_ERR;
      }

      if ((msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
          && replies[i].resp == nullptr) {
        for (int j = 0; j <= i; ++j) {
          if (replies[j].resp != nullptr) {
            std::free(replies[j].resp);
          }
        }
        std::free(replies);
        return PAM_BUF_ERR;
      }
    }

    *response = replies;
    return PAM_SUCCESS;
  }

  [[nodiscard]] bool writeAll(int fd, const void* data, std::size_t len) {
    auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = len;
    while (remaining > 0) {
      const ssize_t n = ::write(fd, bytes, remaining);
      if (n > 0) {
        bytes += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool readAll(int fd, void* data, std::size_t len) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t remaining = len;
    while (remaining > 0) {
      const ssize_t n = ::read(fd, bytes, remaining);
      if (n > 0) {
        bytes += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

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
    const bool sent = writeAll(fd, &len, sizeof(len)) && (len == 0 || writeAll(fd, password.data(), len));
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

  [[nodiscard]] bool writeResult(int fd, const PamAuthenticator::Result& result) {
    const std::uint8_t success = result.success ? 1 : 0;
    if (!writeAll(fd, &success, sizeof(success))) {
      return false;
    }
    const std::uint32_t len = static_cast<std::uint32_t>(std::min(result.message.size(), kMaxPamMessageBytes));
    if (!writeAll(fd, &len, sizeof(len))) {
      return false;
    }
    if (len > 0 && !writeAll(fd, result.message.data(), len)) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool readResult(int fd, PamAuthenticator::Result& result) {
    std::uint8_t success = 0;
    if (!readAll(fd, &success, sizeof(success)) || success > 1) {
      return false;
    }
    std::uint32_t len = 0;
    if (!readAll(fd, &len, sizeof(len))) {
      return false;
    }
    if (len > kMaxPamMessageBytes) {
      return false;
    }
    result.success = success != 0;
    result.message.clear();
    if (len > 0) {
      result.message.resize(len);
      if (!readAll(fd, result.message.data(), len)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] PamAuthenticator::Result authenticateDirect(std::string_view password, std::string_view service) {
    std::string user = PamAuthenticator::currentUsername();
    if (user.empty()) {
      return PamAuthenticator::Result{.success = false, .message = i18n::tr("auth.pam.user-unavailable")};
    }

    std::string passwordCopy(password);
    PamConversationData convData{.password = passwordCopy.c_str()};
    pam_conv conv = {
        .conv = &pamConversation,
        .appdata_ptr = &convData,
    };

    kLog.debug("authenticating user='{}' service='{}'", user, service);

    PamHandle pamh;
    const int startRc = pam_start(service.data(), user.c_str(), &conv, &pamh.h);
    if (startRc != PAM_SUCCESS || pamh.h == nullptr) {
      kLog.error(
          "pam_start failed rc={} ({})", startRc, pamh.h != nullptr ? pam_strerror(pamh.h, startRc) : "no handle"
      );
      secureClear(passwordCopy);
      return PamAuthenticator::Result{.success = false, .message = i18n::tr("auth.pam.start-failed")};
    }

    int rc = pam_authenticate(pamh.h, 0);
    kLog.debug("pam_authenticate rc={} ({})", rc, pam_strerror(pamh.h, rc));
    if (rc == PAM_SUCCESS) {
      // An unprivileged locker can't read /etc/shadow for the account stack, so
      // ignore PAM_AUTHINFO_UNAVAIL; pam_authenticate already proved identity.
      const int acctRc = pam_acct_mgmt(pamh.h, 0);
      kLog.debug("pam_acct_mgmt rc={} ({})", acctRc, pam_strerror(pamh.h, acctRc));
      if (acctRc != PAM_SUCCESS && acctRc != PAM_AUTHINFO_UNAVAIL) {
        rc = acctRc;
      }
    }
    const char* err = pam_strerror(pamh.h, rc);
    const std::string errStr = err != nullptr ? err : i18n::tr("auth.pam.authentication-failed");
    pamh.lastRc = rc;

    secureClear(passwordCopy);

    if (rc == PAM_SUCCESS) {
      kLog.debug("authentication succeeded for user='{}'", user);
      return PamAuthenticator::Result{.success = true, .message = {}};
    }

    kLog.warn("authentication failed for user='{}' rc={} ({})", user, rc, errStr);
    return PamAuthenticator::Result{.success = false, .message = errStr};
  }

} // namespace

PamAuthenticator::Result PamAuthenticator::authenticateCurrentUser(
    std::string_view password, std::string_view service, std::string_view language, std::string_view startFailureMessage
) const {
  // Re-exec before invoking PAM so the helper cannot inherit locked library
  // state from the shell's other threads.
  const auto fail = [startFailureMessage]() {
    return Result{.success = false, .message = std::string(startFailureMessage)};
  };

  if (password.size() > kMaxPasswordBytes || service.empty() || language.empty()) {
    return fail();
  }

  std::string serviceCopy(service);
  std::string languageCopy(language);

  int inPipe[2] = {-1, -1};
  int outPipe[2] = {-1, -1};
  if (!createPipe(inPipe)) {
    return fail();
  }
  if (!createPipe(outPipe)) {
    closePipe(inPipe);
    return fail();
  }

  const char* helperArgv[] = {
      "noctalia", "pam-helper", serviceCopy.c_str(), languageCopy.c_str(), nullptr,
  };

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
    ::execv("/proc/self/exe", const_cast<char* const*>(helperArgv));
    ::_exit(127);
  }

  closeFd(inPipe[0]);
  closeFd(outPipe[1]);

  const bool sentOk = sendPassword(inPipe[1], password);
  closeFd(inPipe[1]);

  Result result;
  const bool readOk = sentOk && readResult(outPipe[0], result);
  closeFd(outPipe[0]);

  int status = 0;
  pid_t waitResult = -1;
  do {
    waitResult = ::waitpid(pid, &status, 0);
  } while (waitResult < 0 && errno == EINTR);

  const bool exited = waitResult == pid && WIFEXITED(status);
  const int exitCode = exited ? WEXITSTATUS(status) : -1;
  const bool statusOk = readOk && (exitCode == 0 || exitCode == 1) && ((exitCode == 0) == result.success);
  if (!sentOk || !statusOk) {
    kLog.warn(
        "pam helper failed (sent={} read={} waited={} exited={} status={})", sentOk, readOk, waitResult == pid, exited,
        exitCode
    );
    return fail();
  }

  return result;
}

int PamAuthenticator::runHelperMode(int argc, char* argv[]) {
  if (argc != 4 || argv[2][0] == '\0' || argv[3][0] == '\0') {
    return 2;
  }

  const std::string_view service = argv[2];
  const std::string_view language = argv[3];
  i18n::Service::instance().init(language);
  if (i18n::Service::instance().language() != language) {
    return 2;
  }

  std::uint32_t len = 0;
  if (!readAll(STDIN_FILENO, &len, sizeof(len)) || len > kMaxPasswordBytes) {
    return 2;
  }
  std::string password(len, '\0');
  if (len > 0 && !readAll(STDIN_FILENO, password.data(), len)) {
    secureClear(password);
    return 2;
  }

  Result result = authenticateDirect(password, service);
  secureClear(password);

  if (!writeResult(STDOUT_FILENO, result)) {
    return 2;
  }
  return result.success ? 0 : 1;
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
