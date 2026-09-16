#pragma once

#include "compositors/compositor_detect.h"
#include "wayland/wayland_toplevels.h"

#include <string_view>
#include <vector>

namespace shell {

  [[nodiscard]] inline bool canActivateWindow(const ToplevelInfo& window) {
    return window.handle != nullptr
        || !window.identifier.empty()
        || (compositors::isKde() && (!window.title.empty() || !window.appId.empty()));
  }

  [[nodiscard]] inline bool matchesActiveWindow(
      const ToplevelInfo& window, const ActiveToplevel& active, std::string_view focusedCompositorWindowId,
      const std::vector<ToplevelInfo>& windows
  ) {
    if (active.handle != nullptr && window.handle == active.handle) {
      return true;
    }

    // Exact-identity ext toplevels have no wlr handle; match them by the compositor's focused
    // window id, since `active.identifier` is an appId+title synthetic key for wlr toplevels.
    if (window.exactIdentity
        && !window.identifier.empty()
        && !focusedCompositorWindowId.empty()
        && window.identifier == focusedCompositorWindowId) {
      return true;
    }

    if (active.identifier.empty() || window.identifier.empty() || active.identifier != window.identifier) {
      return false;
    }

    int count = 0;
    for (const auto& w : windows) {
      if (w.identifier == active.identifier && ++count > 1) {
        return false;
      }
    }
    return true;
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

  // Verbatim dock semantics: the last activatable element in vector order. The dock's launch-focus
  // path depends on this exact pick; use newestActivatableWindow only where `order` is meaningful.
  [[nodiscard]] inline const ToplevelInfo* lastActivatableWindow(const std::vector<ToplevelInfo>& windows) {
    const ToplevelInfo* best = nullptr;
    for (const auto& window : windows) {
      if (!canActivateWindow(window)) {
        continue;
      }
      best = &window;
    }
    return best;
  }

} // namespace shell
