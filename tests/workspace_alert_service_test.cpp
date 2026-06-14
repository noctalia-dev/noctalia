#include "compositors/workspace_alert_service.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  std::vector<Workspace> sampleWorkspaces() {
    return {
        Workspace{.id = "1", .name = "1", .key = "1", .index = 1, .active = false, .urgent = false, .occupied = true},
        Workspace{.id = "2", .name = "2", .key = "2", .index = 2, .active = false, .urgent = false, .occupied = true},
        Workspace{.id = "3", .name = "3", .key = "3", .index = 3, .active = true, .urgent = false, .occupied = true},
        Workspace{.id = "4", .name = "4", .key = "4", .index = 4, .active = false, .urgent = true, .occupied = true},
        Workspace{.id = "5", .name = "5", .key = "", .index = 5, .active = false, .urgent = false, .occupied = false},
    };
  }

} // namespace

int main() {
  bool ok = true;

  WorkspaceAlertService service;
  ok &= check(service.empty(), "new service is empty");
  ok &= check(service.add("2"), "add returns true for new key");
  ok &= check(!service.add("2"), "duplicate add returns false");
  ok &= check(!service.add(""), "empty add is rejected");
  ok &= check(service.contains("2"), "contains added key");

  auto overlaid = sampleWorkspaces();
  service.applyOverlay(overlaid);
  ok &= check(overlaid[1].urgent, "overlay marks non-active alerted workspace urgent");
  ok &= check(!overlaid[2].urgent, "overlay does not mark active workspace urgent");
  ok &= check(overlaid[3].urgent, "overlay preserves compositor urgent state");
  ok &= check(!overlaid[4].urgent, "overlay ignores empty workspace keys");
  ok &= check(service.contains("2"), "overlay is pure read and keeps alert");

  WorkspaceAlertService keyOnlyService;
  ok &= check(keyOnlyService.add("external-id"), "id-like key add succeeds");
  ok &= check(keyOnlyService.add("display-name"), "name-like key add succeeds");
  std::vector<Workspace> keyOnlyRows{
      Workspace{
          .id = "external-id",
          .name = "display-name",
          .key = "canonical-key",
          .index = 6,
          .active = false,
          .urgent = false,
          .occupied = true,
      },
  };
  keyOnlyService.applyOverlay(keyOnlyRows);
  ok &= check(!keyOnlyRows[0].urgent, "overlay ignores id and name when key differs");
  ok &= check(keyOnlyService.add("canonical-key"), "canonical key add succeeds");
  keyOnlyService.applyOverlay(keyOnlyRows);
  ok &= check(keyOnlyRows[0].urgent, "overlay matches canonical workspace key");

  ok &= check(service.add("3"), "active key add succeeds");
  auto activeRows = sampleWorkspaces();
  const std::size_t cleared = service.clearActive(activeRows);
  ok &= check(cleared == 1, "clearActive clears active alert");
  ok &= check(!service.contains("3"), "active key removed");
  ok &= check(service.contains("2"), "inactive key remains");

  std::vector<WorkspaceWindowAssignment> assignments{
      WorkspaceWindowAssignment{.windowId = "10", .workspaceKey = "1", .appId = "kitty", .title = "shell"},
      WorkspaceWindowAssignment{.windowId = "20", .workspaceKey = "2", .appId = "code", .title = "editor"},
      WorkspaceWindowAssignment{.windowId = "30", .workspaceKey = "", .appId = "empty", .title = "empty"},
  };
  ok &= check(WorkspaceAlertService::workspaceKeyForWindow("20", assignments) == "2", "resolves window id");
  ok &= check(!WorkspaceAlertService::workspaceKeyForWindow("30", assignments).has_value(), "rejects empty assignment key");
  ok &= check(!WorkspaceAlertService::workspaceKeyForWindow("99", assignments).has_value(), "rejects unknown window id");

  ok &= check(WorkspaceAlertService::isKnownWorkspaceKey("1", sampleWorkspaces()), "known key validates");
  ok &= check(!WorkspaceAlertService::isKnownWorkspaceKey("9", sampleWorkspaces()), "unknown key rejects");
  ok &= check(!WorkspaceAlertService::isKnownWorkspaceKey("", sampleWorkspaces()), "empty key rejects");
  ok &= check(
      WorkspaceAlertService::isKnownWorkspaceKey(
          "canonical-key",
          {Workspace{
              .id = "external-id",
              .name = "display-name",
              .key = "canonical-key",
              .index = 6,
              .active = false,
              .urgent = false,
              .occupied = true,
          }}
      ),
      "known workspace validation uses canonical key"
  );
  ok &= check(
      !WorkspaceAlertService::isKnownWorkspaceKey(
          "external-id",
          {Workspace{
              .id = "external-id",
              .name = "display-name",
              .key = "canonical-key",
              .index = 6,
              .active = false,
              .urgent = false,
              .occupied = true,
          }}
      ),
      "known workspace validation ignores id when key differs"
  );
  ok &= check(
      !WorkspaceAlertService::isKnownWorkspaceKey(
          "display-name",
          {Workspace{
              .id = "external-id",
              .name = "display-name",
              .key = "canonical-key",
              .index = 6,
              .active = false,
              .urgent = false,
              .occupied = true,
          }}
      ),
      "known workspace validation ignores name when key differs"
  );

  ok &= check(service.add("1"), "sort key add succeeds");
  const auto keys = service.keys();
  ok &= check(keys.size() == 2 && keys[0] == "1" && keys[1] == "2", "keys are sorted");
  ok &= check(service.clear("1"), "clear returns true for present key");
  ok &= check(!service.contains("1"), "clear removes key");
  service.clearAll();
  ok &= check(service.empty(), "clearAll empties service");

  return ok ? 0 : 1;
}
