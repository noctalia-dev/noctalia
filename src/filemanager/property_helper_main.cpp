#include "filemanager/property_operations.h"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

int main(int argc, char** argv) {
  filemanager::PropertyRequest request;
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i)
    arguments.emplace_back(argv[i]);
  filemanager::PropertyResult result;
  // This binary has no setuid bit. Only pkexec grants privilege through its dedicated action.
  if (geteuid() != 0 || !g_getenv("PKEXEC_UID"))
    result.error = "Run only through the installed Polkit action.";
  else if (!filemanager::parsePropertyArguments(arguments, request))
    result.error = "Malformed Properties request.";
  else {
    const auto flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0)
      result.error = "No cancellation channel.";
    else {
      pollfd startup{STDIN_FILENO, POLLIN | POLLHUP, 0};
      // Wait for the client's readiness byte or EOF before permitting any mutation.
      const bool ready = poll(&startup, 1, 5000) > 0;
      bool live = false;
      const auto cancelled = [&] {
        if (!ready)
          return true;
        pollfd input{STDIN_FILENO, POLLIN | POLLHUP, 0};
        if (poll(&input, 1, 0) < 0 || (input.revents & (POLLHUP | POLLERR | POLLNVAL)))
          return true;
        char byte = 0;
        if (input.revents & POLLIN) {
          if (read(STDIN_FILENO, &byte, 1) != 1 || byte != 'G' || live)
            return true;
          live = true;
        }
        return !live;
      };
      auto last = std::chrono::steady_clock::now();
      result = filemanager::executePropertyRequest(request, cancelled, [&](const auto& update) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(100))
          return;
        last = now;
        std::printf(
            "P %llu %llu %llu\n", static_cast<unsigned long long>(update.succeeded),
            static_cast<unsigned long long>(update.failed), static_cast<unsigned long long>(update.skipped)
        );
        std::fflush(stdout);
      });
    }
  }
  const auto message = result.error.substr(0, 2048);
  char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(message.data()), message.size());
  std::printf(
      "R %llu %llu %llu %u %u %s\n", static_cast<unsigned long long>(result.succeeded),
      static_cast<unsigned long long>(result.failed), static_cast<unsigned long long>(result.skipped),
      result.cancelled ? 1U : 0U, result.ownershipEffects ? 1U : 0U, *encoded ? encoded : "-"
  );
  g_free(encoded);
  return result.ok() ? 0 : 1;
}
