#pragma once

#include "compositors/keyboard_backend.h"

#include <optional>
#include <string>

namespace compositors::triad {
  class TriadRuntime;
} // namespace compositors::triad

class TriadKeyboardBackend {
public:
  explicit TriadKeyboardBackend(compositors::triad::TriadRuntime& runtime);

  [[nodiscard]] bool isAvailable() const noexcept;
  [[nodiscard]] bool cycleLayout() const;
  [[nodiscard]] std::optional<KeyboardLayoutState> layoutState() const;
  [[nodiscard]] std::optional<std::string> currentLayoutName() const;

private:
  compositors::triad::TriadRuntime& m_runtime;
};
