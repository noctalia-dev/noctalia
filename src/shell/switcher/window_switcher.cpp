#include "shell/switcher/window_switcher.h"

#include "capture/toplevel_thumbnail_capture.h"
#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "compositors/hyprland/hyprland_window_id.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/surface/shadow.h"
#include "shell/switcher/window_switcher_carousel_style.h"
#include "shell/switcher/window_switcher_compact_style.h"
#include "shell/switcher/window_switcher_membership.h"
#include "shell/switcher/window_switcher_tile.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "system/internal_app_metadata.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("window-switcher");
  constexpr std::size_t kVisibleCards = 5;
  constexpr float kVisibleOpacityThreshold = 0.01F;

  [[nodiscard]] WindowSwitcherStyleLayout computeSwitcherLayout(
      float screenWidth, float screenHeight, float scale, std::size_t windowCount, std::size_t selectedIndex,
      const ShellConfig::WindowSwitcherConfig& config
  ) {
    const WindowSwitcherStyleContext context{
        .screenWidth = screenWidth,
        .screenHeight = screenHeight,
        .scale = scale,
        .windowCount = windowCount,
        .selectedIndex = selectedIndex,
        .showCaption = config.showCaption,
        .showCount = config.showCount,
    };
    if (config.style == ShellConfig::WindowSwitcherStyle::Compact) {
      return computeWindowSwitcherCompactLayout(context);
    }
    return computeWindowSwitcherCarouselLayout(context);
  }

  [[nodiscard]] std::string resolveWindowIconPath(const std::string& appId, IconResolver& iconResolver, int iconSize) {
    if (appId.empty()) {
      return {};
    }

    if (const auto internal = internal_apps::metadataForAppId(appId);
        internal.has_value() && !internal->iconPath.empty()) {
      return internal->iconPath;
    }

    const DesktopEntry desktopEntry = app_identity::resolveRunningDesktopEntry(appId, desktopEntries());
    if (!desktopEntry.icon.empty()) {
      if (const std::string resolved = iconResolver.resolve(desktopEntry.icon, iconSize); !resolved.empty()) {
        return resolved;
      }
    }

    if (const std::string resolved = iconResolver.resolve(appId, iconSize); !resolved.empty()) {
      return resolved;
    }

    return iconResolver.resolve("application-x-executable", iconSize);
  }

  [[nodiscard]] std::string resolveWindowAppLabel(const std::string& appId) {
    if (appId.empty()) {
      return {};
    }
    const DesktopEntry desktopEntry = app_identity::resolveRunningDesktopEntry(appId, desktopEntries());
    if (!desktopEntry.name.empty()) {
      return desktopEntry.name;
    }
    return appId;
  }

  [[nodiscard]] float shellUiScale(const ConfigService* config) noexcept {
    return config != nullptr ? config->config().accessibility.uiScale : 1.0F;
  }

  [[nodiscard]] bool isAltModifier(std::uint32_t sym) noexcept { return sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R; }

  [[nodiscard]] const WaylandOutput* findOutput(const WaylandConnection& wayland, wl_output* output) {
    for (const auto& entry : wayland.outputs()) {
      if (entry.output == output) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::uintptr_t wlrHandleForToplevel(const ToplevelInfo& info) noexcept {
    if (info.handle != nullptr) {
      return reinterpret_cast<std::uintptr_t>(info.handle);
    }
    return 0;
  }

  [[nodiscard]] std::uintptr_t extHandleForToplevel(const ToplevelInfo& info) noexcept {
    if (info.extHandle != nullptr) {
      return reinterpret_cast<std::uintptr_t>(info.extHandle);
    }
    return 0;
  }

  [[nodiscard]] std::optional<std::string>
  windowIdForToplevelInfo(const CompositorPlatform& platform, const ToplevelInfo& info) {
    if (info.exactIdentity && !info.identifier.empty()) {
      return info.identifier;
    }
    return platform.compositorWindowIdForToplevelInfo(info);
  }

  [[nodiscard]] std::string canonicalWindowId(std::string_view windowId) {
    if (windowId.empty()) {
      return {};
    }
    if (compositors::isHyprland()) {
      if (const std::string normalized = compositors::hyprland::normalizeWindowId(windowId); !normalized.empty()) {
        return normalized;
      }
    }
    return std::string(windowId);
  }

  void activateWindowSwitcherEntry(CompositorPlatform& platform, const WindowSwitcherEntry& entry) {
    // Niri: foreign-toplevel activate does not reliably focus/scroll the column.
    if (compositors::isNiri() && !entry.windowId.empty()) {
      platform.focusCompositorWindow(entry.windowId);
      return;
    }
    // Umbriel's exact-id action preserves the switcher's pointer-warp intent without changing taskbar activation.
    if (compositors::isUmbriel() && !entry.windowId.empty()) {
      platform.focusCompositorWindow(entry.windowId, true);
      return;
    }
    // Prefer wlr-foreign-toplevel activate (same path as the taskbar). On Hyprland this
    // raises floating windows and respects cursor:no_warps / scrolling follow_focus;
    // dispatch focuswindow alone does not.
    if (entry.closeHandle != 0) {
      auto* handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(entry.closeHandle);
      if (platform.containsWlrToplevelHandle(handle)) {
        platform.activateToplevel(handle);
        return;
      }
    }
    if (entry.windowId.empty()) {
      return;
    }
    if (zwlr_foreign_toplevel_handle_v1* handle = platform.toplevelHandleForCompositorWindowId(entry.windowId);
        handle != nullptr && platform.containsWlrToplevelHandle(handle)) {
      platform.activateToplevel(handle);
      return;
    }
    platform.focusCompositorWindow(entry.windowId, true);
  }

  [[nodiscard]] std::string identityKeyForEntry(const WindowSwitcherEntry& entry) {
    const std::string canonical = canonicalWindowId(entry.windowId);
    if (!canonical.empty()) {
      return canonical;
    }
    if (entry.closeHandle != 0) {
      return "handle:" + std::to_string(entry.closeHandle);
    }
    return {};
  }

  [[nodiscard]] std::string currentFocusedWindowKey(const CompositorPlatform& platform) {
    const auto focusedId = platform.focusedCompositorWindowId();
    if (!focusedId.has_value()) {
      return {};
    }
    return canonicalWindowId(*focusedId);
  }

  [[nodiscard]] std::uintptr_t resolveCloseHandle(
      const CompositorPlatform& platform, std::string_view windowId, std::string_view appId, std::string_view title
  ) {
    if (!windowId.empty()) {
      if (zwlr_foreign_toplevel_handle_v1* handle = platform.toplevelHandleForCompositorWindowId(windowId);
          handle != nullptr) {
        return reinterpret_cast<std::uintptr_t>(handle);
      }
    }

    const std::string idLower = StringUtils::toLower(appId);
    for (const auto& info : platform.windowsForApp(idLower, idLower)) {
      if (!windowId.empty()) {
        if (const auto mapped = platform.compositorWindowIdForToplevelInfo(info); mapped.has_value()
            && (compositors::isHyprland() ? compositors::hyprland::windowIdsEqual(*mapped, windowId)
                                          : *mapped == windowId)) {
          return wlrHandleForToplevel(info);
        }
      }
      if (!title.empty() && info.title == title) {
        return wlrHandleForToplevel(info);
      }
    }
    return 0;
  }

  [[nodiscard]] std::uintptr_t
  resolveCaptureHandle(const WaylandConnection& wayland, std::string_view appId, std::string_view title) {
    const std::string idLower = StringUtils::toLower(appId);
    const auto windows = wayland.extWindowsForApp(idLower, idLower);
    ext_foreign_toplevel_handle_v1* matched = nullptr;
    for (const auto& info : windows) {
      if (info.extHandle == nullptr || (!title.empty() && info.title != title)) {
        continue;
      }
      if (matched != nullptr) {
        return 0;
      }
      matched = info.extHandle;
    }
    if (matched != nullptr) {
      return reinterpret_cast<std::uintptr_t>(matched);
    }
    if (windows.size() == 1 && windows.front().extHandle != nullptr) {
      return reinterpret_cast<std::uintptr_t>(windows.front().extHandle);
    }
    return 0;
  }

  [[nodiscard]] WindowSwitcherEntry makeEntryFromToplevel(
      const CompositorPlatform& platform, IconResolver& iconResolver, int iconSize, const std::string& appId,
      const ToplevelInfo& info
  ) {
    WindowSwitcherEntry entry;
    if (const auto windowId = windowIdForToplevelInfo(platform, info); windowId.has_value()) {
      entry.windowId = *windowId;
    } else if (info.handle != nullptr) {
      entry.windowId = "toplevel:" + std::to_string(reinterpret_cast<std::uintptr_t>(info.handle));
    }
    entry.closeHandle = wlrHandleForToplevel(info);
    entry.captureHandle = extHandleForToplevel(info);
    entry.appId = info.appId.empty() ? appId : info.appId;
    entry.appLabel = resolveWindowAppLabel(entry.appId);
    entry.title = info.title.empty() ? entry.appLabel : info.title;
    entry.iconPath = resolveWindowIconPath(entry.appId, iconResolver, iconSize);
    return entry;
  }

  struct WindowSwitcherCandidate {
    WindowSwitcherEntry entry;
    std::string workspaceKey;
    std::int32_t sortX = 0;
    std::int32_t sortY = 0;
    std::uint64_t toplevelOrder = 0;
    // Windows with no MRU rank sort after every ranked window.
    std::size_t mruIndex = std::numeric_limits<std::size_t>::max();
  };

  [[nodiscard]] WindowSwitcherEntry makeEntryFromAssignment(
      const CompositorPlatform& platform, const WaylandConnection& wayland, IconResolver& iconResolver, int iconSize,
      const WorkspaceWindowAssignment& assignment
  ) {
    WindowSwitcherEntry entry;
    entry.windowId = assignment.windowId;
    entry.appId = assignment.appId;
    entry.appLabel = resolveWindowAppLabel(entry.appId);
    entry.title = !assignment.title.empty() ? assignment.title : entry.appLabel;
    entry.iconPath = resolveWindowIconPath(entry.appId, iconResolver, iconSize);
    entry.closeHandle = resolveCloseHandle(platform, entry.windowId, entry.appId, entry.title);
    entry.captureHandle = resolveCaptureHandle(wayland, entry.appId, entry.title);
    return entry;
  }

  void indexLiveToplevelsByWindowId(
      const CompositorPlatform& platform, wl_output* outputFilter, std::unordered_map<std::string, ToplevelInfo>& out
  ) {
    std::unordered_set<std::uintptr_t> seenWlrHandles;
    std::unordered_set<std::uintptr_t> seenExtHandles;

    for (const auto& appId : platform.runningAppIds(outputFilter)) {
      const std::string lower = StringUtils::toLower(appId);
      for (const auto& info : platform.enrichedWindowsForApp(lower, lower, outputFilter)) {
        const std::uintptr_t wlrHandle = wlrHandleForToplevel(info);
        const std::uintptr_t extHandle = extHandleForToplevel(info);
        if (wlrHandle != 0 && seenWlrHandles.contains(wlrHandle)) {
          continue;
        }
        if (extHandle != 0 && seenExtHandles.contains(extHandle)) {
          continue;
        }

        const auto mappedId = windowIdForToplevelInfo(platform, info);
        if (!mappedId.has_value() || mappedId->empty()) {
          continue;
        }
        const std::string key = canonicalWindowId(*mappedId);
        if (key.empty()) {
          continue;
        }

        if (wlrHandle != 0) {
          seenWlrHandles.insert(wlrHandle);
        }
        if (extHandle != 0) {
          seenExtHandles.insert(extHandle);
        }
        out[key] = info;
      }
    }
  }

  // Identity keys of every window the switcher can list right now. Compositors reuse
  // window ids (Hyprland reuses addresses), so MRU ranks must expire with the window.
  [[nodiscard]] std::unordered_set<std::string> liveWindowKeys(const CompositorPlatform& platform) {
    std::unordered_set<std::string> keys;
    keys.reserve(32);
    for (const auto& assignment : platform.workspaceWindowAssignments()) {
      if (std::string key = canonicalWindowId(assignment.windowId); !key.empty()) {
        keys.insert(std::move(key));
      }
    }

    std::unordered_map<std::string, ToplevelInfo> liveToplevelById;
    indexLiveToplevelsByWindowId(platform, nullptr, liveToplevelById);
    for (const auto& live : liveToplevelById) {
      keys.insert(live.first);
    }
    return keys;
  }

  [[nodiscard]] std::optional<std::string>
  focusedWindowAssignmentKey(const CompositorPlatform& platform, wl_output* output) {
    const auto focusedId = platform.focusedCompositorWindowId();
    if (!focusedId.has_value() || focusedId->empty()) {
      return std::nullopt;
    }
    const std::string focusedKey = canonicalWindowId(*focusedId);
    const std::string focusedRaw = focusedKey.empty() ? *focusedId : focusedKey;
    for (const auto& assignment : platform.workspaceWindowAssignments(output)) {
      if (assignment.workspaceKey.empty() || assignment.windowId.empty()) {
        continue;
      }
      const std::string key = canonicalWindowId(assignment.windowId);
      if (key == focusedRaw || (compositors::isHyprland() && compositors::hyprland::windowIdsEqual(key, focusedRaw))) {
        return assignment.workspaceKey;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] switcher_membership::OutputMembership
  buildOutputMembership(const CompositorPlatform& platform, wl_output* output) {
    switcher_membership::OutputMembership membership;
    membership.workspaces = platform.workspaces(output);
    membership.overlayKeys = platform.openOverlayWorkspaceKeys(output);
    membership.assignments = platform.workspaceWindowAssignments(output);
    const bool hasActive =
        std::ranges::any_of(membership.workspaces, [](const Workspace& workspace) { return workspace.active; });
    // Without an active workspace the focus is the only hint of what the user sees.
    if (!hasActive) {
      membership.focusedKey = focusedWindowAssignmentKey(platform, output).value_or(std::string{});
    }
    return membership;
  }

  [[nodiscard]] std::vector<switcher_membership::OutputMembership> collectOutputMemberships(
      const CompositorPlatform& platform, const WaylandConnection& wayland, wl_output* outputFilter
  ) {
    std::vector<switcher_membership::OutputMembership> memberships;
    const auto appendForOutput = [&](wl_output* output) {
      if (output != nullptr) {
        memberships.push_back(buildOutputMembership(platform, output));
      }
    };

    if (outputFilter != nullptr) {
      appendForOutput(outputFilter);
      return memberships;
    }
    memberships.reserve(wayland.outputs().size());
    for (const auto& output : wayland.outputs()) {
      appendForOutput(output.output);
    }
    return memberships;
  }

  [[nodiscard]] std::vector<WorkspaceWindowAssignment> collectSwitcherAssignments(
      const CompositorPlatform& platform, const WaylandConnection& wayland, wl_output* outputFilter,
      bool currentWorkspaceOnly, bool& membershipFilterApplied
  ) {
    membershipFilterApplied = false;
    if (!currentWorkspaceOnly) {
      return platform.workspaceWindowAssignments(outputFilter);
    }

    const std::vector<switcher_membership::OutputMembership> memberships =
        collectOutputMemberships(platform, wayland, outputFilter);
    // No output reports assignments: turning the filter on would then hide every window,
    // since the switcher drops the toplevels it has no assignment for. Better unfiltered.
    if (!switcher_membership::hasWorkspaceMembershipData(memberships)) {
      return platform.workspaceWindowAssignments(outputFilter);
    }

    membershipFilterApplied = true;
    return switcher_membership::assignmentsOnVisibleWorkspaces(memberships);
  }

  void buildWindowEntries(
      const CompositorPlatform& platform, const WaylandConnection& wayland, IconResolver& iconResolver, int iconSize,
      wl_output* outputFilter, std::vector<WindowSwitcherEntry>& out, const std::optional<std::string>& focusedId,
      const std::deque<std::string>* mruKeys, bool currentWorkspaceOnly
  ) {
    bool membershipFilterApplied = false;
    const std::vector<WorkspaceWindowAssignment> assignments =
        collectSwitcherAssignments(platform, wayland, outputFilter, currentWorkspaceOnly, membershipFilterApplied);

    std::unordered_map<std::string, WorkspaceWindowAssignment> assignmentById;
    assignmentById.reserve(assignments.size());
    for (const auto& assignment : assignments) {
      const std::string key = canonicalWindowId(assignment.windowId);
      if (key.empty()) {
        continue;
      }
      assignmentById.try_emplace(key, assignment);
    }

    std::unordered_map<std::string, ToplevelInfo> liveToplevelById;
    indexLiveToplevelsByWindowId(platform, outputFilter, liveToplevelById);

    std::unordered_set<std::string> seenKeys;
    std::vector<WindowSwitcherCandidate> candidates;
    candidates.reserve(assignmentById.size() + liveToplevelById.size());

    // Empty while MRU ordering is off, which leaves every candidate at rank max.
    std::unordered_map<std::string_view, std::size_t> mruRanks;
    if (mruKeys != nullptr) {
      mruRanks.reserve(mruKeys->size());
      for (std::size_t i = 0; i < mruKeys->size(); ++i) {
        mruRanks.try_emplace((*mruKeys)[i], i);
      }
    }

    auto addCandidate = [&](WindowSwitcherCandidate candidate, const std::string& key) {
      if (key.empty() || seenKeys.contains(key)) {
        return;
      }
      seenKeys.insert(key);
      if (const auto rank = mruRanks.find(key); rank != mruRanks.end()) {
        candidate.mruIndex = rank->second;
      }
      candidates.push_back(std::move(candidate));
    };

    for (const auto& [key, assignment] : assignmentById) {
      WindowSwitcherCandidate candidate;
      candidate.entry = makeEntryFromAssignment(platform, wayland, iconResolver, iconSize, assignment);
      candidate.workspaceKey = assignment.workspaceKey;
      candidate.sortX = assignment.x;
      candidate.sortY = assignment.y;
      if (const auto live = liveToplevelById.find(key); live != liveToplevelById.end()) {
        if (!live->second.title.empty()) {
          candidate.entry.title = live->second.title;
        }
        if (const std::uintptr_t wlrHandle = wlrHandleForToplevel(live->second); wlrHandle != 0) {
          candidate.entry.closeHandle = wlrHandle;
        }
        candidate.entry.captureHandle = extHandleForToplevel(live->second);
        candidate.toplevelOrder = live->second.order;
      }
      addCandidate(std::move(candidate), key);
    }

    for (const auto& [key, info] : liveToplevelById) {
      if (seenKeys.contains(key)) {
        continue;
      }
      if (membershipFilterApplied) {
        continue;
      }
      WindowSwitcherCandidate candidate;
      candidate.entry = makeEntryFromToplevel(platform, iconResolver, iconSize, info.appId, info);
      if (const auto mappedId = windowIdForToplevelInfo(platform, info); mappedId.has_value() && !mappedId->empty()) {
        candidate.entry.windowId = *mappedId;
      }
      candidate.toplevelOrder = info.order;
      addCandidate(std::move(candidate), key);
    }

    std::ranges::stable_sort(candidates, [](const WindowSwitcherCandidate& a, const WindowSwitcherCandidate& b) {
      if (a.mruIndex != b.mruIndex) {
        return a.mruIndex < b.mruIndex;
      }
      if (a.workspaceKey != b.workspaceKey) {
        return a.workspaceKey < b.workspaceKey;
      }
      if (a.sortY != b.sortY) {
        return a.sortY < b.sortY;
      }
      if (a.sortX != b.sortX) {
        return a.sortX < b.sortX;
      }
      if (a.toplevelOrder != b.toplevelOrder) {
        return a.toplevelOrder < b.toplevelOrder;
      }
      const std::string titleA = !a.entry.title.empty() ? a.entry.title : a.entry.appId;
      const std::string titleB = !b.entry.title.empty() ? b.entry.title : b.entry.appId;
      return StringUtils::toLower(titleA) < StringUtils::toLower(titleB);
    });

    out.clear();
    out.reserve(candidates.size());

    std::optional<std::string> focusedKey;
    if (focusedId.has_value()) {
      focusedKey = canonicalWindowId(*focusedId);
      if (focusedKey->empty()) {
        focusedKey = *focusedId;
      }
    }

    if (focusedKey.has_value()) {
      for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        const std::string key = identityKeyForEntry(it->entry);
        if (key == *focusedKey
            || (compositors::isHyprland() && compositors::hyprland::windowIdsEqual(key, *focusedKey))) {
          out.push_back(std::move(it->entry));
          candidates.erase(it);
          break;
        }
      }
    }

    for (auto& candidate : candidates) {
      out.push_back(std::move(candidate.entry));
    }
  }

} // namespace

WindowSwitcher::WindowSwitcher() = default;

WindowSwitcher::~WindowSwitcher() { destroySurface(); }

struct WindowSwitcher::Instance {
  wl_output* output = nullptr;
  float uiLayoutScale = 1.0F;
  std::unique_ptr<LayerSurface> surface;
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  InputArea* input = nullptr;
  InputArea* panel = nullptr;
  Box* panelBackground = nullptr;
  Box* dimmer = nullptr;
  InputArea* strip = nullptr;
  std::vector<WindowSwitcherTile*> tiles;
  Label* counterLabel = nullptr;
  Box* countBackground = nullptr;
  Label* emptyLabel = nullptr;
  InputDispatcher inputDispatcher;
  WindowSwitcherStyleLayout styleLayout;
  AnimationManager::Id carouselAnimId = 0;
  bool contentSyncPending = false;
  bool pointerInside = false;
  bool revealStarted = false;
  ShellConfig::WindowSwitcherStyle style = ShellConfig::WindowSwitcherStyle::Carousel;
  bool showCaption = true;
  bool showCount = true;
  bool showAppIcon = true;
};

void WindowSwitcher::initialize(
    WaylandConnection& wayland, RenderContext* renderContext, CompositorPlatform& platform, ConfigService* config,
    AsyncTextureCache* asyncTextures
) {
  m_wayland = &wayland;
  m_renderContext = renderContext;
  m_platform = &platform;
  m_config = config;
  m_asyncTextures = asyncTextures;
  m_thumbnailCapture = std::make_unique<ToplevelThumbnailCapture>(wayland);
}

void WindowSwitcher::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::windowSwitcher, [this](const std::string& args) -> std::string {
    const std::string token = StringUtils::trim(args);
    if (token == "close" || token == "hide") {
      if (m_active) {
        hide();
      }
      return "ok\n";
    }
    if (m_platform == nullptr) {
      return "error: compositor unavailable\n";
    }
    wl_output* output = m_platform->preferredInteractiveOutput();
    if (output == nullptr && m_wayland != nullptr && !m_wayland->outputs().empty()) {
      output = m_wayland->outputs().front().output;
    }
    if (output == nullptr) {
      return "error: no output available\n";
    }
    show(output);
    return "ok\n";
  });
}

void WindowSwitcher::onConfigReload() {
  if (!m_active) {
    return;
  }
  refreshWindows();
  if (m_instance != nullptr) {
    m_instance->styleLayout = {};
  }
  requestSceneUpdate();
}

void WindowSwitcher::onOutputChange() {
  if (!m_active) {
    m_windows.clear();
    m_selectedIndex = 0;
    destroySurface();
    return;
  }
  if (m_instance != nullptr && m_output != nullptr) {
    const auto* out = findOutput(*m_wayland, m_output);
    if (out == nullptr) {
      hide();
    }
  }
}

bool WindowSwitcher::mruEnabled() const { return m_config != nullptr && m_config->config().shell.windowSwitcher.mru; }

void WindowSwitcher::recordFocusedWindow() {
  if (m_platform == nullptr || !mruEnabled()) {
    return;
  }
  promoteMruKey(currentFocusedWindowKey(*m_platform));
}

void WindowSwitcher::promoteMruKey(const std::string& key) {
  if (key.empty() || m_platform == nullptr) {
    return;
  }

  const std::unordered_set<std::string> live = liveWindowKeys(*m_platform);
  std::erase_if(m_mruKeys, [&](const std::string& existing) { return existing != key && !live.contains(existing); });

  auto it = std::ranges::find(m_mruKeys, key);
  if (it == m_mruKeys.end()) {
    m_mruKeys.insert(m_mruKeys.begin(), key);
    return;
  }
  if (it != m_mruKeys.begin()) {
    std::rotate(m_mruKeys.begin(), it, it + 1);
  }
}

void WindowSwitcher::onToplevelChange() {
  if (!m_active) {
    recordFocusedWindow();
    return;
  }
  const std::size_t previousCount = m_windows.size();
  refreshWindows();
  if (m_instance != nullptr && m_windows.size() != previousCount) {
    m_instance->styleLayout = {};
  } else if (m_instance != nullptr) {
    m_instance->contentSyncPending = true;
  }
  requestSceneUpdate();
}

void WindowSwitcher::show(wl_output* output) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_platform == nullptr || output == nullptr) {
    return;
  }

  const bool wasActive = m_active;
  const bool outputChanged = output != m_output;
  if (!wasActive) {
    recordFocusedWindow();
  }
  m_output = output;
  const auto focusedId = m_platform->focusedCompositorWindowId();
  refreshWindows();

  if (wasActive) {
    cycleSelection(1);
  } else {
    bool focusedListedFirst = false;
    if (focusedId.has_value() && !m_windows.empty()) {
      const std::string frontKey = identityKeyForEntry(m_windows.front());
      const std::string focusedKey = canonicalWindowId(*focusedId);
      const std::string focusedCompare = focusedKey.empty() ? *focusedId : focusedKey;
      focusedListedFirst = frontKey == focusedCompare
          || (compositors::isHyprland() && compositors::hyprland::windowIdsEqual(frontKey, focusedCompare));
    }
    m_selectedIndex = (m_windows.size() > 1 && focusedListedFirst) ? 1 : 0;
  }
  m_active = true;

  ensureSurface();
  if (m_instance == nullptr) {
    hide();
    return;
  }
  requestSceneUpdate();
  if (!wasActive || outputChanged) {
    startThumbnailCaptures();
  }
}

void WindowSwitcher::hide() {
  if (!m_active && m_instance == nullptr) {
    return;
  }

  m_active = false;
  m_output = nullptr;
  m_windows.clear();
  m_selectedIndex = 0;
  cancelThumbnailCaptures();
  destroySurface();
}

void WindowSwitcher::refreshWindows() {
  if (m_platform == nullptr) {
    m_windows.clear();
    return;
  }

  std::optional<std::string> selectedKey;
  std::unordered_map<std::string, std::shared_ptr<const ScreencopyImage>> thumbnails;
  thumbnails.reserve(m_windows.size());
  for (const auto& entry : m_windows) {
    if (entry.thumbnail != nullptr) {
      if (std::string key = identityKeyForEntry(entry); !key.empty()) {
        thumbnails.emplace(std::move(key), entry.thumbnail);
      }
    }
  }
  if (m_active && m_selectedIndex < m_windows.size()) {
    selectedKey = identityKeyForEntry(m_windows[m_selectedIndex]);
    if (selectedKey->empty()) {
      selectedKey.reset();
    }
  }

  const int iconSize = static_cast<int>(std::round((Style::controlHeightLg + Style::spaceLg) * shellUiScale(m_config)));
  const bool allOutputs = m_config == nullptr || m_config->config().shell.windowSwitcher.showAllOutputs;
  const bool currentWorkspaceOnly = m_config != nullptr && m_config->config().shell.windowSwitcher.currentWorkspaceOnly;
  buildWindowEntries(
      *m_platform, *m_wayland, m_iconResolver, iconSize, allOutputs ? nullptr : m_output, m_windows,
      m_platform->focusedCompositorWindowId(), mruEnabled() ? &m_mruKeys : nullptr, currentWorkspaceOnly
  );

  for (auto& entry : m_windows) {
    const auto thumbnail = thumbnails.find(identityKeyForEntry(entry));
    if (thumbnail != thumbnails.end()) {
      entry.thumbnail = thumbnail->second;
    }
  }

  if (selectedKey.has_value()) {
    for (std::size_t i = 0; i < m_windows.size(); ++i) {
      const std::string key = identityKeyForEntry(m_windows[i]);
      if (key == *selectedKey
          || (compositors::isHyprland() && compositors::hyprland::windowIdsEqual(key, *selectedKey))) {
        m_selectedIndex = i;
        return;
      }
    }
  }

  if (m_selectedIndex >= m_windows.size()) {
    m_selectedIndex = m_windows.empty() ? 0 : m_windows.size() - 1;
  }
}

void WindowSwitcher::startThumbnailCaptures() {
  cancelThumbnailCaptures();
  if (!m_active || m_wayland == nullptr || m_thumbnailCapture == nullptr || !m_thumbnailCapture->available()) {
    return;
  }

  std::vector<bool> queued(m_windows.size(), false);
  auto enqueue = [this, &queued](std::size_t index) {
    if (index >= m_windows.size() || queued[index]) {
      return;
    }
    queued[index] = true;
    const WindowSwitcherEntry& entry = m_windows[index];
    const std::string key = identityKeyForEntry(entry);
    if (!key.empty() && entry.captureHandle != 0 && entry.thumbnail == nullptr) {
      m_thumbnailQueue.push_back(ThumbnailRequest{.windowKey = key, .captureHandle = entry.captureHandle});
    }
  };
  if (m_selectedIndex < m_windows.size()) {
    enqueue(m_selectedIndex);
    const std::size_t visibleCount = std::min(kVisibleCards, m_windows.size());
    const std::size_t centerSlot = visibleCount / 2;
    const std::size_t start = (m_selectedIndex + m_windows.size() - centerSlot) % m_windows.size();
    for (std::size_t slot = 0; slot < visibleCount; ++slot) {
      enqueue((start + slot) % m_windows.size());
    }
  }
  for (std::size_t i = 0; i < m_windows.size(); ++i) {
    enqueue(i);
  }
  captureNextThumbnail();
}

void WindowSwitcher::captureNextThumbnail() {
  if (!m_active || m_wayland == nullptr || m_thumbnailCapture == nullptr || m_thumbnailCapture->busy()) {
    return;
  }

  while (!m_thumbnailQueue.empty()) {
    ThumbnailRequest request = std::move(m_thumbnailQueue.front());
    m_thumbnailQueue.pop_front();
    auto* handle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(request.captureHandle);
    bool handleIsLive = false;
    m_wayland->visitExtToplevelHandles([&](ext_foreign_toplevel_handle_v1* live) { handleIsLive |= live == handle; });
    if (!handleIsLive) {
      continue;
    }

    m_thumbnailCapture->capture(
        handle, 640, 400,
        [this, windowKey = std::move(request.windowKey)](std::optional<ScreencopyImage> image, std::string error) {
          if (!error.empty()) {
            kLog.debug("thumbnail capture skipped for {}: {}", windowKey, error);
          } else if (m_active && image.has_value()) {
            auto thumbnail = std::make_shared<ScreencopyImage>(std::move(*image));
            for (auto& entry : m_windows) {
              if (identityKeyForEntry(entry) == windowKey) {
                entry.thumbnail = thumbnail;
              }
            }
            if (m_instance != nullptr) {
              m_instance->contentSyncPending = true;
            }
            requestSceneUpdate();
          }
          captureNextThumbnail();
        }
    );
    return;
  }
}

void WindowSwitcher::cancelThumbnailCaptures() {
  m_thumbnailQueue.clear();
  if (m_thumbnailCapture != nullptr) {
    m_thumbnailCapture->cancelInFlight();
  }
}

void WindowSwitcher::setSelectedIndex(std::size_t index) {
  if (m_windows.empty()) {
    m_selectedIndex = 0;
    return;
  }
  m_selectedIndex = index % m_windows.size();
  prioritizeSelectedThumbnail();
  syncSelection(true);
  requestSceneUpdate();
}

void WindowSwitcher::cycleSelection(int delta) {
  if (m_windows.empty()) {
    return;
  }
  const auto count = m_windows.size();
  const auto next = (static_cast<long>(m_selectedIndex) + delta) % static_cast<long>(count);
  m_selectedIndex =
      next >= 0 ? static_cast<std::size_t>(next) : static_cast<std::size_t>(next + static_cast<long>(count));
  prioritizeSelectedThumbnail();
  syncSelection(true);
  requestSceneUpdate();
}

void WindowSwitcher::navigateList(int delta) { cycleSelection(delta); }

void WindowSwitcher::prioritizeSelectedThumbnail() {
  if (m_selectedIndex >= m_windows.size() || m_windows[m_selectedIndex].thumbnail != nullptr) {
    return;
  }
  const std::string selectedKey = identityKeyForEntry(m_windows[m_selectedIndex]);
  const auto request = std::ranges::find_if(m_thumbnailQueue, [&](const ThumbnailRequest& queued) {
    return queued.windowKey == selectedKey;
  });
  if (request != m_thumbnailQueue.end()) {
    std::rotate(m_thumbnailQueue.begin(), request, request + 1);
  }
}

void WindowSwitcher::activateSelected() {
  if (m_platform == nullptr || m_windows.empty() || m_selectedIndex >= m_windows.size()) {
    return;
  }
  const WindowSwitcherEntry entry = m_windows[m_selectedIndex];
  if (mruEnabled()) {
    promoteMruKey(identityKeyForEntry(entry));
  }
  // Hyprland ignores zwlr_foreign_toplevel_handle_v1.activate while an exclusive
  // keyboard layer-shell surface is mapped (hyprwm/Hyprland#4829). Snapshot the
  // selection, tear the overlay down, then activate on the next loop tick.
  CompositorPlatform* platform = m_platform;
  hide();
  DeferredCall::callLater([platform, entry]() { activateWindowSwitcherEntry(*platform, entry); });
}

void WindowSwitcher::closeWindowAt(std::size_t index) {
  if (!m_active || m_platform == nullptr || index >= m_windows.size()) {
    return;
  }
  const WindowSwitcherEntry& entry = m_windows[index];
  zwlr_foreign_toplevel_handle_v1* handle = nullptr;
  if (entry.closeHandle != 0) {
    handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(entry.closeHandle);
    if (!m_platform->containsWlrToplevelHandle(handle)) {
      handle = nullptr;
    }
  }
  if (handle == nullptr && !entry.windowId.empty()) {
    handle = m_platform->toplevelHandleForCompositorWindowId(entry.windowId);
  }
  if (handle == nullptr) {
    const std::uintptr_t resolved = resolveCloseHandle(*m_platform, entry.windowId, entry.appId, entry.title);
    if (resolved != 0) {
      handle = reinterpret_cast<zwlr_foreign_toplevel_handle_v1*>(resolved);
      if (!m_platform->containsWlrToplevelHandle(handle)) {
        handle = nullptr;
      }
    }
  }
  if (handle != nullptr) {
    m_platform->closeToplevel(handle);
  }

  const std::size_t previousCount = m_windows.size();
  refreshWindows();
  if (m_windows.empty()) {
    hide();
    return;
  }
  if (m_selectedIndex >= m_windows.size()) {
    m_selectedIndex = m_windows.size() - 1;
  }
  if (m_instance != nullptr) {
    if (m_windows.size() != previousCount) {
      m_instance->styleLayout = {};
    } else {
      m_instance->contentSyncPending = true;
    }
  }
  requestSceneUpdate();
}

void WindowSwitcher::requestSceneUpdate() {
  if (m_instance != nullptr && m_instance->surface != nullptr) {
    m_instance->surface->requestLayout();
    m_instance->surface->requestRedraw();
  }
}

void WindowSwitcher::syncSelection(bool animate) {
  if (m_instance == nullptr || m_instance->strip == nullptr || m_instance->surface == nullptr) {
    return;
  }
  if (m_instance->tiles.size() != m_windows.size()) {
    m_instance->styleLayout = {};
    m_instance->contentSyncPending = true;
    return;
  }

  struct CardTransition {
    WindowSwitcherTile* tile = nullptr;
    float fromX = 0.0F;
    float fromY = 0.0F;
    float fromScaleX = 1.0F;
    float fromScaleY = 1.0F;
    float fromOpacity = 1.0F;
    float toX = 0.0F;
    float toY = 0.0F;
    float toScaleX = 1.0F;
    float toScaleY = 1.0F;
    float toOpacity = 1.0F;
    bool hideOnComplete = false;
  };

  const std::vector<WindowSwitcherCardTarget> previousTargets = m_instance->styleLayout.cards;
  const bool interrupted = m_instance->carouselAnimId != 0;
  if (interrupted) {
    m_instance->animations.cancel(m_instance->carouselAnimId);
    m_instance->carouselAnimId = 0;
  }

  const ShellConfig::WindowSwitcherConfig switcherConfig =
      m_config != nullptr ? m_config->config().shell.windowSwitcher : ShellConfig::WindowSwitcherConfig{};
  if (m_instance->surface->width() > 0 && m_instance->surface->height() > 0) {
    m_instance->styleLayout = computeSwitcherLayout(
        static_cast<float>(m_instance->surface->width()), static_cast<float>(m_instance->surface->height()),
        m_instance->uiLayoutScale, m_windows.size(), m_selectedIndex, switcherConfig
    );
    positionPanel(
        *m_instance, static_cast<float>(m_instance->surface->width()), static_cast<float>(m_instance->surface->height())
    );
  }

  Renderer& renderer = m_instance->surface->renderTarget().renderer();
  const std::vector<WindowSwitcherCardTarget>& targets = m_instance->styleLayout.cards;

  std::vector<CardTransition> transitions;
  transitions.reserve(m_instance->tiles.size());
  for (std::size_t windowIndex = 0; windowIndex < m_instance->tiles.size(); ++windowIndex) {
    WindowSwitcherTile* tile = m_instance->tiles[windowIndex];
    if (tile == nullptr) {
      continue;
    }

    const bool wasVisible = tile->visible();
    const float oldVisualW = tile->width() * tile->scaleX();
    const float oldVisualH = tile->height() * tile->scaleY();
    const float oldVisualX = tile->x() + (tile->width() - oldVisualW) * 0.5F;
    const float oldVisualY = tile->y() + (tile->height() - oldVisualH) * 0.5F;
    const float oldOpacity = tile->opacity();
    const WindowSwitcherCardTarget& target = targets[windowIndex];
    const bool wasTargetVisible = windowIndex < previousTargets.size() && previousTargets[windowIndex].visible;

    if (target.visible) {
      tile->bind(
          renderer, m_windows[windowIndex], target.depth, target.showCaption, target.wideCaption, target.iconPlacement,
          target.closePlacement
      );
      tile->setCardSize(target.width, target.height);
      tile->setZIndex(target.zIndex);
      tile->setVisible(true);

      if (!animate) {
        tile->setPosition(target.x, target.y);
        tile->setScale(1.0F);
        tile->setOpacity(target.opacity);
        continue;
      }

      float startVisualW = oldVisualW;
      float startVisualH = oldVisualH;
      float startVisualX = oldVisualX;
      float startVisualY = oldVisualY;
      float startOpacity = oldOpacity;
      if (!wasVisible) {
        startVisualW = target.width * Style::windowSwitcherIncomingCardScale;
        startVisualH = target.height * Style::windowSwitcherIncomingCardScale;
        startVisualX = target.x + target.direction * target.width * Style::windowSwitcherIncomingCardSlide;
        startVisualY = target.y + (target.height - startVisualH) * 0.5F;
        startOpacity = 0.0F;
      } else if (interrupted && wasTargetVisible) {
        // A card that entered during the cancelled step now occupies a stable
        // slot and must not remain transparent through successive repeats.
        startOpacity = target.opacity;
      }

      CardTransition transition;
      transition.tile = tile;
      transition.fromScaleX = target.width > 0.0F ? startVisualW / target.width : 1.0F;
      transition.fromScaleY = target.height > 0.0F ? startVisualH / target.height : 1.0F;
      transition.fromX = startVisualX - (target.width - startVisualW) * 0.5F;
      transition.fromY = startVisualY - (target.height - startVisualH) * 0.5F;
      transition.fromOpacity = startOpacity;
      transition.toX = target.x;
      transition.toY = target.y;
      transition.toOpacity = target.opacity;
      transitions.push_back(transition);
      continue;
    }

    if (!wasVisible || !animate || (interrupted && !wasTargetVisible)) {
      tile->setVisible(false);
      tile->setScale(1.0F);
      continue;
    }

    const float direction = oldVisualX + oldVisualW * 0.5F < m_instance->styleLayout.stripWidth * 0.5F ? -1.0F : 1.0F;
    const float targetVisualW = oldVisualW * Style::windowSwitcherIncomingCardScale;
    const float targetVisualH = oldVisualH * Style::windowSwitcherIncomingCardScale;
    const float targetVisualX = oldVisualX + direction * oldVisualW * Style::windowSwitcherOutgoingCardSlide;
    const float targetVisualY = oldVisualY + (oldVisualH - targetVisualH) * 0.5F;
    tile->setZIndex(0);

    CardTransition transition;
    transition.tile = tile;
    transition.fromX = tile->x();
    transition.fromY = tile->y();
    transition.fromScaleX = tile->scaleX();
    transition.fromScaleY = tile->scaleY();
    transition.fromOpacity = oldOpacity;
    transition.toScaleX = tile->width() > 0.0F ? targetVisualW / tile->width() : 1.0F;
    transition.toScaleY = tile->height() > 0.0F ? targetVisualH / tile->height() : 1.0F;
    transition.toX = targetVisualX - (tile->width() - targetVisualW) * 0.5F;
    transition.toY = targetVisualY - (tile->height() - targetVisualH) * 0.5F;
    transition.toOpacity = 0.0F;
    transition.hideOnComplete = true;
    transitions.push_back(transition);
  }

  m_instance->contentSyncPending = false;
  if (m_instance->emptyLabel != nullptr) {
    m_instance->emptyLabel->setVisible(m_windows.empty());
  }
  if (m_instance->counterLabel != nullptr) {
    if (m_windows.empty()) {
      m_instance->counterLabel->setText(i18n::trp("window-switcher.count", 0));
    } else {
      m_instance->counterLabel->setText(std::to_string(m_selectedIndex + 1) + " / " + std::to_string(m_windows.size()));
    }
  }

  if (!animate || transitions.empty()) {
    return;
  }

  for (const CardTransition& transition : transitions) {
    transition.tile->setPosition(transition.fromX, transition.fromY);
    transition.tile->setScale(transition.fromScaleX, transition.fromScaleY);
    transition.tile->setOpacity(transition.fromOpacity);
  }

  Instance* instance = m_instance;
  InputArea* animationOwner = instance->strip;
  instance->carouselAnimId = instance->animations.animate(
      0.0F, 1.0F, Style::animNormal, Easing::EaseOutCubic,
      [instance, transitions](float progress) {
        for (const CardTransition& transition : transitions) {
          transition.tile->setPosition(
              std::lerp(transition.fromX, transition.toX, progress),
              std::lerp(transition.fromY, transition.toY, progress)
          );
          transition.tile->setScale(
              std::lerp(transition.fromScaleX, transition.toScaleX, progress),
              std::lerp(transition.fromScaleY, transition.toScaleY, progress)
          );
          transition.tile->setOpacity(std::lerp(transition.fromOpacity, transition.toOpacity, progress));
        }
        if (instance->pointerInside) {
          instance->inputDispatcher.syncPointerHover();
        }
      },
      [this, instance, transitions]() {
        if (m_instance != instance) {
          return;
        }
        instance->carouselAnimId = 0;
        for (const CardTransition& transition : transitions) {
          if (transition.hideOnComplete) {
            transition.tile->setVisible(false);
            transition.tile->setScale(1.0F);
          }
        }
        if (instance->pointerInside) {
          instance->inputDispatcher.syncPointerHover();
        }
        if (instance->contentSyncPending) {
          requestSceneUpdate();
        }
      },
      animationOwner
  );
}

bool WindowSwitcher::matchesTrigger(const KeyboardEvent& event) const noexcept {
  if (!event.pressed || event.preedit) {
    return false;
  }
  if (m_config == nullptr) {
    return false;
  }
  if ((event.modifiers & KeyMod::Alt) == 0 || (event.modifiers & KeyMod::Super) != 0) {
    return false;
  }

  const std::uint32_t normalizedModifiers = event.modifiers & ~(KeyMod::Alt | KeyMod::Super);
  return m_config->matchesKeybind(KeybindAction::TabNext, event.sym, normalizedModifiers)
      || m_config->matchesKeybind(KeybindAction::TabPrevious, event.sym, normalizedModifiers);
}

bool WindowSwitcher::isModifierRelease(const KeyboardEvent& event) const noexcept {
  return !event.pressed && (isAltModifier(event.sym) || event.sym == XKB_KEY_Super_L || event.sym == XKB_KEY_Super_R);
}

bool WindowSwitcher::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_active && m_instance == nullptr) {
    hide();
    return false;
  }

  if (!m_active) {
    if (matchesTrigger(event)) {
      if (m_platform == nullptr) {
        return false;
      }
      wl_output* output = m_platform->preferredInteractiveOutput();
      if (output == nullptr && m_wayland != nullptr && !m_wayland->outputs().empty()) {
        output = m_wayland->outputs().front().output;
      }
      if (output == nullptr) {
        return false;
      }
      show(output);
      return true;
    }
    return false;
  }

  if (isModifierRelease(event)) {
    activateSelected();
    hide();
    return true;
  }

  if (!event.pressed || event.preedit) {
    return true;
  }

  const std::uint32_t normalizedModifiers = event.modifiers & ~(KeyMod::Alt | KeyMod::Super);
  auto matchesAction = [&](KeybindAction action) {
    if (m_config != nullptr) {
      return m_config->matchesKeybind(action, event.sym, normalizedModifiers);
    }
    return KeybindMatcher::matches(action, event.sym, normalizedModifiers);
  };

  if (matchesAction(KeybindAction::Cancel)) {
    hide();
    return true;
  }

  if (matchesAction(KeybindAction::TabPrevious)) {
    cycleSelection(-1);
    return true;
  }

  if (matchesAction(KeybindAction::TabNext)) {
    cycleSelection(1);
    return true;
  }

  if (matchesAction(KeybindAction::Validate)) {
    activateSelected();
    hide();
    return true;
  }

  if (matchesAction(KeybindAction::Left)) {
    navigateList(-1);
    return true;
  }
  if (matchesAction(KeybindAction::Right)) {
    navigateList(1);
    return true;
  }
  if (matchesAction(KeybindAction::Up)) {
    navigateList(-1);
    return true;
  }
  if (matchesAction(KeybindAction::Down)) {
    navigateList(1);
    return true;
  }

  return true;
}

bool WindowSwitcher::onPointerEvent(const PointerEvent& event) {
  if (!m_active || m_instance == nullptr || m_instance->surface == nullptr) {
    return false;
  }

  Instance* target = m_instance;
  const bool onTarget =
      event.surface != nullptr && target->surface != nullptr && event.surface == target->surface->wlSurface();

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onTarget) {
      target->pointerInside = true;
      target->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    return onTarget;
  case PointerEvent::Type::Leave:
    if (onTarget || target->pointerInside) {
      target->pointerInside = false;
      target->inputDispatcher.pointerLeave();
    }
    return onTarget || target->pointerInside;
  case PointerEvent::Type::Motion:
    if (onTarget) {
      target->pointerInside = true;
    }
    if (onTarget || target->pointerInside) {
      target->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
      return true;
    }
    return false;
  case PointerEvent::Type::Button: {
    const bool pressed = event.pressed;
    if (onTarget) {
      target->pointerInside = true;
    }
    if (!onTarget && !target->pointerInside) {
      if (pressed) {
        hide();
        return true;
      }
      return false;
    }
    if (pressed
        && onTarget
        && (target->inputDispatcher.hoveredArea() == nullptr
            || target->inputDispatcher.hoveredArea() == target->input)) {
      hide();
      return true;
    }
    return target->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed, event.serial, event.time,
        event.touch
    );
  }
  case PointerEvent::Type::Axis:
    if (onTarget || target->pointerInside) {
      return target->inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines
      );
    }
    return false;
  }

  return false;
}

void WindowSwitcher::ensureSurface() {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_output == nullptr) {
    return;
  }
  const auto* output = findOutput(*m_wayland, m_output);
  if (output == nullptr || !output->hasUsableGeometry()) {
    return;
  }

  if (m_instance != nullptr && m_instance->output == m_output && m_instance->surface != nullptr) {
    return;
  }

  destroySurface();

  auto inst = std::make_unique<Instance>();
  inst->output = m_output;
  inst->uiLayoutScale = shellUiScale(m_config);

  auto config = LayerSurfaceConfig{
      .nameSpace = "noctalia-window-switcher",
      .layer = LayerShellLayer::Overlay,
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
      .width = 0,
      .height = 0,
      .exclusiveZone = -1,
      .keyboard = LayerShellKeyboard::Exclusive,
      .defaultWidth = static_cast<std::uint32_t>(output->effectiveLogicalWidth()),
      .defaultHeight = static_cast<std::uint32_t>(output->effectiveLogicalHeight()),
  };

  inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(config));
  inst->surface->setRenderContext(m_renderContext);
  inst->surface->setAnimationManager(&inst->animations);

  auto* instPtr = inst.get();
  inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    if (instPtr->surface != nullptr) {
      instPtr->surface->requestLayout();
    }
  });
  inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
    if (!m_active || m_instance != instPtr) {
      return;
    }
    prepareFrame(*instPtr, needsUpdate, needsLayout);
  });
  inst->surface->setFrameTickCallback([this, instPtr](float /*deltaMs*/) {
    if (!m_active || m_instance != instPtr || instPtr->surface == nullptr) {
      return;
    }
    positionPanel(
        *instPtr, static_cast<float>(instPtr->surface->width()), static_cast<float>(instPtr->surface->height())
    );
  });
  inst->surface->setClosedCallback([this]() { DeferredCall::callLater([this]() { hide(); }); });

  if (!inst->surface->initialize(m_output)) {
    kLog.warn("failed to initialize window switcher overlay");
    return;
  }

  m_instance = inst.release();
}

void WindowSwitcher::destroySurface() {
  if (m_instance == nullptr) {
    return;
  }

  m_instance->inputDispatcher.setSceneRoot(nullptr);
  m_instance->animations.cancelAll();
  if (m_instance->surface != nullptr) {
    m_instance->surface->setClosedCallback(nullptr);
    m_instance->surface->setPrepareFrameCallback(nullptr);
    m_instance->surface->setFrameTickCallback(nullptr);
    m_instance->surface->setConfigureCallback(nullptr);
    m_instance->surface->setSceneRoot(nullptr);
  }
  delete m_instance;
  m_instance = nullptr;
}

void WindowSwitcher::prepareFrame(Instance& instance, bool /*needsUpdate*/, bool /*needsLayout*/) {
  if (!m_active || m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  const auto width = instance.surface->width();
  const auto height = instance.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const ShellConfig::WindowSwitcherConfig switcherConfig =
      m_config != nullptr ? m_config->config().shell.windowSwitcher : ShellConfig::WindowSwitcherConfig{};
  const auto layout = computeSwitcherLayout(
      static_cast<float>(width), static_cast<float>(height), instance.uiLayoutScale, m_windows.size(), m_selectedIndex,
      switcherConfig
  );
  const bool needsSceneBuild = instance.sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(instance.sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(instance.sceneRoot->height())) != height
      || instance.tiles.size() != m_windows.size()
      || instance.style != switcherConfig.style
      || instance.showCaption != switcherConfig.showCaption
      || instance.showCount != switcherConfig.showCount
      || instance.showAppIcon != switcherConfig.showAppIcon;
  if (needsSceneBuild) {
    instance.styleLayout = layout;
    buildScene(instance, width, height);
  } else {
    const bool layoutChanged = !instance.styleLayout.sameGeometryAs(layout);
    instance.styleLayout = layout;
    if ((instance.contentSyncPending || layoutChanged) && instance.carouselAnimId == 0) {
      syncSelection(layoutChanged);
    }
    if (instance.sceneRoot != nullptr && instance.sceneRoot->layoutDirty()) {
      instance.sceneRoot->layout(renderer);
    }
    positionPanel(instance, static_cast<float>(width), static_cast<float>(height));
  }
  if (instance.pointerInside) {
    instance.inputDispatcher.syncPointerHover();
  }
}

void WindowSwitcher::positionPanel(Instance& instance, float screenW, float screenH) {
  if (instance.panel == nullptr || instance.strip == nullptr) {
    return;
  }
  const WindowSwitcherStyleLayout& layout = instance.styleLayout;
  const float panelX = std::round((screenW - layout.panelWidth) * 0.5F);
  const float panelY = std::round((screenH - layout.panelHeight) * 0.5F);
  instance.panel->setPosition(panelX, panelY);
  instance.panel->setFrameSize(layout.panelWidth, layout.panelHeight);
  if (instance.panelBackground != nullptr) {
    instance.panelBackground->setVisible(layout.boxed);
    instance.panelBackground->setPosition(0.0F, 0.0F);
    instance.panelBackground->setFrameSize(layout.panelWidth, layout.panelHeight);
  }
  instance.strip->setPosition(layout.stripX, layout.stripY);
  instance.strip->setFrameSize(layout.stripWidth, layout.stripHeight);
  if (instance.countBackground != nullptr) {
    instance.countBackground->setPosition(layout.countX, layout.countY);
    instance.countBackground->setFrameSize(layout.countWidth, layout.countHeight);
  }
  if (instance.counterLabel != nullptr) {
    instance.counterLabel->setPosition(
        (layout.countWidth - instance.counterLabel->width()) * 0.5F,
        (layout.countHeight - instance.counterLabel->height()) * 0.5F
    );
  }
  if (instance.emptyLabel != nullptr) {
    instance.emptyLabel->setPosition(
        layout.stripX + (layout.stripWidth - instance.emptyLabel->width()) * 0.5F,
        layout.stripY + (layout.stripHeight - instance.emptyLabel->height()) * 0.5F
    );
  }

  std::vector<InputRect> blurRects;
  if (layout.boxed && instance.panelBackground != nullptr) {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(instance.panelBackground, left, top, right, bottom);
    const int blurX = static_cast<int>(std::floor(left));
    const int blurY = static_cast<int>(std::floor(top));
    const int blurW = std::max(1, static_cast<int>(std::ceil(right) - std::floor(left)));
    const int blurH = std::max(1, static_cast<int>(std::ceil(bottom) - std::floor(top)));
    auto strips =
        Surface::tessellateRoundedRect(blurX, blurY, blurW, blurH, Style::scaledRadiusXl(instance.uiLayoutScale));
    blurRects.insert(blurRects.end(), strips.begin(), strips.end());
  }
  for (const WindowSwitcherTile* tile : instance.tiles) {
    if (tile == nullptr || !tile->visible() || tile->opacity() <= kVisibleOpacityThreshold) {
      continue;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(tile, left, top, right, bottom);
    const int blurX = static_cast<int>(std::floor(left));
    const int blurY = static_cast<int>(std::floor(top));
    const int blurW = std::max(1, static_cast<int>(std::ceil(right) - std::floor(left)));
    const int blurH = std::max(1, static_cast<int>(std::ceil(bottom) - std::floor(top)));
    auto strips =
        Surface::tessellateRoundedRect(blurX, blurY, blurW, blurH, Style::scaledRadiusXl(instance.uiLayoutScale));
    blurRects.insert(blurRects.end(), strips.begin(), strips.end());
  }
  if (instance.countBackground != nullptr) {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(instance.countBackground, left, top, right, bottom);
    const int blurX = static_cast<int>(std::floor(left));
    const int blurY = static_cast<int>(std::floor(top));
    const int blurW = std::max(1, static_cast<int>(std::ceil(right) - std::floor(left)));
    const int blurH = std::max(1, static_cast<int>(std::ceil(bottom) - std::floor(top)));
    auto strips =
        Surface::tessellateRoundedRect(blurX, blurY, blurW, blurH, Style::scaledRadiusMd(instance.uiLayoutScale));
    blurRects.insert(blurRects.end(), strips.begin(), strips.end());
  }
  instance.surface->setBlurRegion(blurRects);
}

void WindowSwitcher::buildScene(Instance& instance, std::uint32_t width, std::uint32_t height) {
  UiPhaseScope layoutPhase(UiPhase::Layout);

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const float scale = instance.uiLayoutScale;

  Renderer& renderer = instance.surface->renderTarget().renderer();
  instance.sceneRoot = ui::node({});
  instance.sceneRoot->setSize(w, h);

  auto input = ui::inputArea({});
  input->setFrameSize(w, h);
  input->setFocusable(true);
  input->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
  input->setOnKeyDown([this](const InputArea::KeyData& key) {
    KeyboardEvent event{
        .sym = key.sym,
        .utf32 = key.utf32,
        .modifiers = key.modifiers,
        .pressed = key.pressed,
        .preedit = key.preedit,
    };
    (void)onKeyboardEvent(event);
  });

  const WindowSwitcherStyleLayout& layout = instance.styleLayout;

  input->addChild(
      ui::box({
          .out = &instance.dimmer,
          .fill = colorSpecFromRole(ColorRole::Shadow),
          .width = w,
          .height = h,
          .opacity = Style::windowSwitcherDimOpacity,
          .participatesInLayout = false,
      })
  );

  auto panel = ui::inputArea({
      .out = &instance.panel,
      .acceptedButtons = InputArea::buttonMask(BTN_LEFT),
      .frameWidth = layout.panelWidth,
      .frameHeight = layout.panelHeight,
      .participatesInLayout = false,
  });
  panel->addChild(
      ui::box({
          .out = &instance.panelBackground,
          .fill = colorSpecFromRole(ColorRole::Surface),
          .border = scaleAlpha(colorSpecFromRole(ColorRole::Outline), Style::disabledOutlineAlpha),
          .borderWidth = Style::borderWidth,
          .radius = Style::scaledRadiusXl(scale),
          .visible = layout.boxed,
          .participatesInLayout = false,
      })
  );
  std::optional<ColorSpec> iconTint;
  if (m_config != nullptr) {
    iconTint = effectiveShellAppIconColorizationTint(m_config->config().shell);
  }
  const ShellConfig::ShadowConfig shadowConfig =
      m_config != nullptr ? m_config->config().shell.shadow : ShellConfig::ShadowConfig{};
  const float cardRadius = Style::scaledRadiusXl(scale);
  const RoundedRectStyle selectedShadowStyle = shell::surface_shadow::style(
      shadowConfig, 1.0F, shell::surface_shadow::Shape{.radius = Radii{cardRadius, cardRadius, cardRadius, cardRadius}}
  );
  auto strip = ui::inputArea({
      .out = &instance.strip,
      .frameWidth = layout.stripWidth,
      .frameHeight = layout.stripHeight,
      .participatesInLayout = false,
  });
  instance.tiles.clear();
  instance.counterLabel = nullptr;
  instance.countBackground = nullptr;
  instance.carouselAnimId = 0;
  const ShellConfig::WindowSwitcherConfig switcherConfig =
      m_config != nullptr ? m_config->config().shell.windowSwitcher : ShellConfig::WindowSwitcherConfig{};
  instance.style = switcherConfig.style;
  instance.showCaption = switcherConfig.showCaption;
  instance.showCount = switcherConfig.showCount;
  instance.showAppIcon = switcherConfig.showAppIcon;
  if (!m_windows.empty()) {
    for (std::size_t windowIndex = 0; windowIndex < m_windows.size(); ++windowIndex) {
      auto tile = std::make_unique<WindowSwitcherTile>(scale, m_asyncTextures);
      WindowSwitcherTile* tilePtr = tile.get();
      const WindowSwitcherCardTarget& target = layout.cards[windowIndex];
      tile->setCardSize(target.width, target.height);
      tile->setVisible(false);
      tile->setAppIconColorizeTint(iconTint);
      tile->setShadowStyle(selectedShadowStyle);
      tile->setShowCaption(switcherConfig.showCaption);
      tile->setShowAppIcon(switcherConfig.showAppIcon);
      tile->setOnInvalidate([this]() { requestSceneUpdate(); });
      tile->setOnActivate([this, windowIndex]() {
        setSelectedIndex(windowIndex);
        activateSelected();
      });
      tile->setOnClose([this, windowIndex]() { closeWindowAt(windowIndex); });
      instance.tiles.push_back(tilePtr);
      strip->addChild(std::move(tile));
    }
  }
  panel->addChild(std::move(strip));

  if (switcherConfig.showCount) {
    auto countBackground = ui::box({
        .out = &instance.countBackground,
        .fill = colorSpecFromRole(ColorRole::Surface),
        .border = scaleAlpha(colorSpecFromRole(ColorRole::Outline), Style::disabledOutlineAlpha),
        .borderWidth = Style::borderWidth,
        .radius = Style::scaledRadiusMd(scale),
        .width = layout.countWidth,
        .height = layout.countHeight,
        .participatesInLayout = false,
    });
    countBackground->addChild(
        ui::label({
            .out = &instance.counterLabel,
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::Medium,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .participatesInLayout = false,
        })
    );
    panel->addChild(std::move(countBackground));
  }

  panel->addChild(
      ui::label({
          .out = &instance.emptyLabel,
          .text = i18n::tr("window-switcher.empty"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .textAlign = TextAlign::Center,
          .visible = m_windows.empty(),
          .participatesInLayout = false,
      })
  );

  input->addChild(std::move(panel));

  instance.input = input.get();
  instance.sceneRoot->addChild(std::move(input));
  syncSelection(false);
  instance.sceneRoot->layout(renderer);
  positionPanel(instance, w, h);

  instance.surface->setSceneRoot(instance.sceneRoot.get());
  instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
  instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    if (m_wayland != nullptr) {
      m_wayland->setCursorShape(serial, shape);
    }
  });
  if (instance.input != nullptr) {
    instance.inputDispatcher.setFocus(instance.input);
  }

  if (!instance.revealStarted && instance.panel != nullptr && instance.dimmer != nullptr) {
    instance.revealStarted = true;
    instance.panel->setOpacity(0.0F);
    instance.panel->setScale(Style::windowSwitcherRevealScale);
    instance.dimmer->setOpacity(0.0F);
    instance.animations.animate(
        0.0F, 1.0F, Style::animNormal, Easing::EaseOutCubic,
        [panelNode = instance.panel, dimmer = instance.dimmer](float value) {
          panelNode->setOpacity(value);
          panelNode->setScale(Style::windowSwitcherRevealScale + (1.0F - Style::windowSwitcherRevealScale) * value);
          dimmer->setOpacity(Style::windowSwitcherDimOpacity * value);
        },
        {}, instance.panel
    );
  }
}
