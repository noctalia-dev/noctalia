#pragma once

#include "dbus/tray/tray_service.h"
#include "shell/common/window_activation.h"
#include "shell/tray/tray_identifier.h"
#include "system/app_identity.h"
#include "util/string_utils.h"
#include "wayland/wayland_toplevels.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tray {

  // Lookup seam so matching logic is testable without a real CompositorPlatform;
  // production binds this to CompositorPlatform::windowsForApp.
  using WindowLookup = std::function<std::vector<ToplevelInfo>(const std::string& candidateLower)>;

  // Running app ids as the compositor reports them (raw case). Production binds this to
  // CompositorPlatform::runningAppIds; it is a separate seam from WindowLookup so the
  // reverse-DNS pass stays testable.
  using RunningAppIds = std::function<std::vector<std::string>()>;

  namespace detail {

    // Ambiguity guard: two running apps sharing a tail must never cause a wrong window to
    // be focused, so a non-unique tail match resolves to nothing.
    [[nodiscard]] inline std::optional<std::string>
    uniqueAppIdWithTail(const std::vector<std::string>& appIds, std::string_view candidateLower) {
      const auto tail = app_identity::appIdTail(candidateLower);
      if (tail.size() < 3 || looksGenericStatusItemName(tail)) {
        return std::nullopt;
      }

      std::optional<std::string> match;
      int matchCount = 0;
      for (const auto& appId : appIds) {
        const auto lower = StringUtils::toLower(appId);
        if (app_identity::appIdTail(lower) == tail) {
          match = lower;
          ++matchCount;
        }
      }

      if (matchCount != 1) {
        return std::nullopt;
      }
      return match;
    }

  } // namespace detail

  struct MatchedWindows {
    std::vector<ToplevelInfo> windows;
    std::optional<ToplevelInfo> newest;
  };

  [[nodiscard]] inline MatchedWindows findNewestWindowForTrayItem(
      const TrayItemInfo& item, const WindowLookup& lookup, const RunningAppIds& runningAppIds
  ) {
    const auto candidates = windowMatchCandidates(item);
    if (candidates.empty()) {
      return {};
    }

    for (const auto& candidate : candidates) {
      const auto windows = lookup(candidate);
      const ToplevelInfo* best = shell::newestActivatableWindow(windows);
      if (best != nullptr) {
        MatchedWindows result;
        result.newest = *best;
        result.windows = windows;
        return result;
      }
    }

    const auto appIds = runningAppIds();
    for (const auto& candidate : candidates) {
      const auto resolved = detail::uniqueAppIdWithTail(appIds, candidate);
      if (!resolved.has_value() || *resolved == candidate) {
        continue;
      }
      const auto windows = lookup(*resolved);
      const ToplevelInfo* best = shell::newestActivatableWindow(windows);
      if (best != nullptr) {
        MatchedWindows result;
        result.newest = *best;
        result.windows = windows;
        return result;
      }
    }

    return {};
  }

  [[nodiscard]] inline bool trayWindowIsFocused(
      const ToplevelInfo& window, const std::vector<ToplevelInfo>& windows, const std::optional<ActiveToplevel>& active,
      std::string_view focusedCompositorWindowId
  ) {
    return active.has_value() && shell::matchesActiveWindow(window, *active, focusedCompositorWindowId, windows);
  }

  enum class TrayClickAction { ActivateItem, FocusWindow, OpenMenu };

  struct TrayClickDecision {
    TrayClickAction action = TrayClickAction::ActivateItem;
    std::optional<ToplevelInfo> window;
  };

  // An already-focused window falls through to Activate so apps whose tray icon is an
  // Activate-driven show/hide toggle keep their hide half (upstream issue 3859).
  [[nodiscard]] inline TrayClickDecision decideTrayClick(
      const TrayItemInfo& item, const WindowLookup& lookup, const RunningAppIds& runningAppIds,
      const std::optional<ActiveToplevel>& active, std::string_view focusedCompositorWindowId, bool focusExistingWindow
  ) {
    // Menu-only either declared (SNI ItemIsMenu) or structural (no Activate method exported,
    // per introspection — the libappindicator/ayatana class). Requires a real exported
    // DBusMenu, else left click would be a silent no-op.
    if (trayItemPrefersMenu(item)) {
      return {TrayClickAction::OpenMenu, std::nullopt};
    }
    if (!focusExistingWindow) {
      return {TrayClickAction::ActivateItem, std::nullopt};
    }
    const auto matched = findNewestWindowForTrayItem(item, lookup, runningAppIds);
    if (!matched.newest.has_value()) {
      return {TrayClickAction::ActivateItem, std::nullopt};
    }
    if (trayWindowIsFocused(*matched.newest, matched.windows, active, focusedCompositorWindowId)) {
      return {TrayClickAction::ActivateItem, std::nullopt};
    }
    return {TrayClickAction::FocusWindow, matched.newest};
  }

} // namespace tray
