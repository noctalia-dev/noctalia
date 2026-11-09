#include "compositors/sway/sway_keyboard_backend.h"
#include "compositors/sway/sway_runtime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr std::string_view kIpcMagic = "i3-ipc";
  constexpr std::size_t kIpcHeaderSize = 14;
  constexpr std::uint32_t kIpcSubscribe = 2;
  constexpr std::uint32_t kIpcInputEvent = 0x80000015U;

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

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

  // Waits until the backend's socket is readable, then lets it drain everything sent so far.
  void pump(SwayKeyboardBackend& backend) {
    pollfd pfd{.fd = backend.pollFd(), .events = POLLIN, .revents = 0};
    if (::poll(&pfd, 1, 1000) > 0) {
      backend.dispatchPoll(pfd.revents);
    }
  }

} // namespace

int main() {
  bool ok = true;

  const auto dir = std::filesystem::temp_directory_path() / ("noctalia-sway-kb-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const std::string socketPath = (dir / "ipc.sock").string();

  const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
  ok &= check(listener >= 0, "listener socket created");
  ok &= check(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "listener bound");
  ok &= check(::listen(listener, 1) == 0, "listener listening");

  ::setenv("SWAYSOCK", socketPath.c_str(), 1);
  compositors::sway::SwayRuntime runtime;
  int changes = 0;
  {
    SwayKeyboardBackend backend(runtime);
    backend.setChangeCallback([&changes] { ++changes; });

    ok &= check(backend.pollFd() < 0, "no socket before connecting");
    ok &= check(backend.connectSocket(), "connects to the sway IPC socket");
    ok &= check(backend.pollFd() >= 0, "exposes the socket for polling");

    const int peer = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    ok &= check(peer >= 0, "fake sway accepted the connection");

    std::uint32_t type = 0;
    std::string payload;
    ok &= check(readFrame(peer, type, payload), "received a subscribe frame");
    ok &= check(type == kIpcSubscribe, "frame is a SUBSCRIBE request");
    ok &= check(payload == R"(["input"])", "subscribes to input events");

    // The subscribe reply and unrelated input changes do not count as layout switches.
    sendAll(peer, frame(kIpcSubscribe, R"({"success":true})"));
    sendAll(peer, frame(kIpcInputEvent, R"({"change":"libinput_config","input":{"type":"pointer"}})"));
    pump(backend);
    ok &= check(changes == 0, "ignores the subscribe reply and non-layout input events");

    sendAll(peer, frame(kIpcInputEvent, R"({"change":"xkb_layout","input":{"type":"keyboard"}})"));
    pump(backend);
    ok &= check(changes == 1, "an xkb_layout event notifies the change callback");

    // Two events split across writes: a partial frame must wait for the rest.
    const std::string keymap = frame(kIpcInputEvent, R"({"change":"xkb_keymap","input":{"type":"keyboard"}})");
    sendAll(peer, keymap.substr(0, 10));
    pump(backend);
    ok &= check(changes == 1, "a partial frame is buffered");
    sendAll(peer, keymap.substr(10) + frame(kIpcInputEvent, R"({"change":"xkb_layout"})"));
    pump(backend);
    ok &= check(changes == 3, "buffered and back-to-back frames are all handled");

    ::close(peer);
    pump(backend);
    ok &= check(backend.pollFd() < 0, "closes the socket when sway hangs up");
    ok &= check(changes == 4, "a hang-up notifies so the label is re-read");
  }

  ::close(listener);
  std::filesystem::remove_all(dir);

  if (ok) {
    std::cout << "sway_keyboard_backend: all checks passed\n";
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
