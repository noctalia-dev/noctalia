#include "compositors/niri/niri_output_backend.h"

#include "compositors/niri/niri_runtime.h"
#include "util/string_utils.h"

#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace {

  constexpr std::string_view kFocusedOutputRequest = "\"FocusedOutput\"\n";

  [[nodiscard]] std::optional<std::string> trimOutputName(const nlohmann::json& value) {
    if (value.is_string()) {
      const auto trimmed = StringUtils::trim(value.get<std::string>());
      return trimmed.empty() ? std::nullopt : std::optional<std::string>{trimmed};
    }
    if (value.is_object()) {
      if (auto it = value.find("name"); it != value.end()) {
        return trimOutputName(*it);
      }
    }
    return std::nullopt;
  }

  // niri IPC returns one JSON line per request: {"Ok": ...} or {"Err": ...}
  [[nodiscard]] std::optional<std::string> parseFocusedOutputName(const nlohmann::json& json) {
    if (!json.is_object()) {
      return std::nullopt;
    }
    if (json.contains("Err")) {
      return std::nullopt;
    }
    const auto okIt = json.find("Ok");
    if (okIt == json.end() || !okIt->is_object()) {
      return std::nullopt;
    }
    const auto focusedIt = okIt->find("FocusedOutput");
    if (focusedIt == okIt->end() || focusedIt->is_null()) {
      return std::nullopt;
    }
    return trimOutputName(*focusedIt);
  }

} // namespace

NiriOutputBackend::NiriOutputBackend(compositors::niri::NiriRuntime& runtime) : m_runtime(runtime) {}

std::optional<std::string> NiriOutputBackend::focusedOutputName() const {
  if (auto response = m_runtime.requestJson(kFocusedOutputRequest); response.has_value()) {
    if (auto parsed = parseFocusedOutputName(*response); parsed.has_value()) {
      return parsed;
    }
  }

  return std::nullopt;
}

namespace compositors::niri {

  bool setOutputPower(NiriRuntime& runtime, bool on) {
    return runtime.requestAction(
        nlohmann::json{
            {on ? "PowerOnMonitors" : "PowerOffMonitors", nlohmann::json::object()},
        }
    );
  }

} // namespace compositors::niri
