#include "shell/switcher/window_switcher.h"

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
#include "shell/switcher/window_switcher_tile.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "system/internal_app_metadata.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("window-switcher");
  constexpr std::size_t kGridColumns = 5;
  constexpr float kDimOpacity = 0.62f;
  constexpr float kMinCellWidth = 164.0f;
  constexpr float kMaxCellWidth = 224.0f;
  constexpr float kWindowPreviewAspect = 16.0f / 10.0f;
  constexpr float kCaptionBlock = 48.0f;

  struct SwitcherGridMetrics {
    std::size_t columns = 1;
    float cellW = 0.0f;
    float cellH = 0.0f;
    float gridW = 0.0f;
    float gridH = 0.0f;
    float colGap = 0.0f;
    float rowGap = 0.0f;

    [[nodiscard]] bool sameLayoutAs(const SwitcherGridMetrics& other) const noexcept {
      return columns == other.columns && std::abs(cellW - other.cellW) < 0.5f && std::abs(cellH - other.cellH) < 0.5f;
    }
  };

  [[nodiscard]] SwitcherGridMetrics
  computeSwitcherGridMetrics(float screenW, float screenH, float scale, std::size_t itemCount) {
    SwitcherGridMetrics metrics;
    metrics.colGap = Style::spaceMd * scale;
    metrics.rowGap = Style::spaceMd * scale;

    metrics.columns = itemCount == 0 ? 1 : std::min(kGridColumns, itemCount);
    const std::size_t rows = itemCount == 0 ? 1 : (itemCount + metrics.columns - 1) / metrics.columns;

    const float sidePad = Style::spaceLg * scale * 2.0f;
    const float minCellW = kMinCellWidth * scale;
    const float maxCellW = kMaxCellWidth * scale;
    const float captionBlock = kCaptionBlock * scale + Style::spaceSm * scale * 2.0f;

    const float availableW = std::max(0.0f, screenW - sidePad);
    float cellW = metrics.columns == 0
        ? minCellW
        : (availableW - metrics.colGap * static_cast<float>(metrics.columns - 1)) / static_cast<float>(metrics.columns);
    cellW = std::clamp(cellW, minCellW, maxCellW);

    auto cellHeightForWidth = [captionBlock](float width) { return width / kWindowPreviewAspect + captionBlock; };

    float cellH = cellHeightForWidth(cellW);
    float gridH = cellH * static_cast<float>(rows) + metrics.rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0);
    const float maxGridH = std::max(0.0f, screenH - sidePad);
    if (gridH > maxGridH && rows > 0) {
      cellH = (maxGridH - metrics.rowGap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
      cellW = std::min(cellW, (cellH - captionBlock) * kWindowPreviewAspect);
      cellW = std::max(minCellW * 0.88f, cellW);
      cellH = cellHeightForWidth(cellW);
      gridH = cellH * static_cast<float>(rows) + metrics.rowGap * static_cast<float>(rows > 0 ? rows - 1 : 0);
    }

    metrics.cellW = cellW;
    metrics.cellH = cellH;
    metrics.gridW =
        cellW * static_cast<float>(metrics.columns) + metrics.colGap * static_cast<float>(metrics.columns - 1);
    metrics.gridH = gridH;
    return metrics;
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
    return config != nullptr ? config->config().accessibility.uiScale : 1.0f;
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
    platform.focusCompositorWindow(entry.windowId);
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

  [[nodiscard]] WindowSwitcherEntry makeEntryFromToplevel(
      const CompositorPlatform& platform, IconResolver& iconResolver, int iconSize, const std::string& appId,
      const ToplevelInfo& info
  ) {
    WindowSwitcherEntry entry;
    if (const auto windowId = platform.compositorWindowIdForToplevelInfo(info); windowId.has_value()) {
      entry.windowId = *windowId;
    } else if (info.handle != nullptr) {
      entry.windowId = "toplevel:" + std::to_string(reinterpret_cast<std::uintptr_t>(info.handle));
    }
    entry.closeHandle = wlrHandleForToplevel(info);
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
  };

  [[nodiscard]] WindowSwitcherEntry makeEntryFromAssignment(
      const CompositorPlatform& platform, IconResolver& iconResolver, int iconSize,
      const WorkspaceWindowAssignment& assignment
  ) {
    WindowSwitcherEntry entry;
    entry.windowId = assignment.windowId;
    entry.appId = assignment.appId;
    entry.appLabel = resolveWindowAppLabel(entry.appId);
    entry.title = !assignment.title.empty() ? assignment.title : entry.appLabel;
    entry.iconPath = resolveWindowIconPath(entry.appId, iconResolver, iconSize);
    entry.closeHandle = resolveCloseHandle(platform, entry.windowId, entry.appId, entry.title);
    return entry;
  }

  void
  indexLiveToplevelsByWindowId(const CompositorPlatform& platform, std::unordered_map<std::string, ToplevelInfo>& out) {
    std::unordered_set<std::uintptr_t> seenWlrHandles;
    std::unordered_set<std::uintptr_t> seenExtHandles;

    for (const auto& appId : platform.runningAppIds()) {
      const std::string lower = StringUtils::toLower(appId);
      for (const auto& info : platform.windowsForApp(lower, lower)) {
        const std::uintptr_t wlrHandle = wlrHandleForToplevel(info);
        const std::uintptr_t extHandle = extHandleForToplevel(info);
        if (wlrHandle != 0 && seenWlrHandles.contains(wlrHandle)) {
          continue;
        }
        if (extHandle != 0 && seenExtHandles.contains(extHandle)) {
          continue;
        }

        const auto mappedId = platform.compositorWindowIdForToplevelInfo(info);
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

  void buildWindowEntries(
      const CompositorPlatform& platform, IconResolver& iconResolver, int iconSize,
      std::vector<WindowSwitcherEntry>& out, const std::optional<std::string>& focusedId
  ) {
    std::unordered_map<std::string, WorkspaceWindowAssignment> assignmentById;
    assignmentById.reserve(32);
    for (const auto& assignment : platform.workspaceWindowAssignments()) {
      if (assignment.windowId.empty()) {
        continue;
      }
      const std::string key = canonicalWindowId(assignment.windowId);
      if (key.empty()) {
        continue;
      }
      assignmentById[key] = assignment;
    }

    std::unordered_map<std::string, ToplevelInfo> liveToplevelById;
    indexLiveToplevelsByWindowId(platform, liveToplevelById);

    std::unordered_set<std::string> seenKeys;
    std::vector<WindowSwitcherCandidate> candidates;
    candidates.reserve(assignmentById.size() + liveToplevelById.size());

    auto addCandidate = [&](WindowSwitcherCandidate candidate, const std::string& key) {
      if (key.empty() || seenKeys.contains(key)) {
        return;
      }
      seenKeys.insert(key);
      candidates.push_back(std::move(candidate));
    };

    for (const auto& [key, assignment] : assignmentById) {
      WindowSwitcherCandidate candidate;
      candidate.entry = makeEntryFromAssignment(platform, iconResolver, iconSize, assignment);
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
        candidate.toplevelOrder = live->second.order;
      }
      addCandidate(std::move(candidate), key);
    }

    for (const auto& [key, info] : liveToplevelById) {
      if (seenKeys.contains(key)) {
        continue;
      }
      WindowSwitcherCandidate candidate;
      candidate.entry = makeEntryFromToplevel(platform, iconResolver, iconSize, info.appId, info);
      if (const auto mappedId = platform.compositorWindowIdForToplevelInfo(info);
          mappedId.has_value() && !mappedId->empty()) {
        candidate.entry.windowId = *mappedId;
      }
      candidate.toplevelOrder = info.order;
      addCandidate(std::move(candidate), key);
    }

    std::ranges::stable_sort(candidates, [](const WindowSwitcherCandidate& a, const WindowSwitcherCandidate& b) {
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

  class WindowSwitcherGridAdapter final : public VirtualGridAdapter {
  public:
    WindowSwitcherGridAdapter(float scale, AsyncTextureCache* cache, std::optional<ColorSpec> iconTint)
        : m_scale(scale), m_cache(cache), m_iconTint(iconTint) {}

    void setEntries(const std::vector<WindowSwitcherEntry>* entries) { m_entries = entries; }
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }
    void setOnClose(std::function<void(std::size_t)> callback) { m_onClose = std::move(callback); }
    void setOnInvalidate(std::function<void()> callback) { m_onInvalidate = std::move(callback); }

    [[nodiscard]] std::size_t itemCount() const override { return m_entries == nullptr ? 0U : m_entries->size(); }

    [[nodiscard]] std::unique_ptr<Node> createTile() override {
      std::unique_ptr<WindowSwitcherTile> tile = std::make_unique<WindowSwitcherTile>(m_scale, m_cache);
      tile->setAppIconColorizeTint(m_iconTint);
      tile->setOnInvalidate(m_onInvalidate);
      return tile;
    }

    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
      if (m_renderer == nullptr || m_entries == nullptr || index >= m_entries->size()) {
        return;
      }
      auto* windowTile = dynamic_cast<WindowSwitcherTile*>(&tile);
      if (windowTile == nullptr) {
        return;
      }
      windowTile->setCellSize(windowTile->width(), windowTile->height());
      windowTile->bind(*m_renderer, (*m_entries)[index], selected, hovered);
    }

    void onActivate(std::size_t index) override {
      if (m_onActivate) {
        m_onActivate(index);
      }
    }

    bool
    onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
      if (!WindowSwitcherTile::hitTestCloseRegion(cellWidth, cellHeight, m_scale, cellLocalX, cellLocalY)) {
        return false;
      }
      if (m_onClose) {
        m_onClose(index);
      }
      return true;
    }

    [[nodiscard]] bool overlayHitTest(
        std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight
    ) const override {
      (void)index;
      return WindowSwitcherTile::hitTestCloseRegion(cellWidth, cellHeight, m_scale, cellLocalX, cellLocalY);
    }

    void applyOverlayHover(Node& tile, bool hovered) override {
      static_cast<WindowSwitcherTile&>(tile).setCloseHovered(hovered);
    }

  private:
    float m_scale = 1.0f;
    AsyncTextureCache* m_cache = nullptr;
    std::optional<ColorSpec> m_iconTint;
    Renderer* m_renderer = nullptr;
    const std::vector<WindowSwitcherEntry>* m_entries = nullptr;
    std::function<void(std::size_t)> m_onActivate;
    std::function<void(std::size_t)> m_onClose;
    std::function<void()> m_onInvalidate;
  };

} // namespace

WindowSwitcher::~WindowSwitcher() { destroySurface(); }

struct WindowSwitcher::Instance {
  wl_output* output = nullptr;
  float uiLayoutScale = 1.0f;
  std::unique_ptr<LayerSurface> surface;
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  InputArea* input = nullptr;
  VirtualGridView* grid = nullptr;
  Label* emptyLabel = nullptr;
  InputDispatcher inputDispatcher;
  std::unique_ptr<WindowSwitcherGridAdapter> adapter;
  SwitcherGridMetrics gridMetrics;
  bool pointerInside = false;
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
}

void WindowSwitcher::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "window-switcher",
      [this](const std::string& args) -> std::string {
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
      },
      "window-switcher [close]", "Open or close the window switcher overlay"
  );
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

void WindowSwitcher::onToplevelChange() {
  if (!m_active) {
    return;
  }
  const std::size_t previousCount = m_windows.size();
  refreshWindows();
  if (m_instance != nullptr && m_windows.size() != previousCount) {
    m_instance->gridMetrics = {};
  }
  requestSceneUpdate();
}

void WindowSwitcher::show(wl_output* output) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_platform == nullptr || output == nullptr) {
    return;
  }

  const bool wasActive = m_active;
  refreshWindows();

  m_output = output;
  if (wasActive) {
    cycleSelection(1);
  } else {
    m_selectedIndex = m_windows.size() > 1 ? 1 : 0;
  }
  m_active = true;

  ensureSurface();
  if (m_instance == nullptr) {
    hide();
    return;
  }
  requestSceneUpdate();
}

void WindowSwitcher::hide() {
  if (!m_active && m_instance == nullptr) {
    return;
  }

  m_active = false;
  m_output = nullptr;
  m_windows.clear();
  m_selectedIndex = 0;
  m_gridColumns = kGridColumns;
  destroySurface();
}

void WindowSwitcher::refreshWindows() {
  if (m_platform == nullptr) {
    m_windows.clear();
    return;
  }

  std::optional<std::string> selectedKey;
  if (m_active && m_selectedIndex < m_windows.size()) {
    selectedKey = identityKeyForEntry(m_windows[m_selectedIndex]);
    if (selectedKey->empty()) {
      selectedKey.reset();
    }
  }

  IconResolver iconResolver;
  const int iconSize = 96;
  buildWindowEntries(*m_platform, iconResolver, iconSize, m_windows, m_platform->focusedCompositorWindowId());

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

void WindowSwitcher::setSelectedIndex(std::size_t index) {
  if (m_windows.empty()) {
    m_selectedIndex = 0;
    return;
  }
  m_selectedIndex = index % m_windows.size();
  syncGridSelection();
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
  syncGridSelection();
  requestSceneUpdate();
}

void WindowSwitcher::navigateGrid(int colDelta, int rowDelta) {
  if (m_windows.empty()) {
    return;
  }
  const std::size_t columns = std::max<std::size_t>(1, m_gridColumns);
  const auto col = static_cast<int>(m_selectedIndex % columns);
  const auto row = static_cast<int>(m_selectedIndex / columns);
  int nextCol = col + colDelta;
  int nextRow = row + rowDelta;
  nextCol = std::clamp(nextCol, 0, static_cast<int>(columns) - 1);
  nextRow = std::max(0, nextRow);
  std::size_t nextIndex = static_cast<std::size_t>(nextRow) * columns + static_cast<std::size_t>(nextCol);
  if (nextIndex >= m_windows.size()) {
    nextIndex = m_windows.size() - 1;
  }
  setSelectedIndex(nextIndex);
}

void WindowSwitcher::activateSelected() {
  if (m_platform == nullptr || m_windows.empty() || m_selectedIndex >= m_windows.size()) {
    return;
  }
  // Hyprland ignores zwlr_foreign_toplevel_handle_v1.activate while an exclusive
  // keyboard layer-shell surface is mapped (hyprwm/Hyprland#4829). Snapshot the
  // selection, tear the overlay down, then activate on the next loop tick.
  const WindowSwitcherEntry entry = m_windows[m_selectedIndex];
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

  refreshWindows();
  if (m_windows.empty()) {
    hide();
    return;
  }
  if (m_selectedIndex >= m_windows.size()) {
    m_selectedIndex = m_windows.size() - 1;
  }
  requestSceneUpdate();
}

void WindowSwitcher::requestSceneUpdate() {
  if (m_instance != nullptr && m_instance->surface != nullptr) {
    m_instance->surface->requestLayout();
    m_instance->surface->requestRedraw();
  }
}

void WindowSwitcher::syncGridSelection() {
  if (m_instance == nullptr || m_instance->grid == nullptr || m_instance->adapter == nullptr) {
    return;
  }
  m_instance->adapter->setRenderer(m_renderContext);
  m_instance->adapter->setEntries(&m_windows);
  m_instance->grid->setSelectedIndex(m_selectedIndex);
  m_instance->grid->scrollToIndex(m_selectedIndex);
  m_instance->grid->notifyDataChanged();
  if (m_instance->emptyLabel != nullptr) {
    m_instance->emptyLabel->setVisible(m_windows.empty());
  }
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
    navigateGrid(-1, 0);
    return true;
  }
  if (matchesAction(KeybindAction::Right)) {
    navigateGrid(1, 0);
    return true;
  }
  if (matchesAction(KeybindAction::Up)) {
    navigateGrid(0, -1);
    return true;
  }
  if (matchesAction(KeybindAction::Down)) {
    navigateGrid(0, 1);
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
    const bool pressed = (event.state == 1);
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
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
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

  const auto metrics = computeSwitcherGridMetrics(
      static_cast<float>(width), static_cast<float>(height), instance.uiLayoutScale, m_windows.size()
  );
  const bool needsSceneBuild = instance.sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(instance.sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(instance.sceneRoot->height())) != height
      || !instance.gridMetrics.sameLayoutAs(metrics);
  if (needsSceneBuild) {
    instance.gridMetrics = metrics;
    m_gridColumns = metrics.columns;
    buildScene(instance, width, height);
  } else {
    syncGridSelection();
    if (instance.sceneRoot != nullptr && instance.sceneRoot->layoutDirty()) {
      instance.sceneRoot->layout(*m_renderContext);
      positionGrid(instance, static_cast<float>(width), static_cast<float>(height));
    }
  }
}

void WindowSwitcher::positionGrid(Instance& instance, float screenW, float screenH) {
  if (instance.grid == nullptr) {
    return;
  }
  instance.grid->setPosition(
      std::round((screenW - instance.grid->width()) * 0.5f), std::round((screenH - instance.grid->height()) * 0.5f)
  );
}

void WindowSwitcher::buildScene(Instance& instance, std::uint32_t width, std::uint32_t height) {
  UiPhaseScope layoutPhase(UiPhase::Layout);

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const float scale = instance.uiLayoutScale;

  instance.sceneRoot = std::make_unique<Node>();
  instance.sceneRoot->setSize(w, h);

  auto input = std::make_unique<InputArea>();
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

  input->addChild(
      ui::box({
          .fill = fixedColorSpec(rgba(0.0f, 0.0f, 0.0f, 1.0f)),
          .width = w,
          .height = h,
          .opacity = kDimOpacity,
          .participatesInLayout = false,
      })
  );

  const SwitcherGridMetrics& metrics = instance.gridMetrics;

  std::optional<ColorSpec> iconTint;
  if (m_config != nullptr) {
    iconTint = effectiveShellAppIconColorizationTint(m_config->config().shell);
  }
  instance.adapter = std::make_unique<WindowSwitcherGridAdapter>(scale, m_asyncTextures, iconTint);
  instance.adapter->setEntries(&m_windows);
  instance.adapter->setRenderer(m_renderContext);
  instance.adapter->setOnActivate([this](std::size_t index) {
    setSelectedIndex(index);
    activateSelected();
    hide();
  });
  instance.adapter->setOnClose([this](std::size_t index) { closeWindowAt(index); });
  instance.adapter->setOnInvalidate([this]() { requestSceneUpdate(); });

  input->addChild(
      ui::virtualGridView({
          .out = &instance.grid,
          .columns = metrics.columns,
          .cellHeight = metrics.cellH,
          .squareCells = false,
          .columnGap = metrics.colGap,
          .rowGap = metrics.rowGap,
          .overscanRows = 1,
          .adapter = instance.adapter.get(),
          .width = metrics.gridW,
          .height = metrics.gridH,
          .visible = !m_windows.empty(),
          .participatesInLayout = false,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (idx.has_value()) {
                  m_selectedIndex = *idx;
                }
              },
          .configure =
              [gridW = metrics.gridW, gridH = metrics.gridH](VirtualGridView& grid) {
                grid.setFillWidth(false);
                grid.setFillHeight(false);
                grid.setMinWidth(gridW);
                grid.setMinHeight(gridH);
                grid.setMaxWidth(gridW);
                grid.setMaxHeight(gridH);
                grid.setSize(gridW, gridH);
              },
      })
  );

  input->addChild(
      ui::label({
          .out = &instance.emptyLabel,
          .text = i18n::tr("window-switcher.empty"),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface, 0.88f),
          .visible = m_windows.empty(),
          .participatesInLayout = false,
          .configure = [screenW = w, screenH = h](Label& label) {
            label.setTextAlign(TextAlign::Center);
            label.setPosition(std::round(screenW * 0.5f - 80.0f), std::round(screenH * 0.5f - 10.0f));
          },
      })
  );

  instance.input = input.get();
  instance.sceneRoot->addChild(std::move(input));
  syncGridSelection();
  instance.sceneRoot->layout(*m_renderContext);
  positionGrid(instance, w, h);

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
}
