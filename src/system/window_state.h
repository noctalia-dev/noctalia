#pragma once

// Compositor-confirmed state and placement eligibility are independent. An absent
// optional state means the backend cannot currently report minimise support.
struct WindowMinimizeState {
  bool minimized = false;
  bool minimizable = false;

  bool operator==(const WindowMinimizeState&) const = default;
};
