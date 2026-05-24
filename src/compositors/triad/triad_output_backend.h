#pragma once

#include <optional>
#include <string>

namespace compositors::triad {
  class TriadRuntime;
} // namespace compositors::triad

class TriadOutputBackend {
public:
  explicit TriadOutputBackend(compositors::triad::TriadRuntime& runtime);

  [[nodiscard]] std::optional<std::string> focusedOutputName() const;

private:
  compositors::triad::TriadRuntime& m_runtime;
};

namespace compositors::triad {

  [[nodiscard]] bool setOutputPower(TriadRuntime& runtime, bool on);

} // namespace compositors::triad
