#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/hyprland_workspace_backend.h"
#include "tests/test_check.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
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
    LegacySpecialOpen,
    TypedIdentity,
    TypedSpecialOverlay,
    TypedSpecialOverlayClosed,
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

  // Hyprland keeps the regular workspace active while a special one covers it.
  constexpr std::string_view kLegacySpecialOpenWorkspacesJson = R"([
    {"id": 8, "name": "8", "monitor": "WAYLAND-1"},
    {"id": -99, "name": "special:special", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kLegacySpecialOpenMonitorsJson = R"([
    {
      "name": "WAYLAND-1",
      "activeWorkspace": {"id": 8, "name": "8"},
      "specialWorkspace": {"id": -99, "name": "special:special"}
    }
  ])";

  constexpr std::string_view kLegacySpecialOpenClientsJson = R"([
    {
      "address": "0x101",
      "workspace": {"id": 8, "name": "8"},
      "class": "org.example.Numbered",
      "initialClass": "org.example.Numbered",
      "title": "Regular window",
      "at": [10, 7]
    },
    {
      "address": "0x102",
      "workspace": {"id": -99, "name": "special:special"},
      "class": "org.example.Scratch",
      "initialClass": "org.example.Scratch",
      "title": "Scratch window",
      "at": [0, 0]
    }
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

  // Only one of the two monitors has a special workspace open, so the overlay must be
  // reported per output.
  constexpr std::string_view kTypedSpecialOverlayWorkspacesJson = R"([
    {"address": "8", "type": "numbered", "name": "Eight", "monitor": "WAYLAND-1"},
    {"address": "10", "type": "numbered", "name": "Ten", "monitor": "WAYLAND-2"},
    {"address": "special:magic", "type": "special", "name": "Scratch", "monitor": "WAYLAND-1"}
  ])";

  constexpr std::string_view kTypedSpecialOverlayMonitorsJson = R"([
    {
      "name": "WAYLAND-1",
      "activeWorkspace": {"address": "8", "type": "numbered", "name": "Eight"},
      "specialWorkspace": {"address": "special:magic", "type": "special", "name": "Scratch"}
    },
    {"name": "WAYLAND-2", "activeWorkspace": {"address": "10", "type": "numbered", "name": "Ten"}}
  ])";

  constexpr std::string_view kTypedSpecialOverlayClosedMonitorsJson = R"([
    {"name": "WAYLAND-1", "activeWorkspace": {"address": "8", "type": "numbered", "name": "Eight"}},
    {"name": "WAYLAND-2", "activeWorkspace": {"address": "10", "type": "numbered", "name": "Ten"}}
  ])";

  constexpr std::string_view kTypedSpecialOverlayClientsJson = R"([
    {
      "address": "0x101",
      "workspace": {"address": "8", "type": "numbered", "name": "Eight"},
      "class": "org.example.Eight",
      "initialClass": "org.example.Eight",
      "title": "Eight window",
      "at": [10, 7]
    },
    {
      "address": "0x102",
      "workspace": {"address": "special:magic", "type": "special", "name": "Scratch"},
      "class": "org.example.Scratch",
      "initialClass": "org.example.Scratch",
      "title": "Scratch window",
      "at": [0, 0]
    },
    {
      "address": "0x103",
      "workspace": {"address": "10", "type": "numbered", "name": "Ten"},
      "class": "org.example.Ten",
      "initialClass": "org.example.Ten",
      "title": "Ten window",
      "at": [0, 0]
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

  std::string gRuntimeDir;

  void removeRuntimeDir() {
    std::error_code ec;
    std::filesystem::remove_all(gRuntimeDir, ec);
  }

  std::string replyFor(std::string_view command, SnapshotSchema schema) {
    if (command.contains("j/workspaces")) {
      switch (schema) {
      case SnapshotSchema::Legacy:
        return std::string(kLegacyWorkspacesJson);
      case SnapshotSchema::LegacySpecialOpen:
        return std::string(kLegacySpecialOpenWorkspacesJson);
      case SnapshotSchema::TypedIdentity:
        return std::string(kTypedWorkspacesJson);
      case SnapshotSchema::TypedSpecialOverlay:
        return std::string(kTypedSpecialOverlayWorkspacesJson);
      case SnapshotSchema::TypedSpecialOverlayClosed:
        return std::string(kTypedSpecialOverlayWorkspacesJson);
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
      if (schema == SnapshotSchema::LegacySpecialOpen) {
        return std::string(kLegacySpecialOpenMonitorsJson);
      }
      if (schema == SnapshotSchema::TypedIdentity) {
        return std::string(kTypedMonitorsJson);
      }
      if (schema == SnapshotSchema::TypedSpecialOverlay) {
        return std::string(kTypedSpecialOverlayMonitorsJson);
      }
      if (schema == SnapshotSchema::TypedSpecialOverlayClosed) {
        return std::string(kTypedSpecialOverlayClosedMonitorsJson);
      }
      return std::string(schema == SnapshotSchema::NormalIdentity ? kNormalMonitorsJson : kRejectedMonitorsJson);
    }
    if (command.contains("j/clients")) {
      if (schema == SnapshotSchema::Legacy) {
        return std::string(kLegacyClientsJson);
      }
      if (schema == SnapshotSchema::LegacySpecialOpen) {
        return std::string(kLegacySpecialOpenClientsJson);
      }
      if (schema == SnapshotSchema::TypedIdentity) {
        return std::string(kTypedClientsJson);
      }
      if (schema == SnapshotSchema::TypedSpecialOverlay) {
        return std::string(kTypedSpecialOverlayClientsJson);
      }
      if (schema == SnapshotSchema::TypedSpecialOverlayClosed) {
        return std::string(kTypedSpecialOverlayClientsJson);
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
    TEST_CHECK(listener >= 0);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    TEST_CHECK(socketPath.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    TEST_CHECK(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    TEST_CHECK(::listen(listener, 8) == 0);
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

  void
  checkIds(std::string_view what, const std::vector<Workspace>& workspaces, const std::vector<std::string>& expected) {
    std::vector<std::string> actual;
    actual.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
      actual.push_back(workspace.id);
    }
    if (actual != expected) {
      std::println(stderr, "{} has unexpected workspace order: {}", what, describe(workspaces));
    }
    TEST_CHECK(actual == expected);
  }

  void checkLegacyOrder(const char* what, const std::vector<Workspace>& workspaces) {
    static const std::vector<std::string> expected{"-1338", "-1337", "8"};
    checkIds(what, workspaces, expected);
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

  void checkLegacySnapshot(HyprlandWorkspaceBackend& backend) {
    const auto all = backend.all();
    checkLegacyOrder("legacy all()", all);
    checkLegacyOrder("legacy forOutput()", backend.forOutput(reinterpret_cast<wl_output*>(0x1)));

    const auto* named = findWorkspace(all, "-1337");
    const auto* numbered = findWorkspace(all, "8");
    TEST_CHECK(named != nullptr && named->occupied && !named->active);
    TEST_CHECK(numbered != nullptr && numbered->occupied && numbered->active);

    const auto appIds = backend.appIdsByWorkspace(nullptr);
    TEST_CHECK(appIds.contains("0") && appIds.at("0") == std::vector<std::string>{"org.example.Named"});
    TEST_CHECK(appIds.contains("8") && appIds.at("8") == std::vector<std::string>{"org.example.Numbered"});

    const auto windows = backend.workspaceWindows(reinterpret_cast<wl_output*>(0x1));
    const auto* namedWindow = findWindow(windows, "100");
    const auto* numberedWindow = findWindow(windows, "101");
    TEST_CHECK(windows.size() == 2);
    TEST_CHECK(namedWindow != nullptr && namedWindow->workspaceKey == "0" && namedWindow->appId == "org.example.Named");
    TEST_CHECK(
        numberedWindow != nullptr
        && numberedWindow->workspaceKey == "8"
        && numberedWindow->appId == "org.example.Numbered"
    );
  }

  void checkAddressIdentitySnapshot(HyprlandWorkspaceBackend& backend, std::string_view schemaName) {
    const auto message = [schemaName](std::string_view detail) {
      return std::string(schemaName) + ": " + std::string(detail);
    };

    const auto all = backend.all();
    checkIds(message("all()"), all, {"name:alpha", "name:8", "2", "8", "10"});
    checkIds(
        message("forOutput()"), backend.forOutput(reinterpret_cast<wl_output*>(0x1)), {"name:alpha", "name:8", "2", "8"}
    );

    const auto* namedEight = findWorkspace(all, "name:8");
    const auto* numberedEight = findWorkspace(all, "8");
    const auto* ten = findWorkspace(all, "10");
    const auto* two = findWorkspace(all, "2");
    const auto* alpha = findWorkspace(all, "name:alpha");
    TEST_CHECK(namedEight != nullptr && namedEight->active && namedEight->occupied);
    TEST_CHECK(numberedEight != nullptr && !numberedEight->active && numberedEight->occupied);
    TEST_CHECK(ten != nullptr && ten->active && ten->occupied);
    TEST_CHECK(two != nullptr && !two->active && !two->occupied);
    TEST_CHECK(alpha != nullptr && alpha->occupied && alpha->urgent);

    const auto appIds = backend.appIdsByWorkspace(nullptr);
    TEST_CHECK(appIds.size() == 4);
    TEST_CHECK(appIds.contains("name:8") && appIds.at("name:8") == std::vector<std::string>{"org.example.Named"});
    TEST_CHECK(appIds.contains("8") && appIds.at("8") == std::vector<std::string>{"org.example.Numbered"});
    TEST_CHECK(appIds.contains("10") && appIds.at("10") == std::vector<std::string>{"org.example.Remote"});
    TEST_CHECK(
        appIds.contains("name:alpha") && appIds.at("name:alpha") == std::vector<std::string>{"org.example.Alpha"}
    );

    const auto windows = backend.workspaceWindows(reinterpret_cast<wl_output*>(0x1));
    const auto* namedWindow = findWindow(windows, "100");
    const auto* numberedWindow = findWindow(windows, "101");
    const auto* alphaWindow = findWindow(windows, "103");
    TEST_CHECK(windows.size() == 3);
    TEST_CHECK(
        namedWindow != nullptr && namedWindow->workspaceKey == "name:8" && namedWindow->appId == "org.example.Named"
    );
    TEST_CHECK(
        numberedWindow != nullptr
        && numberedWindow->workspaceKey == "8"
        && numberedWindow->appId == "org.example.Numbered"
    );
    TEST_CHECK(
        alphaWindow != nullptr && alphaWindow->workspaceKey == "name:alpha" && alphaWindow->title == "Alpha window"
    );

    auto& eventHandler = static_cast<compositors::hyprland::HyprlandEventHandler&>(backend);
    eventHandler.handleEvent("activewindowv2", "0x100");
    TEST_CHECK(backend.focusedWindowId() == std::optional<std::string>{"100"});
  }

  void checkLegacySpecialWorkspaceOpen(HyprlandWorkspaceBackend& backend) {
    auto* output = reinterpret_cast<wl_output*>(0x1);

    const auto all = backend.all();
    checkIds("legacy special all()", all, {"8"});
    checkIds("legacy special forOutput()", backend.forOutput(output), {"8"});

    // "Is anything active" is therefore no hint that a special workspace covers it.
    const auto* numbered = findWorkspace(all, "8");
    TEST_CHECK(numbered != nullptr && numbered->active);

    // The overlay key must use the same spelling as the window assignments of its windows.
    TEST_CHECK(backend.openOverlayWorkspaceKeys(output) == std::vector<std::string>{"special:special"});
    TEST_CHECK(backend.openOverlayWorkspaceKeys(nullptr) == std::vector<std::string>{"special:special"});

    const auto windows = backend.workspaceWindows(output);
    const auto* scratch = findWindow(windows, "102");
    TEST_CHECK(
        scratch != nullptr && scratch->workspaceKey == "special:special" && scratch->appId == "org.example.Scratch"
    );
    TEST_CHECK(windows.size() == 2);
  }

  void checkPerOutputSpecialWorkspace(HyprlandWorkspaceBackend& backend) {
    auto* left = reinterpret_cast<wl_output*>(0x1);
    auto* right = reinterpret_cast<wl_output*>(0x2);

    checkIds("overlay all()", backend.all(), {"8", "10"});
    checkIds("overlay forOutput(left)", backend.forOutput(left), {"8"});
    checkIds("overlay forOutput(right)", backend.forOutput(right), {"10"});

    TEST_CHECK(backend.openOverlayWorkspaceKeys(left) == std::vector<std::string>{"special:magic"});
    TEST_CHECK(backend.openOverlayWorkspaceKeys(right).empty());
    TEST_CHECK(backend.openOverlayWorkspaceKeys(nullptr) == std::vector<std::string>{"special:magic"});

    const auto leftWindows = backend.workspaceWindows(left);
    const auto* scratchWindow = findWindow(leftWindows, "102");
    TEST_CHECK(leftWindows.size() == 2);
    TEST_CHECK(scratchWindow != nullptr && scratchWindow->workspaceKey == "special:magic");
    const auto rightWindows = backend.workspaceWindows(right);
    const auto* tenWindow = findWindow(rightWindows, "103");
    TEST_CHECK(rightWindows.size() == 1 && tenWindow != nullptr && tenWindow->workspaceKey == "10");
  }

  // Toggling an already-populated special workspace changes no window, so the switcher only
  // learns about it when activespecialv2 refreshes the monitor snapshot.
  void checkSpecialWorkspaceToggle(HyprlandWorkspaceBackend& backend, std::atomic<SnapshotSchema>& schema) {
    auto* left = reinterpret_cast<wl_output*>(0x1);
    auto& eventHandler = static_cast<compositors::hyprland::HyprlandEventHandler&>(backend);

    TEST_CHECK(backend.openOverlayWorkspaceKeys(left) == std::vector<std::string>{"special:magic"});

    schema.store(SnapshotSchema::TypedSpecialOverlayClosed);
    eventHandler.handleEvent("activespecialv2", "special:magic,special:magic");
    TEST_CHECK(backend.openOverlayWorkspaceKeys(left).empty());

    schema.store(SnapshotSchema::TypedSpecialOverlay);
    eventHandler.handleEvent("activespecial", "special:magic");
    TEST_CHECK(backend.openOverlayWorkspaceKeys(left) == std::vector<std::string>{"special:magic"});
  }

} // namespace

int main() {
  gRuntimeDir = "/tmp/noctalia-hypr-workspace-test-" + std::to_string(::getpid());
  const std::string& runtimeDir = gRuntimeDir;
  const std::string signature = "test";
  const std::string socketDir = runtimeDir + "/hypr/" + signature;

  std::error_code ec;
  std::filesystem::create_directories(socketDir, ec);
  std::atexit(removeRuntimeDir);
  TEST_CHECK(!ec);

  const int requestListener = createListener(socketDir + "/.socket.sock");
  const int eventListener = createListener(socketDir + "/.socket2.sock");

  std::atomic_bool stop{false};
  std::atomic schema{SnapshotSchema::Legacy};
  std::thread server([&] { serve(requestListener, stop, schema); });

  // The runtime resolves its socket paths from the environment on construction.
  ::setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);
  ::setenv("HYPRLAND_INSTANCE_SIGNATURE", signature.c_str(), 1);

  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    backend.syncFromCompositor();

    checkLegacySnapshot(backend);
  }

  schema.store(SnapshotSchema::TypedIdentity);
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    TEST_CHECK(backend.connectSocket());
    checkAddressIdentitySnapshot(backend, "typed identity");

    const auto lastGood = backend.all();
    const auto lastGoodApps = backend.appIdsByWorkspace(nullptr);
    auto& eventHandler = static_cast<compositors::hyprland::HyprlandEventHandler&>(backend);
    schema.store(SnapshotSchema::Mixed);
    eventHandler.handleEvent("configreloaded", "");
    TEST_CHECK(sameWorkspaces(backend.all(), lastGood));
    TEST_CHECK(backend.appIdsByWorkspace(nullptr) == lastGoodApps);

    schema.store(SnapshotSchema::Malformed);
    eventHandler.handleEvent("configreloaded", "");
    TEST_CHECK(sameWorkspaces(backend.all(), lastGood));
    TEST_CHECK(backend.appIdsByWorkspace(nullptr) == lastGoodApps);
  }

  schema.store(SnapshotSchema::NormalIdentity);
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    TEST_CHECK(backend.connectSocket());
    checkAddressIdentitySnapshot(backend, "normal identity");
  }

  schema.store(SnapshotSchema::LegacySpecialOpen);
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend([](wl_output*) { return std::string("WAYLAND-1"); }, runtime);
    backend.syncFromCompositor();

    checkLegacySpecialWorkspaceOpen(backend);
  }

  schema.store(SnapshotSchema::TypedSpecialOverlay);
  {
    compositors::hyprland::HyprlandRuntime runtime;
    HyprlandWorkspaceBackend backend(
        [](wl_output* output) {
          return output == reinterpret_cast<wl_output*>(0x2) ? std::string("WAYLAND-2") : std::string("WAYLAND-1");
        },
        runtime
    );
    backend.syncFromCompositor();

    checkPerOutputSpecialWorkspace(backend);
    checkSpecialWorkspaceToggle(backend, schema);
  }

  stop.store(true);
  ::shutdown(requestListener, SHUT_RDWR);
  ::close(requestListener);
  server.join();
  ::close(eventListener);

  return 0;
}
