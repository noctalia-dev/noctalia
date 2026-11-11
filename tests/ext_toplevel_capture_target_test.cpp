#include "shell/wayland/ext_foreign_toplevels.h"
#include "tests/test_check.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
  int handleTokenA = 0;
  int handleTokenB = 0;

  ext_foreign_toplevel_handle_v1* const handleA = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(&handleTokenA);
  ext_foreign_toplevel_handle_v1* const handleB = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(&handleTokenB);

  ToplevelInfo window(ext_foreign_toplevel_handle_v1* extHandle, std::string title) {
    return ToplevelInfo{
        .title = std::move(title),
        .appId = "app",
        .extHandle = extHandle,
    };
  }

  void selectsUniqueTitleMatch() {
    const std::vector<ToplevelInfo> windows{
        window(handleA, "Editor"),
        window(handleB, "Terminal"),
    };
    const auto editor = uniqueExtHandleForTitle(windows, "Editor");
    const auto caseMismatch = uniqueExtHandleForTitle(windows, "terminal");
    const auto missing = uniqueExtHandleForTitle(windows, "Missing");
    TEST_CHECK(editor.handle == handleA && editor.matchCount == 1);
    TEST_CHECK(caseMismatch.handle == nullptr && caseMismatch.matchCount == 0);
    TEST_CHECK(missing.handle == nullptr && missing.matchCount == 0);
  }

  void rejectsAmbiguity() {
    const std::vector<ToplevelInfo> windows{
        window(handleA, "Editor"),
        window(handleB, "Editor"),
    };
    const auto ambiguous = uniqueExtHandleForTitle(windows, "Editor");
    const auto empty = uniqueExtHandleForTitle(windows, "");
    TEST_CHECK(ambiguous.handle == nullptr && ambiguous.matchCount == 2);
    TEST_CHECK(empty.handle == nullptr && empty.matchCount == 2);
  }

  void rejectsAmbiguityRegardlessOfOrder() {
    const std::vector<ToplevelInfo> windows{
        window(handleA, "Editor"),
        window(handleB, "Editor"),
    };
    const std::vector<ToplevelInfo> reversed{
        window(handleA, "Editor"),
        window(handleB, "Editor"),
    };
    const auto forward = uniqueExtHandleForTitle(windows, "Editor");
    const auto backward = uniqueExtHandleForTitle(reversed, "Editor");
    const auto emptyTitled = uniqueExtHandleForTitle(windows, "");
    TEST_CHECK(forward.handle == nullptr && forward.matchCount == 2);
    TEST_CHECK(backward.handle == nullptr && backward.matchCount == 2);
    TEST_CHECK(emptyTitled.handle == nullptr && emptyTitled.matchCount == 2);
  }

  void ignoresEntriesWithoutExtHandle() {
    const std::vector<ToplevelInfo> windows{
        window(nullptr, "Editor"),
        window(handleA, "Editor"),
    };
    const auto match = uniqueExtHandleForTitle(windows, "Editor");
    TEST_CHECK(match.handle == handleA && match.matchCount == 1);
  }

  void emptySelection() {
    const std::vector<ToplevelInfo> windows{window(handleA, "Editor")};
    const auto only = uniqueExtHandleForTitle(windows, "");
    const auto none = uniqueExtHandleForTitle({}, "Editor");
    TEST_CHECK(only.handle == handleA && only.matchCount == 1);
    TEST_CHECK(none.handle == nullptr && none.matchCount == 0);
  }
} // namespace

int main() {
  selectsUniqueTitleMatch();
  rejectsAmbiguity();
  rejectsAmbiguityRegardlessOfOrder();
  ignoresEntriesWithoutExtHandle();
  emptySelection();
  return 0;
}
