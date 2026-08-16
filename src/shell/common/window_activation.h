#pragma once

#include "compositors/compositor_detect.h"
#include "wayland/wayland_toplevels.h"

#include <vector>

namespace shell {

  [[nodiscard]] inline bool canActivateWindow(const ToplevelInfo& window) {
    return window.handle != nullptr
        || !window.identifier.empty()
        || (compositors::isKde() && (!window.title.empty() || !window.appId.empty()));
  }

  namespace detail {

    [[nodiscard]] inline bool isExtOnlyWindow(const ToplevelInfo& window) {
      return window.handle == nullptr && window.extHandle != nullptr;
    }

    [[nodiscard]] inline const ToplevelInfo*
    highestOrderWindow(const std::vector<ToplevelInfo>& windows, bool extOnly) {
      const ToplevelInfo* best = nullptr;
      for (const auto& window : windows) {
        if (isExtOnlyWindow(window) != extOnly || !canActivateWindow(window)) {
          continue;
        }
        // >= keeps the last of an order tie, matching the previous position-based behavior.
        if (best == nullptr || window.order >= best->order) {
          best = &window;
        }
      }
      return best;
    }

  } // namespace detail

  // wlr and ext_foreign_toplevel_list stamp `order` from independent counters, so it only ranks
  // windows from the same protocol; Hyprland's windowsForApp appends ext-only toplevels after the
  // wlr ones, which makes vector position meaningless across that boundary. Rank inside the
  // wlr/KDE group first (those activate by handle), fall back to the ext-only group.
  [[nodiscard]] inline const ToplevelInfo* newestActivatableWindow(const std::vector<ToplevelInfo>& windows) {
    if (const ToplevelInfo* best = detail::highestOrderWindow(windows, false); best != nullptr) {
      return best;
    }
    return detail::highestOrderWindow(windows, true);
  }

} // namespace shell
