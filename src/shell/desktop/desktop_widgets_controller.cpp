#include "shell/desktop/desktop_widgets_controller.h"

#include "ipc/ipc_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widgets_editor.h"
#include "shell/desktop/desktop_widgets_host.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>

namespace {

  constexpr std::string_view kDesktopWidgetIdPrefix = "desktop-widget-";
  constexpr float kDefaultDesktopAudioVisualizerAspectRatio = 240.0f / 96.0f;

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

    if (widget.type == "sticker") {
      const auto opacityIt = widget.settings.find("opacity");
      if (opacityIt == widget.settings.end()) {
        widget.settings.insert_or_assign("opacity", static_cast<double>(1.0));
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
      widget.settings.insert_or_assign("opacity", static_cast<double>(1.0));
      return;
    }

    if (widget.type != "audio_visualizer") {
      return;
    }

    bool hasValidAspectRatio = false;
    const auto it = widget.settings.find("aspect_ratio");
    if (it != widget.settings.end()) {
      if (const auto* doubleValue = std::get_if<double>(&it->second); doubleValue != nullptr && *doubleValue > 0.0) {
        hasValidAspectRatio = true;
      }
      if (const auto* intValue = std::get_if<std::int64_t>(&it->second); intValue != nullptr && *intValue > 0) {
        hasValidAspectRatio = true;
      }
    }

    if (!hasValidAspectRatio) {
      widget.settings.insert_or_assign("aspect_ratio", static_cast<double>(kDefaultDesktopAudioVisualizerAspectRatio));
    }
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

void DesktopWidgetsController::initialize(
    WaylandConnection& wayland, ConfigService* config, PipeWireSpectrum* pipewireSpectrum,
    const WeatherService* weather, RenderContext* renderContext, MprisService* mpris, HttpClient* httpClient,
    SystemMonitorService* sysmon
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  m_host = std::make_unique<DesktopWidgetsHost>();
  m_host->initialize(wayland, config, pipewireSpectrum, weather, renderContext, mpris, httpClient, sysmon);
  m_editor = std::make_unique<DesktopWidgetsEditor>();
  m_editor->initialize(wayland, config, pipewireSpectrum, weather, renderContext, mpris, httpClient, sysmon);
  m_editor->setExitRequestedCallback([this]() { exitEdit(); });
  loadSnapshotFromConfig();
  m_initialized = true;
  applyVisibility();

  if (m_config != nullptr) {
    m_config->addReloadCallback([this]() { handleConfigReload(); });
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
  if (m_config != nullptr && !m_config->config().desktopWidgets.enabled) {
    return;
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

bool DesktopWidgetsController::isEditing() const noexcept { return m_editor != nullptr && m_editor->isOpen(); }

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

  const bool enabled = m_config->config().desktopWidgets.enabled;
  if (!enabled) {
    if (isEditing() && m_editor != nullptr) {
      m_snapshot = m_editor->close();
      saveSnapshotToConfig();
    }
    m_host->hide();
    return;
  }

  if (!isEditing()) {
    m_host->show(m_snapshot);
  }
}

void DesktopWidgetsController::handleConfigReload() {
  if (!m_initialized) {
    return;
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
