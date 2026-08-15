#include "ipc/ipc_service.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
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
      TEST_CHECK(n > 0);
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
    TEST_CHECK(fd >= 0);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = socketPath.string();
    TEST_CHECK(path.size() < sizeof(addr.sun_path));
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    TEST_CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    writeAll(fd, command);
    TEST_CHECK(::shutdown(fd, SHUT_WR) == 0);

    ipc.dispatch();
    std::string response = readAll(fd);
    ::close(fd);
    return response;
  }

} // namespace

int main() {
  const auto runtimeDir = makeTempDir();
  TEST_CHECK(!runtimeDir.empty());
  constexpr const char* kWaylandDisplay = "noctalia-ipc-service-test";
  TEST_CHECK(::setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1) == 0);
  TEST_CHECK(::setenv("WAYLAND_DISPLAY", kWaylandDisplay, 1) == 0);

  IpcService ipc;
  constexpr noctalia::cli::Command nonMsgCommand{"not-msg", {}, {}, {}, {}, {}, {}, false};
  ipc.bind(nonMsgCommand, [](const std::string&) { return "unexpected\n"; });
  TEST_CHECK(!ipc.hasHandler("not-msg"));

  ipc.bind(noctalia::cli::msg::panelToggle, [](const std::string& args) { return "visible:" + args + "\n"; });
  ipc.bind(
      noctalia::cli::msg::status, [](const std::string& args) { return "status:" + args + "\n"; },
      IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
  );

  TEST_CHECK(ipc.execute("panel-toggle ok") == "visible:ok\n");
  TEST_CHECK(ipc.execute("status ok") == "status:ok\n");
  TEST_CHECK(ipc.execute("panel-toggle line1\nline2\nline3") == "visible:line1\nline2\nline3\n");

  // Metadata is sourced from the CLI schema, while hasHandler() agrees with execute() dispatch.
  {
    const auto infos = ipc.handlers();
    TEST_CHECK(infos.size() == 2);
    const auto visible = std::ranges::find(infos, "panel-toggle", &IpcService::HandlerInfo::command);
    TEST_CHECK(visible != infos.end());
    TEST_CHECK(visible->args == "<id> [context]");
    TEST_CHECK(visible->signature() == "panel-toggle <id> [context]");
    TEST_CHECK(visible->actionEditorVisibility == IpcService::ActionEditorVisibility::Shown);
    TEST_CHECK(
        visible->description
        == "Toggle a panel by id, optionally with context (e.g. launcher /emo, control-center audio)"
    );

    const auto status = std::ranges::find(infos, "status", &IpcService::HandlerInfo::command);
    TEST_CHECK(status != infos.end());
    TEST_CHECK(status->args.empty());
    TEST_CHECK(status->actionEditorVisibility == IpcService::ActionEditorVisibility::Hidden);
    TEST_CHECK(status->description == "Print current state as JSON");
    TEST_CHECK(ipc.hasHandler("panel-toggle"));
    TEST_CHECK(ipc.hasHandler("status"));
    TEST_CHECK(!ipc.hasHandler("no-such-command"));
  }

  // Action-editor visibility does not affect execution or help output.
  {
    ipc.bind(
        noctalia::cli::msg::logLevelStatus, [](const std::string&) { return "state\n"; },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    TEST_CHECK(ipc.execute("log-level-status") == "state\n");
    TEST_CHECK(ipc.hasHandler("log-level-status"));

    const auto infos = ipc.handlers();
    const auto query = std::ranges::find(infos, "log-level-status", &IpcService::HandlerInfo::command);
    TEST_CHECK(query != infos.end());
    TEST_CHECK(query->actionEditorVisibility == IpcService::ActionEditorVisibility::Hidden);
    TEST_CHECK(ipc.execute("--help").contains("log-level-status"));

    const auto visible = std::ranges::find(infos, "panel-toggle", &IpcService::HandlerInfo::command);
    TEST_CHECK(visible != infos.end());
    TEST_CHECK(visible->actionEditorVisibility == IpcService::ActionEditorVisibility::Shown);
  }

  // A cycling command runs like any other, but declares that a scroll flick should move one
  // position rather than one per notch.
  {
    ipc.bindCycle(noctalia::cli::msg::workspaceSwitch, [](const std::string&) { return "moved\n"; });
    TEST_CHECK(ipc.execute("workspace-switch next") == "moved\n");
    TEST_CHECK(ipc.handlerCycles("workspace-switch"));
    TEST_CHECK(!ipc.handlerCycles("panel-toggle"));
    TEST_CHECK(!ipc.handlerCycles("no-such-command"));

    const auto infos = ipc.handlers();
    const auto cycle = std::ranges::find(infos, "workspace-switch", &IpcService::HandlerInfo::command);
    TEST_CHECK(cycle != infos.end());
    TEST_CHECK(cycle->cycles);
    TEST_CHECK(cycle->actionEditorVisibility == IpcService::ActionEditorVisibility::Shown);
  }

  // `exec` and `none` are reserved by the bar widget action grammar and must never become
  // IPC commands, or a binding would resolve to two different things.
  TEST_CHECK(!ipc.hasHandler("exec"));
  TEST_CHECK(!ipc.hasHandler("none"));

  // The invocation context is empty unless a scope is active, and scopes nest.
  {
    TEST_CHECK(!ipc.invocationContext().has_value());
    const IpcService::InvocationScope outer(ipc, IpcInvocationContext{.widgetName = "media", .barName = "default"});
    TEST_CHECK(ipc.invocationContext().has_value());
    TEST_CHECK(ipc.invocationContext()->widgetName == "media");
    {
      const IpcService::InvocationScope inner(ipc, IpcInvocationContext{.widgetName = "clock"});
      TEST_CHECK(ipc.invocationContext()->widgetName == "clock");
      const IpcService::InvocationScope cleared(ipc, std::nullopt);
      TEST_CHECK(!ipc.invocationContext().has_value());
    }
    TEST_CHECK(ipc.invocationContext()->widgetName == "media");
    TEST_CHECK(ipc.invocationContext()->barName == "default");
  }
  TEST_CHECK(!ipc.invocationContext().has_value());

  const std::string help = ipc.execute("--help");
  TEST_CHECK(help.contains("panel-toggle <id> [context]"));
  TEST_CHECK(help.contains("Toggle a panel by id"));
  TEST_CHECK(help.contains("status"));
  TEST_CHECK(help.contains("Print current state as JSON"));

  ipc.bind(noctalia::cli::msg::panelToggle, [](const std::string&) { return "replaced\n"; });
  TEST_CHECK(ipc.execute("panel-toggle") == "replaced\n");
  const std::string updatedHelp = ipc.execute("--help");
  TEST_CHECK(updatedHelp.contains("panel-toggle <id> [context]"));
  TEST_CHECK(updatedHelp.contains("Toggle a panel by id"));

  TEST_CHECK(ipc.start());
  const auto socketPath = runtimeDir / ("noctalia-" + std::string(kWaylandDisplay) + ".sock");
  TEST_CHECK(sendRaw(ipc, socketPath, "status line1\nline2\nline3") == "status:line1\nline2\nline3\n");
  return 0;
}
