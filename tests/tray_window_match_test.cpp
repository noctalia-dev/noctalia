#include "dbus/tray/tray_service.h"
#include "shell/tray/tray_window_match.h"
#include "tests/test_check.h"
#include "util/string_utils.h"
#include "wayland/wayland_toplevels.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

  TrayItemInfo makeItem(std::string id) {
    TrayItemInfo item;
    item.id = std::move(id);
    return item;
  }

  ToplevelInfo
  makeWindow(std::string identifier, std::uint64_t order = 0, zwlr_foreign_toplevel_handle_v1* handle = nullptr) {
    ToplevelInfo window;
    window.identifier = std::move(identifier);
    window.order = order;
    window.handle = handle;
    return window;
  }

} // namespace

int main() {
  {
    const auto item = makeItem("firefox-app");
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{}; };
    TEST_CHECK(!tray::findNewestWindowForTrayItem(item, lookup).has_value());
  }

  {
    const auto item = makeItem("Slack_status_icon_1");
    const auto older = makeWindow("slack-window-a", 1);
    const auto newer = makeWindow("slack-window-b", 2);
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (StringUtils::toLower(candidate).contains("slack")) {
        return std::vector<ToplevelInfo>{newer, older};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup);
    TEST_CHECK(result.has_value());
    TEST_CHECK(result->identifier == newer.identifier);
  }

  {
    const auto item = makeItem("RegularApp");
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{ToplevelInfo{}}; };
    TEST_CHECK(!tray::findNewestWindowForTrayItem(item, lookup).has_value());
  }

  {
    const auto item = makeItem(":1.234/org/status/electron");
    TEST_CHECK(tray::isTransientUniqueIdentifier(item.id));
    TEST_CHECK(tray::pinMatchCandidates(item).empty());
    bool lookupCalled = false;
    const tray::WindowLookup lookup = [&](const std::string&) {
      lookupCalled = true;
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup);
    TEST_CHECK(!lookupCalled);
    TEST_CHECK(!result.has_value());
  }

  {
    auto* sharedHandle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto window = makeWindow("window-a", 0, sharedHandle);
    ActiveToplevel active;
    active.handle = sharedHandle;
    active.identifier = "window-b";
    TEST_CHECK(tray::trayWindowIsFocused(window, active));
  }

  {
    const auto window = makeWindow("shared-identifier");
    ActiveToplevel active;
    active.identifier = "shared-identifier";
    TEST_CHECK(tray::trayWindowIsFocused(window, active));
  }

  {
    const auto window = makeWindow("window-a");
    TEST_CHECK(!tray::trayWindowIsFocused(window, std::nullopt));
  }

  {
    const auto window = makeWindow("");
    ActiveToplevel active;
    TEST_CHECK(!tray::trayWindowIsFocused(window, active));
  }

  // decideTrayClick: no matched window -> ActivateItem, no window.
  {
    const auto item = makeItem("firefox-app");
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{}; };
    const auto decision = tray::decideTrayClick(item, lookup, std::nullopt, true);
    TEST_CHECK(decision.action == tray::TrayClickAction::ActivateItem);
    TEST_CHECK(!decision.window.has_value());
  }

  // decideTrayClick: matched window, no active window -> FocusWindow with the newest match.
  {
    const auto item = makeItem("Slack_status_icon_1");
    const auto older = makeWindow("slack-window-a", 1);
    const auto newer = makeWindow("slack-window-b", 2);
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (StringUtils::toLower(candidate).contains("slack")) {
        return std::vector<ToplevelInfo>{newer, older};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto decision = tray::decideTrayClick(item, lookup, std::nullopt, true);
    TEST_CHECK(decision.action == tray::TrayClickAction::FocusWindow);
    TEST_CHECK(decision.window.has_value());
    TEST_CHECK(decision.window->identifier == newer.identifier);
  }

  // decideTrayClick: matched window is the active one by handle -> Ignore, no window.
  {
    auto* sharedHandle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x2);
    const auto item = makeItem("someapp");
    const auto window = makeWindow("window-x", 1, sharedHandle);
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "someapp") {
        return std::vector<ToplevelInfo>{window};
      }
      return std::vector<ToplevelInfo>{};
    };
    ActiveToplevel active;
    active.handle = sharedHandle;
    const auto decision = tray::decideTrayClick(item, lookup, active, true);
    TEST_CHECK(decision.action == tray::TrayClickAction::Ignore);
    TEST_CHECK(!decision.window.has_value());
  }

  // decideTrayClick: matched window is the active one by identifier -> Ignore, no window.
  {
    const auto item = makeItem("otherapp");
    const auto window = makeWindow("shared-id", 1);
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "otherapp") {
        return std::vector<ToplevelInfo>{window};
      }
      return std::vector<ToplevelInfo>{};
    };
    ActiveToplevel active;
    active.identifier = "shared-id";
    const auto decision = tray::decideTrayClick(item, lookup, active, true);
    TEST_CHECK(decision.action == tray::TrayClickAction::Ignore);
    TEST_CHECK(!decision.window.has_value());
  }

  // decideTrayClick: itemIsMenu -> OpenMenu, no window, lookup never called.
  {
    auto item = makeItem("menu-only-app");
    item.itemIsMenu = true;
    bool lookupCalled = false;
    const tray::WindowLookup lookup = [&](const std::string&) {
      lookupCalled = true;
      return std::vector<ToplevelInfo>{};
    };
    const auto decision = tray::decideTrayClick(item, lookup, std::nullopt, true);
    TEST_CHECK(decision.action == tray::TrayClickAction::OpenMenu);
    TEST_CHECK(!decision.window.has_value());
    TEST_CHECK(!lookupCalled);
  }

  // decideTrayClick: itemIsMenu wins even when a window would otherwise match.
  {
    auto item = makeItem("menu-only-with-window");
    item.itemIsMenu = true;
    const auto window = makeWindow("menu-only-window", 1);
    bool lookupCalled = false;
    const tray::WindowLookup lookup = [&](const std::string&) {
      lookupCalled = true;
      return std::vector<ToplevelInfo>{window};
    };
    const auto decision = tray::decideTrayClick(item, lookup, std::nullopt, true);
    TEST_CHECK(decision.action == tray::TrayClickAction::OpenMenu);
    TEST_CHECK(!decision.window.has_value());
    TEST_CHECK(!lookupCalled);
  }

  // decideTrayClick: itemIsMenu = false, focusExistingWindow = false, matching window present
  // -> ActivateItem (focus feature disabled short-circuits before any window lookup outcome).
  {
    const auto item = makeItem("regular-app-no-focus-feature");
    const auto window = makeWindow("regular-app-window", 1);
    const tray::WindowLookup lookup = [&](const std::string&) { return std::vector<ToplevelInfo>{window}; };
    const auto decision = tray::decideTrayClick(item, lookup, std::nullopt, false);
    TEST_CHECK(decision.action == tray::TrayClickAction::ActivateItem);
    TEST_CHECK(!decision.window.has_value());
  }

  // windowMatchCandidates ordering: processName is tried before title, so a processName match
  // wins even when a different window would match on title.
  {
    TrayItemInfo item;
    item.processName = "myproc";
    item.title = "mytitle";
    const auto processWindow = makeWindow("process-window", 1);
    const auto titleWindow = makeWindow("title-window", 2);
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "myproc") {
        return std::vector<ToplevelInfo>{processWindow};
      }
      if (candidate == "mytitle") {
        return std::vector<ToplevelInfo>{titleWindow};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup);
    TEST_CHECK(result.has_value());
    TEST_CHECK(result->identifier == processWindow.identifier);
  }
}
