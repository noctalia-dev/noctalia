#pragma once

#include <functional>

struct xdg_popup;

// Gives a parentless xdg_popup its parent, for parent roles that xdg_surface.get_popup cannot name
// (a desktop-shell surface whose protocol has its own get_popup request). The owner of that role
// supplies it; PopupSurface::initialize() calls it once, after creating the popup and before the
// popup's first commit.
using PopupParentAttach = std::function<void(xdg_popup*)>;
