#pragma once

#include "compositors/niri/niri_event_handler.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace compositors::niri {
  class NiriRuntime;
} // namespace compositors::niri

// Caches the niri focused output from the persistent event stream so a query is
// a cache read rather than a blocking IPC round-trip.
class NiriOutputBackend final : public compositors::niri::NiriEventHandler {
public:
  explicit NiriOutputBackend(compositors::niri::NiriRuntime& runtime);

  [[nodiscard]] std::optional<std::string> focusedOutputName() const;

  void handleEvent(std::string_view key, const nlohmann::json& value) override;
  void handleStreamReset() override;

private:
  std::unordered_map<std::uint64_t, std::string> m_workspaceOutputs;
  std::optional<std::string> m_focusedOutput;
};

namespace compositors::niri {

  [[nodiscard]] bool setOutputPower(NiriRuntime& runtime, bool on);

} // namespace compositors::niri
