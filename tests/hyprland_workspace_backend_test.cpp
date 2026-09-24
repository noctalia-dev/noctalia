#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/hyprland_workspace_backend.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  enum class SnapshotSchema {
    Legacy,
    TypedIdentity,
    NormalIdentity,
    Mixed,
    Malformed,
  };

  // Named workspaces (`workspace = name:0, ...`) get negative ids, in an order unrelated to
  // their names. The special workspace must stay out of the listing entirely.
  constexpr std::string_view kLegacyWorkspacesJson = R"([
    {"id": -1338, "name": "Grave", "monitor": "WAYLAND-1"},
    {"id": -1337, "name": "0", "monitor": "WAYLAND-1"},
    {"id": 8, "name": "8", "monitor": "WAYLAND-1"},
    {"id": -99, "name": "special:special", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kLegacyMonitorsJson = R"([
    {"name": "WAYLAND-1", "activeWorkspace": {"id": 8, "name": "8"}}
  ])";

  constexpr std::string_view kLegacyClientsJson = R"([
    {
      "address": "0x100",
      "workspace": {"id": -1337, "name": "0"},
      "class": "org.example.Named",
      "initialClass": "org.example.Named",
      "title": "Named window",
      "at": [30, 4]
    },
    {
      "address": "0x101",
      "workspace": {"id": 8, "name": "8"},
      "class": "org.example.Numbered",
      "initialClass": "org.example.Numbered",
      "title": "Numbered window",
      "at": [10, 7]
    }
  ])";

  // Named and numbered workspaces may have the same address in the typed-identity schema.
  // The backend must retain the kind in its public key so joins remain unambiguous.
  constexpr std::string_view kTypedWorkspacesJson = R"([
    {"address": "8", "type": "named", "name": "Named Eight", "monitor": "WAYLAND-1"},
    {"address": "10", "type": "numbered", "name": "Ten", "monitor": "WAYLAND-2"},
    {"address": "2", "type": "numbered", "name": "Two", "monitor": "WAYLAND-1"},
    {"address": "8", "type": "numbered", "name": "Eight", "monitor": "WAYLAND-1"},
    {"address": "alpha", "type": "named", "name": "Alpha", "monitor": "WAYLAND-1"},
    {"address": "special:scratch", "type": "special", "name": "Scratch", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kTypedMonitorsJson = R"([
    {"name": "WAYLAND-1", "activeWorkspace": {"address": "8", "type": "named", "name": "Named Eight"}},
    {"name": "WAYLAND-2", "activeWorkspace": {"address": "10", "type": "numbered", "name": "Ten"}}
  ])";

  constexpr std::string_view kTypedClientsJson = R"([
    {
      "address": "0x100",
      "workspace": {"address": "8", "type": "named", "name": "Named Eight"},
      "class": "org.example.Named",
      "initialClass": "org.example.Named",
      "title": "Named window",
      "at": [30, 4]
    },
    {
      "address": "0x101",
      "workspace": {"address": "8", "type": "numbered", "name": "Eight"},
      "class": "org.example.Numbered",
      "initialClass": "org.example.Numbered",
      "title": "Numbered window",
      "at": [10, 7]
    },
    {
      "address": "0x102",
      "workspace": {"address": "10", "type": "numbered", "name": "Ten"},
      "class": "org.example.Remote",
      "initialClass": "org.example.Remote",
      "title": "Other output",
      "at": [0, 0]
    },
    {
      "address": "0x103",
      "workspace": {"address": "alpha", "type": "named", "name": "Alpha"},
      "class": "",
      "initialClass": "org.example.Alpha",
      "title": "Alpha\nwindow",
      "at": [20, 1],
      "urgent": true
    }
  ])";

  // The normal-identity schema reports all ordinary workspaces as "normal" and
  // distinguishes numbered workspaces with an optional numeric id.
  constexpr std::string_view kNormalWorkspacesJson = R"([
    {"address": "8", "type": "normal", "name": "Named Eight", "monitor": "WAYLAND-1"},
    {"address": "10", "id": 10, "type": "normal", "name": "Ten", "monitor": "WAYLAND-2"},
    {"address": "2", "id": 2, "type": "normal", "name": "Two", "monitor": "WAYLAND-1"},
    {"address": "8", "id": 8, "type": "normal", "name": "Eight", "monitor": "WAYLAND-1"},
    {"address": "alpha", "type": "normal", "name": "Alpha", "monitor": "WAYLAND-1"},
    {"address": "special:scratch", "type": "special", "name": "Scratch", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kNormalMonitorsJson = R"([
    {"name": "WAYLAND-1", "activeWorkspace": {"address": "8", "type": "normal", "name": "Named Eight"}},
    {"name": "WAYLAND-2", "activeWorkspace": {"address": "10", "id": 10, "type": "normal", "name": "Ten"}}
  ])";

  constexpr std::string_view kNormalClientsJson = R"([
    {
      "address": "0x100",
      "workspace": {"address": "8", "type": "normal", "name": "Named Eight"},
      "class": "org.example.Named",
      "initialClass": "org.example.Named",
      "title": "Named window",
      "at": [30, 4]
    },
    {
      "address": "0x101",
      "workspace": {"address": "8", "id": 8, "type": "normal", "name": "Eight"},
      "class": "org.example.Numbered",
      "initialClass": "org.example.Numbered",
      "title": "Numbered window",
      "at": [10, 7]
    },
    {
      "address": "0x102",
      "workspace": {"address": "10", "id": 10, "type": "normal", "name": "Ten"},
      "class": "org.example.Remote",
      "initialClass": "org.example.Remote",
      "title": "Other output",
      "at": [0, 0]
    },
    {
      "address": "0x103",
      "workspace": {"address": "alpha", "type": "normal", "name": "Alpha"},
      "class": "",
      "initialClass": "org.example.Alpha",
      "title": "Alpha\nwindow",
      "at": [20, 1],
      "urgent": true
    }
  ])";

  constexpr std::string_view kMixedWorkspacesJson = R"([
    {"address": "8", "type": "named", "name": "Named Eight", "monitor": "WAYLAND-1"},
    {"id": 2, "name": "Two", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kMalformedWorkspacesJson = R"([
    {"address": "not-a-number", "type": "numbered", "name": "Broken", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kRejectedMonitorsJson = R"([
    {"name": "WAYLAND-1", "activeWorkspace": {"address": "8", "type": "numbered", "name": "Eight"}}
  ])";

  bool check(bool condition, std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  std::string replyFor(std::string_view command, SnapshotSchema schema) {
    if (command.contains("j/workspaces")) {
      switch (schema) {
      case SnapshotSchema::Legacy:
        return std::string(kLegacyWorkspacesJson);
      case SnapshotSchema::TypedIdentity:
        return std::string(kTypedWorkspacesJson);
      case SnapshotSchema::NormalIdentity:
        return std::string(kNormalWorkspacesJson);
      case SnapshotSchema::Mixed:
        return std::string(kMixedWorkspacesJson);
      case SnapshotSchema::Malformed:
        return std::string(kMalformedWorkspacesJson);
      }
    }
    if (command.contains("j/monitors")) {
      if (schema == SnapshotSchema::Legacy) {
        return std::string(kLegacyMonitorsJson);
      }
      if (schema == SnapshotSchema::TypedIdentity) {
        return std::string(kTypedMonitorsJson);
      }
      return std::string(schema == SnapshotSchema::NormalIdentity ? kNormalMonitorsJson : kRejectedMonitorsJson);
    }
    if (command.contains("j/clients")) {
      if (schema == SnapshotSchema::Legacy) {
        return std::string(kLegacyClientsJson);
      }
      if (schema == SnapshotSchema::TypedIdentity) {
        return std::string(kTypedClientsJson);
      }
      return schema == SnapshotSchema::NormalIdentity ? std::string(kNormalClientsJson) : "[]";
    }
    if (command.contains("j/status")) {
      return R"({"configProvider": "lua"})";
    }
    return "[]";
  }

  // Stands in for hyprctl's request socket: read the command until the peer half-closes,
  // answer with JSON, close.
  void serve(int listener, const std::atomic_bool& stop, const std::atomic<SnapshotSchema>& schema) {
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
      const std::string reply = replyFor(request, schema.load());
      (void)::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
      ::close(client);
    }
  }

  int createListener(const std::string& socketPath) {
    const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (!check(listener >= 0, "socket() failed")) {
      return -1;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (!check(socketPath.size() < sizeof(address.sun_path), "socket path too long")) {
      ::close(listener);
      return -1;
    }
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (!check(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "bind() failed")) {
      ::close(listener);
      return -1;
    }
    if (!check(::listen(listener, 8) == 0, "listen() failed")) {
      ::close(listener);
      return -1;
    }
    return listener;
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

  bool
  hasIds(std::string_view what, const std::vector<Workspace>& workspaces, const std::vector<std::string>& expected) {
    std::vector<std::string> actual;
    actual.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
      actual.push_back(workspace.id);
    }
    if (actual == expected) {
      return true;
    }
    std::cerr << "FAIL: " << what << " has unexpected workspace order: " << describe(workspaces) << '\n';
    return false;
  }

  bool orderedByLegacyId(const char* what, const std::vector<Workspace>& workspaces) {
    static const std::vector<std::string> expected{"-1338", "-1337", "8"};
    return hasIds(what, workspaces, expected);
  }

  const Workspace* findWorkspace(const std::vector<Workspace>& workspaces, std::string_view id) {
    for (const auto& workspace : workspaces) {
      if (workspace.id == id) {
        return &workspace;
      }
    }
    return nullptr;
  }

  const WorkspaceWindow* findWindow(const std::vector<WorkspaceWindow>& windows, std::string_view id) {
    for (const auto& window : windows) {
      if (window.windowId == id) {
        return &window;
      }
    }
    return nullptr;
  }

  bool sameWorkspaces(const std::vector<Workspace>& lhs, const std::vector<Workspace>& rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      if (lhs[i].id != rhs[i].id
          || lhs[i].name != rhs[i].name
          || lhs[i].coordinates != rhs[i].coordinates
          || lhs[i].index != rhs[i].index
          || lhs[i].active != rhs[i].active
          || lhs[i].urgent != rhs[i].urgent
          || lhs[i].occupied != rhs[i].occupied) {
        return false;
      }
    }
    return true;
  }

  bool checkLegacySnapshot(HyprlandWorkspaceBackend& backend) {
    bool ok = true;
    const auto all = backend.all();
    ok = orderedByLegacyId("legacy all()", all) && ok;
    ok = orderedByLegacyId("legacy forOutput()", backend.forOutput(reinterpret_cast<wl_output*>(0x1))) && ok;

    const auto* named = findWorkspace(all, "-1337");
    const auto* numbered = findWorkspace(all, "8");
    ok = check(named != nullptr && named->occupied && !named->active, "legacy named workspace flags are incorrect")
        && ok;
    ok = check(
             numbered != nullptr && numbered->occupied && numbered->active,
             "legacy numbered workspace flags are incorrect"
         )
        && ok;

    const auto appIds = backend.appIdsByWorkspace(nullptr);
    ok = check(
             appIds.contains("0") && appIds.at("0") == std::vector<std::string>{"org.example.Named"},
             "legacy named taskbar assignment failed"
         )
        && ok;
    ok = check(
             appIds.contains("8") && appIds.at("8") == std::vector<std::string>{"org.example.Numbered"},
             "legacy numbered taskbar assignment failed"
         )
        && ok;

    const auto windows = backend.workspaceWindows(reinterpret_cast<wl_output*>(0x1));
    const auto* namedWindow = findWindow(windows, "100");
    const auto* numberedWindow = findWindow(windows, "101");
    ok = check(windows.size() == 2, "legacy taskbar window assignments have unexpected size") && ok;
    ok = check(
             namedWindow != nullptr && namedWindow->workspaceKey == "0" && namedWindow->appId == "org.example.Named",
             "legacy named taskbar window assignment failed"
         )
        && ok;
    ok = check(
             numberedWindow != nullptr
                 && numberedWindow->workspaceKey == "8"
                 && numberedWindow->appId == "org.example.Numbered",
             "legacy numbered taskbar window assignment failed"
         )
        && ok;
    return ok;
  }

  bool checkAddressIdentitySnapshot(HyprlandWorkspaceBackend& backend, std::string_view schemaName) {
    bool ok = true;
    const auto message = [schemaName](std::string_view detail) {
      return std::string(schemaName) + ": " + std::string(detail);
    };

    const auto all = backend.all();
    ok = hasIds(message("all()"), all, {"name:alpha", "name:8", "2", "8", "10"}) && ok;
    ok = hasIds(
             message("forOutput()"), backend.forOutput(reinterpret_cast<wl_output*>(0x1)),
             {"name:alpha", "name:8", "2", "8"}
         )
        && ok;

    const auto* namedEight = findWorkspace(all, "name:8");
    const auto* numberedEight = findWorkspace(all, "8");
    const auto* ten = findWorkspace(all, "10");
    const auto* two = findWorkspace(all, "2");
    const auto* alpha = findWorkspace(all, "name:alpha");
    ok = check(
             namedEight != nullptr && namedEight->active && namedEight->occupied, message("named address 8 join failed")
         )
        && ok;
    ok = check(
             numberedEight != nullptr && !numberedEight->active && numberedEight->occupied,
             message("numbered address 8 join collided with named address 8")
         )
        && ok;
    ok = check(ten != nullptr && ten->active && ten->occupied, message("second-monitor active/client join failed"))
        && ok;
    ok =
        check(two != nullptr && !two->active && !two->occupied, message("empty numbered workspace flags are incorrect"))
        && ok;
    ok = check(alpha != nullptr && alpha->occupied && alpha->urgent, message("named client/urgent join failed")) && ok;

    const auto appIds = backend.appIdsByWorkspace(nullptr);
    ok = check(appIds.size() == 4, message("client assignments have unexpected keys")) && ok;
    ok = check(
             appIds.contains("name:8") && appIds.at("name:8") == std::vector<std::string>{"org.example.Named"},
             message("named address 8 app assignment failed")
         )
        && ok;
    ok = check(
             appIds.contains("8") && appIds.at("8") == std::vector<std::string>{"org.example.Numbered"},
             message("numbered address 8 app assignment failed")
         )
        && ok;
    ok = check(
             appIds.contains("10") && appIds.at("10") == std::vector<std::string>{"org.example.Remote"},
             message("numbered workspace app assignment failed")
         )
        && ok;
    ok = check(
             appIds.contains("name:alpha") && appIds.at("name:alpha") == std::vector<std::string>{"org.example.Alpha"},
             message("initialClass fallback assignment failed")
         )
        && ok;

    const auto windows = backend.workspaceWindows(reinterpret_cast<wl_output*>(0x1));
    const auto* namedWindow = findWindow(windows, "100");
    const auto* numberedWindow = findWindow(windows, "101");
    const auto* alphaWindow = findWindow(windows, "103");
    ok = check(windows.size() == 3, message("output-filtered window assignments have unexpected size")) && ok;
    ok = check(
             namedWindow != nullptr
                 && namedWindow->workspaceKey == "name:8"
                 && namedWindow->appId == "org.example.Named",
             message("named address 8 window assignment failed")
         )
        && ok;
    ok = check(
             numberedWindow != nullptr
                 && numberedWindow->workspaceKey == "8"
                 && numberedWindow->appId == "org.example.Numbered",
             message("numbered address 8 window assignment failed")
         )
        && ok;
    ok =
        check(
            alphaWindow != nullptr && alphaWindow->workspaceKey == "name:alpha" && alphaWindow->title == "Alpha window",
            message("named workspace window data failed")
        )
        && ok;

    auto& eventHandler = static_cast<compositors::hyprland::HyprlandEventHandler&>(backend);
    eventHandler.handleEvent("activewindowv2", "0x100");
    ok = check(backend.focusedWindowId() == std::optional<std::string>{"100"}, message("focused window event was lost"))
        && ok;
    return ok;
  }

} // namespace

int main() {
  const std::string runtimeDir = "/tmp/noctalia-hypr-workspace-test-" + std::to_string(::getpid());
  const std::string signature = "test";
  const std::string socketDir = runtimeDir + "/hypr/" + signature;

  std::error_code ec;
  std::filesystem::create_directories(socketDir, ec);
  if (!check(!ec, "cannot create test socket directory")) {
    return 1;
  }

  const int requestListener = createListener(socketDir + "/.socket.sock");
  if (requestListener < 0) {
    return 1;
  }
  const int eventListener = createListener(socketDir + "/.socket2.sock");
  if (eventListener < 0) {
    ::close(requestListener);
    return 1;
  }

  std::atomic_bool stop{false};
  std::atomic schema{SnapshotSchema::Legacy};
  std::thread server([&]() { serve(requestListener, stop, schema); });

  // The runtime resolves its socket paths from the environment on construction.
  ::setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);
  ::setenv("HYPRLAND_INSTANCE_SIGNATURE", signature.c_str(), 1);

  bool ok = true;
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    backend.syncFromCompositor();

    ok = checkLegacySnapshot(backend) && ok;
  }

  schema.store(SnapshotSchema::TypedIdentity);
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    ok = check(backend.connectSocket(), "typed-identity backend must connect to the event socket") && ok;
    ok = checkAddressIdentitySnapshot(backend, "typed identity") && ok;

    const auto lastGood = backend.all();
    const auto lastGoodApps = backend.appIdsByWorkspace(nullptr);
    auto& eventHandler = static_cast<compositors::hyprland::HyprlandEventHandler&>(backend);
    schema.store(SnapshotSchema::Mixed);
    eventHandler.handleEvent("configreloaded", "");
    ok = check(sameWorkspaces(backend.all(), lastGood), "mixed-schema snapshot replaced the last good state") && ok;
    ok = check(
             backend.appIdsByWorkspace(nullptr) == lastGoodApps,
             "mixed-schema snapshot changed the last good client assignments"
         )
        && ok;

    schema.store(SnapshotSchema::Malformed);
    eventHandler.handleEvent("configreloaded", "");
    ok = check(sameWorkspaces(backend.all(), lastGood), "malformed snapshot replaced the last good state") && ok;
    ok = check(
             backend.appIdsByWorkspace(nullptr) == lastGoodApps,
             "malformed snapshot changed the last good client assignments"
         )
        && ok;
  }

  schema.store(SnapshotSchema::NormalIdentity);
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    ok = check(backend.connectSocket(), "normal-identity backend must connect to the event socket") && ok;
    ok = checkAddressIdentitySnapshot(backend, "normal identity") && ok;
  }

  stop.store(true);
  ::shutdown(requestListener, SHUT_RDWR);
  ::close(requestListener);
  server.join();
  ::close(eventListener);
  std::filesystem::remove_all(runtimeDir, ec);

  return ok ? 0 : 1;
}
