#include "compositors/triad/triad_output_backend.h"

#include "compositors/triad/triad_runtime.h"
#include "core/log.h"

#include <json.hpp>
#include <optional>
#include <string>

namespace {

  constexpr Logger kLog("triad_output");

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

  [[nodiscard]] std::optional<std::uint64_t> jsonUnsigned(const nlohmann::json& json) {
    if (json.is_number_unsigned()) {
      return json.get<std::uint64_t>();
    }
    if (json.is_number_integer()) {
      const auto value = json.get<std::int64_t>();
      if (value >= 0) {
        return static_cast<std::uint64_t>(value);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::string> activeOutputFromLayoutState(const nlohmann::json& state) {
    const auto activeIt = state.find("active_workspace_idx");
    const auto workspacesIt = state.find("workspaces");
    if (activeIt == state.end() || workspacesIt == state.end() || !workspacesIt->is_array()) {
      return std::nullopt;
    }
    const auto activeIdx = jsonUnsigned(*activeIt);
    if (!activeIdx.has_value()) {
      return std::nullopt;
    }
    for (const auto& workspace : *workspacesIt) {
      if (!workspace.is_object()) {
        continue;
      }
      const auto idxIt = workspace.find("workspace_idx");
      const auto outputIt = workspace.find("output");
      if (idxIt == workspace.end() || outputIt == workspace.end() || !outputIt->is_string()) {
        continue;
      }
      const auto workspaceIdx = jsonUnsigned(*idxIt);
      if (workspaceIdx.has_value() && *workspaceIdx == *activeIdx) {
        return outputIt->get<std::string>();
      }
    }
    return std::nullopt;
  }

} // namespace

TriadOutputBackend::TriadOutputBackend(compositors::triad::TriadRuntime& runtime) : m_runtime(runtime) {}

std::optional<std::string> TriadOutputBackend::focusedOutputName() const {
  if (auto response = m_runtime.requestJson("state"); response.has_value()) {
    if (const auto* state = triadState(*response); state != nullptr) {
      if (auto output = activeOutputFromLayoutState(*state); output.has_value() && !output->empty()) {
        return output;
      }
      const auto layoutIt = state->find("layout");
      if (layoutIt != state->end() && layoutIt->is_object()) {
        if (auto output = activeOutputFromLayoutState(*layoutIt); output.has_value() && !output->empty()) {
          return output;
        }
      }
    }
  }

  kLog.debug("failed to resolve focused output from triad IPC");
  return std::nullopt;
}

namespace compositors::triad {

  bool setOutputPower(TriadRuntime& runtime, bool on) {
    return runtime.requestAction(on ? "power-on-monitors" : "power-off-monitors");
  }

} // namespace compositors::triad
