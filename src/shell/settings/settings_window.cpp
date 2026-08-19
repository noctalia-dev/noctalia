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
#include "render/core/async_texture_cache.h"
#include "render/render_context.h"
#include "render/text/font_weight_catalog.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/settings_content_plugins.h"
#include "shell/settings/settings_dialog_presenter.h"
#include "shell/tooltip/tooltip_manager.h"
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
#include "util/clamp.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr Logger kLog("settings");

  // Golden rectangle oriented like the output: the constrained dimension takes the fraction,
  // the other follows phi. The fixed size is only used when output geometry is unknown.
  constexpr float kWindowOutputFraction = 0.66F;
  constexpr float kGoldenRatio = std::numbers::phi_v<float>;
  constexpr float kWindowWidth = 1280.0F;
  constexpr float kWindowHeight = 600.0F;
  constexpr float kWindowMinWidth = 1020.0F;
  constexpr float kWindowMinHeight = 500.0F;

  // min_size hints are orientation-aware: portrait outputs constrain width, so the hint
  // follows the same 66% short-edge budget as the default open size instead of 1020.
  [[nodiscard]] std::pair<float, float> settingsWindowMinSize(float outputW, float outputH, float scale) {
    float minW = kWindowMinWidth * scale;
    float minH = kWindowMinHeight * scale;
    if (outputW <= 0.0F || outputH <= 0.0F) {
      return {minW, minH};
    }
    if (outputW < outputH) {
      minW = std::min(minW, outputW * kWindowOutputFraction);
    }
    minW = std::min(minW, outputW);
    minH = std::min(minH, outputH);
    return {minW, minH};
  }

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
      kLog.info("profile {}: {:.1F}ms", label, watch.elapsedMs());
    }
  }

} // namespace

SettingsWindow::SettingsWindow() = default;

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
  m_modalHost.initialize(
      m_inputDispatcher,
      [this]() {
        if (m_surface != nullptr) {
          m_surface->requestLayout();
        }
      },
      [this]() {
        if (m_surface != nullptr) {
          m_surface->requestUpdateOnly();
        }
      }
  );
}

void SettingsWindow::initializeDialogPresenter(
    ColorPickerDialogPresenter& colorFallback, GlyphPickerDialogPresenter& glyphFallback,
    FileDialogPresenter& fileFallback, ThumbnailService& thumbnails
) {
  m_dialogPresenter = std::make_unique<settings::SettingsDialogPresenter>();
  m_dialogPresenter->initialize(
      m_modalHost, m_animations, thumbnails, colorFallback, glyphFallback, fileFallback,
      [this]() { return shouldUseModalDialogs(); }, [this]() { return uiScale(); },
      [this]() { dismissOpenSelectDropdown(); }
  );
}

void SettingsWindow::shutdownDialogPresenter() {
  if (m_dialogPresenter != nullptr) {
    m_dialogPresenter->shutdown();
  }
}

ColorPickerDialogPresenter* SettingsWindow::colorPickerDialogPresenter() noexcept { return m_dialogPresenter.get(); }

GlyphPickerDialogPresenter* SettingsWindow::glyphPickerDialogPresenter() noexcept { return m_dialogPresenter.get(); }

FileDialogPresenter* SettingsWindow::fileDialogPresenter() noexcept { return m_dialogPresenter.get(); }

bool SettingsWindow::shouldUseModalDialogs() const noexcept {
  if (!isOpen() || m_wayland == nullptr) {
    return false;
  }
  wl_surface* source = m_wayland->lastInputSource() == WaylandSeat::InputSource::Keyboard
      ? m_wayland->lastKeyboardSurface()
      : m_wayland->lastPointerSurface();
  return source != nullptr && ownsKeyboardSurface(source);
}

float SettingsWindow::uiScale() const {
  if (m_config == nullptr) {
    return 1.0F;
  }
  return std::max(0.1F, m_config->config().accessibility.uiScale);
}

bool SettingsWindow::headerDragRegionContains(float sceneX, float sceneY) const {
  if (m_sceneRoot == nullptr || m_headerRow == nullptr) {
    return false;
  }

  float left = 0.0F;
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;
  Node::transformedBounds(m_headerRow, left, top, right, bottom);

  const float sceneWidth = m_sceneRoot->width();
  const float sceneHeight = m_sceneRoot->height();
  const float dragLeft = std::min(0.0F, left);
  const float dragTop = std::min(0.0F, top);
  const float dragRight = std::max(sceneWidth, right);
  const float dragBottom = std::clamp(bottom, 0.0F, sceneHeight);
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
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->wlSurface() == surface) {
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
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    return makeContext(
        m_widgetAddPopup->wlSurface(), m_widgetAddPopup->xdgSurface(), m_widgetAddPopup->width(),
        m_widgetAddPopup->height()
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
  if (m_searchPickerPopup != nullptr && surface == m_searchPickerPopup->wlSurface()) {
    return makeContext(
        m_searchPickerPopup->wlSurface(), m_searchPickerPopup->xdgSurface(), m_searchPickerPopup->width(),
        m_searchPickerPopup->height()
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
  TooltipManager::instance().forceDestroy();

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

  m_surface->setOutputChangedCallback([this](wl_output* currentOutput) {
    m_output = currentOutput;
    if (currentOutput != nullptr && m_surface != nullptr) {
      m_surface->requestLayout();
    }
  });

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
  float desiredWidth = kWindowWidth * scale;
  float desiredHeight = kWindowHeight * scale;
  float minWidthF = kWindowMinWidth * scale;
  float minHeightF = kWindowMinHeight * scale;
  if (const WaylandOutput* info = m_wayland->findOutputByWl(output); info != nullptr && info->hasUsableGeometry()) {
    const auto outputW = static_cast<float>(info->effectiveLogicalWidth());
    const auto outputH = static_cast<float>(info->effectiveLogicalHeight());
    std::tie(minWidthF, minHeightF) = settingsWindowMinSize(outputW, outputH, scale);
    if (outputW >= outputH) {
      desiredHeight = util::clampOrdered(outputH * kWindowOutputFraction, std::min(minHeightF, outputH), outputH);
      desiredWidth = util::clampOrdered(desiredHeight * kGoldenRatio, std::min(minWidthF, outputW), outputW);
    } else {
      desiredWidth = util::clampOrdered(outputW * kWindowOutputFraction, std::min(minWidthF, outputW), outputW);
      desiredHeight = util::clampOrdered(desiredWidth * kGoldenRatio, std::min(minHeightF, outputH), outputH);
    }
  }
  const auto width = static_cast<std::uint32_t>(std::round(desiredWidth));
  const auto height = static_cast<std::uint32_t>(std::round(desiredHeight));
  const auto minWidth = static_cast<std::uint32_t>(std::round(minWidthF));
  const auto minHeight = static_cast<std::uint32_t>(std::round(minHeightF));

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
  m_minWidthHint = minWidth;
  m_minHeightHint = minHeight;
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
  m_contentScrollState.offset = 0.0F;
  m_sidebarScrollState.offset = 0.0F;

  const bool wasOpen = isOpen();
  open();
  if (wasOpen && isOpen()) {
    requestSceneRebuild();
  }
}

bool SettingsWindow::openToPlugin(std::string pluginId) {
  const auto* manifest = scripting::PluginRegistry::instance().findManifest(pluginId);
  if (manifest == nullptr || !settings::pluginHasSettings(*manifest)) {
    return false;
  }

  clearTransientSettingsState();
  clearStatusMessage();
  m_searchQuery.clear();
  m_selectedSection = "plugins";
  m_pendingOpenPluginSettingsId = std::move(pluginId);
  m_contentScrollState.offset = 0.0F;
  m_sidebarScrollState.offset = 0.0F;

  const bool wasOpen = isOpen();
  open();
  if (wasOpen && isOpen()) {
    requestSceneRebuild();
  }
  return true;
}

void SettingsWindow::close() {
  if (!isOpen()) {
    return;
  }
  destroyWindow();
}

void SettingsWindow::closeForWidgetEditor() {
  if (!isOpen()) {
    return;
  }
  m_reopenAfterWidgetEditorSection = m_selectedSection;
  DeferredCall::callLater([this]() { close(); });
}

void SettingsWindow::reopenAfterWidgetEditor() {
  if (m_reopenAfterWidgetEditorSection.empty()) {
    return;
  }
  open(std::exchange(m_reopenAfterWidgetEditorSection, {}));
}

void SettingsWindow::dismissOpenSelectDropdown() {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->closeSelectDropdown();
  }
}

void SettingsWindow::destroyWindow() {
  m_modalHost.closeAll();
  m_configExportDialogModal.reset();
  m_editorSheetModal.reset();
  m_modalHost.detach();
  if (m_surface != nullptr) {
    // Drop stale pointer coords before tearing down the scene. Otherwise the next open
    // replays hover at the last click (often the close button) and paints it hovered.
    m_inputDispatcher.pointerLeave();
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
  if (m_searchPickerPopup != nullptr) {
    m_searchPickerPopup->close();
    m_searchPickerPopup.reset();
  }
  if (m_selectPopup != nullptr) {
    m_selectPopup->closeSelectDropdown();
  }
  m_sceneRoot.reset();
  m_surface.reset();
  m_pointerInside = false;
  m_output = nullptr;
  m_minWidthHint = 0;
  m_minHeightHint = 0;
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
  m_pendingResetSettingPaths.clear();
  m_searchQuery.clear();
  m_selectedSection.clear();
  m_selectedBarName.clear();
  m_selectedMonitorOverride.clear();
  m_pendingOpenWidgetInspectorName.clear();
  m_pendingOpenPluginSettingsId.clear();
  m_editingWidgetName.clear();
  m_editingCapsuleGroupId.clear();
  m_selectedLaneWidgets.clear();
  m_pendingDeleteWidgetName.clear();
  m_pendingDeleteWidgetSettingPath.clear();
  m_renamingWidgetName.clear();
  m_showOverriddenOnly = false;
  m_sidebarScrollState = {};
  m_contentScrollState = {};

  // Plugin-store thumbnails are the only async textures this window holds; drop the
  // zero-ref residents once the scene (and with it every Image) is gone.
  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures]() { asyncTextures->trimUnused(0); });
  }
}

void SettingsWindow::prepareFrame(bool needsUpdate, bool needsLayout) {
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
  Renderer& renderer = m_surface->renderTarget().renderer();
  logSettingsProfile("prepareFrame makeCurrent", phaseProfileWatch);
  if (needsUpdate && m_modalHost.isOpen()) {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_modalHost.update(renderer);
  }

  // Rebuild the entire scene only on first build or when something explicitly
  // requested it (config change, nav click, etc.). Pure size changes — which
  // niri delivers at refresh rate during window animations (slide-in on focus
  // return, workspace transitions) — should just re-layout the existing tree.
  // Rebuilding on every configure causes a 25+ rebuild storm during niri
  // animations, freezing input response for ~150 ms.
  const bool firstBuild = m_sceneRoot == nullptr;
  const bool sizeChanged = !firstBuild && (m_lastSceneWidth != width || m_lastSceneHeight != height);
  const bool needRebuild = firstBuild || m_rebuildRequested;

  const float scale = uiScale();
  float minWidthF = kWindowMinWidth * scale;
  float minHeightF = kWindowMinHeight * scale;
  wl_output* currentOutput = nullptr;
  if (m_wayland != nullptr && m_surface != nullptr) {
    currentOutput = m_wayland->outputForSurface(m_surface->wlSurface());
  }
  if (currentOutput == nullptr) {
    currentOutput = m_output;
  }
  if (const WaylandOutput* info = m_wayland->findOutputByWl(currentOutput);
      info != nullptr && info->hasUsableGeometry()) {
    std::tie(minWidthF, minHeightF) = settingsWindowMinSize(
        static_cast<float>(info->effectiveLogicalWidth()), static_cast<float>(info->effectiveLogicalHeight()), scale
    );
  }
  const auto newMinW = static_cast<std::uint32_t>(std::round(minWidthF));
  const auto newMinH = static_cast<std::uint32_t>(std::round(minHeightF));
  if (newMinW != m_minWidthHint || newMinH != m_minHeightHint) {
    phaseProfileWatch.reset();
    m_surface->setMinSize(newMinW, newMinH);
    m_minWidthHint = newMinW;
    m_minHeightHint = newMinH;
    logSettingsProfile("prepareFrame updateMinSize", phaseProfileWatch);
  }

  if (needRebuild) {
    phaseProfileWatch.reset();
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_inputDispatcher.stashTabFocus();
    buildScene(width, height);
    m_inputDispatcher.restoreStashedTabFocus();
    if (m_focusSearchOnRebuild) {
      if (m_settingsSearchInput != nullptr && m_settingsSearchInput->inputArea() != nullptr) {
        m_inputDispatcher.setFocus(m_settingsSearchInput->inputArea());
      }
      m_focusSearchOnRebuild = false;
    }
    logSettingsProfile("prepareFrame buildScene", phaseProfileWatch);
    m_lastSceneWidth = width;
    m_lastSceneHeight = height;
    m_rebuildRequested = false;
    m_contentRebuildRequested = false;
    m_settingsRegistryRefreshRequested = false;
    m_filterRowRefreshRequested = false;
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
    m_modalHost.resize(w, h);
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
      m_deferFocusScrollToLayout = true;
      m_inputDispatcher.restoreStashedTabFocus();
      m_deferFocusScrollToLayout = false;
      m_contentRebuildRequested = false;
    }
    logSettingsProfile("prepareFrame rebuildContent", phaseProfileWatch);
    phaseProfileWatch.reset();
    m_sceneRoot->layout(renderer);
    logSettingsProfile("prepareFrame layout", phaseProfileWatch);
    phaseProfileWatch.reset();
    applyPendingContentScrollTarget(Style::spaceMd * uiScale());
    logSettingsProfile("prepareFrame scrollTarget", phaseProfileWatch);
    m_lastSceneWidth = width;
    m_lastSceneHeight = height;
  }

  maybeOpenPendingEditor();
  logSettingsProfile("prepareFrame total", totalProfileWatch);
}

void SettingsWindow::maybeOpenPendingEditor() {
  const bool pending = !m_pendingOpenWidgetInspectorName.empty() || !m_pendingOpenPluginSettingsId.empty();
  if (!pending || m_surface == nullptr || m_wayland == nullptr || m_config == nullptr) {
    return;
  }
  if (!m_pendingOpenPluginSettingsId.empty()) {
    std::string pluginId = std::move(m_pendingOpenPluginSettingsId);
    m_pendingOpenPluginSettingsId.clear();
    openPluginSettingsEditor(std::move(pluginId));
    return;
  }

  std::string widgetName = std::move(m_pendingOpenWidgetInspectorName);
  m_pendingOpenWidgetInspectorName.clear();
  // The inspector takes a per-lane path {"bar", name, <lane>} (same shape the lane-card gear passes);
  // resolve which lane this widget lives in so it isn't a 2-element path that mislocates the bar name.
  std::vector<std::string> lanePath = barWidgetLanePath(m_config->config(), m_selectedBarName, widgetName);
  openWidgetInspectorEditor(std::move(lanePath), std::move(widgetName));
}

void SettingsWindow::requestSceneRebuild() {
  m_deferredSceneRebuild = true;
  scheduleDeferredRebuild();
}

void SettingsWindow::requestContentRebuild(bool refreshRegistry, bool refreshFilterRow, bool rebuildEditorSheet) {
  m_deferredRefreshRegistry = m_deferredRefreshRegistry || refreshRegistry;
  m_deferredRefreshFilterRow = m_deferredRefreshFilterRow || refreshFilterRow;
  m_deferredRebuildEditorSheet = m_deferredRebuildEditorSheet || rebuildEditorSheet;
  scheduleDeferredRebuild();
}

void SettingsWindow::scheduleDeferredRebuild() {
  if (m_deferredRebuildQueued) {
    return;
  }
  m_deferredRebuildQueued = true;
  DeferredCall::callLater([this]() {
    m_deferredRebuildQueued = false;
    const bool sceneRebuild = m_deferredSceneRebuild;
    const bool refreshRegistry = m_deferredRefreshRegistry;
    const bool refreshFilterRow = m_deferredRefreshFilterRow;
    const bool rebuildEditorSheet = m_deferredRebuildEditorSheet;
    m_deferredSceneRebuild = false;
    m_deferredRefreshRegistry = false;
    m_deferredRefreshFilterRow = false;
    m_deferredRebuildEditorSheet = false;
    if (m_surface == nullptr) {
      return;
    }

    if (sceneRebuild) {
      m_rebuildRequested = true;
      m_contentRebuildRequested = false;
      m_settingsRegistryRefreshRequested = false;
      m_filterRowRefreshRequested = false;
    } else {
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
    }
    m_surface->requestLayout();
    if ((sceneRebuild || rebuildEditorSheet) && m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
      m_editorSheetModal->rebuildBody();
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
    // Refresh the browsable catalog (throttled) so newly published plugins and update
    // badges appear on open, then list against the fetched revision.
    manager->fetchStaleCatalogs(pluginsSnapshot);
    auto plugins = manager->list(pluginsSnapshot, scripting::CatalogAccess::Network);
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
  m_pendingOpenPluginSettingsId.clear();
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
  m_pendingResetSettingPaths.clear();
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_configExportDialogModal != nullptr && m_configExportDialogModal->isOpen()) {
    m_configExportDialogModal->close();
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
      && event.pressed) {
    m_widgetAddPopup->close();
    return true;
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_searchPickerPopup != nullptr
      && m_searchPickerPopup->isOpen()
      && !m_searchPickerPopup->isInitializing()
      && event.type == PointerEvent::Type::Button
      && event.pressed) {
    m_searchPickerPopup->close();
    return true;
  }
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
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
      && event.pressed) {
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
    const bool pressed = event.pressed;
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
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed, event.serial, event.time,
          event.touch
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

  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen() && !m_searchPickerPopup->isInitializing()) {
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_searchPickerPopup->close();
      return;
    }
    m_searchPickerPopup->onKeyboardEvent(event);
    return;
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }

  if (m_modalHost.onKeyboardEvent(event)) {
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
    if (m_configExportDialogModal != nullptr && m_configExportDialogModal->isOpen()) {
      m_configExportDialogModal->requestRedraw();
    }
    if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
      m_editorSheetModal->requestRedraw();
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
    if (m_configExportDialogModal != nullptr && m_configExportDialogModal->isOpen()) {
      m_configExportDialogModal->requestLayout();
    }
    if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
      m_editorSheetModal->requestLayout();
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

void SettingsWindow::invalidatePluginSourceCache(const std::string& sourceName) {
  m_pluginFileCache.invalidateSource(sourceName);
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
