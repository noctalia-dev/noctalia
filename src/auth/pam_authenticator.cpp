#include "auth/pam_authenticator.h"

#include "i18n/i18n.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <security/pam_appl.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

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

} // namespace

PamAuthenticator::Result PamAuthenticator::authenticateCurrentUser(std::string_view password) const {
  std::string user = currentUsername();
  if (user.empty()) {
    return Result{.success = false, .message = i18n::tr("auth.pam.user-unavailable")};
  }

  std::string passwordCopy(password);
  PamConversationData convData{.password = passwordCopy.c_str()};
  pam_conv conv = {
      .conv = &pamConversation,
      .appdata_ptr = &convData,
  };

  PamHandle pamh;
  const int startRc = pam_start("login", user.c_str(), &conv, &pamh.h);
  if (startRc != PAM_SUCCESS || pamh.h == nullptr) {
    secureClear(passwordCopy);
    return Result{.success = false, .message = i18n::tr("auth.pam.start-failed")};
  }

  int rc = pam_authenticate(pamh.h, 0);
  if (rc == PAM_SUCCESS) {
    rc = pam_acct_mgmt(pamh.h, 0);
  }
  const char* err = pam_strerror(pamh.h, rc);
  const std::string errStr = err != nullptr ? err : i18n::tr("auth.pam.authentication-failed");
  pamh.lastRc = rc;

  secureClear(passwordCopy);

  if (rc == PAM_SUCCESS) {
    return Result{.success = true, .message = {}};
  }

  return Result{.success = false, .message = errStr};
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
