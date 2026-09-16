#include "shell/common/window_activation.h"
#include "tests/test_check.h"
#include "wayland/wayland_toplevels.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

  ToplevelInfo makeWindow(
      std::string identifier, std::uint64_t order, zwlr_foreign_toplevel_handle_v1* handle = nullptr,
      ext_foreign_toplevel_handle_v1* extHandle = nullptr
  ) {
    ToplevelInfo window;
    window.identifier = std::move(identifier);
    window.order = order;
    window.handle = handle;
    window.extHandle = extHandle;
    return window;
  }

} // namespace

int main() {
  {
    const std::vector<ToplevelInfo> windows;
    TEST_CHECK(shell::newestActivatableWindow(windows) == nullptr);
  }

  {
    const std::vector<ToplevelInfo> windows{ToplevelInfo{}};
    TEST_CHECK(shell::newestActivatableWindow(windows) == nullptr);
  }

  {
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto three = makeWindow("window-three", 3, handle);
    const auto one = makeWindow("window-one", 1, handle);
    const auto two = makeWindow("window-two", 2, handle);
    const std::vector<ToplevelInfo> windows{three, one, two};
    const auto* result = shell::newestActivatableWindow(windows);
    TEST_CHECK(result != nullptr);
    TEST_CHECK(result->identifier == three.identifier);
  }

  {
    auto notActivatable = ToplevelInfo{};
    notActivatable.order = 5;
    const auto activatable = makeWindow("activatable", 2, reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1));
    const std::vector<ToplevelInfo> windows{notActivatable, activatable};
    const auto* result = shell::newestActivatableWindow(windows);
    TEST_CHECK(result != nullptr);
    TEST_CHECK(result->identifier == activatable.identifier);
  }

  {
    auto* wlrHandle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    auto* extHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x10);
    const auto wlrOne = makeWindow("wlr-one", 1, wlrHandle);
    const auto wlrTwo = makeWindow("wlr-two", 2, wlrHandle);
    const auto extOnly = makeWindow("ext-only", 9, nullptr, extHandle);
    const std::vector<ToplevelInfo> windows{wlrOne, wlrTwo, extOnly};
    const auto* result = shell::newestActivatableWindow(windows);
    TEST_CHECK(result != nullptr);
    TEST_CHECK(result->identifier == wlrTwo.identifier);
  }

  {
    auto* extHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x10);
    const auto extSeven = makeWindow("ext-seven", 7, nullptr, extHandle);
    const auto extFour = makeWindow("ext-four", 4, nullptr, extHandle);
    const std::vector<ToplevelInfo> windows{extSeven, extFour};
    const auto* result = shell::newestActivatableWindow(windows);
    TEST_CHECK(result != nullptr);
    TEST_CHECK(result->identifier == extSeven.identifier);
  }

  {
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto first = makeWindow("first", 0, handle);
    const auto second = makeWindow("second", 0, handle);
    const std::vector<ToplevelInfo> windows{first, second};
    const auto* result = shell::newestActivatableWindow(windows);
    TEST_CHECK(result != nullptr);
    TEST_CHECK(result->identifier == second.identifier);
  }

  // lastActivatableWindow picks the last vector element even when an earlier one has a
  // higher `order`, which is exactly what distinguishes it from newestActivatableWindow.
  {
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto highOrderFirst = makeWindow("high-order-first", 9, handle);
    const auto lowOrderLast = makeWindow("low-order-last", 1, handle);
    const std::vector<ToplevelInfo> windows{highOrderFirst, lowOrderLast};
    const auto* last = shell::lastActivatableWindow(windows);
    const auto* newest = shell::newestActivatableWindow(windows);
    TEST_CHECK(last != nullptr);
    TEST_CHECK(last->identifier == lowOrderLast.identifier);
    TEST_CHECK(newest != nullptr);
    TEST_CHECK(newest->identifier == highOrderFirst.identifier);
    TEST_CHECK(last != newest);
  }

  {
    const std::vector<ToplevelInfo> windows;
    TEST_CHECK(shell::lastActivatableWindow(windows) == nullptr);
  }

  {
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    auto notActivatable = ToplevelInfo{};
    notActivatable.order = 5;
    const auto activatable = makeWindow("activatable", 1, handle);
    const std::vector<ToplevelInfo> windows{activatable, notActivatable};
    const auto* result = shell::lastActivatableWindow(windows);
    TEST_CHECK(result != nullptr);
    TEST_CHECK(result->identifier == activatable.identifier);
  }

  // matchesActiveWindow: handle equality wins regardless of identifier.
  {
    auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
    const auto window = makeWindow("window-a", 0, handle);
    ActiveToplevel active;
    active.handle = handle;
    active.identifier = "window-b";
    const std::vector<ToplevelInfo> windows{window};
    TEST_CHECK(shell::matchesActiveWindow(window, active, "", windows));
  }

  // matchesActiveWindow: exactIdentity window matches by focusedCompositorWindowId.
  {
    ToplevelInfo window;
    window.identifier = "compositor-window-id";
    window.exactIdentity = true;
    ActiveToplevel active;
    const std::vector<ToplevelInfo> windows{window};
    TEST_CHECK(shell::matchesActiveWindow(window, active, "compositor-window-id", windows));
  }

  // matchesActiveWindow: ambiguity guard returns false when two windows share the
  // active identifier, even though the identifiers themselves match.
  {
    const auto windowOne = makeWindow("shared-identifier", 0);
    const auto windowTwo = makeWindow("shared-identifier", 0);
    ActiveToplevel active;
    active.identifier = "shared-identifier";
    const std::vector<ToplevelInfo> windows{windowOne, windowTwo};
    TEST_CHECK(!shell::matchesActiveWindow(windowOne, active, "", windows));
  }

  // matchesActiveWindow: identifier match is accepted when it is unique in the vector.
  {
    const auto window = makeWindow("unique-identifier", 0);
    ActiveToplevel active;
    active.identifier = "unique-identifier";
    const std::vector<ToplevelInfo> windows{window};
    TEST_CHECK(shell::matchesActiveWindow(window, active, "", windows));
  }
}
