#pragma once

#include "compositors/keyboard_backend.h"
#include "compositors/umbriel/umbriel_event_handler.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::umbriel {
  class UmbrielRuntime;
} // namespace compositors::umbriel

class UmbrielKeyboardBackend : public compositors::umbriel::UmbrielEventHandler {
public:
  using ChangeCallback = std::function<void()>;

  explicit UmbrielKeyboardBackend(compositors::umbriel::UmbrielRuntime& runtime);

  [[nodiscard]] bool isAvailable() const noexcept;
  [[nodiscard]] bool cycleLayout() const;
  [[nodiscard]] std::optional<KeyboardLayoutState> layoutState() const;
  [[nodiscard]] std::optional<std::string> currentLayoutName() const;

  void setChangeCallback(ChangeCallback callback);
  void handleEvent(std::string_view event, const nlohmann::json& data) override;

private:
  ChangeCallback m_changeCallback;
};
