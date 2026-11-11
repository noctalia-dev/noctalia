#pragma once

#include "compositors/keyboard_backend.h"
#include "compositors/sway/sway_event_handler.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Layout switches made outside the bar (keybinds, `swaymsg`, `noctalia msg keyboard-layout-cycle`)
// arrive as input events on the runtime's shared IPC connection and reach the change callback.
class SwayKeyboardBackend : public compositors::sway::SwayEventHandler {
public:
  using ChangeCallback = std::function<void()>;

  explicit SwayKeyboardBackend(compositors::sway::SwayRuntime& runtime);

  bool isAvailable() const noexcept;
  bool cycleLayout() const;
  std::optional<KeyboardLayoutState> layoutState() const;
  std::optional<std::string> currentLayoutName() const;
  void setChangeCallback(ChangeCallback callback);

  void handleMessage(std::uint32_t type, std::string_view payload) override;
  void handleBatchEnd() override;
  void handleDisconnect() override;

private:
  void notifyChanged();

  ChangeCallback m_changeCallback;
  bool m_layoutChanged = false;
};
