#include "compositors/hyprland/hyprland_keyboard_backend.h"

#include "compositors/hyprland/hyprland_runtime.h"

#include <string_view>

HyprlandKeyboardBackend::HyprlandKeyboardBackend(compositors::hyprland::HyprlandRuntime& runtime)
    : compositors::hyprland::HyprlandEventHandler(runtime) {}

HyprlandKeyboardBackend::~HyprlandKeyboardBackend() { cleanup(); }

bool HyprlandKeyboardBackend::isAvailable() const noexcept { return m_runtime.available(); }

bool HyprlandKeyboardBackend::cycleLayout() const { return m_runtime.request("switchxkblayout all next").has_value(); }

std::optional<KeyboardLayoutState> HyprlandKeyboardBackend::layoutState() const {
  const auto current = currentLayoutName();
  if (!current.has_value()) {
    return std::nullopt;
  }
  return KeyboardLayoutState{{*current}, 0};
}

std::optional<std::string> HyprlandKeyboardBackend::currentLayoutName() const {
  if (m_currentLayoutName.empty()) {
    return std::nullopt;
  }
  return m_currentLayoutName;
}

void HyprlandKeyboardBackend::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void HyprlandKeyboardBackend::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void HyprlandKeyboardBackend::notifyCleanup() {
  m_currentLayoutName.clear();
  m_mainKeyboardName.clear();
}

void HyprlandKeyboardBackend::cleanup() { m_runtime.cleanup(); }

void HyprlandKeyboardBackend::seedLayoutFromDevices() {
  const auto json = m_runtime.requestJson("j/devices");
  if (!json || !json->is_object()) {
    return;
  }

  const auto keyboardsIt = json->find("keyboards");
  if (keyboardsIt == json->end() || !keyboardsIt->is_array()) {
    return;
  }

  for (const auto& keyboard : *keyboardsIt) {
    if (!keyboard.is_object() || !keyboard.value("main", false)) {
      continue;
    }
    const std::string layout = keyboard.value("active_keymap", "");
    if (!layout.empty() && layout != "error") {
      m_currentLayoutName = layout;
      m_mainKeyboardName = keyboard.value("name", "");
      return;
    }
  }
}

void HyprlandKeyboardBackend::handleEvent(std::string_view event, std::string_view data) {

  if (event != "activelayout") {
    return;
  }

  const auto comma = data.find(',');
  if (comma == std::string_view::npos || comma + 1 >= data.size()) {
    return;
  }

  const std::string_view keyboardName = data.substr(0, comma);
  const std::string_view layoutName = data.substr(comma + 1);

  if (!m_mainKeyboardName.empty() && keyboardName != m_mainKeyboardName) {
    return;
  }

  m_currentLayoutName = std::string(layoutName);
  notifyChanged();
}
