#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wallpaper {

  // A wallpaper change that was requested while a fullscreen hold was active.
  // An empty connector means "every output".
  struct DeferredWallpaperChange {
    std::string path;
    std::string connector;
  };

  // Tracks whether a fullscreen hold is active and what work was deferred while it was.
  // Kept free of Qt and Wayland types so the rules can be unit tested on their own.
  class FullscreenHold {
  public:
    // A hold request: true = a fullscreen, activated, non-minimized toplevel exists.
    // Returns true when the hold state changed.
    bool setFullscreenActive(bool active);

    [[nodiscard]] bool isActive() const noexcept;

    // Defer a wallpaper change (a path plus an optional connector).
    // While the hold is active the change is queued instead of applied.
    // Returns true when the change was queued, false when it was applied straight away.
    bool deferOrApply(std::string path, std::string connector);

    // Changes that were deferred and are now released (the hold lifted).
    // Returns them in the order they were deferred; clears the queue.
    [[nodiscard]] std::vector<DeferredWallpaperChange> release();

  private:
    bool m_active = false;
    std::vector<DeferredWallpaperChange> m_deferred;
  };

} // namespace wallpaper
