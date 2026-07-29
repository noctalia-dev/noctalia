#include "app/application.h"
#include "app/single_instance_lock.h"
#include "config/cli.h"
#include "core/build_info.h"
#include "core/log.h"
#include "core/process/process_fds.h"
#include "ipc/cli.h"
#include "launcher/dmenu_cli.h"
#include "scripting/plugin_lint.h"
#include "theme/cli.h"
#include "theme/firefox_theme/firefox_theme.h"

#include <array>
#include <cerrno>
#include <clocale>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <string>
#include <unistd.h>

#ifdef __GLIBC__
#ifdef NOCTALIA_USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#else
#include <malloc.h>
#endif
#endif

namespace {

  enum class SpawnResult { Parent, Error };

  constexpr const char* kDaemonPipeEnv = "NOCTALIA_DAEMON_PIPE_FD";
  int g_daemonPipe = -1;

  void closeFd(int& fd) {
    if (fd == -1) {
      return;
    }
    (void)::close(fd);
    fd = -1;
  }

  bool writeAll(int fd, const void* data, std::size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
      const ssize_t written = ::write(fd, bytes, size);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (written == 0) {
        return false;
      }
      bytes += written;
      size -= static_cast<std::size_t>(written);
    }
    return true;
  }

  bool readAll(int fd, void* data, std::size_t size) {
    char* bytes = static_cast<char*>(data);
    while (size > 0) {
      const ssize_t received = ::read(fd, bytes, size);
      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      if (received == 0) {
        return false;
      }
      bytes += received;
      size -= static_cast<std::size_t>(received);
    }
    return true;
  }

  bool redirectStdioToNull() {
    int fd = ::open("/dev/null", O_RDWR);
    if (fd == -1) {
      std::perror("open(\"/dev/null\")");
      return false;
    }

    bool ok = true;
    if (::dup2(fd, STDIN_FILENO) == -1) {
      std::perror("dup2(stdin)");
      ok = false;
    }
    if (::dup2(fd, STDOUT_FILENO) == -1) {
      std::perror("dup2(stdout)");
      ok = false;
    }
    if (::dup2(fd, STDERR_FILENO) == -1) {
      std::perror("dup2(stderr)");
      ok = false;
    }

    if (fd > STDERR_FILENO) {
      (void)::close(fd);
    }
    return ok;
  }

  void completeDaemonStartup(int code) {
    if (g_daemonPipe == -1) {
      return;
    }

    const int result = code;
    if (!writeAll(g_daemonPipe, &result, sizeof(result))) {
      std::println(stderr, "error: failed to notify daemon parent: {}", std::strerror(errno));
    }
    closeFd(g_daemonPipe);
    if (code == 0 && !redirectStdioToNull()) {
      std::println(stderr, "error: failed to redirect daemon stdio");
    }
  }

  int runTopLevelFlag(const char* flag) {
    if (std::strcmp(flag, "--version") == 0 || std::strcmp(flag, "-v") == 0) {
      const std::string version = noctalia::build_info::displayVersion();
      std::println("noctalia {}", version);
      return 0;
    }
    if (std::strcmp(flag, "--help") == 0 || std::strcmp(flag, "-h") == 0) {
      std::println(
          "Usage: noctalia [OPTIONS]\n"
          "\n"
          "Options:\n"
          "  -h, --help       Show this help message\n"
          "  -v, --version    Show version information\n"
          "  -d, --daemon     Run in background\n"
          "\n"
          "Subcommands:\n"
          "  msg <command>    Send a command to the running instance\n"
          "                   Run 'noctalia msg --help' for available commands\n"
          "  theme <image>    Generate a color palette from an image\n"
          "                   Run 'noctalia theme --help' for options\n"
          "  config <command> Validate config and support/replay helpers\n"
          "                   Run 'noctalia config --help' for options\n"
          "  plugins <cmd>    Offline plugin author tools (lint)\n"
          "                   Run 'noctalia plugins --help' for options\n"
          "\n"
          "For more information and documentation, visit:\n"
          "  https://noctalia.dev"
      );
      return 0;
    }
    return -1;
  }

  bool takeDaemonPipeFromEnv() {
    const char* value = std::getenv(kDaemonPipeEnv);
    if (value == nullptr || value[0] == '\0') {
      return false;
    }

    errno = 0;
    char* end = nullptr;
    const long fd = std::strtol(value, &end, 10);
    (void)::unsetenv(kDaemonPipeEnv);
    if (errno != 0 || end == value || *end != '\0' || fd < 0) {
      std::println(stderr, "error: invalid {} value: {}", kDaemonPipeEnv, value);
      return false;
    }

    g_daemonPipe = static_cast<int>(fd);
    return true;
  }

  SpawnResult daemonize(pid_t* outPid, int* parentPipe, char* const argv[]) {
    auto pipeFds = std::array<int, 2>{-1, -1};
    if (::pipe(pipeFds.data()) == -1) {
      std::perror("pipe");
      return SpawnResult::Error;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
      std::perror("fork");
      closeFd(pipeFds[0]);
      closeFd(pipeFds[1]);
      return SpawnResult::Error;
    }

    if (pid > 0) {
      if (outPid)
        *outPid = pid;
      if (parentPipe)
        *parentPipe = pipeFds[0];
      closeFd(pipeFds[1]);
      return SpawnResult::Parent;
    }

    closeFd(pipeFds[0]);
    // Match v4's early daemon boundary, but exec before shell startup so GLib,
    // D-Bus, and polkit start from a normal process image rather than a raw
    // post-fork child.
    if (::setsid() == -1) {
      std::perror("setsid");
      const int daemonResult = 1;
      (void)writeAll(pipeFds[1], &daemonResult, sizeof(daemonResult));
      closeFd(pipeFds[1]);
      _exit(1);
    }

    const std::string pipeFd = std::to_string(pipeFds[1]);
    if (::setenv(kDaemonPipeEnv, pipeFd.c_str(), 1) == -1) {
      std::perror("setenv");
      const int daemonResult = 1;
      (void)writeAll(pipeFds[1], &daemonResult, sizeof(daemonResult));
      closeFd(pipeFds[1]);
      _exit(1);
    }

    ::execvp(argv[0], argv);
    std::perror("execvp");
    const int daemonResult = 1;
    (void)writeAll(pipeFds[1], &daemonResult, sizeof(daemonResult));
    closeFd(pipeFds[1]);
    _exit(1);
  }

  int runShell() {
    // Raise the soft fd limit before any Wayland/EGL init. The NVIDIA EGL/Wayland
    // driver leaks internal sync_file fences slowly across a session; the default
    // 1024 soft cap can be exhausted in a long-running session, after which the
    // Wayland connection fails fatally.
    ProcessFds::raiseOpenFileLimit();

    // Claim the single-instance lock before any shell/Wayland init so the answer
    // is settled before bars or surfaces are created. Held for the process lifetime.
    SingleInstanceLock instanceLock;
    if (!instanceLock.tryAcquire()) {
      std::println(stderr, "error: noctalia is already running");
      completeDaemonStartup(1);
      _exit(1);
    }
    try {
      Application app;
      app.run([]() { completeDaemonStartup(0); });
    } catch (const std::exception& e) {
      logError("fatal: {}", e.what());
      completeDaemonStartup(1);
      return 1;
    }
    return 0;
  }

} // namespace

#ifdef NOCTALIA_USE_JEMALLOC
const char* malloc_conf = "narenas:2,dirty_decay_ms:1000,muzzy_decay_ms:5000,lg_tcache_max:12";
#endif

int main(int argc, char* argv[]) {

#if defined(__GLIBC__) && !defined(NOCTALIA_USE_JEMALLOC)
  mallopt(M_ARENA_MAX, 2);
#endif

  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  const bool isDaemonChild = takeDaemonPipeFromEnv();
  bool shouldDaemonize = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--daemon") == 0
        || std::strcmp(argv[i], "--daemonize") == 0
        || std::strcmp(argv[i], "-d") == 0) {
      shouldDaemonize = true;
      for (int j = i; j < argc - 1; ++j) {
        argv[j] = argv[j + 1];
      }
      --argc;
      argv[argc] = nullptr;
      break;
    }
  }

  if (argc >= 2) {
    if (noctalia::theme::isFirefoxNativeMessagingLaunch(argc, argv))
      return noctalia::theme::runFirefoxNativeMessagingHost();
    if (std::strcmp(argv[1], "firefox-theme") == 0)
      return noctalia::theme::runFirefoxThemeCli(argc, argv);
    if (std::strcmp(argv[1], "theme") == 0)
      return noctalia::theme::runCli(argc, argv);
    if (std::strcmp(argv[1], "msg") == 0)
      return noctalia::ipc::runCli(argc, argv);
    if (std::strcmp(argv[1], "config") == 0)
      return noctalia::config::runCli(argc, argv);
    if (std::strcmp(argv[1], "dmenu") == 0)
      return noctalia::launcher::runDmenuCli(argc, argv);
    if (std::strcmp(argv[1], "plugins") == 0)
      return noctalia::plugins::runCli(argc, argv);
  }

  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-') {
      const int rc = runTopLevelFlag(argv[i]);
      if (rc >= 0)
        return rc;

      std::println(stderr, "error: unknown option: {}", argv[i]);
      return 1;
    }
  }

  {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0' && ::chdir(home) != 0) {
      std::println(stderr, "warning: failed to chdir to HOME ({}): {}", home, std::strerror(errno));
    }
  }

  if (shouldDaemonize && !isDaemonChild) {
    pid_t pid = -1;
    int parentPipe = -1;
    SpawnResult result = daemonize(&pid, &parentPipe, argv);

    if (result == SpawnResult::Error) {
      return 1;
    }
    if (result == SpawnResult::Parent) {
      int daemonResult = 1;
      const bool receivedResult = readAll(parentPipe, &daemonResult, sizeof(daemonResult));
      closeFd(parentPipe);
      if (!receivedResult) {
        std::println(stderr, "error: failed to wait for daemon startup");
        return 1;
      }
      if (daemonResult == 0) {
        std::println("noctalia started [pid: {}]", pid);
      }
      return daemonResult;
    }
  }

  return runShell();
}
