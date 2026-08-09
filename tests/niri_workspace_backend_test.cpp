#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/niri_workspace_backend.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

} // namespace

int main() {
  bool ok = true;

  const std::string socketPath = "/tmp/noctalia-niri-workspace-test-" + std::to_string(getpid()) + ".sock";
  ::unlink(socketPath.c_str());

  const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (!check(listener >= 0, "socket() failed")) {
    return 1;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (!check(socketPath.size() < sizeof(address.sun_path), "socket path too long")) {
    return 1;
  }
  std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
  if (!check(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "bind() failed")) {
    return 1;
  }
  if (!check(::listen(listener, 1) == 0, "listen() failed")) {
    return 1;
  }

  // Exercise backend event handling without NIRI_SOCKET so construction does not
  // open the persistent event-stream connection (that path is covered elsewhere).
  compositors::niri::NiriRuntime runtime;
  NiriWorkspaceBackend backend(runtime);
  int changes = 0;
  backend.setChangeCallback([&]() { ++changes; });
  backend.handleEvent(
      "WorkspacesChanged",
      {{"workspaces",
        {
            {{"id", 1}, {"idx", 1}, {"output", "DP-1"}},
            {{"id", 2}, {"idx", 2}, {"output", "DP-1"}},
        }}}
  );
  changes = 0;
  backend.handleEvent(
      "WindowsChanged",
      {{"windows",
        {
            {{"id", 41},
             {"workspace_id", 1},
             {"app_id", "com.mitchellh.ghostty"},
             {"is_focused", true},
             {"layout", {{"pos_in_scrolling_layout", {1, 1}}}}},
            {{"id", 42},
             {"workspace_id", 2},
             {"app_id", "com.mitchellh.ghostty"},
             {"is_focused", false},
             {"layout", {{"pos_in_scrolling_layout", {1, 1}}}}},
        }}}
  );
  ok &= check(backend.focusedWindowId() == std::optional<std::string>("41"), "initial focused window should be 41");

  backend.handleEvent("WindowFocusChanged", {{"id", 42}});
  ok &= check(backend.focusedWindowId() == std::optional<std::string>("42"), "focused window should be 42 after event");
  ok &= check(changes == 2, "expected 2 changes after windows+focus events");

  // A new scrolling-layout position must invalidate taskbar ordering even
  // though workspace/app membership is unchanged.
  backend.handleEvent(
      "WindowOpenedOrChanged",
      {{"window",
        {{"id", 41},
         {"workspace_id", 1},
         {"app_id", "com.mitchellh.ghostty"},
         {"is_focused", false},
         {"layout", {{"pos_in_scrolling_layout", {2, 1}}}}}}}
  );
  ok &= check(changes == 3, "layout position change should trigger a change");
  auto windows = backend.workspaceWindows();
  const auto window41 = std::ranges::find(windows, "41", &WorkspaceWindow::windowId);
  ok &= check(
      window41 != windows.end() && window41->x == 2 && window41->y == 1, "window 41 should have updated position"
  );

  std::string request;
  int serverError = 0;
  std::jthread server([&]() {
    const int client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      serverError = 1;
      return;
    }
    char buffer[512];
    const ssize_t size = ::read(client, buffer, sizeof(buffer));
    if (size <= 0) {
      ::close(client);
      serverError = 2;
      return;
    }
    request.assign(buffer, static_cast<std::size_t>(size));
    constexpr std::string_view response = "{\"Ok\":null}\n";
    (void)::write(client, response.data(), response.size());
    ::close(client);
  });

  if (!check(::setenv("NIRI_SOCKET", socketPath.c_str(), 1) == 0, "setenv(NIRI_SOCKET) failed")) {
    return 1;
  }
  runtime.refresh();

  ok &= check(backend.closeWindowById("42"), "closeWindowById should succeed");
  server.join();
  ok &= check(serverError == 0, "server thread encountered an error");
  const nlohmann::json expectedRequest{{"Action", {{"CloseWindow", {{"id", 42}}}}}};
  ok &= check(nlohmann::json::parse(request) == expectedRequest, "request payload mismatch");
  ok &= check(!backend.closeWindowById("not-an-id"), "closeWindowById with bad id should fail");

  ::close(listener);
  ::unlink(socketPath.c_str());
  return ok ? 0 : 1;
}
