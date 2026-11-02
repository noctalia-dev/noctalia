#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace compositors::sway {
  class SwayRuntime;
} // namespace compositors::sway

class SwayOutputBackend {
public:
  explicit SwayOutputBackend(compositors::sway::SwayRuntime& runtime);

  [[nodiscard]] std::optional<std::string> focusedOutputName() const;
  [[nodiscard]] std::unordered_map<std::string, bool> outputPowerStates() const;

private:
  compositors::sway::SwayRuntime& m_runtime;
};

namespace compositors::sway {

  [[nodiscard]] bool setOutputPower(const SwayRuntime& runtime, bool on);

} // namespace compositors::sway
