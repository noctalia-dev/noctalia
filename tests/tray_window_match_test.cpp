#include "dbus/tray/tray_service.h"
#include "shell/tray/tray_window_match.h"
#include "tests/test_check.h"
#include "util/string_utils.h"
#include "wayland/wayland_toplevels.h"

#include <algorithm>
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
  const tray::RunningAppIds noRunningApps = [] { return std::vector<std::string>{}; };

  {
    const auto item = makeItem("firefox-app");
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{}; };
    TEST_CHECK(!tray::findNewestWindowForTrayItem(item, lookup, noRunningApps).newest.has_value());
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
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, noRunningApps);
    TEST_CHECK(result.newest.has_value());
    TEST_CHECK(result.newest->identifier == newer.identifier);
  }

  {
    const auto item = makeItem("RegularApp");
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{ToplevelInfo{}}; };
    TEST_CHECK(!tray::findNewestWindowForTrayItem(item, lookup, noRunningApps).newest.has_value());
  }

  // Transient item id short-circuits before any candidate exists: neither lookup nor
  // runningAppIds is ever invoked.
  {
    const auto item = makeItem(":1.234/org/status/electron");
    TEST_CHECK(tray::isTransientUniqueIdentifier(item.id));
    TEST_CHECK(tray::pinMatchCandidates(item).empty());
    bool lookupCalled = false;
    bool runningAppIdsCalled = false;
    const tray::WindowLookup lookup = [&](const std::string&) {
      lookupCalled = true;
      return std::vector<ToplevelInfo>{};
    };
    const tray::RunningAppIds runningAppIds = [&] {
      runningAppIdsCalled = true;
      return std::vector<std::string>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, runningAppIds);
    TEST_CHECK(!lookupCalled);
    TEST_CHECK(!runningAppIdsCalled);
    TEST_CHECK(!result.newest.has_value());
  }

  // Reverse-DNS tail match: SNI id "keepassxc" matches a running app id whose reverse-DNS
  // tail is "keepassxc", but only after the exact pass fails.
  {
    const auto item = makeItem("keepassxc");
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto window = makeWindow("keepassxc-window", 1, handle);
    const tray::RunningAppIds runningAppIds = [] { return std::vector<std::string>{"org.keepassxc.KeePassXC"}; };
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "org.keepassxc.keepassxc") {
        return std::vector<ToplevelInfo>{window};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, runningAppIds);
    TEST_CHECK(result.newest.has_value());
    TEST_CHECK(result.newest->identifier == window.identifier);
  }

  // Ambiguity guard: two running apps share the "slack" tail, so the tail pass must never
  // resolve either one and lookup must never be called with either resolved app id.
  {
    const auto item = makeItem("slack");
    const tray::RunningAppIds runningAppIds = [] { return std::vector<std::string>{"org.a.Slack", "com.b.Slack"}; };
    std::vector<std::string> lookupCalls;
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      lookupCalls.push_back(candidate);
      if (candidate != "slack" && candidate.contains("slack")) {
        return std::vector<ToplevelInfo>{makeWindow("ambiguous-window")};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, runningAppIds);
    TEST_CHECK(!result.newest.has_value());
    TEST_CHECK(!std::ranges::contains(lookupCalls, std::string("org.a.slack")));
    TEST_CHECK(!std::ranges::contains(lookupCalls, std::string("com.b.slack")));
  }

  // Exact match must always win over a tail match, even when a tail match would also succeed.
  {
    const auto item = makeItem("slack");
    const auto exactWindow =
        makeWindow("slack-exact-window", 1, reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1));
    const auto tailWindow = makeWindow("slack-tail-window", 1, reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x2));
    const tray::RunningAppIds runningAppIds = [] { return std::vector<std::string>{"org.x.Slack"}; };
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "slack") {
        return std::vector<ToplevelInfo>{exactWindow};
      }
      if (candidate == "org.x.slack") {
        return std::vector<ToplevelInfo>{tailWindow};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, runningAppIds);
    TEST_CHECK(result.newest.has_value());
    TEST_CHECK(result.newest->identifier == exactWindow.identifier);
  }

  // Short-tail guard: "ab" is under the 3-char minimum, so the tail pass must never call
  // lookup with the running app id sharing that short tail.
  {
    const auto item = makeItem("ab");
    const tray::RunningAppIds runningAppIds = [] { return std::vector<std::string>{"org.x.ab"}; };
    std::vector<std::string> lookupCalls;
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      lookupCalls.push_back(candidate);
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, runningAppIds);
    TEST_CHECK(!result.newest.has_value());
    TEST_CHECK(!std::ranges::contains(lookupCalls, std::string("org.x.ab")));
  }

  // findNewestWindowForTrayItem: MatchedWindows.windows carries the full candidate vector
  // returned by the winning lookup call, not just the chosen window.
  {
    const auto item = makeItem("multi-window-app");
    const auto winner = makeWindow("multi-window-app-newest", 2);
    const auto sibling = makeWindow("multi-window-app-older", 1);
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "multi-window-app") {
        return std::vector<ToplevelInfo>{winner, sibling};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, noRunningApps);
    TEST_CHECK(result.newest.has_value());
    TEST_CHECK(result.newest->identifier == winner.identifier);
    TEST_CHECK(result.windows.size() == 2);
    TEST_CHECK(result.windows[0].identifier == winner.identifier);
    TEST_CHECK(result.windows[1].identifier == sibling.identifier);
  }

  {
    auto* sharedHandle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto window = makeWindow("window-a", 0, sharedHandle);
    ActiveToplevel active;
    active.handle = sharedHandle;
    active.identifier = "window-b";
    const std::vector<ToplevelInfo> windows{window};
    TEST_CHECK(tray::trayWindowIsFocused(window, windows, active, ""));
  }

  {
    const auto window = makeWindow("shared-identifier");
    ActiveToplevel active;
    active.identifier = "shared-identifier";
    const std::vector<ToplevelInfo> windows{window};
    TEST_CHECK(tray::trayWindowIsFocused(window, windows, active, ""));
  }

  {
    const auto window = makeWindow("window-a");
    const std::vector<ToplevelInfo> windows{window};
    TEST_CHECK(!tray::trayWindowIsFocused(window, windows, std::nullopt, ""));
  }

  {
    const auto window = makeWindow("");
    ActiveToplevel active;
    const std::vector<ToplevelInfo> windows{window};
    TEST_CHECK(!tray::trayWindowIsFocused(window, windows, active, ""));
  }

  // trayWindowIsFocused: false positive guard - two windows sharing the active identifier
  // must never resolve to focused.
  {
    const auto windowOne = makeWindow("shared-identifier");
    const auto windowTwo = makeWindow("shared-identifier");
    ActiveToplevel active;
    active.identifier = "shared-identifier";
    const std::vector<ToplevelInfo> windows{windowOne, windowTwo};
    TEST_CHECK(!tray::trayWindowIsFocused(windowOne, windows, active, ""));
  }

  // decideTrayClick: no matched window -> ActivateItem, no window.
  {
    const auto item = makeItem("firefox-app");
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{}; };
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
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
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
    TEST_CHECK(decision.action == tray::TrayClickAction::FocusWindow);
    TEST_CHECK(decision.window.has_value());
    TEST_CHECK(decision.window->identifier == newer.identifier);
  }

  // decideTrayClick: matched window is the active one by handle -> falls through to
  // ActivateItem, preserving each app's Activate-driven hide toggle.
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
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, active, "", true);
    TEST_CHECK(decision.action == tray::TrayClickAction::ActivateItem);
    TEST_CHECK(!decision.window.has_value());
  }

  // decideTrayClick: matched window is the active one by identifier -> falls through to
  // ActivateItem, preserving each app's Activate-driven hide toggle.
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
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, active, "", true);
    TEST_CHECK(decision.action == tray::TrayClickAction::ActivateItem);
    TEST_CHECK(!decision.window.has_value());
  }

  // decideTrayClick: itemIsMenu with a real DBusMenu -> OpenMenu, no window, lookup never called.
  {
    auto item = makeItem("menu-only-app");
    item.itemIsMenu = true;
    item.menuObjectPath = "/MenuBar";
    bool lookupCalled = false;
    const tray::WindowLookup lookup = [&](const std::string&) {
      lookupCalled = true;
      return std::vector<ToplevelInfo>{};
    };
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
    TEST_CHECK(decision.action == tray::TrayClickAction::OpenMenu);
    TEST_CHECK(!decision.window.has_value());
    TEST_CHECK(!lookupCalled);
  }

  // decideTrayClick: itemIsMenu with a real DBusMenu wins even when a window would otherwise match.
  {
    auto item = makeItem("menu-only-with-window");
    item.itemIsMenu = true;
    item.menuObjectPath = "/MenuBar";
    const auto window = makeWindow("menu-only-window", 1);
    bool lookupCalled = false;
    const tray::WindowLookup lookup = [&](const std::string&) {
      lookupCalled = true;
      return std::vector<ToplevelInfo>{window};
    };
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
    TEST_CHECK(decision.action == tray::TrayClickAction::OpenMenu);
    TEST_CHECK(!decision.window.has_value());
    TEST_CHECK(!lookupCalled);
  }

  // decideTrayClick: itemIsMenu true but menuObjectPath empty -> NOT OpenMenu, falls through
  // to the window path.
  {
    auto item = makeItem("menu-flag-no-menu-path");
    item.itemIsMenu = true;
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{}; };
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
    TEST_CHECK(decision.action != tray::TrayClickAction::OpenMenu);
  }

  // decideTrayClick: itemIsMenu true but menuObjectPath is the sentinel "/NO_DBUSMENU" ->
  // NOT OpenMenu.
  {
    auto item = makeItem("menu-flag-sentinel-menu-path");
    item.itemIsMenu = true;
    item.menuObjectPath = "/NO_DBUSMENU";
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{}; };
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
    TEST_CHECK(decision.action != tray::TrayClickAction::OpenMenu);
  }

  // decideTrayClick: itemIsMenu = false, focusExistingWindow = false, matching window present
  // -> ActivateItem (focus feature disabled short-circuits before any window lookup outcome).
  {
    const auto item = makeItem("regular-app-no-focus-feature");
    const auto window = makeWindow("regular-app-window", 1);
    const tray::WindowLookup lookup = [&](const std::string&) { return std::vector<ToplevelInfo>{window}; };
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", false);
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
    const auto result = tray::findNewestWindowForTrayItem(item, lookup, noRunningApps);
    TEST_CHECK(result.newest.has_value());
    TEST_CHECK(result.newest->identifier == processWindow.identifier);
  }

  // decideTrayClick end-to-end: no exact match, tail-resolved window present -> FocusWindow
  // with the tail-matched window.
  {
    const auto item = makeItem("keepassxc");
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto window = makeWindow("keepassxc-window", 1, handle);
    const tray::RunningAppIds runningAppIds = [] { return std::vector<std::string>{"org.keepassxc.KeePassXC"}; };
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "org.keepassxc.keepassxc") {
        return std::vector<ToplevelInfo>{window};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto decision = tray::decideTrayClick(item, lookup, runningAppIds, std::nullopt, "", true);
    TEST_CHECK(decision.action == tray::TrayClickAction::FocusWindow);
    TEST_CHECK(decision.window.has_value());
    TEST_CHECK(decision.window->identifier == window.identifier);
  }

  // trayItemPrefersMenu: ItemIsMenu with a real DBusMenu -> menu.
  {
    auto item = makeItem("menu-only");
    item.itemIsMenu = true;
    item.menuObjectPath = "/MenuBar";
    TEST_CHECK(trayItemPrefersMenu(item));
  }

  // trayItemPrefersMenu: ItemIsMenu without a menu -> not menu.
  {
    auto item = makeItem("menu-flag-no-menu");
    item.itemIsMenu = true;
    TEST_CHECK(!trayItemPrefersMenu(item));
  }

  // trayItemPrefersMenu: regular item with a menu but no ItemIsMenu -> not menu; the menu
  // stays on right click only.
  {
    auto item = makeItem("regular");
    item.menuObjectPath = "/MenuBar";
    TEST_CHECK(!trayItemPrefersMenu(item));
  }

  // decideTrayClick: regular item with a menu and an unfocused matching window -> the menu
  // must never open on left click; the window is focused.
  {
    auto item = makeItem("windowed-app-with-menu");
    item.menuObjectPath = "/MenuBar";
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto window = makeWindow("windowed-app-window", 1, handle);
    const tray::WindowLookup lookup = [&](const std::string& candidate) {
      if (candidate == "windowed-app-with-menu") {
        return std::vector<ToplevelInfo>{window};
      }
      return std::vector<ToplevelInfo>{};
    };
    const auto decision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
    TEST_CHECK(decision.action == tray::TrayClickAction::FocusWindow);
    TEST_CHECK(decision.window.has_value());
  }

  // decideTrayClick: regular item with a menu but no matching window -> ActivateItem, never
  // the menu.
  {
    auto item = makeItem("menu-but-no-window");
    item.menuObjectPath = "/MenuBar";
    const tray::WindowLookup lookup = [](const std::string&) { return std::vector<ToplevelInfo>{}; };
    const auto onDecision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", true);
    TEST_CHECK(onDecision.action == tray::TrayClickAction::ActivateItem);
    const auto offDecision = tray::decideTrayClick(item, lookup, noRunningApps, std::nullopt, "", false);
    TEST_CHECK(offDecision.action == tray::TrayClickAction::ActivateItem);
  }
}
