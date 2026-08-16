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
}
