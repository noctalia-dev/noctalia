#pragma once

#include <functional>
#include <optional>
#include <string>

// Minimal text-clipboard abstraction. UI controls (e.g. Input) copy and paste
// through this interface so they stay agnostic of the Wayland data-control
// transport and the clipboard-history service that implements it.
class TextClipboard {
public:
  virtual ~TextClipboard() = default;

  // Most recent plain-text clipboard selection currently known, if any.
  [[nodiscard]] virtual std::optional<std::string> clipboardText() = 0;

  // Asynchronously read the current clipboard text selection. Synchronous
  // implementations default to forwarding clipboardText().
  virtual void requestClipboardText(std::function<void(std::optional<std::string>)> callback) {
    if (callback) {
      callback(clipboardText());
    }
  }

  // Replace the system clipboard selection with the given text.
  virtual void setClipboardText(std::string text) = 0;
};
