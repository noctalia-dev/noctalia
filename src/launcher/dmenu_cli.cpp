#include "launcher/dmenu_cli.h"

#include "launcher/dmenu_socket_path.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace noctalia::launcher {

  namespace {

    [[nodiscard]] bool writeAll(int fd, const char* data, std::size_t size) {
      std::size_t sent = 0;
      while (sent < size) {
        const auto n = ::write(fd, data + sent, size - sent);
        if (n <= 0) {
          if (n < 0 && (errno == EINTR)) {
            continue;
          }
          return false;
        }
        sent += static_cast<std::size_t>(n);
      }
      return true;
    }

    void printUsage() {
      std::println(
          stderr,
          "Usage: noctalia dmenu [-p prompt]\n"
          "Reads newline-separated items from stdin, presents them in the launcher,\n"
          "and prints the selection to stdout."
      );
    }

  } // namespace

  int runDmenuCli(int argc, char** argv) {
    std::string prompt;
    for (int i = 2; i < argc; ++i) {
      const char* arg = argv[i];
      if ((std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--prompt") == 0) && i + 1 < argc) {
        prompt = argv[++i];
      } else if (std::strncmp(arg, "--prompt=", 9) == 0) {
        prompt = arg + 9;
      } else if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
        printUsage();
        return 0;
      } else {
        std::println(stderr, "error: unknown argument: {}", arg);
        printUsage();
        return 2;
      }
    }

    // Read all of stdin.
    std::string candidates;
    {
      char buf[8192];
      for (;;) {
        const auto n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        candidates.append(buf, static_cast<std::size_t>(n));
      }
    }

    const auto socketPath = resolveDmenuSocketPath();
    if (socketPath.path.empty()) {
      std::println(stderr, "error: {}", socketPath.error);
      return 1;
    }

    const std::string& path = socketPath.path;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      std::println(stderr, "error: socket() failed: {}", std::strerror(errno));
      return 1;
    }

    // Bound the connect/send, but not the recv — the user may take any time to pick.
    timeval tv{};
    tv.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      std::println(stderr, "error: socket path too long");
      ::close(fd);
      return 1;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
      std::println(stderr, "error: noctalia is not running");
      ::close(fd);
      return 1;
    }

    // Header line is the prompt, then candidate bytes follow on the same connection.
    if (!writeAll(fd, prompt.data(), prompt.size())
        || ::write(fd, "\n", 1) != 1
        || !writeAll(fd, candidates.data(), candidates.size())) {
      std::println(stderr, "error: write() failed: {}", std::strerror(errno));
      ::close(fd);
      return 1;
    }
    ::shutdown(fd, SHUT_WR); // signal end of candidates

    // Read the selection until the daemon closes the connection.
    std::string response;
    {
      char buf[4096];
      for (;;) {
        const auto n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        response.append(buf, static_cast<std::size_t>(n));
      }
    }
    ::close(fd);

    if (response.empty()) {
      return 1; // cancelled
    }
    std::fputs(response.c_str(), stdout);
    return 0;
  }

} // namespace noctalia::launcher
