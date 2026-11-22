#include "filemanager/admin_helper_session.h"

#include <csignal>
#include <cstdlib>
#include <unistd.h>

namespace {
  volatile std::sig_atomic_t terminate = 0;

  void requestTermination(int) { terminate = 1; }
}

int main(int argc, char** argv) {
  (void)argv;
  std::uint32_t invokingUid = 0;
  std::string error;
  // This binary has no setuid bit. Only pkexec grants privilege through its dedicated Polkit action.
  if (!filemanager::validateAdminHelperInvocation(argc, geteuid(), std::getenv("PKEXEC_UID"), invokingUid, error))
    return 1;
  if (!filemanager::applyAdminHelperProcessHardening(error))
    return 1;

  struct sigaction action{};
  action.sa_handler = requestTermination;
  if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGTERM, &action, nullptr) != 0)
    return 1;

  filemanager::AdminHelperSessionConfig config;
  config.helperPid = static_cast<std::uint32_t>(getpid());
  config.invokingUid = invokingUid;
  config.handshakeTimeoutMs = 5000;
  config.terminate = &terminate;
  config.creationUmask = static_cast<mode_t>(0022);

  const auto outcome = filemanager::runAdminHelperSession(STDIN_FILENO, STDOUT_FILENO, config);
  switch (outcome.reason) {
  case filemanager::AdminHelperSessionExitReason::CleanShutdown:
  case filemanager::AdminHelperSessionExitReason::ClientEof:
    return outcome.handshakeCompleted ? 0 : 1;
  case filemanager::AdminHelperSessionExitReason::HandshakeTimeout:
  case filemanager::AdminHelperSessionExitReason::InactivityTimeout:
  case filemanager::AdminHelperSessionExitReason::VersionMismatch:
  case filemanager::AdminHelperSessionExitReason::ProtocolViolation:
  case filemanager::AdminHelperSessionExitReason::IoError:
    return 1;
  }
  return 1;
}
