#include "wayland/ext_foreign_toplevels.h"

#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  int handleTokenA = 0;
  int handleTokenB = 0;

  ext_foreign_toplevel_handle_v1* const handleA = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(&handleTokenA);
  ext_foreign_toplevel_handle_v1* const handleB = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(&handleTokenB);

  [[nodiscard]] ToplevelInfo window(ext_foreign_toplevel_handle_v1* extHandle, std::string title) {
    return ToplevelInfo{
        .title = std::move(title),
        .appId = "app",
        .extHandle = extHandle,
    };
  }

  bool selectsUniqueTitleMatch() {
    const std::vector<ToplevelInfo> windows{
        window(handleA, "Editor"),
        window(handleB, "Terminal"),
    };
    const auto editor = uniqueExtHandleForTitle(windows, "Editor");
    const auto caseMismatch = uniqueExtHandleForTitle(windows, "terminal");
    const auto missing = uniqueExtHandleForTitle(windows, "Missing");
    return expect(editor.handle == handleA && editor.matchCount == 1, "unique title should resolve to its handle")
        && expect(caseMismatch.handle == nullptr && caseMismatch.matchCount == 0, "title match should be exact")
        && expect(missing.handle == nullptr && missing.matchCount == 0, "unknown title should not resolve");
  }

  bool rejectsAmbiguity() {
    const std::vector<ToplevelInfo> windows{
        window(handleA, "Editor"),
        window(handleB, "Editor"),
    };
    const auto ambiguous = uniqueExtHandleForTitle(windows, "Editor");
    const auto empty = uniqueExtHandleForTitle(windows, "");
    return expect(
               ambiguous.handle == nullptr && ambiguous.matchCount == 2, "two same-title windows should be ambiguous"
           )
        && expect(empty.handle == nullptr && empty.matchCount == 2, "empty title should not resolve among many");
  }

  bool ignoresEntriesWithoutExtHandle() {
    const std::vector<ToplevelInfo> windows{
        window(nullptr, "Editor"),
        window(handleA, "Editor"),
    };
    const auto match = uniqueExtHandleForTitle(windows, "Editor");
    return expect(match.handle == handleA && match.matchCount == 1, "wlr-only entries should be ignored");
  }

  bool emptySelection() {
    const std::vector<ToplevelInfo> windows{window(handleA, "Editor")};
    const auto only = uniqueExtHandleForTitle(windows, "");
    const auto none = uniqueExtHandleForTitle({}, "Editor");
    return expect(only.handle == handleA && only.matchCount == 1, "empty title accepts the only candidate")
        && expect(none.handle == nullptr && none.matchCount == 0, "no candidates should not resolve");
  }
} // namespace

int main() {
  bool ok = true;
  ok = selectsUniqueTitleMatch() && ok;
  ok = rejectsAmbiguity() && ok;
  ok = ignoresEntriesWithoutExtHandle() && ok;
  ok = emptySelection() && ok;
  return ok ? 0 : 1;
}
