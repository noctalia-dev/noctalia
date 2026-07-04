#include "compositors/niri/niri_output_backend.h"

#include "compositors/niri/niri_runtime.h"
#include "util/string_utils.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

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

  [[nodiscard]] std::optional<std::uint64_t> parseWorkspaceId(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
      return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
      const auto signedId = value.get<std::int64_t>();
      if (signedId >= 0) {
        return static_cast<std::uint64_t>(signedId);
      }
    }
    return std::nullopt;
  }

} // namespace

NiriOutputBackend::NiriOutputBackend(compositors::niri::NiriRuntime& runtime)
    : compositors::niri::NiriEventHandler(runtime) {}

std::optional<std::string> NiriOutputBackend::focusedOutputName() const { return m_focusedOutput; }

void NiriOutputBackend::handleEvent(std::string_view key, const nlohmann::json& value) {
  // niri sends a full WorkspacesChanged snapshot on subscription and whenever the
  // workspace set changes; each workspace carries its output and is_focused flag.
  if (key == "WorkspacesChanged") {
    const auto workspacesIt = value.is_object() ? value.find("workspaces") : value.end();
    if (workspacesIt == value.end() || !workspacesIt->is_array()) {
      return;
    }

    m_workspaceOutputs.clear();
    for (const auto& workspace : *workspacesIt) {
      if (!workspace.is_object()) {
        continue;
      }
      const auto idIt = workspace.find("id");
      if (idIt == workspace.end()) {
        continue;
      }
      const auto id = parseWorkspaceId(*idIt);
      if (!id.has_value()) {
        continue;
      }

      auto output = trimOutputName(workspace.value("output", nlohmann::json{}));
      if (!output.has_value()) {
        continue;
      }
      const auto focusedIt = workspace.find("is_focused");
      const bool focused = focusedIt != workspace.end() && focusedIt->is_boolean() && focusedIt->get<bool>();
      if (focused) {
        m_focusedOutput = *output;
      }
      m_workspaceOutputs[*id] = std::move(*output);
    }
    return;
  }

  // Focus moving between workspaces/outputs arrives incrementally; resolve the
  // activated workspace's output from the cached map.
  if (key == "WorkspaceActivated") {
    if (!value.is_object()) {
      return;
    }
    const auto focusedIt = value.find("focused");
    if (focusedIt == value.end() || !focusedIt->is_boolean() || !focusedIt->get<bool>()) {
      return;
    }
    const auto idIt = value.find("id");
    if (idIt == value.end()) {
      return;
    }
    const auto id = parseWorkspaceId(*idIt);
    if (!id.has_value()) {
      return;
    }
    if (const auto it = m_workspaceOutputs.find(*id); it != m_workspaceOutputs.end()) {
      m_focusedOutput = it->second;
    }
  }
}

void NiriOutputBackend::handleStreamReset() {
  m_workspaceOutputs.clear();
  m_focusedOutput.reset();
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
