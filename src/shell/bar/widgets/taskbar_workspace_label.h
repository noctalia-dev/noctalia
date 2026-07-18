#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Chooses the text drawn on a taskbar workspace badge.
//
// When showName is true, a named workspace (e.g. a niri "workspace n") shows its
// name in place of the numeric index; unnamed workspaces carry an empty name and
// fall back to the numeric label. When showName is false the name is ignored and
// the short numeric label is always used (avoids long names overlapping icons).
// KDE virtual desktops ship verbose default names ("Desktop 1"), so when a KDE
// desktop index is known it always wins over the name. The caller keeps a
// separate numeric key for window grouping, so this only affects what is
// displayed, never how windows are matched to workspaces.
[[nodiscard]] inline std::string taskbarWorkspaceLabel(
    bool isKde, std::uint32_t index, std::string_view name, std::string_view indexLabel, bool showName = true
) {
  if (isKde && index > 0) {
    return std::to_string(index);
  }
  if (showName && !name.empty()) {
    return std::string(name);
  }
  return std::string(indexLabel);
}
