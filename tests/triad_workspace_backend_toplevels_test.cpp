#include "system/internal_app_metadata.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define private public
#include "compositors/triad/triad_workspace_backend.h"
#undef private

#include "compositors/triad/triad_runtime.h"

namespace internal_apps {

  const InternalAppDefinition* appDefinitionForWindowTitle(std::string_view /*windowTitle*/) { return nullptr; }

  std::optional<AppMetadata> metadataForAppId(std::string_view /*appId*/) { return std::nullopt; }

  void applyMetadataToDesktopEntry(DesktopEntry& /*entry*/) {}

} // namespace internal_apps

int main() {
  setenv("TRIAD_SOCKET", "/tmp/noctalia-triad-workspace-backend-test.sock", 1);

  compositors::triad::TriadRuntime runtime;
  TriadWorkspaceBackend backend(runtime);

  backend.m_workspaces.emplace(
      1,
      TriadWorkspaceBackend::WorkspaceState{
          .index = 1,
          .tagId = 1,
          .output = "eDP-1",
          .active = true,
          .globalActive = true,
          .occupied = true,
      }
  );
  backend.m_windows.emplace(
      100,
      TriadWorkspaceBackend::WindowState{
          .id = 100,
          .workspaceIndex = 1,
          .output = "eDP-1",
          .appId = "kitty",
          .title = "~",
          .minimized = true,
      }
  );
  backend.m_windows.emplace(
      200,
      TriadWorkspaceBackend::WindowState{
          .id = 200,
          .workspaceIndex = 1,
          .output = "eDP-1",
          .appId = "kitty",
          .title = "~",
      }
  );
  backend.m_windows.emplace(
      300,
      TriadWorkspaceBackend::WindowState{
          .id = 300,
          .workspaceIndex = 1,
          .output = "eDP-1",
          .appId = "brave-origin-beta",
          .title = "Triad",
      }
  );

  const auto windows = backend.toplevelsForApp("kitty", "kitty", "eDP-1");
  assert(windows.size() == 2);
  assert(windows[0].identifier == "100");
  assert(windows[1].identifier == "200");
  assert(windows[0].title == "~");
  assert(windows[1].title == "~");
  assert(backend.resolveFocusWindowId("kitty:~") == std::nullopt);
  assert(backend.resolveFocusWindowId("100").value() == 100);

  const auto parsed = TriadWorkspaceBackend::parseWindow(
      nlohmann::json{{"id", 400}, {"workspace_idx", 1}, {"app_id", "kitty"}, {"title", "~"}, {"is_minimized", true}}
  );
  assert(parsed.has_value());
  assert(parsed->minimized);

  return 0;
}
