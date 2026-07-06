#pragma once

#include "compositors/keyboard_backend.h"
#include "hyprland_event_handler.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class HyprlandKeyboardBackend : public compositors::hyprland::HyprlandEventHandler {
public:
  using ChangeCallback = std::function<void()>;

  explicit HyprlandKeyboardBackend(compositors::hyprland::HyprlandRuntime& runtime);
  ~HyprlandKeyboardBackend();

  HyprlandKeyboardBackend(const HyprlandKeyboardBackend&) = delete;
  HyprlandKeyboardBackend& operator=(const HyprlandKeyboardBackend&) = delete;

  [[nodiscard]] bool isAvailable() const noexcept;
  [[nodiscard]] bool cycleLayout() const;
  [[nodiscard]] std::optional<KeyboardLayoutState> layoutState() const;
  [[nodiscard]] std::optional<std::string> currentLayoutName() const;

  void setChangeCallback(ChangeCallback callback);
  void cleanup();
  void notifyCleanup();
  void notifyChanged();

private:
  void seedLayoutFromDevices();
  void handleEvent(std::string_view event, std::string_view data);

  std::string m_currentLayoutName;
  std::string m_mainKeyboardName;
  ChangeCallback m_changeCallback;
};
