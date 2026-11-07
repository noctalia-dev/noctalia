#include "auth/pam_authenticator.h"
#include "test_check.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <future>
#include <poll.h>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {

  using namespace std::chrono_literals;

  constexpr std::string_view kStartFailure = "helper did not complete";

  bool writeBytes(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
      const ssize_t count = ::write(fd, bytes, size);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      bytes += count;
      size -= static_cast<std::size_t>(count);
    }
    return true;
  }

  bool readBytes(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
      const ssize_t count = ::read(fd, bytes, size);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      bytes += count;
      size -= static_cast<std::size_t>(count);
    }
    return true;
  }

  int runTestHelper(std::string_view mode, std::string_view readyFdText) {
    // This executable impersonates pam-helper. No system PAM service is used.
    ::alarm(10);
    if (mode != "no-read") {
      std::uint32_t size = 0;
      TEST_CHECK(readBytes(STDIN_FILENO, &size, sizeof(size)));
      TEST_CHECK(size <= 64 * 1024);
      std::string password(size, '\0');
      TEST_CHECK(readBytes(STDIN_FILENO, password.data(), password.size()));
    }

    if (mode == "success" || mode == "failure" || mode == "after-result") {
      const std::uint8_t success = mode == "failure" ? 0 : 1;
      const std::string message = mode == "failure" ? "rejected" : "";
      const auto size = static_cast<std::uint32_t>(message.size());
      TEST_CHECK(writeBytes(STDOUT_FILENO, &success, sizeof(success)));
      TEST_CHECK(writeBytes(STDOUT_FILENO, &size, sizeof(size)));
      TEST_CHECK(writeBytes(STDOUT_FILENO, message.data(), message.size()));
      if (mode != "after-result") {
        return success != 0 ? 0 : 1;
      }
    } else if (mode == "partial") {
      const std::uint8_t success = 1;
      TEST_CHECK(writeBytes(STDOUT_FILENO, &success, sizeof(success)));
    } else {
      TEST_CHECK(mode == "no-read" || mode == "blocked");
    }

    int readyFd = -1;
    const auto parsed = std::from_chars(readyFdText.data(), readyFdText.data() + readyFdText.size(), readyFd);
    TEST_CHECK(parsed.ec == std::errc{});
    TEST_CHECK(parsed.ptr == readyFdText.data() + readyFdText.size());
    TEST_CHECK(readyFd >= 0);
    const pid_t pid = ::getpid();
    TEST_CHECK(writeBytes(readyFd, &pid, sizeof(pid)));
    while (true) {
      ::pause();
    }
  }

  void checkNoChildren() {
    int status = 0;
    TEST_CHECK(::waitpid(-1, &status, WNOHANG) == -1);
    TEST_CHECK(errno == ECHILD);
  }

  void checkResults() {
    const PamAuthenticator authenticator;
    TEST_CHECK(authenticator.authenticateCurrentUser("test", "success", "unused", kStartFailure).success);
    std::stop_source cancellation;
    TEST_CHECK(authenticator
                   .authenticateCurrentUser("test", "success", "unused", kStartFailure, cancellation.get_token())
                   .success);
    const auto failure =
        authenticator.authenticateCurrentUser("test", "failure", "unused", kStartFailure, cancellation.get_token());
    TEST_CHECK(!failure.success);
    TEST_CHECK(failure.message == "rejected");
    // A completed attempt has no callback left that could signal another process.
    TEST_CHECK(cancellation.request_stop());
    checkNoChildren();
  }

  void checkAlreadyCancelled() {
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto result = PamAuthenticator{}.authenticateCurrentUser(
        "test", "success", "unused", kStartFailure, cancellation.get_token()
    );
    TEST_CHECK(!result.success);
    checkNoChildren();
  }

  void checkCancellation(std::string_view mode) {
    int ready[2] = {-1, -1};
    TEST_CHECK(::pipe(ready) == 0);
    std::stop_source cancellation;
    const std::string password(mode == "no-read" ? 64 * 1024 : 4, 'x');
    const std::string readyFd = std::to_string(ready[1]);
    auto result = std::async(std::launch::async, [&] {
      return PamAuthenticator{}.authenticateCurrentUser(
          password, mode, readyFd, kStartFailure, cancellation.get_token()
      );
    });

    pollfd readyPoll{.fd = ready[0], .events = POLLIN, .revents = 0};
    TEST_CHECK(::poll(&readyPoll, 1, 2000) == 1);
    pid_t helperPid = -1;
    TEST_CHECK(readBytes(ready[0], &helperPid, sizeof(helperPid)));
    TEST_CHECK(cancellation.request_stop());
    TEST_CHECK(result.wait_for(2s) == std::future_status::ready);
    const auto cancelled = result.get();
    TEST_CHECK(!cancelled.success);
    TEST_CHECK(cancelled.message == kStartFailure);
    TEST_CHECK(::kill(helperPid, 0) == -1);
    TEST_CHECK(errno == ESRCH);
    ::close(ready[0]);
    ::close(ready[1]);
    checkNoChildren();

    // A new attempt works without waiting for the abandoned one.
    TEST_CHECK(PamAuthenticator{}.authenticateCurrentUser("test", "success", "unused", kStartFailure).success);
  }

} // namespace

int main(int argc, char* argv[]) {
  if (argc == 4 && std::string_view(argv[1]) == "pam-helper") {
    return runTestHelper(argv[2], argv[3]);
  }
  ::alarm(30);
  checkResults();
  checkAlreadyCancelled();
  for (int attempt = 0; attempt < 5; ++attempt) {
    checkCancellation("no-read");
    checkCancellation("blocked");
    checkCancellation("partial");
    checkCancellation("after-result");
  }
  std::println("PAM helper cancellation: all checks passed");
}
