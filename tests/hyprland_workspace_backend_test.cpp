#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/hyprland_workspace_backend.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  // Named workspaces (`workspace = name:0, ...`) get negative ids, in an order unrelated to
  // their names. The special workspace must stay out of the listing entirely.
  constexpr std::string_view kWorkspacesJson = R"([
    {"id": -1338, "name": "Grave", "monitor": "WAYLAND-1"},
    {"id": -1337, "name": "0", "monitor": "WAYLAND-1"},
    {"id": 8, "name": "8", "monitor": "WAYLAND-1"},
    {"id": -99, "name": "special:special", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kMonitorsJson = R"([
    {"name": "WAYLAND-1", "activeWorkspace": {"id": 8, "name": "8"}}
  ])";

  std::string replyFor(std::string_view command) {
    if (command.contains("j/workspaces")) {
      return std::string(kWorkspacesJson);
    }
    if (command.contains("j/monitors")) {
      return std::string(kMonitorsJson);
    }
    if (command.contains("j/status")) {
      return R"({"configProvider": "lua"})";
    }
    return "[]";
  }

  // Stands in for hyprctl's request socket: read the command until the peer half-closes,
  // answer with JSON, close.
  void serve(int listener, const std::atomic_bool& stop) {
    while (!stop.load()) {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client < 0) {
        if (stop.load()) {
          return;
        }
        continue;
      }
      std::string request;
      char buffer[1024];
      while (true) {
        const ssize_t bytes = ::recv(client, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
          break;
        }
        request.append(buffer, buffer + bytes);
      }
      const std::string reply = replyFor(request);
      (void)::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
      ::close(client);
    }
  }

  std::string describe(const std::vector<Workspace>& workspaces) {
    std::string out;
    for (const auto& workspace : workspaces) {
      if (!out.empty()) {
        out += ", ";
      }
      out += workspace.name + "(" + workspace.id + ")";
    }
    return out;
  }

  bool orderedById(const char* what, const std::vector<Workspace>& workspaces) {
    static const std::vector<std::string> expected{"-1338", "-1337", "8"};

    std::vector<std::string> actual;
    actual.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
      actual.push_back(workspace.id);
    }
    if (actual == expected) {
      return true;
    }
    std::cerr << "FAIL: " << what << " must be ordered by ascending hyprland id, got " << describe(workspaces) << '\n';
    return false;
  }

} // namespace

int main() {
  const std::string runtimeDir = "/tmp/noctalia-hypr-workspace-test-" + std::to_string(::getpid());
  const std::string signature = "test";
  const std::string socketDir = runtimeDir + "/hypr/" + signature;

  std::error_code ec;
  std::filesystem::create_directories(socketDir, ec);
  if (ec) {
    std::cerr << "FAIL: cannot create " << socketDir << ": " << ec.message() << '\n';
    return 1;
  }
  const std::string socketPath = socketDir + "/.socket.sock";

  const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    std::cerr << "FAIL: socket() failed\n";
    return 1;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socketPath.size() >= sizeof(address.sun_path)) {
    std::cerr << "FAIL: socket path too long\n";
    return 1;
  }
  std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "FAIL: bind() failed\n";
    return 1;
  }
  if (::listen(listener, 8) != 0) {
    std::cerr << "FAIL: listen() failed\n";
    return 1;
  }

  std::atomic_bool stop{false};
  std::thread server([&]() { serve(listener, stop); });

  // The runtime resolves its socket paths from the environment on construction.
  ::setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);
  ::setenv("HYPRLAND_INSTANCE_SIGNATURE", signature.c_str(), 1);

  bool ok = true;
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    backend.syncFromCompositor();

    ok = orderedById("all()", backend.all()) && ok;
    ok = orderedById("forOutput()", backend.forOutput(reinterpret_cast<wl_output*>(0x1))) && ok;
  }

  stop.store(true);
  ::shutdown(listener, SHUT_RDWR);
  ::close(listener);
  server.join();
  std::filesystem::remove_all(runtimeDir, ec);

  return ok ? 0 : 1;
}
