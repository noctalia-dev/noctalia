#pragma once

#include "compositors/keyboard_backend.h"

#include <optional>
#include <string>

namespace compositors::sway {
  class SwayRuntime;
} // namespace compositors::sway

class SwayKeyboardBackend {
public:
  explicit SwayKeyboardBackend(compositors::sway::SwayRuntime& runtime);

  bool isAvailable() const noexcept;
  bool cycleLayout() const;
  std::optional<KeyboardLayoutState> layoutState() const;
  std::optional<std::string> currentLayoutName() const;

private:
  compositors::sway::SwayRuntime& m_runtime;
};
