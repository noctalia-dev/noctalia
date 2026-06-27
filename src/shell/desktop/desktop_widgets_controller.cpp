#include "shell/desktop/desktop_widgets_controller.h"

#include "config/config_service.h"
#include "ipc/ipc_service.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widgets_host.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/desktop/editor/desktop_widgets_editor_types.h"
#include "shell/lockscreen/lockscreen_widgets_controller.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>

namespace {

  constexpr std::string_view kDesktopWidgetIdPrefix = "desktop-widget-";

  void clampOpacitySetting(DesktopWidgetState& widget, const std::string& key, double fallback) {
    const auto it = widget.settings.find(key);
    if (it == widget.settings.end()) {
      return;
    }
    if (const auto* doubleValue = std::get_if<double>(&it->second)) {
      widget.settings.insert_or_assign(key, std::clamp(*doubleValue, 0.0, 1.0));
      return;
    }
    if (const auto* intValue = std::get_if<std::int64_t>(&it->second)) {
      widget.settings.insert_or_assign(key, std::clamp(static_cast<double>(*intValue), 0.0, 1.0));
      return;
    }
    widget.settings.insert_or_assign(key, fallback);
  }

  void normalizeDesktopWidgetSettings(DesktopWidgetState& widget) {
    clampOpacitySetting(widget, "background_opacity", 0.8);

    if (widget.type == "sticker" || widget.type == "label") {
      const auto opacityIt = widget.settings.find("opacity");
      if (opacityIt == widget.settings.end()) {
        widget.settings.insert_or_assign("opacity", 1.0);
        return;
      }
      if (const auto* doubleValue = std::get_if<double>(&opacityIt->second)) {
        widget.settings.insert_or_assign("opacity", std::clamp(*doubleValue, 0.0, 1.0));
        return;
      }
      if (const auto* intValue = std::get_if<std::int64_t>(&opacityIt->second)) {
        const double clamped = std::clamp(static_cast<double>(*intValue), 0.0, 1.0);
        widget.settings.insert_or_assign("opacity", clamped);
        return;
      }
      widget.settings.insert_or_assign("opacity", 1.0);
      return;
    }

    if (widget.type != "audio_visualizer") {
      return;
    }

    widget.settings.erase("aspect_ratio"); // setting removed; drop stale key
    widget.settings.erase("min_value");
  }

  bool parseDesktopWidgetCounter(std::string_view id, std::uint64_t& value) {
    if (!id.starts_with(kDesktopWidgetIdPrefix)) {
      return false;
    }

    const std::string_view suffix = id.substr(kDesktopWidgetIdPrefix.size());
    if (suffix.empty()) {
      return false;
    }

    value = 0;
    const auto* begin = suffix.data();
    const auto* end = suffix.data() + suffix.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
    return ec == std::errc{} && ptr == end;
  }

  std::string makeDesktopWidgetId(std::uint64_t counter) { return std::format("desktop-widget-{:016x}", counter); }

} // namespace

DesktopWidgetsController::DesktopWidgetsController() = default;

DesktopWidgetsController::~DesktopWidgetsController() = default;

void DesktopWidgetsController::initialize(const DesktopWidgetsControllerServices& services) {
  m_wayland = &services.widgets.wayland;
  m_config = services.widgets.config;
  m_lockscreenWidgets = services.lockscreenWidgets;
  m_renderContext = services.widgets.renderContext;
  m_host = std::make_unique<DesktopWidgetsHost>();
  m_host->initialize(services.widgets);
  m_editor = std::make_unique<DesktopWidgetsEditor>(DesktopWidgetsEditorProfile::desktop());
  m_editor->initialize(services.widgets);
  m_editor->setExitRequestedCallback([this]() { exitEdit(); });
  loadSnapshotFromConfig();
  m_initialized = true;
  if (m_config != nullptr) {
    m_lastEnabled = m_config->config().desktopWidgets.enabled;
  }
  applyVisibility();

  if (m_config != nullptr) {
    m_config->addReloadCallback([this]() { handleConfigReload(); }, "desktop-widgets");
  }
}

void DesktopWidgetsController::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "desktop-widgets-edit",
      [this](const std::string&) -> std::string {
        enterEdit();
        return "ok\n";
      },
      "desktop-widgets-edit", "Open the desktop widgets editor"
  );

  ipc.registerHandler(
      "desktop-widgets-exit",
      [this](const std::string&) -> std::string {
        exitEdit();
        return "ok\n";
      },
      "desktop-widgets-exit", "Close the desktop widgets editor"
  );

  ipc.registerHandler(
      "desktop-widgets-toggle-edit",
      [this](const std::string&) -> std::string {
        toggleEdit();
        return "ok\n";
      },
      "desktop-widgets-toggle-edit", "Toggle desktop widgets edit mode"
  );

  // Ephemeral runtime show/hide override layered on top of the saved `desktop_widgets.enabled`
  // setting (bidirectional version of bar-show/bar-hide/bar-toggle). These never touch settings.toml
  // -- they flip a runtime override that resets on restart -- so a peek-desktop keybind can reveal
  // widgets on demand without rewriting the user's saved preference on every keypress. `show` is a
  // force-show: it reveals widgets even when the saved setting is disabled, so an opt-in workflow
  // (saved default off, revealed only on demand) works without persisting transient state.
  ipc.registerHandler(
      "desktop-widgets-show",
      [this](const std::string&) -> std::string {
        setRuntimeVisibility(RuntimeVisibility::ForceShown);
        return "ok\n";
      },
      "desktop-widgets-show", "Show desktop widgets now (runtime only; does not change the saved setting)"
  );

  ipc.registerHandler(
      "desktop-widgets-hide",
      [this](const std::string&) -> std::string {
        setRuntimeVisibility(RuntimeVisibility::ForceHidden);
        return "ok\n";
      },
      "desktop-widgets-hide", "Hide desktop widgets now (runtime only; does not change the saved setting)"
  );

  ipc.registerHandler(
      "desktop-widgets-toggle",
      [this](const std::string&) -> std::string {
        toggleRuntimeVisibility();
        return isEffectivelyVisible() ? "shown\n" : "hidden\n";
      },
      "desktop-widgets-toggle", "Toggle desktop widgets visibility (runtime only; does not change the saved setting)"
  );
}

bool DesktopWidgetsController::runtimeWantsVisible() const noexcept {
  switch (m_runtimeVisibility) {
  case RuntimeVisibility::ForceShown:
    return true;
  case RuntimeVisibility::ForceHidden:
    return false;
  case RuntimeVisibility::FollowConfig:
    return m_config != nullptr && m_config->config().desktopWidgets.enabled;
  }
  return false;
}

void DesktopWidgetsController::setRuntimeVisibility(RuntimeVisibility visibility) {
  if (m_runtimeVisibility == visibility) {
    return;
  }
  m_runtimeVisibility = visibility;
  applyVisibility();
}

void DesktopWidgetsController::toggleRuntimeVisibility() {
  setRuntimeVisibility(isEffectivelyVisible() ? RuntimeVisibility::ForceHidden : RuntimeVisibility::ForceShown);
}

bool DesktopWidgetsController::isEffectivelyVisible() const noexcept {
  if (!m_initialized) {
    return false;
  }
  return runtimeWantsVisible() && !m_displaySuppressed && !isEditing();
}

void DesktopWidgetsController::onOutputChange() {
  if (!m_initialized) {
    return;
  }
  normalizeSnapshot();
  if (isEditing()) {
    m_editor->onOutputChange();
  } else if (m_host != nullptr) {
    m_host->onOutputChange();
  }
}

void DesktopWidgetsController::onSecondTick() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->onSecondTick();
  } else if (m_host != nullptr) {
    m_host->onSecondTick();
  }
}

void DesktopWidgetsController::requestUpdate() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestUpdate();
  } else if (m_host != nullptr) {
    m_host->requestUpdate();
  }
}

void DesktopWidgetsController::requestLayout() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestLayout();
  } else if (m_host != nullptr) {
    m_host->requestLayout();
  }
}

void DesktopWidgetsController::requestRedraw() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestRedraw();
  } else if (m_host != nullptr) {
    m_host->requestRedraw();
  }
}

void DesktopWidgetsController::enterEdit() {
  if (!m_initialized || m_editor == nullptr || m_host == nullptr || isEditing()) {
    return;
  }
  if (m_lockscreenWidgets != nullptr && m_lockscreenWidgets->isEditing()) {
    m_lockscreenWidgets->exitEdit();
  }
  if (m_config != nullptr && !m_config->config().desktopWidgets.enabled) {
    return;
  }
  if (m_onEnterEdit) {
    m_onEnterEdit();
  }
  // Open the editor before tearing down host widgets so the PipeWire spectrum
  // listener hand-off does not briefly drop to zero listeners (which resets the
  // stream and leaves a new editor instance with empty spectrum values).
  m_editor->open(m_snapshot);
  m_host->hide();
}

void DesktopWidgetsController::exitEdit() {
  if (!isEditing() || m_editor == nullptr) {
    return;
  }

  m_snapshot = m_editor->snapshot();
  normalizeSnapshot();
  m_host->show(m_snapshot);
  (void)m_editor->close();
  saveSnapshotToConfig();
  applyVisibility();
}

void DesktopWidgetsController::toggleEdit() {
  if (isEditing()) {
    exitEdit();
  } else {
    enterEdit();
  }
}

void DesktopWidgetsController::setOnEnterEditCallback(std::function<void()> callback) {
  m_onEnterEdit = std::move(callback);
}

void DesktopWidgetsController::suppressDisplay() {
  m_displaySuppressed = true;
  if (isEditing()) {
    exitEdit();
  } else if (m_host != nullptr) {
    m_host->hide();
  }
}

void DesktopWidgetsController::unsuppressDisplay() {
  m_displaySuppressed = false;
  applyVisibility();
}

bool DesktopWidgetsController::isEditing() const noexcept { return m_editor != nullptr && m_editor->isOpen(); }

std::optional<LayerPopupParentContext>
DesktopWidgetsController::popupParentContextForSurface(wl_surface* surface) const {
  if (!isEditing() || m_editor == nullptr) {
    return std::nullopt;
  }
  return m_editor->popupParentContextForSurface(surface);
}

std::optional<LayerPopupParentContext> DesktopWidgetsController::fallbackPopupParentContext() const {
  if (!isEditing() || m_editor == nullptr) {
    return std::nullopt;
  }
  return m_editor->fallbackPopupParentContext();
}

bool DesktopWidgetsController::onPointerEvent(const PointerEvent& event) {
  if (isEditing() && m_editor != nullptr) {
    return m_editor->onPointerEvent(event);
  }
  if (m_host != nullptr) {
    return m_host->onPointerEvent(event);
  }
  return false;
}

void DesktopWidgetsController::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isEditing() || m_editor == nullptr) {
    return;
  }
  m_editor->onKeyboardEvent(event);
}

void DesktopWidgetsController::loadSnapshotFromConfig() {
  if (m_config == nullptr) {
    m_snapshot = DesktopWidgetsSnapshot{};
    return;
  }
  m_snapshot = m_config->config().desktopWidgets;
  normalizeSnapshot();
}

void DesktopWidgetsController::saveSnapshotToConfig() {
  if (m_config == nullptr) {
    return;
  }
  m_config->setDesktopWidgetsState(m_snapshot);
}

void DesktopWidgetsController::applyVisibility() {
  if (!m_initialized || m_host == nullptr || m_config == nullptr) {
    return;
  }

  // The runtime override resolves against the saved setting: ForceShown reveals widgets even when the
  // setting is disabled, ForceHidden suppresses them even when enabled, FollowConfig honors it.
  if (!runtimeWantsVisible()) {
    if (isEditing() && m_editor != nullptr) {
      m_snapshot = m_editor->close();
      saveSnapshotToConfig();
    }
    m_host->hide();
    return;
  }

  if (m_displaySuppressed || isEditing()) {
    m_host->hide();
    return;
  }

  m_host->show(m_snapshot);
}

void DesktopWidgetsController::handleConfigReload() {
  if (!m_initialized) {
    return;
  }

  // An explicit change to the saved enable toggle cancels any IPC runtime override, so the settings
  // UI takes back control. Gated on an actual transition: unrelated reloads leave the override intact.
  if (m_config != nullptr) {
    const bool enabled = m_config->config().desktopWidgets.enabled;
    if (enabled != m_lastEnabled) {
      m_lastEnabled = enabled;
      m_runtimeVisibility = RuntimeVisibility::FollowConfig;
    }
  }

  if (!isEditing()) {
    loadSnapshotFromConfig();
    if (m_host != nullptr) {
      m_host->rebuild(m_snapshot);
    }
  }
  applyVisibility();
}

void DesktopWidgetsController::normalizeSnapshot() {
  if (m_wayland == nullptr) {
    return;
  }

  std::uint64_t maxCounter = 0;
  for (const auto& widget : m_snapshot.widgets) {
    std::uint64_t counter = 0;
    if (parseDesktopWidgetCounter(widget.id, counter)) {
      maxCounter = std::max(maxCounter, counter);
    }
  }

  std::unordered_set<std::string> seenIds;
  for (auto& widget : m_snapshot.widgets) {
    normalizeDesktopWidgetSettings(widget);

    if (widget.id.empty() || seenIds.contains(widget.id)) {
      const std::uint64_t nextCounter =
          maxCounter == std::numeric_limits<std::uint64_t>::max() ? maxCounter : (maxCounter + 1);
      maxCounter = nextCounter;
      widget.id = makeDesktopWidgetId(nextCounter);
    }
    seenIds.insert(widget.id);

    if (widget.outputName.empty()) {
      const WaylandOutput* output = desktop_widgets::resolveEffectiveOutput(*m_wayland, widget.outputName);
      if (output != nullptr) {
        widget.outputName = desktop_widgets::outputKey(*output);
      }
      continue;
    }

    if (const WaylandOutput* exact = desktop_widgets::findOutputByKey(*m_wayland, widget.outputName);
        exact != nullptr) {
      widget.outputName = desktop_widgets::outputKey(*exact);
    }
    // cx/cy clamping is owned by the editor (during drag) and the host (on widget creation and
    // prepareFrame). Both of those paths know the widget's actual intrinsic size; clamping here
    // with an estimate can push widgets that the editor had legitimately placed at the edge.
  }
}
