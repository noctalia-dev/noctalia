#pragma once

#include "system/desktop_entry.h"

#include <optional>
#include <string>
#include <string_view>

// Toggle the freedesktop NoDisplay= key for a desktop entry.
//
// - Entries already living under $XDG_DATA_HOME/applications (user-level) are
//   edited in place.
// - Entries from any other directory (system dirs like /usr/share/applications)
//   get a user-level override file: a full copy of the system file with
//   NoDisplay flipped, written atomically. The override shadows the system file
//   entirely (no .desktop merge), so a full copy preserves Icon/Exec/etc.
//
// Returns true on success. On failure the caller may surface err.
bool setDesktopEntryHidden(const DesktopEntry& entry, bool hidden, std::string* err = nullptr);

// Resolve the pacman package that owns a .desktop file (Arch-only).
//
// Runs `pacman -Qo <path>`. Returns std::nullopt when pacman is missing, the
// path is not owned by any package, or the query fails — which also covers
// non-Arch systems, AppImages, Steam shortcuts and hand-made launchers.
std::optional<std::string> resolveOwningPackage(std::string_view desktopFilePath);
