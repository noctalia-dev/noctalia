#include "shell/settings/settings_window.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "idle/idle_manager.h"
#include "render/render_context.h"
#include "system/dependency_service.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("settings");

  constexpr float kWindowWidth = 1080.0f;
  constexpr float kWindowHeight = 600.0f;
  constexpr float kWindowMinWidth = 900.0f;
  constexpr float kWindowMinHeight = 500.0f;

} // namespace

SettingsWindow::~SettingsWindow() = default;

void SettingsWindow::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, DependencyService* dependencies,
    UPowerService* upower, IdleManager* idleManager
) {
  m_wayland = &wayland;
  m_idleManager = idleManager;
  m_config = config;
  m_renderContext = renderContext;
  m_dependencies = dependencies;
  m_upower = upower;
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
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->wlSurface() == surface) {
    return true;
  }
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->ownsSelectDropdownSurface(surface)) {
    return true;
  }
  return m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && m_selectPopup->wlSurface() == surface;
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
    return makeContext(m_surface->wlSurface(), m_surface->xdgSurface(), m_surface->width(), m_surface->height());
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
  if (m_sessionActionsEditorPopup != nullptr && surface == m_sessionActionsEditorPopup->wlSurface()) {
    return makeContext(
        m_sessionActionsEditorPopup->wlSurface(), m_sessionActionsEditorPopup->xdgSurface(),
        m_sessionActionsEditorPopup->width(), m_sessionActionsEditorPopup->height()
    );
  }
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->ownsSelectDropdownSurface(surface)) {
    return makeContext(
        m_sessionActionsEditorPopup->wlSurface(), m_sessionActionsEditorPopup->xdgSurface(),
        m_sessionActionsEditorPopup->width(), m_sessionActionsEditorPopup->height()
    );
  }
  return std::nullopt;
}

void SettingsWindow::open() {
  if (m_wayland == nullptr || m_renderContext == nullptr || !m_wayland->hasXdgShell()) {
    return;
  }

  if (m_dependencies != nullptr) {
    m_dependencies->rescan();
  }

  if (isOpen()) {
    m_wayland->activateSurface(m_surface->wlSurface());
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
  const std::uint32_t width = static_cast<std::uint32_t>(std::round(kWindowWidth * scale));
  const std::uint32_t height = static_cast<std::uint32_t>(std::round(kWindowHeight * scale));
  const std::uint32_t minWidth = static_cast<std::uint32_t>(std::round(kWindowMinWidth * scale));
  const std::uint32_t minHeight = static_cast<std::uint32_t>(std::round(kWindowMinHeight * scale));

  ToplevelSurfaceConfig cfg{
      .width = std::max<std::uint32_t>(1, width),
      .height = std::max<std::uint32_t>(1, height),
      .minWidth = minWidth,
      .minHeight = minHeight,
      .title = i18n::tr("settings.window.native-title"),
      .appId = "dev.noctalia.Noctalia.Settings",
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
  m_editingWidgetName = std::move(widgetName);
  m_contentScrollState.offset = 0.0f;
  m_scrollToPendingContentTarget = true;
  m_pendingContentScrollTarget = nullptr;
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

void SettingsWindow::destroyWindow() {
  if (m_surface != nullptr) {
    m_inputDispatcher.setSceneRoot(nullptr);
    m_surface->setSceneRoot(nullptr);
  }
  m_idleLiveStatusLabel = nullptr;
  m_mainContainer = nullptr;
  m_headerRow = nullptr;
  m_contentContainer = nullptr;
  m_contentScrollView = nullptr;
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
  if (m_sessionActionsEditorPopup != nullptr) {
    m_sessionActionsEditorPopup->close();
    m_sessionActionsEditorPopup.reset();
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
  m_editingWidgetName.clear();
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

  const auto width = m_surface->width();
  const auto height = m_surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());

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
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildScene(width, height);
    m_lastSceneWidth = width;
    m_lastSceneHeight = height;
    m_rebuildRequested = false;
    m_contentRebuildRequested = false;
    const float scale = uiScale();
    const auto newMinW = static_cast<std::uint32_t>(std::round(kWindowMinWidth * scale));
    const auto newMinH = static_cast<std::uint32_t>(std::round(kWindowMinHeight * scale));
    m_surface->setMinSize(newMinW, newMinH);
    m_surface->clampToMinSize(newMinW, newMinH);
  } else if ((m_contentRebuildRequested || sizeChanged || needsLayout) && m_sceneRoot != nullptr) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    m_sceneRoot->setSize(w, h);
    if (m_panelBackground != nullptr) {
      m_panelBackground->setSize(w, h);
    }
    if (m_mainContainer != nullptr) {
      m_mainContainer->setSize(w, h);
    }
    if (m_contentRebuildRequested) {
      rebuildSettingsContent();
      m_contentRebuildRequested = false;
    }
    m_sceneRoot->layout(*m_renderContext);
    applyPendingContentScrollTarget(Style::spaceMd * uiScale());
    m_lastSceneWidth = width;
    m_lastSceneHeight = height;
  }
}

void SettingsWindow::requestSceneRebuild() {
  DeferredCall::callLater([this]() {
    if (m_surface == nullptr) {
      return;
    }
    m_rebuildRequested = true;
    m_contentRebuildRequested = false;
    m_surface->requestLayout();
  });
}

void SettingsWindow::requestContentRebuild() {
  DeferredCall::callLater([this]() {
    if (m_surface == nullptr) {
      return;
    }
    if (m_sceneRoot == nullptr || m_contentContainer == nullptr) {
      m_rebuildRequested = true;
    } else if (!m_rebuildRequested) {
      m_contentRebuildRequested = true;
    }
    m_surface->requestLayout();
  });
}

void SettingsWindow::clearStatusMessage() {
  m_statusMessage.clear();
  m_statusIsError = false;
}

void SettingsWindow::clearTransientSettingsState() {
  m_editingWidgetName.clear();
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
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen() && event.type == PointerEvent::Type::Button &&
      event.state == 1) {
    m_widgetAddPopup->close();
    return true;
  }
  if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen() &&
      event.type == PointerEvent::Type::Button && event.state == 1) {
    m_configExportDialogPopup->close();
    return true;
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen() && event.type == PointerEvent::Type::Button &&
      event.state == 1) {
    m_searchPickerPopup->close();
    return true;
  }
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->onPointerEvent(event)) {
    return true;
  }
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen() &&
      event.type == PointerEvent::Type::Button && event.state == 1) {
    m_sessionActionsEditorPopup->close();
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
  if (m_actionsMenuPopup != nullptr && m_actionsMenuPopup->isOpen() && event.type == PointerEvent::Type::Button &&
      event.state == 1) {
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
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
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
      if (pressed && event.button == BTN_LEFT && m_inputDispatcher.hoveredArea() == nullptr &&
          headerDragRegionContains(static_cast<float>(event.sx), static_cast<float>(event.sy))) {
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

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_widgetAddPopup->close();
      return;
    }
    m_widgetAddPopup->onKeyboardEvent(event);
    return;
  }

  if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen()) {
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_configExportDialogPopup->close();
      return;
    }
    m_configExportDialogPopup->onKeyboardEvent(event);
    return;
  }

  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_searchPickerPopup->close();
      return;
    }
    m_searchPickerPopup->onKeyboardEvent(event);
    return;
  }

  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
    if (m_sessionActionsEditorPopup->isSelectDropdownOpen()) {
      m_sessionActionsEditorPopup->onKeyboardEvent(event);
      return;
    }
    if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
      m_sessionActionsEditorPopup->close();
      return;
    }
    m_sessionActionsEditorPopup->onKeyboardEvent(event);
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
    if (!m_editingWidgetName.empty() || !m_renamingWidgetName.empty() || !m_pendingDeleteWidgetName.empty() ||
        !m_pendingDeleteWidgetSettingPath.empty() || !m_creatingBarName.empty() || !m_renamingBarName.empty() ||
        !m_pendingDeleteBarName.empty() || !m_creatingMonitorOverrideBarName.empty() ||
        !m_renamingMonitorOverrideBarName.empty() || !m_pendingDeleteMonitorOverrideBarName.empty()) {
      m_editingWidgetName.clear();
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
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_sceneRoot != nullptr && m_surface != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }
}

void SettingsWindow::onThemeChanged() {
  if (isOpen()) {
    if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
      m_widgetAddPopup->requestRedraw();
    }
    if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen()) {
      m_configExportDialogPopup->requestRedraw();
    }
    if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
      m_sessionActionsEditorPopup->requestRedraw();
    }
    m_surface->requestRedraw();
  }
}

void SettingsWindow::onFontChanged() {
  if (isOpen()) {
    if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
      m_widgetAddPopup->requestLayout();
    }
    if (m_configExportDialogPopup != nullptr && m_configExportDialogPopup->isOpen()) {
      m_configExportDialogPopup->requestLayout();
    }
    if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
      m_sessionActionsEditorPopup->requestLayout();
    }
    m_surface->requestLayout();
  }
}

void SettingsWindow::onExternalOptionsChanged() { requestSceneRebuild(); }

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
