#pragma once

#include "compositors/keyboard_backend.h"
#include "compositors/niri/niri_event_handler.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::niri {
  class NiriRuntime;
} // namespace compositors::niri

class NiriKeyboardBackend : public compositors::niri::NiriEventHandler {
public:
  using ChangeCallback = std::function<void()>;

  explicit NiriKeyboardBackend(compositors::niri::NiriRuntime& runtime);

  [[nodiscard]] bool isAvailable() const noexcept;
  [[nodiscard]] bool cycleLayout() const;
  [[nodiscard]] std::optional<KeyboardLayoutState> layoutState() const;
  [[nodiscard]] std::optional<std::string> currentLayoutName() const;

  void setChangeCallback(ChangeCallback callback);
  void handleEvent(std::string_view key, const nlohmann::json& value) override;

private:
  ChangeCallback m_changeCallback;
};
