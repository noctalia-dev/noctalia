#include "compositors/niri/niri_keyboard_backend.h"

#include "compositors/niri/niri_runtime.h"

#include <json.hpp>

namespace {

  std::optional<KeyboardLayoutState> parseLayoutState(const nlohmann::json& response) {
    if (!response.is_object()) {
      return std::nullopt;
    }

    const auto okIt = response.find("Ok");
    if (okIt == response.end() || !okIt->is_object()) {
      return std::nullopt;
    }

    const auto layoutsIt = okIt->find("KeyboardLayouts");
    if (layoutsIt == okIt->end() || !layoutsIt->is_object()) {
      return std::nullopt;
    }

    const auto namesIt = layoutsIt->find("names");
    const auto currentIt = layoutsIt->find("current_idx");
    if (namesIt == layoutsIt->end() || !namesIt->is_array() || currentIt == layoutsIt->end() ||
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

NiriKeyboardBackend::NiriKeyboardBackend(compositors::niri::NiriRuntime& runtime) : m_runtime(runtime) {}

bool NiriKeyboardBackend::isAvailable() const noexcept { return m_runtime.available(); }

bool NiriKeyboardBackend::cycleLayout() const {
  if (!isAvailable()) {
    return false;
  }
  return m_runtime.requestAction(
      nlohmann::json{
          {"SwitchLayout", nlohmann::json{{"layout", "Next"}}},
      }
  );
}

std::optional<KeyboardLayoutState> NiriKeyboardBackend::layoutState() const {
  if (!isAvailable()) {
    return std::nullopt;
  }

  const auto response = m_runtime.requestJson("\"KeyboardLayouts\"\n");
  if (!response.has_value()) {
    return std::nullopt;
  }
  return parseLayoutState(*response);
}

std::optional<std::string> NiriKeyboardBackend::currentLayoutName() const {
  const auto state = layoutState();
  if (!state.has_value() || state->currentIndex < 0 || state->currentIndex >= static_cast<int>(state->names.size())) {
    return std::nullopt;
  }
  return state->names[static_cast<std::size_t>(state->currentIndex)];
}
