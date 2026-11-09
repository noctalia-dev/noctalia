#pragma once

#include "compositors/keyboard_backend.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace compositors::sway {
  class SwayRuntime;
} // namespace compositors::sway

class SwayKeyboardBackend {
public:
  using ChangeCallback = std::function<void()>;

  explicit SwayKeyboardBackend(compositors::sway::SwayRuntime& runtime);
  ~SwayKeyboardBackend();

  SwayKeyboardBackend(const SwayKeyboardBackend&) = delete;
  SwayKeyboardBackend& operator=(const SwayKeyboardBackend&) = delete;
  SwayKeyboardBackend(SwayKeyboardBackend&&) = delete;
  SwayKeyboardBackend& operator=(SwayKeyboardBackend&&) = delete;

  bool isAvailable() const noexcept;
  bool cycleLayout() const;
  std::optional<KeyboardLayoutState> layoutState() const;
  std::optional<std::string> currentLayoutName() const;

  // Subscribes to sway's input events so layout switches made outside the bar (keybinds,
  // `swaymsg`, `noctalia msg keyboard-layout-cycle`) reach the change callback.
  bool connectSocket();
  void setChangeCallback(ChangeCallback callback);
  int pollFd() const noexcept { return m_socketFd; }
  void dispatchPoll(short revents);
  void cleanup();

private:
  bool sendMessage(std::uint32_t type, const std::string& payload);
  void readSocket();
  void parseMessages();
  void handleMessage(std::uint32_t type, const std::string& payload);
  void disconnect();

  compositors::sway::SwayRuntime& m_runtime;
  int m_socketFd = -1;
  std::vector<char> m_readBuffer;
  ChangeCallback m_changeCallback;
};
