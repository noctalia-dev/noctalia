#include "ipc/ipc_service.h"

#include <cassert>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-ipc-service-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* result = ::mkdtemp(buffer.data());
    return result != nullptr ? std::filesystem::path(result) : std::filesystem::path{};
  }

  void writeAll(int fd, std::string_view text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
      const auto n = ::write(fd, text.data() + sent, text.size() - sent);
      assert(n > 0);
      sent += static_cast<std::size_t>(n);
    }
  }

  std::string readAll(int fd) {
    std::string response;
    char buf[1024];
    for (;;) {
      const auto n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      response.append(buf, static_cast<std::size_t>(n));
    }
    return response;
  }

  std::string sendRaw(IpcService& ipc, const std::filesystem::path& socketPath, std::string_view command) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = socketPath.string();
    assert(path.size() < sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    assert(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    writeAll(fd, command);
    assert(::shutdown(fd, SHUT_WR) == 0);

    ipc.dispatch();
    std::string response = readAll(fd);
    ::close(fd);
    return response;
  }

} // namespace

int main() {
  const auto runtimeDir = makeTempDir();
  assert(!runtimeDir.empty());
  constexpr const char* kWaylandDisplay = "noctalia-ipc-service-test";
  assert(::setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1) == 0);
  assert(::setenv("WAYLAND_DISPLAY", kWaylandDisplay, 1) == 0);

  IpcService ipc;
  ipc.registerHandler(
      "visible-command", [](const std::string& args) { return "visible:" + args + "\n"; }, "visible-command <value>",
      "Visible command"
  );
  ipc.registerHandler(
      "hidden-command", [](const std::string& args) { return "hidden:" + args + "\n"; }, "hidden-command <value>",
      "Hidden command", IpcService::HandlerVisibility::Hidden
  );

  assert(ipc.execute("visible-command ok") == "visible:ok\n");
  assert(ipc.execute("hidden-command ok") == "hidden:ok\n");
  assert(ipc.execute("visible-command line1\nline2\nline3") == "visible:line1\nline2\nline3\n");

  const std::string help = ipc.execute("--help");
  assert(help.find("visible-command <value>") != std::string::npos);
  assert(help.find("Visible command") != std::string::npos);
  assert(help.find("hidden-command") == std::string::npos);
  assert(help.find("Hidden command") == std::string::npos);

  ipc.registerHandler(
      "visible-command", [](const std::string&) { return "hidden-now\n"; }, "visible-command", "Now hidden",
      IpcService::HandlerVisibility::Hidden
  );

  assert(ipc.execute("visible-command") == "hidden-now\n");
  const std::string updatedHelp = ipc.execute("--help");
  assert(updatedHelp.find("visible-command") == std::string::npos);
  assert(updatedHelp.find("Now hidden") == std::string::npos);

  assert(ipc.start());
  const auto socketPath = runtimeDir / ("noctalia-" + std::string(kWaylandDisplay) + ".sock");
  assert(sendRaw(ipc, socketPath, "hidden-command line1\nline2\nline3") == "hidden:line1\nline2\nline3\n");

  return 0;
}
