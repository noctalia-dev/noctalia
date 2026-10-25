#include "compositors/umbriel/umbriel_runtime.h"
#include "compositors/umbriel/umbriel_workspace_backend.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace {

  struct Fixture {
    int workspaceChanges = 0;
    int windowChanges = 0;
    compositors::umbriel::UmbrielRuntime runtime;
    UmbrielWorkspaceBackend backend{runtime};

    Fixture() {
      backend.setChangeCallback([this]() { ++workspaceChanges; });
      backend.setWindowChangeCallback([this]() { ++windowChanges; });
    }
  };

  nlohmann::json windowSnapshot(std::string_view id, bool minimized, bool minimizable) {
    return {
        {"id", id},
        {"workspace", "DP-1:1"},
        {"app_id", "sample.app"},
        {"title", "Shared title"},
        {"active", false},
        {"x", 10},
        {"y", 20},
        {"minimized", minimized},
        {"minimizable", minimizable},
    };
  }

  const WorkspaceWindow& windowForId(const std::vector<WorkspaceWindow>& windows, std::string_view id) {
    const auto it = std::ranges::find(windows, id, &WorkspaceWindow::windowId);
    TEST_CHECK(it != windows.end());
    return *it;
  }

  void checkState(const UmbrielWorkspaceBackend& backend, std::string_view id, bool minimized, bool minimizable) {
    const auto windows = backend.workspaceWindows();
    const auto& window = windowForId(windows, id);
    TEST_CHECK(window.minimizeState.has_value());
    TEST_CHECK(window.minimizeState->minimized == minimized);
    TEST_CHECK(window.minimizeState->minimizable == minimizable);
  }

  void checkUnknownState(const UmbrielWorkspaceBackend& backend, std::string_view id) {
    const auto windows = backend.workspaceWindows();
    TEST_CHECK(!windowForId(windows, id).minimizeState.has_value());
  }

  void checkSnapshotChanges() {
    Fixture fixture;
    auto snapshot = nlohmann::json::array({windowSnapshot("first", false, true), windowSnapshot("second", true, true)});
    snapshot[0]["active"] = true;
    fixture.backend.handleEvent("windows", snapshot);

    TEST_CHECK(fixture.windowChanges == 1);
    TEST_CHECK(fixture.backend.focusedWindowId() == "first");
    const auto windows = fixture.backend.workspaceWindows("DP-1");
    TEST_CHECK(windows.size() == 2);
    TEST_CHECK(windowForId(windows, "first").title == windowForId(windows, "second").title);
    checkState(fixture.backend, "first", false, true);
    checkState(fixture.backend, "second", true, true);

    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 1);

    // Eligibility and minimise state can change without any presentation metadata changing.
    fixture.backend.setChangeCallback({});
    snapshot[0]["minimizable"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 2);
    checkState(fixture.backend, "first", false, false);
    checkState(fixture.backend, "second", true, true);

    snapshot[1]["minimized"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 3);
    checkState(fixture.backend, "first", false, false);
    checkState(fixture.backend, "second", false, true);

    fixture.backend.setChangeCallback([&fixture]() { ++fixture.workspaceChanges; });
    const int workspaceChangesBeforeReset = fixture.workspaceChanges;
    fixture.backend.handleStreamReset();
    TEST_CHECK(fixture.backend.workspaceWindows().empty());
    TEST_CHECK(!fixture.backend.focusedWindowId().has_value());
    TEST_CHECK(fixture.windowChanges == 4);
    TEST_CHECK(fixture.workspaceChanges == workspaceChangesBeforeReset + 1);

    snapshot[0]["minimized"] = true;
    snapshot[0]["minimizable"] = true;
    snapshot[0]["active"] = false;
    fixture.backend.handleEvent("windows", snapshot);
    TEST_CHECK(fixture.windowChanges == 5);
    checkState(fixture.backend, "first", true, true);

    fixture.backend.handleEvent("windows", nlohmann::json::array());
    TEST_CHECK(fixture.backend.workspaceWindows().empty());
    TEST_CHECK(fixture.windowChanges == 6);
    fixture.backend.handleEvent("windows", nlohmann::json::array());
    TEST_CHECK(fixture.windowChanges == 6);
  }

  void checkMalformedStates() {
    Fixture fixture;
    const auto valid = nlohmann::json::array({windowSnapshot("first", true, true)});
    const std::array<nlohmann::json, 5> invalidValues = {
        nullptr, "true", 1, nlohmann::json::object(), nlohmann::json::array()
    };
    for (const auto* key : {"minimized", "minimizable"}) {
      fixture.backend.handleEvent("windows", valid);
      auto snapshot = valid;
      snapshot[0].erase(key);
      fixture.backend.handleEvent("windows", snapshot);
      checkUnknownState(fixture.backend, "first");

      for (const auto& invalid : invalidValues) {
        fixture.backend.handleEvent("windows", valid);
        checkState(fixture.backend, "first", true, true);
        const int changesBeforeInvalid = fixture.windowChanges;
        snapshot = valid;
        snapshot[0][key] = invalid;
        fixture.backend.handleEvent("windows", snapshot);
        checkUnknownState(fixture.backend, "first");
        TEST_CHECK(fixture.windowChanges == changesBeforeInvalid + 1);
      }
    }

    // A different spelling cannot supply a missing canonical field.
    auto snapshot = valid;
    snapshot[0].erase("minimized");
    snapshot[0]["minimised"] = true;
    fixture.backend.handleEvent("windows", snapshot);
    checkUnknownState(fixture.backend, "first");

    fixture.backend.handleEvent("windows", valid);
    const int changesBeforeInvalidSnapshot = fixture.windowChanges;
    fixture.backend.handleEvent("windows", nlohmann::json::object());
    checkUnknownState(fixture.backend, "first");
    TEST_CHECK(fixture.windowChanges == changesBeforeInvalidSnapshot + 1);
  }

} // namespace

int main() {
  checkSnapshotChanges();
  checkMalformedStates();
  return 0;
}
