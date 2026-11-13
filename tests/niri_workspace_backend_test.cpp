#include "compositors/niri/niri_runtime.h"
#include "compositors/niri/niri_workspace_backend.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  std::string gSocketPath;

} // namespace

int main() {
  gSocketPath = "/tmp/noctalia-niri-workspace-test-" + std::to_string(getpid()) + ".sock";
  const std::string& socketPath = gSocketPath;
  ::unlink(socketPath.c_str());
  std::atexit([] { ::unlink(gSocketPath.c_str()); });

  const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  TEST_CHECK(listener >= 0);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  TEST_CHECK(socketPath.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
  TEST_CHECK(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
  TEST_CHECK(::listen(listener, 1) == 0);

  // Exercise backend event handling without NIRI_SOCKET so construction does not
  // open the persistent event-stream connection (that path is covered elsewhere).
  compositors::niri::NiriRuntime runtime;
  NiriWorkspaceBackend backend(runtime);
  int changes = 0;
  backend.setChangeCallback([&] { ++changes; });
  backend.handleEvent(
      "WorkspacesChanged",
      {{"workspaces",
        {
            {{"id", 1}, {"idx", 1}, {"name", "development"}, {"output", "DP-1"}},
            {{"id", 2}, {"idx", 2}, {"output", "DP-1"}},
        }}}
  );
  TEST_CHECK(backend.workspaceKeys("DP-1") == std::vector<std::string>({"development", "2"}));
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
  TEST_CHECK(backend.focusedWindowId() == std::optional<std::string>("41"));

  backend.handleEvent("WindowFocusChanged", {{"id", 42}});
  TEST_CHECK(backend.focusedWindowId() == std::optional<std::string>("42"));
  TEST_CHECK(changes == 2);

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
  TEST_CHECK(changes == 3);
  auto windows = backend.workspaceWindows();
  const auto window41 = std::ranges::find(windows, "41", &WorkspaceWindow::windowId);
  TEST_CHECK(
      window41 != windows.end() && window41->workspaceKey == "development" && window41->x == 2 && window41->y == 1
  );

  std::string request;
  int serverError = 0;
  std::jthread server([&] {
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
    const ssize_t written = ::write(client, response.data(), response.size());
    if (written != static_cast<ssize_t>(response.size())) {
      serverError = 3;
    }
    ::close(client);
  });

  TEST_CHECK(::setenv("NIRI_SOCKET", socketPath.c_str(), 1) == 0);
  runtime.refresh();

  TEST_CHECK(backend.closeWindowById("42"));
  server.join();
  TEST_CHECK(serverError == 0);
  const nlohmann::json expectedRequest{{"Action", {{"CloseWindow", {{"id", 42}}}}}};
  TEST_CHECK(nlohmann::json::parse(request) == expectedRequest);
  TEST_CHECK(!backend.closeWindowById("not-an-id"));

  ::close(listener);
  return 0;
}
