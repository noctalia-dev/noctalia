#include "ipc/ipc_service.h"

#include <algorithm>
#include <cassert>
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
      "visible-command", [](const std::string& args) { return "visible:" + args + "\n"; }, "<value>", "Visible command"
  );
  ipc.registerHandler(
      "hidden-command", [](const std::string& args) { return "hidden:" + args + "\n"; }, "<value>", "Hidden command",
      IpcService::HandlerOptions{
          .helpVisibility = IpcService::HelpVisibility::Hidden,
          .actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden,
      }
  );

  assert(ipc.execute("visible-command ok") == "visible:ok\n");
  assert(ipc.execute("hidden-command ok") == "hidden:ok\n");
  assert(ipc.execute("visible-command line1\nline2\nline3") == "visible:line1\nline2\nline3\n");

  // Metadata includes every handler, while hasHandler() agrees with execute() dispatch.
  {
    const auto infos = ipc.handlers();
    assert(infos.size() == 2);
    const auto visible = std::ranges::find(infos, "visible-command", &IpcService::HandlerInfo::command);
    assert(visible != infos.end());
    // The registry stores arguments only; the verb is composed back in for display.
    assert(visible->args == "<value>");
    assert(visible->signature() == "visible-command <value>");
    assert(visible->helpVisibility == IpcService::HelpVisibility::Public);
    assert(visible->actionEditorVisibility == IpcService::ActionEditorVisibility::Shown);
    assert(visible->description == "Visible command");

    const auto hidden = std::ranges::find(infos, "hidden-command", &IpcService::HandlerInfo::command);
    assert(hidden != infos.end());
    assert(hidden->helpVisibility == IpcService::HelpVisibility::Hidden);
    assert(hidden->actionEditorVisibility == IpcService::ActionEditorVisibility::Hidden);
    assert(ipc.hasHandler("visible-command"));
    assert(ipc.hasHandler("hidden-command"));
    assert(!ipc.hasHandler("no-such-command"));
  }

  // Action-editor visibility does not affect execution or help output.
  {
    ipc.registerHandler(
        "query-command", [](const std::string&) { return "state\n"; }, "", "Print some state",
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    assert(ipc.execute("query-command") == "state\n");
    assert(ipc.hasHandler("query-command"));

    const auto infos = ipc.handlers();
    const auto query = std::ranges::find(infos, "query-command", &IpcService::HandlerInfo::command);
    assert(query != infos.end());
    assert(query->actionEditorVisibility == IpcService::ActionEditorVisibility::Hidden);
    assert(ipc.execute("--help").contains("query-command"));

    const auto visible = std::ranges::find(infos, "visible-command", &IpcService::HandlerInfo::command);
    assert(visible != infos.end());
    assert(visible->actionEditorVisibility == IpcService::ActionEditorVisibility::Shown);
  }

  // A cycling command runs like any other, but declares that a scroll flick should move one
  // position rather than one per notch.
  {
    ipc.registerCycleHandler("cycle-command", [](const std::string&) { return "moved\n"; }, "<next|prev>", "Step");
    assert(ipc.execute("cycle-command next") == "moved\n");
    assert(ipc.handlerCycles("cycle-command"));
    assert(!ipc.handlerCycles("visible-command"));
    assert(!ipc.handlerCycles("no-such-command"));

    const auto infos = ipc.handlers();
    const auto cycle = std::ranges::find(infos, "cycle-command", &IpcService::HandlerInfo::command);
    assert(cycle != infos.end());
    assert(cycle->cycles);
    // Cycling says nothing about whether an action picker should offer it.
    assert(cycle->actionEditorVisibility == IpcService::ActionEditorVisibility::Shown);
  }

  // `exec` and `none` are reserved by the bar widget action grammar and must never become
  // IPC commands, or a binding would resolve to two different things.
  assert(!ipc.hasHandler("exec"));
  assert(!ipc.hasHandler("none"));

  // The invocation context is empty unless a scope is active, and scopes nest.
  {
    assert(!ipc.invocationContext().has_value());
    const IpcService::InvocationScope outer(ipc, IpcInvocationContext{.widgetName = "media", .barName = "default"});
    assert(ipc.invocationContext().has_value());
    assert(ipc.invocationContext()->widgetName == "media");
    {
      const IpcService::InvocationScope inner(ipc, IpcInvocationContext{.widgetName = "clock"});
      assert(ipc.invocationContext()->widgetName == "clock");
      const IpcService::InvocationScope cleared(ipc, std::nullopt);
      assert(!ipc.invocationContext().has_value());
    }
    assert(ipc.invocationContext()->widgetName == "media");
    assert(ipc.invocationContext()->barName == "default");
  }
  assert(!ipc.invocationContext().has_value());

  const std::string help = ipc.execute("--help");
  assert(help.contains("visible-command <value>"));
  assert(help.contains("Visible command"));
  assert(!help.contains("hidden-command"));
  assert(!help.contains("Hidden command"));

  ipc.registerHandler(
      "visible-command", [](const std::string&) { return "hidden-now\n"; }, "", "Now hidden",
      IpcService::HandlerOptions{.helpVisibility = IpcService::HelpVisibility::Hidden}
  );

  assert(ipc.execute("visible-command") == "hidden-now\n");
  const std::string updatedHelp = ipc.execute("--help");
  assert(!updatedHelp.contains("visible-command"));
  assert(!updatedHelp.contains("Now hidden"));

  assert(ipc.start());
  const auto socketPath = runtimeDir / ("noctalia-" + std::string(kWaylandDisplay) + ".sock");
  assert(sendRaw(ipc, socketPath, "hidden-command line1\nline2\nline3") == "hidden:line1\nline2\nline3\n");

  return 0;
}
