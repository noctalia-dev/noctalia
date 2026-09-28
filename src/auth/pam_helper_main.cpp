// noctalia-pam-helper: runs the PAM conversation for the lock screen.
//
// Kept as a separate, minimal executable so distributions whose PAM stack
// needs extra privileges for unprivileged password checks (e.g. setgid for
// pam_tcb's password-check helper) can grant them to this helper alone, never
// to the shell. It takes no arguments and only authenticates the invoking user.

#include "auth/pam_helper_protocol.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <security/pam_appl.h>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef NOCTALIA_PAM_SERVICE
#error "NOCTALIA_PAM_SERVICE must be defined"
#endif

namespace {

  void secureClear(std::string& value) {
    volatile char* ptr = value.empty() ? nullptr : value.data();
    for (std::size_t i = 0; i < value.size(); ++i) {
      ptr[i] = '\0';
    }
    value.clear();
  }

  struct PamConversationData {
    const char* password = nullptr;
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

  void freeReplies(pam_response* replies, int count) {
    for (int j = 0; j < count; ++j) {
      if (replies[j].resp != nullptr) {
        std::free(replies[j].resp);
      }
    }
    std::free(replies);
  }

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
        freeReplies(replies, i);
        return PAM_CONV_ERR;
      }

      switch (msg[i]->msg_style) {
      case PAM_PROMPT_ECHO_OFF:
        replies[i].resp = ::strdup(data->password != nullptr ? data->password : "");
        break;
      case PAM_PROMPT_ECHO_ON:
        replies[i].resp = ::strdup("");
        break;
      case PAM_ERROR_MSG:
      case PAM_TEXT_INFO:
        replies[i].resp = nullptr;
        break;
      default:
        freeReplies(replies, i + 1);
        return PAM_CONV_ERR;
      }

      if ((msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
          && replies[i].resp == nullptr) {
        freeReplies(replies, i + 1);
        return PAM_BUF_ERR;
      }
    }

    *response = replies;
    return PAM_SUCCESS;
  }

  [[nodiscard]] std::string currentUsername() {
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

  struct Outcome {
    pam_helper::Status status = pam_helper::Status::Failed;
    int pamRc = PAM_SUCCESS;
  };

  [[nodiscard]] Outcome authenticate(const std::string& password) {
    const std::string user = currentUsername();
    if (user.empty()) {
      return Outcome{.status = pam_helper::Status::UserUnavailable, .pamRc = PAM_USER_UNKNOWN};
    }

    PamConversationData convData{.password = password.c_str()};
    const pam_conv conv = {
        .conv = &pamConversation,
        .appdata_ptr = &convData,
    };

    PamHandle pamh;
    const int startRc = pam_start(NOCTALIA_PAM_SERVICE, user.c_str(), &conv, &pamh.h);
    if (startRc != PAM_SUCCESS || pamh.h == nullptr) {
      return Outcome{.status = pam_helper::Status::StartFailed, .pamRc = startRc};
    }

    int rc = pam_authenticate(pamh.h, 0);
    if (rc == PAM_SUCCESS) {
      // An unprivileged locker can't read the shadow database for the account
      // stack: pam_unix reports PAM_AUTHINFO_UNAVAIL, pam_tcb reports
      // PAM_CRED_INSUFFICIENT. pam_authenticate already proved identity.
      const int acctRc = pam_acct_mgmt(pamh.h, 0);
      if (acctRc != PAM_SUCCESS && acctRc != PAM_AUTHINFO_UNAVAIL && acctRc != PAM_CRED_INSUFFICIENT) {
        rc = acctRc;
      }
    }
    pamh.lastRc = rc;

    return Outcome{.status = rc == PAM_SUCCESS ? pam_helper::Status::Success : pam_helper::Status::Failed, .pamRc = rc};
  }

} // namespace

int main(int argc, char* /*argv*/[]) {
  if (argc != 1) {
    return pam_helper::kExitProtocolError;
  }

  std::uint8_t version = 0;
  if (!pam_helper::readAll(STDIN_FILENO, &version, sizeof(version))) {
    return pam_helper::kExitProtocolError;
  }
  if (version != pam_helper::kProtocolVersion) {
    (void)pam_helper::writeAll(STDOUT_FILENO, &pam_helper::kProtocolVersion, sizeof(pam_helper::kProtocolVersion));
    return pam_helper::kExitProtocolError;
  }

  std::uint32_t len = 0;
  if (!pam_helper::readAll(STDIN_FILENO, &len, sizeof(len)) || len > pam_helper::kMaxPasswordBytes) {
    return pam_helper::kExitProtocolError;
  }
  std::string password(len, '\0');
  if (len > 0 && !pam_helper::readAll(STDIN_FILENO, password.data(), len)) {
    secureClear(password);
    return pam_helper::kExitProtocolError;
  }

  const Outcome outcome = authenticate(password);
  secureClear(password);

  const auto status = static_cast<std::uint8_t>(outcome.status);
  const auto pamRc = static_cast<std::int32_t>(outcome.pamRc);
  if (!pam_helper::writeAll(STDOUT_FILENO, &pam_helper::kProtocolVersion, sizeof(pam_helper::kProtocolVersion))
      || !pam_helper::writeAll(STDOUT_FILENO, &status, sizeof(status))
      || !pam_helper::writeAll(STDOUT_FILENO, &pamRc, sizeof(pamRc))) {
    return pam_helper::kExitProtocolError;
  }
  return outcome.status == pam_helper::Status::Success ? pam_helper::kExitSuccess : pam_helper::kExitFailure;
}
