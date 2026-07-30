#include "system/internal_app_metadata.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define private public
#include "wayland/wayland_toplevels.h"
#undef private

namespace internal_apps {

  const InternalAppDefinition* appDefinitionForWindowTitle(std::string_view /*windowTitle*/) { return nullptr; }

  std::optional<AppMetadata> metadataForAppId(std::string_view /*appId*/) { return std::nullopt; }

  void applyMetadataToDesktopEntry(DesktopEntry& /*entry*/) {}

} // namespace internal_apps

int main() {
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
  return 0;
}
