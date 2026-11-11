#include "compositors/sway/sway_keyboard_backend.h"
#include "compositors/sway/sway_runtime.h"
#include "compositors/sway/sway_workspace_backend.h"
#include "tests/test_check.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

  constexpr std::string_view kIpcMagic = "i3-ipc";
  constexpr std::size_t kIpcHeaderSize = 14;
  constexpr std::uint32_t kIpcGetWorkspaces = 1;
  constexpr std::uint32_t kIpcSubscribe = 2;
  constexpr std::uint32_t kIpcGetTree = 4;
  constexpr std::uint32_t kIpcInputEvent = 0x80000015U;

  std::string frame(std::uint32_t type, std::string_view payload) {
    const auto length = static_cast<std::uint32_t>(payload.size());
    std::string out(kIpcMagic);
    out.append(reinterpret_cast<const char*>(&length), sizeof(length));
    out.append(reinterpret_cast<const char*>(&type), sizeof(type));
    out.append(payload);
    return out;
  }

  bool sendAll(int fd, const std::string& data) {
    return ::send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size());
  }

  // Reads one i3-ipc frame from the fake compositor side of the connection.
  bool readFrame(int fd, std::uint32_t& type, std::string& payload) {
    char header[kIpcHeaderSize];
    if (::recv(fd, header, sizeof(header), MSG_WAITALL) != static_cast<ssize_t>(sizeof(header))) {
      return false;
    }
    if (std::string_view(header, kIpcMagic.size()) != kIpcMagic) {
      return false;
    }
    std::uint32_t length = 0;
    std::memcpy(&length, header + kIpcMagic.size(), sizeof(length));
    std::memcpy(&type, header + kIpcMagic.size() + sizeof(length), sizeof(type));
    payload.assign(length, '\0');
    return length == 0 || ::recv(fd, payload.data(), length, MSG_WAITALL) == static_cast<ssize_t>(length);
  }

  // Waits until the runtime's socket is readable, then lets it drain everything sent so far.
  void pump(compositors::sway::SwayRuntime& runtime) {
    pollfd pfd{.fd = runtime.pollFd(), .events = POLLIN, .revents = 0};
    if (::poll(&pfd, 1, 1000) > 0) {
      runtime.dispatchPoll(pfd.revents);
    }
  }

} // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() / ("noctalia-sway-kb-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const std::string socketPath = (dir / "ipc.sock").string();

  const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  TEST_CHECK(listener >= 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
  TEST_CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  TEST_CHECK(::listen(listener, 1) == 0);

  ::setenv("SWAYSOCK", socketPath.c_str(), 1);
  compositors::sway::SwayRuntime runtime;
  int changes = 0;
  {
    SwayKeyboardBackend backend(runtime);
    SwayWorkspaceBackend workspaces({}, runtime);
    backend.setChangeCallback([&changes] { ++changes; });

    // The workspace backend connects the shared socket, as WaylandWorkspaces does on sway.
    TEST_CHECK(runtime.pollFd() < 0);
    TEST_CHECK(workspaces.connectSocket());
    TEST_CHECK(runtime.pollFd() >= 0);

    const int peer = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    TEST_CHECK(peer >= 0);

    // One connection carries one subscription covering both handlers, then the workspace snapshot requests.
    std::uint32_t type = 0;
    std::string payload;
    TEST_CHECK(readFrame(peer, type, payload));
    TEST_CHECK(type == kIpcSubscribe);
    TEST_CHECK(payload == R"(["workspace","window","input"])");
    TEST_CHECK(readFrame(peer, type, payload));
    TEST_CHECK(type == kIpcGetWorkspaces);
    TEST_CHECK(readFrame(peer, type, payload));
    TEST_CHECK(type == kIpcGetTree);

    // Replies reach the workspace backend; they and unrelated input changes are not layout switches.
    sendAll(peer, frame(kIpcSubscribe, R"({"success":true})"));
    sendAll(peer, frame(kIpcGetWorkspaces, R"([{"name":"1","num":1,"output":"DP-1","visible":true}])"));
    sendAll(peer, frame(kIpcInputEvent, R"({"change":"libinput_config","input":{"type":"pointer"}})"));
    pump(runtime);
    TEST_CHECK(workspaces.all().size() == 1);
    TEST_CHECK(changes == 0);

    sendAll(peer, frame(kIpcInputEvent, R"({"change":"xkb_layout","input":{"type":"keyboard"}})"));
    pump(runtime);
    TEST_CHECK(changes == 1);

    // A partial frame waits for the rest.
    const std::string keymap = frame(kIpcInputEvent, R"({"change":"xkb_keymap","input":{"type":"keyboard"}})");
    sendAll(peer, keymap.substr(0, 10));
    pump(runtime);
    TEST_CHECK(changes == 1);
    // sway sends one event per keyboard device on a switch; a batch read in one go notifies once.
    sendAll(peer, keymap.substr(10) + frame(kIpcInputEvent, R"({"change":"xkb_layout"})"));
    pump(runtime);
    TEST_CHECK(changes == 2);

    // A hang-up closes the socket and notifies so the label is re-read.
    ::close(peer);
    pump(runtime);
    TEST_CHECK(runtime.pollFd() < 0);
    TEST_CHECK(changes == 3);
    TEST_CHECK(workspaces.all().empty());
  }

  ::close(listener);
  std::filesystem::remove_all(dir);
  return EXIT_SUCCESS;
}
