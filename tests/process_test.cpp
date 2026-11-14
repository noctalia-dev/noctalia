#include "system/process.h"
#include "tests/test_check.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
      if (ch == '\'') {
        quoted += "'\\''";
      } else {
        quoted += ch;
      }
    }
    quoted += '\'';
    return quoted;
  }

  void capturedAsyncDeliversCallbacksAndResult() {
    std::mutex mutex;
    std::condition_variable cv;
    std::string stdOut;
    std::string stdErr;
    std::optional<process::RunResult> result;
    bool completed = false;

    process::RunCallbacks callbacks;
    callbacks.stdOut = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      stdOut.append(chunk);
    };
    callbacks.stdErr = [&](std::string_view chunk) {
      std::scoped_lock lock(mutex);
      stdErr.append(chunk);
    };
    callbacks.onExit = [&](process::RunResult value) {
      std::scoped_lock lock(mutex);
      result = std::move(value);
      completed = true;
      cv.notify_one();
    };

    process::RunOptions options;
    options.timeout = std::chrono::seconds(2);
    options.maxOutputBytes = 3;

    TEST_CHECK(process::runAsync({"/bin/sh", "-lc", "printf abcdef; printf XYZ >&2"}, std::move(callbacks), options));

    std::unique_lock lock(mutex);
    TEST_CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }));
    TEST_CHECK(result.has_value());
    TEST_CHECK(stdOut == "abcdef");
    TEST_CHECK(stdErr == "XYZ");
    TEST_CHECK(result->exitCode == 0);
    TEST_CHECK(result->out == "abc");
    TEST_CHECK(result->err == "XYZ");
    TEST_CHECK(result->outTruncated);
    TEST_CHECK(!result->errTruncated);
    TEST_CHECK(!result->timedOut);
  }

  void capturedAsyncDeliversCompletionOnly() {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<process::RunResult> result;
    bool completed = false;

    process::RunCallbacks callbacks;
    callbacks.onExit = [&](process::RunResult value) {
      std::scoped_lock lock(mutex);
      result = std::move(value);
      completed = true;
      cv.notify_one();
    };

    TEST_CHECK(process::runAsync({"/bin/sh", "-c", "printf ok; exit 7"}, std::move(callbacks)));

    std::unique_lock lock(mutex);
    TEST_CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }));
    TEST_CHECK(result.has_value());
    TEST_CHECK(result->exitCode == 7);
    TEST_CHECK(result->out == "ok");
    TEST_CHECK(result->err.empty());
  }

  void syncAppliesEnvOverrides() {
    ::setenv("NOCTALIA_PROCESS_UNSET_TEST", "parent", 1);

    process::RunOptions options;
    options.env.push_back({"NOCTALIA_PROCESS_SET_TEST", "child"});
    options.env.push_back({"NOCTALIA_PROCESS_UNSET_TEST", std::nullopt});

    const auto result = process::runSync(
        {"/bin/sh", "-lc", R"(printf '%s/%s' "$NOCTALIA_PROCESS_SET_TEST" "${NOCTALIA_PROCESS_UNSET_TEST-unset}")"},
        options
    );
    ::unsetenv("NOCTALIA_PROCESS_UNSET_TEST");

    TEST_CHECK(result.exitCode == 0);
    TEST_CHECK(result.out == "child/unset");
  }

  void syncFeedsStdinThenEof() {
    process::RunOptions options;
    options.stdinData = "first\nsecond\n";
    options.timeout = std::chrono::seconds(5);
    // `cat` only exits once it sees EOF, so this also checks stdin gets closed.
    const auto fed = process::runSync(std::vector<std::string>{"cat"}, options);
    TEST_CHECK(fed.exitCode == 0 && !fed.timedOut);
    TEST_CHECK(fed.out == "first\nsecond");

    // Stdin larger than a pipe buffer fails the run.
    process::RunOptions oversized;
    oversized.stdinData = std::string(std::size_t{1} << 24, 'x');
    const auto rejected = process::runSync(std::vector<std::string>{"cat"}, oversized);
    TEST_CHECK(rejected.exitCode == -1);
  }

  void stringCommandsSupportShellComposition() {
    const auto result = process::runSync("printf first && printf second");
    TEST_CHECK(result.exitCode == 0);
    TEST_CHECK(result.out == "firstsecond");
  }

  void detachedAsyncInheritsLaunchEnvironment() {
    const std::filesystem::path outPath =
        std::filesystem::temp_directory_path() / ("noctalia_process_env_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    ::setenv("NOCTALIA_WALLPAPER_PATH", "/tmp/noctalia test/wallpaper.png", 1);
    ::setenv("NOCTALIA_WALLPAPER_CONNECTOR", "DP-1", 1);

    const std::string command = R"(printf '%s\n%s' "$NOCTALIA_WALLPAPER_PATH" "$NOCTALIA_WALLPAPER_CONNECTOR" > )"
        + shellQuote(outPath.string());
    const bool launched = process::runAsync(command);
    ::unsetenv("NOCTALIA_WALLPAPER_PATH");
    ::unsetenv("NOCTALIA_WALLPAPER_CONNECTOR");
    TEST_CHECK(launched);

    std::string contents;
    for (int i = 0; i < 50; ++i) {
      std::ifstream in(outPath);
      contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      if (contents == "/tmp/noctalia test/wallpaper.png\nDP-1") {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::filesystem::remove(outPath, ec);
    TEST_CHECK(contents == "/tmp/noctalia test/wallpaper.png\nDP-1");
  }

  void commandExistsRejectsDirectories() {
    TEST_CHECK(!process::commandExists("/usr/bin"));
    TEST_CHECK(!process::commandExists("/"));
    TEST_CHECK(process::commandExists("true"));
    TEST_CHECK(!process::commandExists(""));
    TEST_CHECK(!process::commandExists("/nonexistent"));
  }

  void cgroupDetectsSystemdUserManager() {
    // uwsm compositor unit.
    TEST_CHECK(
        process::cgroupIndicatesSystemdUserManager(
            "0::/user.slice/user-1000.slice/user@1000.service/session.slice/wayland-wm@niri.service\n", 1000
        )
    );
    // Noctalia user service.
    TEST_CHECK(
        process::cgroupIndicatesSystemdUserManager(
            "0::/user.slice/user-1000.slice/user@1000.service/app.slice/noctalia.service\n", 1000
        )
    );
    // Login session scope.
    TEST_CHECK(
        !process::cgroupIndicatesSystemdUserManager("0::/user.slice/user-1000.slice/session-2.scope/noctalia\n", 1000)
    );
    // Another user's manager.
    TEST_CHECK(!process::cgroupIndicatesSystemdUserManager(
        "0::/user.slice/user-1001.slice/user@1001.service/app.slice/noctalia.service\n", 1000
    ));
    // Legacy cgroup v1 dump.
    TEST_CHECK(
        process::cgroupIndicatesSystemdUserManager(
            "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/app.slice/noctalia.service\n", 1000
        )
    );
    TEST_CHECK(!process::cgroupIndicatesSystemdUserManager("", 1000));
  }

  void detachedMissingBinaryReturnsFalse() {
    TEST_CHECK(!process::runAsync(std::vector<std::string>{"/nonexistent/noctalia-missing-binary-xyz"}));
  }

  class NotifyListener {
  public:
    explicit NotifyListener(const std::string& address) : m_fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
      sockaddr_un addr{};
      if (address.empty() || address.size() >= sizeof(addr.sun_path)) {
        return;
      }
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, address.data(), address.size());
      if (address.front() == '@') {
        addr.sun_path[0] = '\0';
      }
      const auto addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address.size());
      m_bound = m_fd >= 0 && ::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), addrLen) == 0;
    }

    ~NotifyListener() {
      if (m_fd >= 0) {
        ::close(m_fd);
      }
    }

    NotifyListener(const NotifyListener&) = delete;
    NotifyListener& operator=(const NotifyListener&) = delete;

    [[nodiscard]] bool bound() const { return m_bound; }

    std::optional<std::string> receive() {
      std::array<char, 256> buffer{};
      const ssize_t received = ::recv(m_fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
      if (received < 0) {
        return std::nullopt;
      }
      return std::string(buffer.data(), static_cast<std::size_t>(received));
    }

  private:
    int m_fd;
    bool m_bound = false;
  };

  void notifyServiceReadySendsToPathSocket() {
    std::string dir = (std::filesystem::temp_directory_path() / "noctalia-notify-XXXXXX").string();
    TEST_CHECK(::mkdtemp(dir.data()) != nullptr);
    const std::string socketPath = dir + "/notify.sock";
    std::optional<std::string> received;
    bool bound = false;
    bool sent = false;
    {
      NotifyListener listener(socketPath);
      bound = listener.bound();
      sent = process::notifyServiceReady(socketPath);
      received = listener.receive();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    TEST_CHECK(bound);
    TEST_CHECK(sent);
    TEST_CHECK(received == "READY=1");
  }

  void notifyServiceReadySendsToAbstractSocket() {
    const std::string address = "@noctalia-notify-test-" + std::to_string(::getpid());
    NotifyListener listener(address);
    TEST_CHECK(listener.bound());
    TEST_CHECK(process::notifyServiceReady(address));
    TEST_CHECK(listener.receive() == "READY=1");
  }

  void notifyServiceReadyHandlesMissingAndInvalidSockets() {
    // An empty socket path is a successful no-op.
    TEST_CHECK(process::notifyServiceReady(""));

    errno = 0;
    TEST_CHECK(!process::notifyServiceReady("relative.sock"));
    TEST_CHECK(errno == EINVAL);

    errno = 0;
    TEST_CHECK(!process::notifyServiceReady("/" + std::string(sizeof(sockaddr_un::sun_path), 'n')));
    TEST_CHECK(errno == EINVAL);

    // A socket nobody listens on fails and reports why.
    errno = 0;
    TEST_CHECK(!process::notifyServiceReady("/nonexistent-noctalia-notify/notify.sock"));
    TEST_CHECK(errno == ENOENT);
  }

  void directSystemdServiceRequiresMatchingExecPid() {
    TEST_CHECK(!process::isDirectSystemdService(42, nullptr));
    TEST_CHECK(!process::isDirectSystemdService(42, ""));
    TEST_CHECK(!process::isDirectSystemdService(42, "41"));
    TEST_CHECK(!process::isDirectSystemdService(42, "42suffix"));
    TEST_CHECK(!process::isDirectSystemdService(42, "-42"));
    TEST_CHECK(!process::isDirectSystemdService(0, "0"));
    TEST_CHECK(process::isDirectSystemdService(42, "42"));
  }

} // namespace

int main() {
  // An empty callback set does not launch.
  TEST_CHECK(!process::runAsync("true", process::RunCallbacks{}));
  capturedAsyncDeliversCallbacksAndResult();
  capturedAsyncDeliversCompletionOnly();
  syncAppliesEnvOverrides();
  syncFeedsStdinThenEof();
  stringCommandsSupportShellComposition();
  detachedAsyncInheritsLaunchEnvironment();
  detachedMissingBinaryReturnsFalse();
  commandExistsRejectsDirectories();
  cgroupDetectsSystemdUserManager();
  notifyServiceReadySendsToPathSocket();
  notifyServiceReadySendsToAbstractSocket();
  notifyServiceReadyHandlesMissingAndInvalidSockets();
  directSystemdServiceRequiresMatchingExecPid();
  return 0;
}
