#include "system/internal_app_metadata.h"
#include "wayland/wayland_protocol_policy.h"

#include <cassert>
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

  assert(shouldBindExtForeignToplevelList(CompositorKind::Niri));
  assert(shouldBindExtForeignToplevelList(CompositorKind::Hyprland));
  assert(shouldBindExtForeignToplevelList(CompositorKind::Kde));
  assert(!shouldBindExtForeignToplevelList(CompositorKind::Unknown));
  assert(!shouldBindExtForeignToplevelList(CompositorKind::Sway));

  WaylandToplevels toplevels;
  auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x1);
  auto [it, inserted] = toplevels.m_handles.try_emplace(handle, WaylandToplevels::ToplevelState{});
  assert(inserted);
  it->second.title = "Sample Chat";
  it->second.appId = "Sample.ChatDesktop";
  it->second.order = toplevels.m_nextOrder++;

  const auto windows = toplevels.windowsForApp("sample-chat-desktop", "samplechat");

  assert(windows.size() == 1);
  assert(windows[0].handle == handle);
  assert(windows[0].appId == "Sample.ChatDesktop");
  assert(toplevels.containsWlrHandle(handle));
  assert(!toplevels.containsWlrHandle(reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(0x2)));
  // Never announced an output (compositor omitted output_enter): matches any filter, and reports the
  // output as unannounced so the platform layer can scope it.
  auto* someOutput = reinterpret_cast<wl_output*>(0x10);
  auto* otherOutput = reinterpret_cast<wl_output*>(0x20);
  const auto unscoped = toplevels.windowsForApp("sample-chat-desktop", "samplechat", someOutput);
  assert(unscoped.size() == 1);
  assert(!unscoped[0].outputAnnounced);

  // Announced a different output: excluded there, present on its own output.
  toplevels.onHandleOutputEnter(handle, otherOutput);
  assert(toplevels.windowsForApp("sample-chat-desktop", "samplechat", someOutput).empty());
  const auto scoped = toplevels.windowsForApp("sample-chat-desktop", "samplechat", otherOutput);
  assert(scoped.size() == 1);
  assert(scoped[0].outputAnnounced);

  // Announced an output, then left all of them: still excluded.
  toplevels.onHandleOutputLeave(handle, otherOutput);
  assert(it->second.output == nullptr);
  assert(toplevels.windowsForApp("sample-chat-desktop", "samplechat", someOutput).empty());

  WaylandExtForeignToplevels extToplevels;
  int changeCount = 0;
  extToplevels.setChangeCallback([&]() { ++changeCount; });

  auto* extHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x30);
  auto [extIt, extInserted] =
      extToplevels.m_handles.try_emplace(extHandle, WaylandExtForeignToplevels::ToplevelState{});
  assert(extInserted);
  extIt->second.order = extToplevels.m_nextOrder++;

  extToplevels.onHandleTitle(extHandle, "noctalia");
  extToplevels.onHandleAppId(extHandle, "com.mitchellh.ghostty");
  extToplevels.onHandleIdentifier(extHandle, "49");

  // Initial properties are an atomic batch and remain hidden until `done`.
  assert(extToplevels.allAppIds().empty());
  assert(extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty").empty());
  std::size_t visited = 0;
  extToplevels.visitExtHandles([&](ext_foreign_toplevel_handle_v1*) { ++visited; });
  assert(visited == 0);

  extToplevels.onHandleDone(extHandle);
  assert(changeCount == 1);
  assert(extToplevels.allAppIds() == std::vector<std::string>{"com.mitchellh.ghostty"});
  auto extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  assert(extWindows.size() == 1);
  assert(extWindows[0].identifier == "49");
  assert(extWindows[0].title == "noctalia");
  visited = 0;
  extToplevels.visitExtHandles([&](ext_foreign_toplevel_handle_v1* current) {
    assert(current == extHandle);
    ++visited;
  });
  assert(visited == 1);

  // Later property batches also remain hidden until their own `done`.
  extToplevels.onHandleTitle(extHandle, "renamed");
  extToplevels.onHandleIdentifier(extHandle, "50");
  extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  assert(extWindows.size() == 1);
  assert(extWindows[0].identifier == "49");
  assert(extWindows[0].title == "noctalia");

  extToplevels.onHandleDone(extHandle);
  assert(changeCount == 2);
  extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  assert(extWindows.size() == 1);
  assert(extWindows[0].identifier == "50");
  assert(extWindows[0].title == "renamed");

  // Identical presentation metadata never collapses distinct exact identities.
  auto* duplicateHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x31);
  auto [duplicateIt, duplicateInserted] =
      extToplevels.m_handles.try_emplace(duplicateHandle, WaylandExtForeignToplevels::ToplevelState{});
  assert(duplicateInserted);
  duplicateIt->second.order = extToplevels.m_nextOrder++;
  extToplevels.onHandleTitle(duplicateHandle, "renamed");
  extToplevels.onHandleAppId(duplicateHandle, "com.mitchellh.ghostty");
  extToplevels.onHandleIdentifier(duplicateHandle, "52");
  extToplevels.onHandleDone(duplicateHandle);

  extWindows = extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty");
  assert(extWindows.size() == 2);
  assert(extWindows[0].identifier == "50");
  assert(extWindows[1].identifier == "52");

  // State removal is safe for both ready and not-yet-ready handles.
  extToplevels.removeHandle(duplicateHandle);
  assert(changeCount == 4);
  assert(extToplevels.windowsForApp("com.mitchellh.ghostty", "ghostty").size() == 1);
  auto* pendingHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(0x32);
  extToplevels.m_handles.try_emplace(pendingHandle, WaylandExtForeignToplevels::ToplevelState{});
  extToplevels.removeHandle(pendingHandle);
  assert(changeCount == 5);
  assert(!extToplevels.m_handles.contains(pendingHandle));

  return 0;
}
