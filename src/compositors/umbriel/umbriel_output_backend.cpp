#include "compositors/umbriel/umbriel_output_backend.h"

#include "compositors/umbriel/umbriel_runtime.h"
#include "util/string_utils.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

UmbrielOutputBackend::UmbrielOutputBackend(compositors::umbriel::UmbrielRuntime& runtime)
    : compositors::umbriel::UmbrielEventHandler(runtime) {}

std::optional<std::string> UmbrielOutputBackend::focusedOutputName() const { return m_focusedOutput; }

void UmbrielOutputBackend::handleEvent(std::string_view event, const nlohmann::json& data) {
  // Full snapshot: follow the single focused workspace, which names the focused
  // output even when that workspace is empty.
  if (event != "workspaces" || !data.is_array()) {
    return;
  }
  for (const auto& entry : data) {
    if (!entry.is_object() || !entry.value("focused", false)) {
      continue;
    }
    const auto outputIt = entry.find("output");
    if (outputIt != entry.end() && outputIt->is_string()) {
      std::string output = StringUtils::trim(outputIt->get<std::string>());
      if (!output.empty()) {
        m_focusedOutput = std::move(output);
      }
    }
    return;
  }
}

void UmbrielOutputBackend::handleStreamReset() { m_focusedOutput.reset(); }

namespace compositors::umbriel {

  bool setOutputPower(UmbrielRuntime& runtime, bool on) { return runtime.requestAction(on ? "dpms-on" : "dpms-off"); }

} // namespace compositors::umbriel
