#pragma once

#include "compositors/keyboard_backend.h"

#include <optional>
#include <string>

namespace compositors::mango {
  class MangoRuntime;
}

class MangoKeyboardBackend {
public:
  explicit MangoKeyboardBackend(compositors::mango::MangoRuntime& runtime);
  ~MangoKeyboardBackend() = default;

  MangoKeyboardBackend(const MangoKeyboardBackend&) = delete;
  MangoKeyboardBackend& operator=(const MangoKeyboardBackend&) = delete;
  MangoKeyboardBackend(MangoKeyboardBackend&&) = delete;
  MangoKeyboardBackend& operator=(MangoKeyboardBackend&&) = delete;

  bool isAvailable() const noexcept;
  bool cycleLayout() const;
  std::optional<KeyboardLayoutState> layoutState() const;
  std::optional<std::string> currentLayoutName() const;

private:
  compositors::mango::MangoRuntime& m_runtime;
};
