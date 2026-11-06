#pragma once

#include <functional>
#include <optional>
#include <string>

// Transport-independent text clipboard. The application supplies cached text or an asynchronous
// provider; the library owns neither the Wayland transport nor clipboard history.
class TextClipboard {
public:
  virtual ~TextClipboard() = default;

  // Most recent plain-text clipboard selection currently known, if any.
  virtual std::optional<std::string> clipboardText() = 0;

  // Replace the system clipboard selection with the given text.
  virtual void setClipboardText(std::string text) = 0;

  // Complete once on the UI thread, inline or later; nullopt means unavailable/failed. Cached
  // providers complete inline by default. Transports needing a read override this without blocking.
  // The by-value callback lets asynchronous overrides take ownership of the request.
  // NOLINTNEXTLINE(performance-unnecessary-value-param)
  virtual void requestClipboardText(std::function<void(std::optional<std::string>)> callback) {
    callback(clipboardText());
  }
};
