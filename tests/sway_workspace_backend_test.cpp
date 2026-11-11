#include "compositors/sway/sway_runtime.h"
#include "compositors/sway/sway_workspace_backend.h"
#include "tests/test_check.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace {

  // nullptr models sway's own gap: XWayland views carry no app_id.
  nlohmann::json viewNode(const char* name, const char* appId, bool focused, const char* extIdentifier) {
    nlohmann::json node = {
        {"type", "con"},
        {"name", name},
        {"app_id", appId == nullptr ? nlohmann::json(nullptr) : nlohmann::json(appId)},
        {"focused", focused},
        {"nodes", nlohmann::json::array()},
        {"floating_nodes", nlohmann::json::array()},
    };
    if (extIdentifier != nullptr) {
      node["foreign_toplevel_identifier"] = extIdentifier;
    }
    return node;
  }

  // A GET_TREE payload with a single workspace holding `views`. When workspaceFocused is true no
  // view is focused, which is what sway reports after focusing the workspace itself.
  std::string tree(bool workspaceFocused, const std::vector<nlohmann::json>& view) {
    return nlohmann::json{
        {"type", "root"},
        {"nodes",
         nlohmann::json::array(
             {{{"type", "output"},
               {"nodes",
                nlohmann::json::array(
                    {{{"type", "workspace"}, {"name", "1"}, {"num", 1}, {"focused", workspaceFocused}, {"nodes", view}}}
                )}}}
         )},
    }
        .dump();
  }

} // namespace

int main() {
  compositors::sway::SwayRuntime runtime;
  SwayWorkspaceBackend backend({}, runtime);

  // Selection follows `focused`, not document order: the focused XWayland view is listed second
  // and has no app_id, the case the wlr/ext app_id asymmetry used to break.
  backend.parseTree(
      tree(false, {viewNode("foot", "foot", false, "tok-native"), viewNode("Steam", nullptr, true, "tok-x11")})
  );
  TEST_CHECK(backend.focusedExtToplevelIdentifier() == std::optional<std::string>("tok-x11"));

  // Reversed order must resolve to the other token, proving no first-match-wins.
  backend.parseTree(
      tree(false, {viewNode("Steam", nullptr, true, "tok-x11"), viewNode("foot", "foot", false, "tok-native")})
  );
  TEST_CHECK(backend.focusedExtToplevelIdentifier() == std::optional<std::string>("tok-x11"));

  // Focus on the workspace: no view is focused, so the one remaining identifier must NOT be
  // used. Capturing it would be a wrong-window capture.
  backend.parseTree(tree(true, {viewNode("foot", "foot", false, "tok-native")}));
  TEST_CHECK(!backend.focusedExtToplevelIdentifier().has_value());

  // No sway IPC socket in a test, so the capability stays off and callers keep their fallback.
  TEST_CHECK(!backend.publishesExtToplevelIdentity());
  return 0;
}
