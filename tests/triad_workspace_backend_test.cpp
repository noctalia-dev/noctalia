#include "compositors/triad/triad_runtime.h"
#include "compositors/triad/triad_workspace_backend.h"
#include "tests/test_check.h"

#include <cstddef>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

class TriadWorkspaceBackendTestAccess {
public:
  static bool applyState(TriadWorkspaceBackend& backend, const nlohmann::json& state) {
    return backend.applyTriadState(state);
  }

  static bool applyWindow(TriadWorkspaceBackend& backend, const nlohmann::json& window) {
    return backend.applyWindow(window);
  }

  static bool handleMessage(TriadWorkspaceBackend& backend, const std::string& message) {
    return backend.handleMessage(message);
  }

  static std::size_t windowCount(const TriadWorkspaceBackend& backend) { return backend.m_windows.size(); }
};

namespace {

  nlohmann::json initialState() {
    return {
        {"overview", {{"is_open", false}}},
        {"outputs", {{{"name", "DP-1"}}, {{"name", "DP-2"}}}},
        {"workspaces",
         {
             {{"workspace_idx", 1},
              {"tag_id", 11},
              {"name", "main"},
              {"output", "DP-1"},
              {"is_output_visible", true},
              {"is_active", true},
              {"occupied", true},
              {"focused_window_id", 4294967297ULL}},
             {{"workspace_idx", 2},
              {"tag_id", 12},
              {"output", "DP-2"},
              {"is_output_visible", true},
              {"is_active", false},
              {"occupied", true},
              {"focused_window_id", 42}},
             {{"workspace_idx", 99},
              {"tag_id", 99},
              {"output", "triad-unassigned"},
              {"is_output_visible", false},
              {"is_active", false},
              {"occupied", false}},
         }},
        {"windows",
         {
             {{"id", 4294967297ULL},
              {"workspace_idx", 1},
              {"output", "DP-1"},
              {"app_id", "helium"},
              {"title", "Browser\nTitle"},
              {"position", {{"column_idx", 1}, {"window_idx", 1}}}},
             {{"id", 42},
              {"workspace_idx", 2},
              {"output", "DP-2"},
              {"app_id", "kitty"},
              {"title", "Terminal"},
              {"position", {{"column_idx", 2}, {"window_idx", 1}}}},
         }},
    };
  }

} // namespace

int main() {
  unsetenv("TRIAD_SOCKET");
  const std::string runtimeDir = "/tmp/noctalia-triad-workspace-test-" + std::to_string(getpid());
  setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);
  compositors::triad::TriadRuntime runtime;
  TriadWorkspaceBackend backend(runtime);

  int changes = 0;
  int overviewChanges = 0;
  backend.setChangeCallback([&changes]() { ++changes; });
  backend.setOverviewChangeCallback([&overviewChanges]() { ++overviewChanges; });

  TEST_CHECK(TriadWorkspaceBackendTestAccess::applyState(backend, initialState()));
  TEST_CHECK(backend.hasOverviewState());
  TEST_CHECK(!backend.isOverviewOpen());
  TEST_CHECK(backend.all().size() == 2);
  TEST_CHECK(backend.workspaceKeys() == std::vector<std::string>({"1", "2"}));
  TEST_CHECK(backend.focusedWindowId() == std::optional<std::string>("4294967297"));
  TEST_CHECK(TriadWorkspaceBackendTestAccess::windowCount(backend) == 2);

  const auto apps = backend.appIdsByWorkspace();
  TEST_CHECK(apps.at("1") == std::vector<std::string>({"helium"}));
  TEST_CHECK(apps.at("2") == std::vector<std::string>({"kitty"}));

  const auto dp1Windows = backend.workspaceWindows("DP-1");
  TEST_CHECK(dp1Windows.size() == 1);
  TEST_CHECK(dp1Windows[0].windowId == "4294967297");
  TEST_CHECK(dp1Windows[0].title == "Browser Title");

  TEST_CHECK(
      TriadWorkspaceBackendTestAccess::applyWindow(
          backend,
          {{"id", 42},
           {"workspace_idx", 1},
           {"output", "DP-1"},
           {"app_id", "kitty"},
           {"title", "Moved"},
           {"position", {{"column_idx", 3}, {"window_idx", 2}}}}
      )
  );
  TEST_CHECK(backend.workspaceWindows("DP-1").size() == 2);
  TEST_CHECK(!TriadWorkspaceBackendTestAccess::applyWindow(backend, {{"id", "bad"}}));

  auto replacement = initialState();
  replacement["overview"]["is_open"] = true;
  replacement["windows"].erase(1);
  TEST_CHECK(
      TriadWorkspaceBackendTestAccess::handleMessage(
          backend, nlohmann::json{{"triad", {{"state", replacement}}}}.dump()
      )
  );
  TEST_CHECK(changes == 1);
  TEST_CHECK(overviewChanges == 1);
  TEST_CHECK(backend.isOverviewOpen());
  TEST_CHECK(TriadWorkspaceBackendTestAccess::windowCount(backend) == 1);
  TEST_CHECK(TriadWorkspaceBackendTestAccess::handleMessage(backend, "not json"));
  TEST_CHECK(changes == 1);

  return 0;
}
