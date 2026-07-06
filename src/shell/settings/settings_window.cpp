#include "shell/settings/settings_window.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/scoped_timer.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "idle/idle_manager.h"
#include "render/render_context.h"
#include "render/text/font_weight_catalog.h"
#include "system/dependency_service.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/split_pane_focus.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("settings");

  constexpr float kWindowWidth = 1280.0f;
  constexpr float kWindowHeight = 600.0f;
  constexpr float kWindowMinWidth = 1020.0f;
  constexpr float kWindowMinHeight = 500.0f;

  // How many frames to wait for the settings window to gain keyboard focus before opening a pending
  // widget-inspector sheet anyway (bounded so a never-focused window can't spin redraws forever).
  constexpr int kPendingWidgetInspectorFrameBudget = 240;

  // Build the {"bar", name, <lane>} path the widget inspector expects, resolving which lane the widget
  // currently lives in (the inspector keys off the bar name at index 1 and the lane at the tail).
  std::vector<std::string>
  barWidgetLanePath(const Config& cfg, const std::string& barName, const std::string& widgetName) {
    std::string_view lane = "center";
    for (const auto& bar : cfg.bars) {
      if (bar.name != barName) {
        continue;
      }
      const auto inLane = [&](const std::vector<std::string>& widgets) {
        return std::ranges::contains(widgets, widgetName);
      };
      if (inLane(bar.startWidgets)) {
        lane = "start";
      } else if (inLane(bar.endWidgets)) {
        lane = "end";
      }
      break;
    }
    return {"bar", barName, std::string(lane)};
  }

  void focusExistingSettingsWindow(WaylandConnection& wayland, wl_surface* surface) {
    static constexpr std::string_view kSettingsAppId = "dev.noctalia.Noctalia";
    wayland.activateSurface(surface);
    wayland.activateToplevelForAppId(kSettingsAppId);
  }

  [[nodiscard]] bool isSettingsSearchTypingKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0) {
      return false;
    }
    if (KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)
        || KeybindMatcher::matches(KeybindAction::TabPrevious, event.sym, event.modifiers)
        || KeybindMatcher::matches(KeybindAction::TabNext, event.sym, event.modifiers)
        || KeybindMatcher::matches(KeybindAction::Up, event.sym, event.modifiers)
        || KeybindMatcher::matches(KeybindAction::Down, event.sym, event.modifiers)
        || KeybindMatcher::matches(KeybindAction::Left, event.sym, event.modifiers)
        || KeybindMatcher::matches(KeybindAction::Right, event.sym, event.modifiers)
        || KeybindMatcher::matches(KeybindAction::Validate, event.sym, event.modifiers)) {
      return false;
    }
    if (KeySymbol::isBackspace(event.sym)) {
      return true;
    }
    return event.utf32 > 0x20U && event.utf32 != 0x7FU;
  }

  void requestSceneInvalidation(Node* sceneRoot, ToplevelSurface* surface) {
    if (sceneRoot == nullptr || surface == nullptr) {
      return;
    }
    if (sceneRoot->layoutDirty()) {
      surface->requestLayout();
    } else if (sceneRoot->paintDirty()) {
      surface->requestRedraw();
    }
  }

  class SettingsProfileWatch {
  public:
    SettingsProfileWatch() {
      if (noctalia::profiling::enabled()) {
        m_watch.emplace();
      }
    }

    void reset() {
      if (m_watch.has_value()) {
        m_watch->reset();
      }
    }

    [[nodiscard]] bool active() const noexcept { return m_watch.has_value(); }
    [[nodiscard]] double elapsedMs() const { return m_watch.has_value() ? m_watch->elapsedMs() : 0.0; }

  private:
    std::optional<noctalia::profiling::StopWatch> m_watch;
  };

  void logSettingsProfile(std::string_view label, const SettingsProfileWatch& watch) {
    if (watch.active()) {
      kLog.info("profile {}: {:.1f}ms", label, watch.elapsedMs());
    }
  }

} // namespace

SettingsWindow::~SettingsWindow() { destroyWindow(); }

void SettingsWindow::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, DependencyService* dependencies,
    UPowerService* upower, IdleManager* idleManager, CompositorPlatform* platform, AccountsService* accounts
) {
  m_wayland = &wayland;
  m_platform = platform;
  m_idleManager = idleManager;
  m_config = config;
  m_renderContext = renderContext;
  m_dependencies = dependencies;
  m_upower = upower;
  m_accounts = accounts;
  m_showAdvanced = m_config != nullptr ? m_config->config().shell.settingsShowAdvanced : false;
}

float SettingsWindow::uiScale() const {
  if (m_config == nullptr) {
    return 1.0f;
  }
  return std::max(0.1f, m_config->config().shell.uiScale);
}

bool SettingsWindow::headerDragRegionContains(float sceneX, float sceneY) const {
  if (m_sceneRoot == nullptr || m_headerRow == nullptr) {
    return false;
  }

  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
  Node::transformedBounds(m_headerRow, left, top, right, bottom);

  const float sceneWidth = m_sceneRoot->width();
  const float sceneHeight = m_sceneRoot->height();
  const float dragLeft = std::min(0.0f, left);
  const float dragTop = std::min(0.0f, top);
  const float dragRight = std::max(sceneWidth, right);
  const float dragBottom = std::clamp(bottom, 0.0f, sceneHeight);
  return sceneX >= dragLeft && sceneX < dragRight && sceneY >= dragTop && sceneY < dragBottom;
}

bool SettingsWindow::ownsKeyboardSurface(wl_surface* surface) const noexcept {
  if (!isOpen() || surface == nullptr || m_surface == nullptr) {
    return false;
  }
  if (surface == m_surface->wlSurface()) {
    return true;
  }
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->wlSurface() == surface) {
    return true;
  }
  if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->wlSurface() == surface) {
    return true;
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->wlSurface() == surface) {
    return true;
  }
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->wlSurface() == surface) {
    return true;
  }
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->ownsSelectDropdownSurface(surface)) {
    return true;
  }
  return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && m_selectPopup->wlSurface() == surface;
}

std::optional<LayerPopupParentContext> SettingsWindow::topmostPopupParentContext() const {
  if (!isOpen() || m_surface == nullptr) {
    return std::nullopt;
  }

  const auto makeContext = [this](
                               wl_surface* wlSurface, xdg_surface* xdgSurface, std::uint32_t width, std::uint32_t height
                           ) -> std::optional<LayerPopupParentContext> {
    if (wlSurface == nullptr || xdgSurface == nullptr || width == 0 || height == 0) {
      return std::nullopt;
    }
    wl_output* output = m_wayland != nullptr ? m_wayland->outputForSurface(wlSurface) : nullptr;
    if (output == nullptr) {
      output = m_output;
    }
    return LayerPopupParentContext{
        .surface = wlSurface,
        .layerSurface = nullptr,
        .xdgSurface = xdgSurface,
        .output = output,
        .width = width,
        .height = height,
    };
  };

  const auto selectContext = [this, &makeContext]() -> std::optional<LayerPopupParentContext> {
    if (m_selectPopup == nullptr || !m_selectPopup->isSelectDropdownOpen()) {
      return std::nullopt;
    }
    return makeContext(
        m_selectPopup->wlSurface(), m_selectPopup->xdgSurface(), m_selectPopup->popupWidth(),
        m_selectPopup->popupHeight()
    );
  };

  if (auto context = selectContext(); context.has_value()) {
    return context;
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    return makeContext(
        m_searchPickerPopup->wlSurface(), m_searchPickerPopup->xdgSurface(), m_searchPickerPopup->width(),
        m_searchPickerPopup->height()
    );
  }
  if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen()) {
    return makeContext(
        m_configExportDialogPopup->wlSurface(), m_configExportDialogPopup->xdgSurface(),
        m_configExportDialogPopup->width(), m_configExportDialogPopup->height()
    );
  }
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    return makeContext(
        m_widgetAddPopup->wlSurface(), m_widgetAddPopup->xdgSurface(), m_widgetAddPopup->width(),
        m_widgetAddPopup->height()
    );
  }
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    return makeContext(
        m_editorSheetPopup->wlSurface(), m_editorSheetPopup->xdgSurface(), m_editorSheetPopup->width(),
        m_editorSheetPopup->height()
    );
  }
  return makeContext(m_surface->wlSurface(), m_surface->xdgSurface(), m_surface->width(), m_surface->height());
}

std::optional<LayerPopupParentContext> SettingsWindow::fallbackPopupParentContext() const {
  auto context = topmostPopupParentContext();
  if (context.has_value()) {
    context->usedFallback = true;
  }
  return context;
}

std::optional<LayerPopupParentContext> SettingsWindow::popupParentContextForSurface(wl_surface* surface) const {
  if (!isOpen() || surface == nullptr || m_surface == nullptr) {
    return std::nullopt;
  }

  const auto makeContext = [this](
                               wl_surface* wlSurface, xdg_surface* xdgSurface, std::uint32_t width, std::uint32_t height
                           ) -> std::optional<LayerPopupParentContext> {
    if (wlSurface == nullptr || xdgSurface == nullptr) {
      return std::nullopt;
    }
    wl_output* output = m_wayland != nullptr ? m_wayland->outputForSurface(wlSurface) : nullptr;
    if (output == nullptr) {
      output = m_output;
    }
    return LayerPopupParentContext{
        .surface = wlSurface,
        .layerSurface = nullptr,
        .xdgSurface = xdgSurface,
        .output = output,
        .width = width,
        .height = height,
    };
  };

  if (surface == m_surface->wlSurface()) {
    return topmostPopupParentContext();
  }
  if (m_widgetAddPopup != nullptr && surface == m_widgetAddPopup->wlSurface()) {
    return makeContext(
        m_widgetAddPopup->wlSurface(), m_widgetAddPopup->xdgSurface(), m_widgetAddPopup->width(),
        m_widgetAddPopup->height()
    );
  }
  if (m_configExportDialogPopup != nullptr && surface == m_configExportDialogPopup->wlSurface()) {
    return makeContext(
        m_configExportDialogPopup->wlSurface(), m_configExportDialogPopup->xdgSurface(),
        m_configExportDialogPopup->width(), m_configExportDialogPopup->height()
    );
  }
  if (m_searchPickerPopup != nullptr && surface == m_searchPickerPopup->wlSurface()) {
    return makeContext(
        m_searchPickerPopup->wlSurface(), m_searchPickerPopup->xdgSurface(), m_searchPickerPopup->width(),
        m_searchPickerPopup->height()
    );
  }
  if (m_editorSheetPopup != nullptr && surface == m_editorSheetPopup->wlSurface()) {
    return makeContext(
        m_editorSheetPopup->wlSurface(), m_editorSheetPopup->xdgSurface(), m_editorSheetPopup->width(),
        m_editorSheetPopup->height()
    );
  }
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && surface == m_selectPopup->wlSurface()) {
    return makeContext(
        m_selectPopup->wlSurface(), m_selectPopup->xdgSurface(), m_selectPopup->popupWidth(),
        m_selectPopup->popupHeight()
    );
  }
  return std::nullopt;
}

void SettingsWindow::open(std::string context) {
  if (!context.empty()) {
    m_selectedSection = std::move(context);
  }

  if (m_wayland == nullptr || m_renderContext == nullptr || !m_wayland->hasXdgShell()) {
    return;
  }

  if (m_dependencies != nullptr) {
    m_dependencies->rescan();
  }

  if (isOpen()) {
    const auto refocus = [this]() {
      if (m_wayland != nullptr && m_surface != nullptr) {
        focusExistingSettingsWindow(*m_wayland, m_surface->wlSurface());
      }
    };
    refocus();
    DeferredCall::callLater(refocus);
    return;
  }

  m_showAdvanced = m_config != nullptr ? m_config->config().shell.settingsShowAdvanced : false;

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    const auto& outs = m_wayland->outputs();
    if (!outs.empty() && outs.front().output != nullptr) {
      output = outs.front().output;
    }
  }
  m_output = output;

  m_surface = std::make_unique<ToplevelSurface>(*m_wayland);
  m_surface->setRenderContext(m_renderContext);
  m_surface->setAnimationManager(&m_animations);

  m_surface->setClosedCallback([this]() { destroyWindow(); });

  m_surface->setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) {
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  });

  m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });

  m_surface->setUpdateCallback([]() {});

  const float scale = uiScale();
  const auto width = static_cast<std::uint32_t>(std::round(kWindowWidth * scale));
  const auto height = static_cast<std::uint32_t>(std::round(kWindowHeight * scale));
  const auto minWidth = static_cast<std::uint32_t>(std::round(kWindowMinWidth * scale));
  const auto minHeight = static_cast<std::uint32_t>(std::round(kWindowMinHeight * scale));

  ToplevelSurfaceConfig cfg{
      .width = std::max<std::uint32_t>(1, width),
      .height = std::max<std::uint32_t>(1, height),
      .minWidth = minWidth,
      .minHeight = minHeight,
      .title = i18n::tr("settings.window.native-title"),
      .appId = "dev.noctalia.Noctalia",
  };

  if (!m_surface->initialize(output, cfg)) {
    kLog.warn("settings: failed to create toplevel surface");
    m_surface.reset();
    return;
  }
  m_pointerInside = false;
  m_lastSceneWidth = 0;
  m_lastSceneHeight = 0;
}

void SettingsWindow::openToBarWidget(std::string barName, std::string widgetName) {
  clearTransientSettingsState();
  clearStatusMessage();
  m_searchQuery.clear();
  m_selectedSection = "bar";
  m_selectedBarName = std::move(barName);
  m_selectedMonitorOverride.clear();
  m_pendingOpenWidgetInspectorName = std::move(widgetName);
  m_pendingOpenWidgetInspectorFrames = kPendingWidgetInspectorFrameBudget;
  m_contentScrollState.offset = 0.0f;
  m_sidebarScrollState.offset = 0.0f;

  const bool wasOpen = isOpen();
  open();
  if (wasOpen && isOpen()) {
    requestSceneRebuild();
  }
}

void SettingsWindow::close() {
  if (!isOpen()) {
    return;
  }
  destroyWindow();
}

void SettingsWindow::dismissOpenSelectDropdown() {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->closeSelectDropdown();
  }
}

void SettingsWindow::destroyWindow() {
  if (m_surface != nullptr) {
    m_inputDispatcher.setSceneRoot(nullptr);
    m_surface->setSceneRoot(nullptr);
  }
  m_idleLiveStatusLabel = nullptr;
  m_mainContainer = nullptr;
  m_headerRow = nullptr;
  m_filterRow = nullptr;
  m_contentContainer = nullptr;
  m_contentScrollView = nullptr;
  m_sidebarScrollView = nullptr;
  m_sidebarNav = nullptr;
  m_actionsMenuButton = nullptr;
  if (m_actionsMenuPopup != nullptr) {
    m_actionsMenuPopup->close();
    m_actionsMenuPopup.reset();
  }
  if (m_widgetAddPopup != nullptr) {
    m_widgetAddPopup->close();
    m_widgetAddPopup.reset();
  }
  if (m_configExportDialogPopup != nullptr) {
    m_configExportDialogPopup->close();
    m_configExportDialogPopup.reset();
  }
  if (m_searchPickerPopup != nullptr) {
    m_searchPickerPopup->close();
    m_searchPickerPopup.reset();
  }
  if (m_editorSheetPopup != nullptr) {
    m_editorSheetPopup->close();
    m_editorSheetPopup.reset();
  }
  if (m_selectPopup != nullptr) {
    m_selectPopup->closeSelectDropdown();
  }
  m_sceneRoot.reset();
  m_surface.reset();
  m_pointerInside = false;
  m_output = nullptr;
  m_lastSceneWidth = 0;
  m_lastSceneHeight = 0;
  m_settingsRegistry.clear();
  m_rebuildRequested = false;
  m_contentRebuildRequested = false;
  m_settingsRegistryRefreshRequested = false;
  m_filterRowRefreshRequested = false;
  m_focusSearchOnRebuild = false;
  m_scrollToPendingContentTarget = false;
  m_pendingContentScrollTarget = nullptr;
  m_statusMessage.clear();
  m_statusIsError = false;
  m_creatingBarName.clear();
  m_renamingBarName.clear();
  m_pendingDeleteBarName.clear();
  m_creatingMonitorOverrideBarName.clear();
  m_creatingMonitorOverrideMatch.clear();
  m_renamingMonitorOverrideBarName.clear();
  m_renamingMonitorOverrideMatch.clear();
  m_pendingDeleteMonitorOverrideBarName.clear();
  m_pendingDeleteMonitorOverrideMatch.clear();
  m_pendingResetPageScope.clear();
  m_searchQuery.clear();
  m_selectedSection.clear();
  m_selectedBarName.clear();
  m_selectedMonitorOverride.clear();
  m_pendingOpenWidgetInspectorName.clear();
  m_editingWidgetName.clear();
  m_editingCapsuleGroupId.clear();
  m_selectedLaneWidgets.clear();
  m_pendingDeleteWidgetName.clear();
  m_pendingDeleteWidgetSettingPath.clear();
  m_renamingWidgetName.clear();
  m_showOverriddenOnly = false;
  m_sidebarScrollState = {};
  m_contentScrollState = {};
}

void SettingsWindow::prepareFrame(bool /*needsUpdate*/, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  SettingsProfileWatch totalProfileWatch;

  const auto width = m_surface->width();
  const auto height = m_surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  SettingsProfileWatch phaseProfileWatch;
  m_renderContext->makeCurrent(m_surface->renderTarget());
  logSettingsProfile("prepareFrame makeCurrent", phaseProfileWatch);

  // Rebuild the entire scene only on first build or when something explicitly
  // requested it (config change, nav click, etc.). Pure size changes — which
  // niri delivers at refresh rate during window animations (slide-in on focus
  // return, workspace transitions) — should just re-layout the existing tree.
  // Rebuilding on every configure causes a 25+ rebuild storm during niri
  // animations, freezing input response for ~150 ms.
  const bool firstBuild = m_sceneRoot == nullptr;
  const bool sizeChanged = !firstBuild && (m_lastSceneWidth != width || m_lastSceneHeight != height);
  const bool needRebuild = firstBuild || m_rebuildRequested;

  if (needRebuild) {
    phaseProfileWatch.reset();
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_inputDispatcher.stashTabFocus();
    buildScene(width, height);
    m_inputDispatcher.restoreStashedTabFocus();
    logSettingsProfile("prepareFrame buildScene", phaseProfileWatch);
    m_lastSceneWidth = width;
    m_lastSceneHeight = height;
    m_rebuildRequested = false;
    m_contentRebuildRequested = false;
    m_settingsRegistryRefreshRequested = false;
    m_filterRowRefreshRequested = false;
    phaseProfileWatch.reset();
    const float scale = uiScale();
    const auto newMinW = static_cast<std::uint32_t>(std::round(kWindowMinWidth * scale));
    const auto newMinH = static_cast<std::uint32_t>(std::round(kWindowMinHeight * scale));
    m_surface->setMinSize(newMinW, newMinH);
    m_surface->clampToMinSize(newMinW, newMinH);
    logSettingsProfile("prepareFrame updateMinSize", phaseProfileWatch);
  } else if ((m_contentRebuildRequested || sizeChanged || needsLayout) && m_sceneRoot != nullptr) {
    phaseProfileWatch.reset();
    UiPhaseScope layoutPhase(UiPhase::Layout);
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    m_sceneRoot->setSize(w, h);
    if (m_panelBackground != nullptr) {
      m_panelBackground->setSize(w, h);
    }
    if (m_mainContainer != nullptr) {
      m_mainContainer->setSize(w, h);
    }
    if (m_contentRebuildRequested) {
      m_inputDispatcher.stashTabFocus();
      if (m_settingsRegistryRefreshRequested) {
        const Config fallbackCfg{};
        const Config& cfg = m_config != nullptr ? m_config->config() : fallbackCfg;
        refreshSettingsRegistry(cfg);
        m_settingsRegistryRefreshRequested = false;
      }
      if (m_filterRowRefreshRequested) {
        rebuildFilterRow(uiScale());
        m_filterRowRefreshRequested = false;
      }
      rebuildSettingsContent();
      m_inputDispatcher.restoreStashedTabFocus();
      m_contentRebuildRequested = false;
    }
    logSettingsProfile("prepareFrame rebuildContent", phaseProfileWatch);
    phaseProfileWatch.reset();
    m_sceneRoot->layout(*m_renderContext);
    logSettingsProfile("prepareFrame layout", phaseProfileWatch);
    phaseProfileWatch.reset();
    applyPendingContentScrollTarget(Style::spaceMd * uiScale());
    logSettingsProfile("prepareFrame scrollTarget", phaseProfileWatch);
    m_lastSceneWidth = width;
    m_lastSceneHeight = height;
  }

  maybeOpenPendingWidgetInspector();
  logSettingsProfile("prepareFrame total", totalProfileWatch);
}

void SettingsWindow::maybeOpenPendingWidgetInspector() {
  if (m_pendingOpenWidgetInspectorName.empty() || m_surface == nullptr || m_wayland == nullptr || m_config == nullptr) {
    return;
  }
  // A grab popup needs an input serial this window owns. Right after a bar middle-click the latest
  // serial still belongs to the bar surface; wait until this window holds keyboard focus (whose enter
  // refreshes the serial) so the compositor accepts the sheet's grab instead of dismissing it.
  const bool focused = m_wayland->lastKeyboardSurface() == m_surface->wlSurface();
  if (!focused && m_pendingOpenWidgetInspectorFrames > 0) {
    --m_pendingOpenWidgetInspectorFrames;
    m_surface->requestRedraw();
    return;
  }
  std::string widgetName = std::move(m_pendingOpenWidgetInspectorName);
  m_pendingOpenWidgetInspectorName.clear();
  // A bar middle-click gives us no press serial the settings surface owns, so the compositor rejects
  // an xdg_popup grab. Open the sheet without a grab — the window holds keyboard focus and routes
  // input to it, and an outside click still dismisses it (handled in onPointerEvent).
  m_pendingEditorSheetNoGrab = true;
  // The inspector takes a per-lane path {"bar", name, <lane>} (same shape the lane-card gear passes);
  // resolve which lane this widget lives in so it isn't a 2-element path that mislocates the bar name.
  openWidgetInspectorEditor(
      barWidgetLanePath(m_config->config(), m_selectedBarName, widgetName), std::move(widgetName)
  );
}

void SettingsWindow::requestSceneRebuild() {
  DeferredCall::callLater([this]() {
    if (m_surface == nullptr) {
      return;
    }
    m_rebuildRequested = true;
    m_contentRebuildRequested = false;
    m_settingsRegistryRefreshRequested = false;
    m_filterRowRefreshRequested = false;
    m_surface->requestLayout();
    // The editor sheet edits the same config: rebuild its body so override/reset controls track
    // value changes in place, the way the inline inspector did when the whole scene rebuilt.
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->rebuildBody();
    }
  });
}

void SettingsWindow::requestContentRebuild(bool refreshRegistry, bool refreshFilterRow, bool rebuildEditorSheet) {
  DeferredCall::callLater([this, refreshRegistry, refreshFilterRow, rebuildEditorSheet]() {
    if (m_surface == nullptr) {
      return;
    }
    if (refreshRegistry) {
      m_settingsRegistryRefreshRequested = true;
    }
    if (refreshFilterRow) {
      m_filterRowRefreshRequested = true;
    }
    if (m_sceneRoot == nullptr || m_contentContainer == nullptr) {
      m_rebuildRequested = true;
      m_settingsRegistryRefreshRequested = false;
      m_filterRowRefreshRequested = false;
    } else if (!m_rebuildRequested) {
      m_contentRebuildRequested = true;
    }
    m_surface->requestLayout();
    if (rebuildEditorSheet && m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->rebuildBody();
    }
  });
}

void SettingsWindow::markPluginListDirty() {
  m_pluginListDirty = true;
  ++m_pluginListRefreshGeneration;
}

void SettingsWindow::refreshPluginListIfNeeded() {
  if (m_pluginManager == nullptr || m_config == nullptr || !m_pluginListDirty || m_pluginListRefreshInFlight) {
    return;
  }

  m_pluginListRefreshInFlight = true;
  const std::uint64_t generation = m_pluginListRefreshGeneration;
  auto* manager = m_pluginManager;
  PluginsConfig pluginsSnapshot = m_config->config().plugins;
  std::thread([this, manager, generation, pluginsSnapshot = std::move(pluginsSnapshot)]() {
    auto plugins = manager->list(pluginsSnapshot);
    DeferredCall::callLater([this, generation, plugins = std::move(plugins)]() mutable {
      m_pluginListRefreshInFlight = false;
      if (generation != m_pluginListRefreshGeneration) {
        refreshPluginListIfNeeded();
        return;
      }
      m_pluginList = std::move(plugins);
      m_pluginListDirty = false;
      if (isOpen() && m_selectedSection == "plugins") {
        requestContentRebuild();
      }
    });
  }).detach();
}

void SettingsWindow::clearStatusMessage() {
  m_statusMessage.clear();
  m_statusIsError = false;
}

void SettingsWindow::clearTransientSettingsState() {
  m_pendingOpenWidgetInspectorName.clear();
  m_editingWidgetName.clear();
  m_editingCapsuleGroupId.clear();
  m_selectedLaneWidgets.clear();
  m_renamingWidgetName.clear();
  m_pendingDeleteWidgetName.clear();
  m_pendingDeleteWidgetSettingPath.clear();
  m_creatingBarName.clear();
  m_renamingBarName.clear();
  m_pendingDeleteBarName.clear();
  m_creatingMonitorOverrideBarName.clear();
  m_creatingMonitorOverrideMatch.clear();
  m_renamingMonitorOverrideBarName.clear();
  m_renamingMonitorOverrideMatch.clear();
  m_pendingDeleteMonitorOverrideBarName.clear();
  m_pendingDeleteMonitorOverrideMatch.clear();
  m_pendingResetPageScope.clear();
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen()) {
    m_configExportDialogPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }
}

bool SettingsWindow::onPointerEvent(const PointerEvent& event) {
  if (!isOpen() || m_surface == nullptr) {
    return false;
  }

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_widgetAddPopup != nullptr
      && m_widgetAddPopup->isOpen()
      && !m_widgetAddPopup->isInitializing()
      && event.type == PointerEvent::Type::Button
      && event.state == 1) {
    m_widgetAddPopup->close();
    return true;
  }
  if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_configExportDialogPopup != nullptr
      && m_configExportDialogPopup->isOpen()
      && !m_configExportDialogPopup->isInitializing()
      && event.type == PointerEvent::Type::Button
      && event.state == 1) {
    m_configExportDialogPopup->close();
    return true;
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_searchPickerPopup != nullptr
      && m_searchPickerPopup->isOpen()
      && !m_searchPickerPopup->isInitializing()
      && event.type == PointerEvent::Type::Button
      && event.state == 1) {
    m_searchPickerPopup->close();
    return true;
  }
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_editorSheetPopup != nullptr
      && m_editorSheetPopup->isOpen()
      && !m_editorSheetPopup->isInitializing()
      && event.type == PointerEvent::Type::Button
      && event.state == 1) {
    m_editorSheetPopup->close();
    return true;
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }

  if (m_actionsMenuPopup != nullptr && m_actionsMenuPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_actionsMenuPopup != nullptr
      && m_actionsMenuPopup->isOpen()
      && event.type == PointerEvent::Type::Button
      && event.state == 1) {
    m_actionsMenuPopup->close();
    return true;
  }

  wl_surface* const ws = m_surface->wlSurface();
  const bool onThis = (event.surface != nullptr && event.surface == ws);
  bool consumed = false;

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onThis) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    break;
  case PointerEvent::Type::Leave:
    if (onThis) {
      m_pointerInside = false;
      m_inputDispatcher.pointerLeave();
    }
    break;
  case PointerEvent::Type::Motion:
    if (onThis || m_pointerInside) {
      if (onThis) {
        m_pointerInside = true;
      }
      const std::uint32_t serial = m_wayland != nullptr ? m_wayland->lastInputSerial() : 0;
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), serial);
      consumed = m_pointerInside;
    }
    break;
  case PointerEvent::Type::Button: {
    const bool pressed = (event.state == 1);
    if (onThis || m_pointerInside) {
      if (onThis) {
        m_pointerInside = true;
      }
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      if (pressed
          && event.button == BTN_LEFT
          && m_inputDispatcher.hoveredArea() == nullptr
          && headerDragRegionContains(static_cast<float>(event.sx), static_cast<float>(event.sy))) {
        m_surface->beginMove(event.serial);
        consumed = true;
        break;
      }
      m_inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
      consumed = m_pointerInside;
    }
    break;
  }
  case PointerEvent::Type::Axis:
    if (m_pointerInside) {
      m_inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines
      );
      consumed = true;
    }
    break;
  }

  if (m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }

  return consumed;
}

void SettingsWindow::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen()) {
    return;
  }

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen() && !m_widgetAddPopup->isInitializing()) {
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_widgetAddPopup->close();
      return;
    }
    m_widgetAddPopup->onKeyboardEvent(event);
    return;
  }

  if (m_configExportDialogPopup != nullptr
      && m_configExportDialogPopup->isOpen()
      && !m_configExportDialogPopup->isInitializing()) {
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_configExportDialogPopup->close();
      return;
    }
    m_configExportDialogPopup->onKeyboardEvent(event);
    return;
  }

  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen() && !m_searchPickerPopup->isInitializing()) {
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_searchPickerPopup->close();
      return;
    }
    m_searchPickerPopup->onKeyboardEvent(event);
    return;
  }

  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen() && !m_editorSheetPopup->isInitializing()) {
    if (m_editorSheetPopup->isSelectDropdownOpen()) {
      m_editorSheetPopup->onKeyboardEvent(event);
      return;
    }
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_editorSheetPopup->close();
      return;
    }
    m_editorSheetPopup->onKeyboardEvent(event);
    return;
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }

  const auto requestRebuild = [this]() {
    if (m_surface != nullptr) {
      m_rebuildRequested = true;
      m_surface->requestLayout();
    }
  };
  if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    if (m_actionsMenuPopup != nullptr && m_actionsMenuPopup->isOpen()) {
      m_actionsMenuPopup->close();
      return;
    }
    if (!m_editingWidgetName.empty()
        || !m_editingCapsuleGroupId.empty()
        || !m_selectedLaneWidgets.empty()
        || !m_renamingWidgetName.empty()
        || !m_pendingDeleteWidgetName.empty()
        || !m_pendingDeleteWidgetSettingPath.empty()
        || !m_creatingBarName.empty()
        || !m_renamingBarName.empty()
        || !m_pendingDeleteBarName.empty()
        || !m_creatingMonitorOverrideBarName.empty()
        || !m_renamingMonitorOverrideBarName.empty()
        || !m_pendingDeleteMonitorOverrideBarName.empty()) {
      m_editingWidgetName.clear();
      m_editingCapsuleGroupId.clear();
      m_selectedLaneWidgets.clear();
      m_renamingWidgetName.clear();
      m_pendingDeleteWidgetName.clear();
      m_pendingDeleteWidgetSettingPath.clear();
      m_creatingBarName.clear();
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      requestRebuild();
      return;
    }
  }
  if (event.pressed
      && !event.preedit
      && (event.modifiers & KeyMod::Ctrl) != 0
      && (event.sym == XKB_KEY_f || event.sym == XKB_KEY_F)) {
    if (m_settingsSearchInput != nullptr && m_settingsSearchInput->inputArea() != nullptr) {
      m_inputDispatcher.setFocus(m_settingsSearchInput->inputArea());
    } else {
      m_focusSearchOnRebuild = true;
      requestSceneRebuild();
    }
    if (m_surface != nullptr) {
      m_surface->requestRedraw();
    }
    return;
  }
  if (m_sidebarNav != nullptr
      && m_sidebarScrollView != nullptr
      && m_contentScrollView != nullptr
      && m_contentScrollView->content() != nullptr) {
    const SplitPaneFocusConfig panes{
        .sidebarFocus = m_sidebarNav->focusArea(),
        .sidebarRoot = m_sidebarScrollView,
        .contentRoot = m_contentScrollView->content(),
        .headerFocus = m_settingsSearchInput != nullptr ? m_settingsSearchInput->inputArea() : nullptr,
    };
    const SplitPaneFocusResult splitResult = handleSplitPaneFocusNavigation(
        m_inputDispatcher, panes, event.sym, event.modifiers, event.pressed, event.preedit
    );
    if (splitResult == SplitPaneFocusResult::Consumed) {
      if (m_sceneRoot != nullptr && m_surface != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
  }
  if (event.pressed
      && !event.preedit
      && m_inputDispatcher.focusedArea() == nullptr
      && m_settingsSearchInput != nullptr
      && m_settingsSearchInput->inputArea() != nullptr
      && (m_actionsMenuPopup == nullptr || !m_actionsMenuPopup->isOpen())
      && isSettingsSearchTypingKey(event)) {
    m_inputDispatcher.setFocus(m_settingsSearchInput->inputArea());
    m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    requestSceneInvalidation(m_sceneRoot.get(), m_surface.get());
    return;
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_sceneRoot != nullptr && m_surface != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }
}

void SettingsWindow::onThemeChanged() { requestRedraw(); }

void SettingsWindow::requestRedraw() {
  if (isOpen()) {
    if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
      m_widgetAddPopup->requestRedraw();
    }
    if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen()) {
      m_configExportDialogPopup->requestRedraw();
    }
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->requestRedraw();
    }
    m_surface->requestRedraw();
  }
}

void SettingsWindow::onFontChanged() {
  text::invalidateFontWeightCatalogCache();
  if (isOpen()) {
    if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
      m_widgetAddPopup->requestLayout();
    }
    if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen()) {
      m_configExportDialogPopup->requestLayout();
    }
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->requestLayout();
    }
    requestSceneRebuild();
  }
}

void SettingsWindow::onExternalOptionsChanged() { requestSceneRebuild(); }

void SettingsWindow::onPluginsChanged() {
  markPluginListDirty();
  if (isOpen() && m_selectedSection == "plugins") {
    requestContentRebuild();
  }
}

void SettingsWindow::refreshIdleLiveStatusText() {
  if (m_idleLiveStatusLabel == nullptr) {
    return;
  }

  const std::int64_t sec = m_idleManager != nullptr ? m_idleManager->liveIdleSeconds() : 0;
  if (sec <= 0) {
    m_idleLiveStatusLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_idleLiveStatusLabel->setText(i18n::tr("settings.idle.live-status.active"));
    return;
  }

  m_idleLiveStatusLabel->setColor(colorSpecFromRole(ColorRole::Primary));
  if (sec == 1) {
    m_idleLiveStatusLabel->setText(i18n::tr("settings.idle.live-status.idle-for-one"));
  } else {
    m_idleLiveStatusLabel->setText(
        i18n::tr("settings.idle.live-status.idle-for-seconds", "seconds", std::to_string(sec))
    );
  }
}

void SettingsWindow::onIdleLiveStatusChanged() {
  if (m_idleLiveStatusLabel == nullptr || m_surface == nullptr) {
    return;
  }
  refreshIdleLiveStatusText();
  m_surface->requestRedraw();
}

void SettingsWindow::onSecondTick() { onIdleLiveStatusChanged(); }
