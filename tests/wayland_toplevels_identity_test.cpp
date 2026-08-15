#include "system/internal_app_metadata.h"
#include "tests/test_check.h"
#include "wayland/wayland_protocol_policy.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define private public
#include "wayland/ext_foreign_toplevels.h"
#include "wayland/wayland_toplevels.h"
#undef private

namespace internal_apps {

  const InternalAppDefinition* appDefinitionForWindowTitle(std::string_view /*windowTitle*/) { return nullptr; }

  std::optional<AppMetadata> metadataForAppId(std::string_view /*appId*/) { return std::nullopt; }

  void applyMetadataToDesktopEntry(DesktopEntry& /*entry*/) {}

} // namespace internal_apps

int main() {
  using compositors::CompositorKind;
  using wayland_protocol_policy::shouldBindExtForeignToplevelList;

  TEST_CHECK(shouldBindExtForeignToplevelList(CompositorKind::Niri));
  TEST_CHECK(shouldBindExtForeignToplevelList(CompositorKind::Hyprland));
  TEST_CHECK(shouldBindExtForeignToplevelList(CompositorKind::Kde));
  TEST_CHECK(!shouldBindExtForeignToplevelList(CompositorKind::Unknown));
  TEST_CHECK(!shouldBindExtForeignToplevelList(CompositorKind::Sway));

  WaylandToplevels toplevels;
  auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
  auto [it, inserted] = toplevels.m_handles.try_emplace(handle, WaylandToplevels::ToplevelState{});
  TEST_CHECK(inserted);
  it->second.title = "Sample Chat";
  it->second.appId = "Sample.ChatDesktop";
  it->second.order = toplevels.m_nextOrder++;

  const auto windows = toplevels.windowsForApp("sample-chat-desktop", "samplechat");

  TEST_CHECK(windows.size() == 1);
  TEST_CHECK(windows[0].handle == handle);
  TEST_CHECK(windows[0].appId == "Sample.ChatDesktop");
  TEST_CHECK(toplevels.containsWlrHandle(handle));
  TEST_CHECK(!toplevels.containsWlrHandle(reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x2)));
  // Never announced an output (compositor omitted output_enter): matches any filter, and reports the
  // output as unannounced so the platform layer can scope it.
  auto* someOutput = reinterpret_cast<wl_output*>(0x10);
  auto* otherOutput = reinterpret_cast<wl_output*>(0x20);
  const auto unscoped = toplevels.windowsForApp("sample-chat-desktop", "samplechat", someOutput);
  TEST_CHECK(unscoped.size() == 1);
  TEST_CHECK(!unscoped[0].outputAnnounced);

  // Announced a different output: excluded there, present on its own output.
  toplevels.onHandleOutputEnter(handle, otherOutput);
  TEST_CHECK(toplevels.windowsForApp("sample-chat-desktop", "samplechat", someOutput).empty());
  const auto scoped = toplevels.windowsForApp("sample-chat-desktop", "samplechat", otherOutput);
  TEST_CHECK(scoped.size() == 1);
  TEST_CHECK(scoped[0].outputAnnounced);

  // Announced an output, then left all of them: still excluded.
  toplevels.onHandleOutputLeave(handle, otherOutput);
  TEST_CHECK(it->second.output == nullptr);
  TEST_CHECK(toplevels.windowsForApp("sample-chat-desktop", "samplechat", someOutput).empty());

  WaylandExtForeignToplevels extToplevels;
  int changeCount = 0;
  extToplevels.setChangeCallback([&]() { ++changeCount; });

  auto* extHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x30);
  auto [extIt, extInserted] =
      extToplevels.m_handles.try_emplace(extHandle, WaylandExtForeignToplevels::ToplevelState{});
  TEST_CHECK(extInserted);
  extIt->second.order = extToplevels.m_nextOrder++;

  extToplevels.onHandleTitle(extHandle, "noctalia");
  extToplevels.onHandleAppId(extHandle, "com.mitchellh.ghostty");
  extToplevels.onHandleIdentifier(extHandle, "49");

  // Initial properties are an atomic batch and remain hidden until `done`.
  TEST_CHECK(extToplevels.allAppIds().empty());
  TEST_CHECK(extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty").empty());
  std::size_t visited = 0;
  extToplevels.visitExtHandles([&](ext_foreign_toplevel_handle_v1*) { ++visited; });
  TEST_CHECK(visited == 0);

  extToplevels.onHandleDone(extHandle);
  TEST_CHECK(changeCount == 1);
  TEST_CHECK(extToplevels.allAppIds() == std::vector<std::string>{"com.mitchellh.ghostty"});
  auto extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  TEST_CHECK(extWindows.size() == 1);
  TEST_CHECK(extWindows[0].identifier == "49");
  TEST_CHECK(extWindows[0].title == "noctalia");
  visited = 0;
  extToplevels.visitExtHandles([&](ext_foreign_toplevel_handle_v1* current) {
    TEST_CHECK(current == extHandle);
    ++visited;
  });
  TEST_CHECK(visited == 1);

  // Later property batches also remain hidden until their own `done`.
  extToplevels.onHandleTitle(extHandle, "renamed");
  extToplevels.onHandleIdentifier(extHandle, "50");
  extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  TEST_CHECK(extWindows.size() == 1);
  TEST_CHECK(extWindows[0].identifier == "49");
  TEST_CHECK(extWindows[0].title == "noctalia");

  extToplevels.onHandleDone(extHandle);
  TEST_CHECK(changeCount == 2);
  extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  TEST_CHECK(extWindows.size() == 1);
  TEST_CHECK(extWindows[0].identifier == "50");
  TEST_CHECK(extWindows[0].title == "renamed");

  // Identical presentation metadata never collapses distinct exact identities.
  auto* duplicateHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x31);
  auto [duplicateIt, duplicateInserted] =
      extToplevels.m_handles.try_emplace(duplicateHandle, WaylandExtForeignToplevels::ToplevelState{});
  TEST_CHECK(duplicateInserted);
  duplicateIt->second.order = extToplevels.m_nextOrder++;
  extToplevels.onHandleTitle(duplicateHandle, "renamed");
  extToplevels.onHandleAppId(duplicateHandle, "com.mitchellh.ghostty");
  extToplevels.onHandleIdentifier(duplicateHandle, "52");
  extToplevels.onHandleDone(duplicateHandle);

  extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  TEST_CHECK(extWindows.size() == 2);
  TEST_CHECK(extWindows[0].identifier == "50");
  TEST_CHECK(extWindows[1].identifier == "52");

  // State removal is safe for both ready and not-yet-ready handles.
  extToplevels.removeHandle(duplicateHandle);
  TEST_CHECK(changeCount == 4);
  TEST_CHECK(extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty").size() == 1);
  auto* pendingHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x32);
  extToplevels.m_handles.try_emplace(pendingHandle, WaylandExtForeignToplevels::ToplevelState{});
  extToplevels.removeHandle(pendingHandle);
  TEST_CHECK(changeCount == 5);
  TEST_CHECK(!extToplevels.m_handles.contains(pendingHandle));

  return 0;
}
