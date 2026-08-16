#pragma once

#include "dbus/tray/tray_service.h"
#include "shell/common/window_activation.h"
#include "shell/tray/tray_identifier.h"
#include "wayland/wayland_toplevels.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tray {

  // Lookup seam so matching logic is testable without a real CompositorPlatform;
  // production binds this to CompositorPlatform::windowsForApp.
  using WindowLookup = std::function<std::vector<ToplevelInfo>(const std::string& candidateLower)>;

  [[nodiscard]] inline std::optional<ToplevelInfo>
  findNewestWindowForTrayItem(const TrayItemInfo& item, const WindowLookup& lookup) {
    for (const auto& candidate : windowMatchCandidates(item)) {
      const auto windows = lookup(candidate);
      const ToplevelInfo* best = shell::newestActivatableWindow(windows);
      if (best != nullptr) {
        return *best;
      }
    }
    return std::nullopt;
  }

  // Intentionally narrower than dock's matchesActiveWindow: the tray already picked a
  // single "newest" window, so no multi-window disambiguation is needed, and exactIdentity
  // is always false for windows from the plain windowsForApp() path.
  [[nodiscard]] inline bool
  trayWindowIsFocused(const ToplevelInfo& window, const std::optional<ActiveToplevel>& active) {
    if (!active.has_value()) {
      return false;
    }
    if (active->handle != nullptr && window.handle == active->handle) {
      return true;
    }
    return !active->identifier.empty() && !window.identifier.empty() && active->identifier == window.identifier;
  }

  enum class TrayClickAction { ActivateItem, FocusWindow, Ignore, OpenMenu };

  struct TrayClickDecision {
    TrayClickAction action = TrayClickAction::ActivateItem;
    std::optional<ToplevelInfo> window;
  };

  // Clicking the tray icon of the already-focused window is a deliberate no-op: it must not
  // fall through to the SNI Activate call.
  [[nodiscard]] inline TrayClickDecision decideTrayClick(
      const TrayItemInfo& item, const WindowLookup& lookup, const std::optional<ActiveToplevel>& active,
      bool focusExistingWindow
  ) {
    // SNI spec: ItemIsMenu means the item only supports its context menu, Activate is
    // meaningless. This takes priority regardless of the focus-existing-window setting.
    if (item.itemIsMenu) {
      return {TrayClickAction::OpenMenu, std::nullopt};
    }
    if (!focusExistingWindow) {
      return {TrayClickAction::ActivateItem, std::nullopt};
    }
    const auto window = findNewestWindowForTrayItem(item, lookup);
    if (!window.has_value()) {
      return {TrayClickAction::ActivateItem, std::nullopt};
    }
    if (trayWindowIsFocused(*window, active)) {
      return {TrayClickAction::Ignore, std::nullopt};
    }
    return {TrayClickAction::FocusWindow, window};
  }

} // namespace tray
