#include "shell/lockscreen/lockscreen_widgets_controller.h"

#include "ipc/ipc_service.h"
#include "shell/bar/bar.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/desktop/editor/desktop_widgets_editor_types.h"
#include "shell/dock/dock.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/lockscreen/lock_surface.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "shell/lockscreen/lockscreen_widgets_host.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>

namespace {

  constexpr std::string_view kLockscreenWidgetIdPrefix = "lockscreen-widget-";

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

  void normalizeLockscreenWidgetSettings(DesktopWidgetState& widget) {
    clampOpacitySetting(widget, "background_opacity", 0.8);

    if (widget.type == "sticker") {
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

  bool parseLockscreenWidgetCounter(std::string_view id, std::uint64_t& value) {
    if (!id.starts_with(kLockscreenWidgetIdPrefix)) {
      return false;
    }

    const std::string_view suffix = id.substr(kLockscreenWidgetIdPrefix.size());
    if (suffix.empty()) {
      return false;
    }

    value = 0;
    const auto* begin = suffix.data();
    const auto* end = suffix.data() + suffix.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
    return ec == std::errc{} && ptr == end;
  }

  std::string makeLockscreenWidgetId(std::uint64_t counter) {
    return std::format("lockscreen-widget-{:016x}", counter);
  }

} // namespace

LockscreenWidgetsController::LockscreenWidgetsController() = default;

LockscreenWidgetsController::~LockscreenWidgetsController() = default;

void LockscreenWidgetsController::initialize(const LockscreenWidgetsControllerServices& services) {
  m_wayland = &services.widgets.wayland;
  m_config = services.widgets.config;
  m_lockScreen = &services.lockScreen;
  m_bar = &services.bar;
  m_dock = &services.dock;
  m_desktopWidgets = services.desktopWidgets;
  m_renderContext = services.widgets.renderContext;
  m_host = std::make_unique<LockscreenWidgetsHost>();
  m_host->initialize(services.widgets);
  m_editor = std::make_unique<DesktopWidgetsEditor>(DesktopWidgetsEditorProfile::lockscreen());
  m_editor->initialize(services.widgets);
  m_editor->setExitRequestedCallback([this]() { exitEdit(); });
  loadSnapshotFromConfig();
  m_initialized = true;
  applyVisibility();

  if (m_config != nullptr) {
    m_config->addReloadCallback([this]() { handleConfigReload(); }, "lockscreen-widgets");
  }
}

void LockscreenWidgetsController::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "lockscreen-widgets-edit",
      [this](const std::string&) -> std::string {
        if (m_config != nullptr && !m_config->isLockScreenEnabled()) {
          return "error: lock screen disabled\n";
        }
        enterEdit();
        return "ok\n";
      },
      "lockscreen-widgets-edit", "Open the lockscreen widgets editor"
  );

  ipc.registerHandler(
      "lockscreen-widgets-exit",
      [this](const std::string&) -> std::string {
        exitEdit();
        return "ok\n";
      },
      "lockscreen-widgets-exit", "Close the lockscreen widgets editor"
  );

  ipc.registerHandler(
      "lockscreen-widgets-toggle-edit",
      [this](const std::string&) -> std::string {
        if (m_config != nullptr && !m_config->isLockScreenEnabled()) {
          return "error: lock screen disabled\n";
        }
        toggleEdit();
        return "ok\n";
      },
      "lockscreen-widgets-toggle-edit", "Toggle lockscreen widgets edit mode"
  );
}

void LockscreenWidgetsController::onLockStateChanged() { applyVisibility(); }

void LockscreenWidgetsController::onOutputChange() {
  if (!m_initialized || m_lockScreen == nullptr) {
    return;
  }
  normalizeSnapshot();
  if (isEditing()) {
    m_editor->onOutputChange();
  } else if (m_host != nullptr) {
    m_host->onOutputChange(*m_lockScreen);
  }
}

void LockscreenWidgetsController::onSecondTick() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->onSecondTick();
  } else if (m_host != nullptr) {
    m_host->onSecondTick();
  }
}

void LockscreenWidgetsController::requestLayout() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestLayout();
  } else if (m_lockScreen != nullptr) {
    m_lockScreen->requestLayout();
  }
}

void LockscreenWidgetsController::requestRedraw() {
  if (!m_initialized) {
    return;
  }
  if (isEditing()) {
    m_editor->requestRedraw();
  } else if (m_lockScreen != nullptr) {
    m_lockScreen->forEachSurface([](LockSurface& surface) { surface.requestRedraw(); });
  }
}

void LockscreenWidgetsController::enterEdit() {
  if (!m_initialized || m_editor == nullptr || m_host == nullptr || isEditing() || m_lockScreen == nullptr) {
    return;
  }
  if (m_config != nullptr && !m_config->isLockScreenEnabled()) {
    return;
  }
  if (m_lockScreen->isActive()) {
    return;
  }
  if (m_config != nullptr && !m_config->config().lockscreenWidgets.enabled) {
    LockscreenWidgetsConfig enabled = m_config->config().lockscreenWidgets;
    enabled.enabled = true;
    if (m_config->setLockscreenWidgetsState(enabled)) {
      m_snapshot = enabled;
    }
  }
  if (m_desktopWidgets != nullptr) {
    if (m_desktopWidgets->isEditing()) {
      m_desktopWidgets->exitEdit();
    } else {
      m_desktopWidgets->suppressDisplay();
    }
  }
  if (m_bar != nullptr) {
    m_bar->suppressDisplay();
  }
  if (m_dock != nullptr) {
    m_dock->suppressDisplay();
  }
  m_editor->open(toDesktopWidgetsEditorSnapshot(m_snapshot));
  m_host->hide();
}

void LockscreenWidgetsController::exitEdit() {
  if (!isEditing() || m_editor == nullptr || m_lockScreen == nullptr) {
    return;
  }

  m_snapshot = fromDesktopWidgetsEditorSnapshot(m_editor->snapshot());
  normalizeSnapshot();
  applyVisibility();
  (void)m_editor->close();
  saveSnapshotToConfig();
  if (m_desktopWidgets != nullptr) {
    m_desktopWidgets->unsuppressDisplay();
  }
  if (m_bar != nullptr) {
    m_bar->unsuppressDisplay();
  }
  if (m_dock != nullptr) {
    m_dock->unsuppressDisplay();
  }
}

void LockscreenWidgetsController::toggleEdit() {
  if (isEditing()) {
    exitEdit();
  } else {
    enterEdit();
  }
}

bool LockscreenWidgetsController::isEditing() const noexcept { return m_editor != nullptr && m_editor->isOpen(); }

std::optional<LayerPopupParentContext>
LockscreenWidgetsController::popupParentContextForSurface(wl_surface* surface) const {
  if (!isEditing() || m_editor == nullptr) {
    return std::nullopt;
  }
  return m_editor->popupParentContextForSurface(surface);
}

std::optional<LayerPopupParentContext> LockscreenWidgetsController::fallbackPopupParentContext() const {
  if (!isEditing() || m_editor == nullptr) {
    return std::nullopt;
  }
  return m_editor->fallbackPopupParentContext();
}

bool LockscreenWidgetsController::onPointerEvent(const PointerEvent& event) {
  if (isEditing() && m_editor != nullptr) {
    return m_editor->onPointerEvent(event);
  }
  return false;
}

void LockscreenWidgetsController::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isEditing() || m_editor == nullptr) {
    return;
  }
  m_editor->onKeyboardEvent(event);
}

void LockscreenWidgetsController::loadSnapshotFromConfig() {
  if (m_config == nullptr) {
    m_snapshot = LockscreenWidgetsSnapshot{};
    return;
  }
  m_snapshot = m_config->config().lockscreenWidgets;
  const std::size_t widgetCountBefore = m_snapshot.widgets.size();
  normalizeSnapshot();
  if (m_snapshot.widgets.size() > widgetCountBefore) {
    saveSnapshotToConfig();
  }
}

void LockscreenWidgetsController::saveSnapshotToConfig() {
  if (m_config == nullptr) {
    return;
  }
  m_config->setLockscreenWidgetsState(m_snapshot);
}

void LockscreenWidgetsController::applyVisibility() {
  if (!m_initialized || m_host == nullptr || m_config == nullptr || m_lockScreen == nullptr) {
    return;
  }

  if (!m_config->isLockScreenEnabled()) {
    if (isEditing() && m_editor != nullptr) {
      m_snapshot = fromDesktopWidgetsEditorSnapshot(m_editor->close());
      saveSnapshotToConfig();
    }
    m_host->hide();
    return;
  }

  const bool enabled = m_config->config().lockscreenWidgets.enabled;
  if (!enabled) {
    if (isEditing() && m_editor != nullptr) {
      m_snapshot = fromDesktopWidgetsEditorSnapshot(m_editor->close());
      saveSnapshotToConfig();
    }
    m_host->hide();
    return;
  }

  if (isEditing()) {
    return;
  }

  if (!m_lockScreen->isActive()) {
    m_host->hide();
    return;
  }

  m_host->show(m_snapshot, *m_lockScreen);
}

void LockscreenWidgetsController::handleConfigReload() {
  if (!m_initialized) {
    return;
  }

  if (!isEditing()) {
    loadSnapshotFromConfig();
    if (m_host != nullptr && m_lockScreen != nullptr) {
      m_host->rebuild(m_snapshot, *m_lockScreen);
      m_lockScreen->requestLayout();
    }
  } else if (m_editor != nullptr) {
    m_editor->requestLayout();
  }
  applyVisibility();
}

void LockscreenWidgetsController::normalizeSnapshot() {
  if (m_wayland == nullptr) {
    return;
  }

  lockscreen_login_box::ensureWidgets(m_snapshot.widgets, *m_wayland);

  std::uint64_t maxCounter = 0;
  for (const auto& widget : m_snapshot.widgets) {
    if (lockscreen_login_box::isLoginBoxWidget(widget)) {
      continue;
    }
    std::uint64_t counter = 0;
    if (parseLockscreenWidgetCounter(widget.id, counter)) {
      maxCounter = std::max(maxCounter, counter);
    }
  }

  std::unordered_set<std::string> seenIds;
  for (auto& widget : m_snapshot.widgets) {
    if (lockscreen_login_box::isLoginBoxWidget(widget)) {
      lockscreen_login_box::normalizeSettings(widget.settings);
      seenIds.insert(widget.id);
      continue;
    }

    normalizeLockscreenWidgetSettings(widget);

    if (widget.id.empty() || seenIds.contains(widget.id)) {
      const std::uint64_t nextCounter =
          maxCounter == std::numeric_limits<std::uint64_t>::max() ? maxCounter : (maxCounter + 1);
      maxCounter = nextCounter;
      widget.id = makeLockscreenWidgetId(nextCounter);
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
  }
}
