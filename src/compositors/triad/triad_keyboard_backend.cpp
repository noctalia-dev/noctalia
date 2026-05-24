#include "compositors/triad/triad_keyboard_backend.h"

#include "compositors/triad/triad_runtime.h"

#include <json.hpp>

namespace {

  [[nodiscard]] const nlohmann::json* triadState(const nlohmann::json& response) {
    if (!response.is_object()) {
      return nullptr;
    }
    const auto triadIt = response.find("triad");
    if (triadIt == response.end() || !triadIt->is_object()) {
      return nullptr;
    }
    const auto stateIt = triadIt->find("state");
    return stateIt != triadIt->end() && stateIt->is_object() ? &(*stateIt) : nullptr;
  }

  [[nodiscard]] std::optional<KeyboardLayoutState> parseLayoutState(const nlohmann::json& response) {
    const auto* stateJson = triadState(response);
    if (stateJson == nullptr) {
      return std::nullopt;
    }

    const auto namesIt = stateJson->find("keyboard_layouts");
    const auto currentIt = stateJson->find("current_keyboard_layout_idx");
    if (namesIt == stateJson->end() || !namesIt->is_array() || currentIt == stateJson->end() ||
        !currentIt->is_number_integer()) {
      return std::nullopt;
    }

    KeyboardLayoutState state;
    state.currentIndex = currentIt->get<int>();
    state.names.reserve(namesIt->size());
    for (const auto& entry : *namesIt) {
      if (!entry.is_string()) {
        return std::nullopt;
      }
      state.names.push_back(entry.get<std::string>());
    }

    if (state.currentIndex < 0 || state.currentIndex >= static_cast<int>(state.names.size())) {
      return std::nullopt;
    }
    return state;
  }

} // namespace

TriadKeyboardBackend::TriadKeyboardBackend(compositors::triad::TriadRuntime& runtime) : m_runtime(runtime) {}

bool TriadKeyboardBackend::isAvailable() const noexcept { return m_runtime.available(); }

bool TriadKeyboardBackend::cycleLayout() const {
  return isAvailable() && m_runtime.requestAction("switch-keyboard-layout", nlohmann::json{{"layout", "next"}});
}

std::optional<KeyboardLayoutState> TriadKeyboardBackend::layoutState() const {
  if (!isAvailable()) {
    return std::nullopt;
  }
  const auto response = m_runtime.requestJson("state");
  return response.has_value() ? parseLayoutState(*response) : std::nullopt;
}

std::optional<std::string> TriadKeyboardBackend::currentLayoutName() const {
  const auto state = layoutState();
  if (!state.has_value() || state->currentIndex < 0 || state->currentIndex >= static_cast<int>(state->names.size())) {
    return std::nullopt;
  }
  return state->names[static_cast<std::size_t>(state->currentIndex)];
}
