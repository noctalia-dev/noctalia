#pragma once

#include <functional>
#include <optional>
#include <poll.h>
#include <string>
#include <vector>

struct KeyboardLayoutState {
  std::vector<std::string> names;
  int currentIndex = -1;
};

class KeyboardLayoutBackend {
public:
  using ChangeCallback = std::function<void()>;

  virtual ~KeyboardLayoutBackend() = default;

  [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
  [[nodiscard]] virtual bool cycleLayout() const = 0;
  [[nodiscard]] virtual std::optional<KeyboardLayoutState> layoutState() const = 0;
  [[nodiscard]] virtual std::optional<std::string> currentLayoutName() const = 0;

  virtual bool connectSocket() { return false; }
  // Overrides take ownership of the callback.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  virtual void setChangeCallback(ChangeCallback /*callback*/) {}
  [[nodiscard]] virtual int pollFd() const noexcept { return -1; }
  [[nodiscard]] virtual short pollEvents() const noexcept { return POLLIN | POLLHUP | POLLERR; }
  virtual void dispatchPoll(short /*revents*/) {}
  virtual void cleanup() {}
};
