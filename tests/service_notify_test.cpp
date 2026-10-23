#include "app/service_notify.h"
#include "test_check.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  class NotifyListener {
  public:
    explicit NotifyListener(const std::string& address) : m_fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
      TEST_CHECK(m_fd >= 0);
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      TEST_CHECK(address.size() < sizeof(addr.sun_path));
      std::memcpy(addr.sun_path, address.data(), address.size());
      if (address[0] == '@') {
        addr.sun_path[0] = '\0';
      }
      const auto addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address.size());
      TEST_CHECK(::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), addrLen) == 0);
    }

    ~NotifyListener() { ::close(m_fd); }

    NotifyListener(const NotifyListener&) = delete;
    NotifyListener& operator=(const NotifyListener&) = delete;

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
  };

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-service-notify-XXXXXX").string();
    TEST_CHECK(::mkdtemp(pattern.data()) != nullptr);
    return pattern;
  }

} // namespace

int main() {
  const std::filesystem::path tempDir = makeTempDir();

  {
    const std::string socketPath = (tempDir / "notify-path.sock").string();
    NotifyListener listener(socketPath);
    TEST_CHECK(::setenv("NOTIFY_SOCKET", socketPath.c_str(), 1) == 0);

    ServiceNotifier notifier;
    notifier.takeSocketFromEnvironment();
    notifier.notifyReady();

    TEST_CHECK(listener.receive() == "READY=1");
  }

  {
    const std::string socketPath = (tempDir / "notify-env.sock").string();
    TEST_CHECK(::setenv("NOTIFY_SOCKET", socketPath.c_str(), 1) == 0);

    ServiceNotifier notifier;
    notifier.takeSocketFromEnvironment();

    TEST_CHECK(std::getenv("NOTIFY_SOCKET") == nullptr);
  }

  {
    const std::string abstractAddress = "@noctalia-service-notify-test-" + std::to_string(::getpid());
    NotifyListener listener(abstractAddress);
    TEST_CHECK(::setenv("NOTIFY_SOCKET", abstractAddress.c_str(), 1) == 0);

    ServiceNotifier notifier;
    notifier.takeSocketFromEnvironment();
    notifier.notifyReady();

    TEST_CHECK(listener.receive() == "READY=1");
  }

  {
    const std::string socketPath = (tempDir / "notify-unset.sock").string();
    NotifyListener listener(socketPath);
    TEST_CHECK(::unsetenv("NOTIFY_SOCKET") == 0);

    ServiceNotifier notifier;
    notifier.takeSocketFromEnvironment();
    TEST_CHECK(::setenv("NOTIFY_SOCKET", socketPath.c_str(), 1) == 0);
    notifier.notifyReady();

    TEST_CHECK(listener.receive() == std::nullopt);
  }

  std::filesystem::remove_all(tempDir);
  return 0;
}
