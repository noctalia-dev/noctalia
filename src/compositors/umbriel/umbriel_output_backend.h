#pragma once

#include "compositors/umbriel/umbriel_event_handler.h"

#include <optional>
#include <string>
#include <string_view>

namespace compositors::umbriel {
  class UmbrielRuntime;
} // namespace compositors::umbriel

// Caches the Umbriel focused output from the persistent event stream so a query
// is a cache read rather than a blocking IPC round-trip. Umbriel's workspaces
// snapshot marks the single focused workspace, which names the focused output
// even when that workspace is empty.
class UmbrielOutputBackend final : public compositors::umbriel::UmbrielEventHandler {
public:
  explicit UmbrielOutputBackend(compositors::umbriel::UmbrielRuntime& runtime);

  [[nodiscard]] std::optional<std::string> focusedOutputName() const;

  void handleEvent(std::string_view event, const nlohmann::json& data) override;
  void handleStreamReset() override;

private:
  std::optional<std::string> m_focusedOutput;
};

namespace compositors::umbriel {

  [[nodiscard]] bool setOutputPower(UmbrielRuntime& runtime, bool on);

} // namespace compositors::umbriel
