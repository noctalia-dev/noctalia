#include "shell/wallpaper/wallpaper_fullscreen_hold.h"

#include <algorithm>
#include <utility>

namespace wallpaper {

  bool FullscreenHold::setFullscreenActive(bool active) {
    if (m_active == active) {
      return false;
    }
    m_active = active;
    return true;
  }

  bool FullscreenHold::isActive() const noexcept { return m_active; }

  bool FullscreenHold::deferOrApply(std::string path, std::string connector) {
    if (!m_active) {
      return false;
    }

    // A repeated manual set of the same wallpaper must not pile up: the newest
    // entry already represents that intent, so refresh it in place.
    if (!m_deferred.empty()) {
      auto& newest = m_deferred.back();
      if (newest.path == path && newest.connector == connector) {
        return true;
      }
    }

    m_deferred.push_back(DeferredWallpaperChange{.path = std::move(path), .connector = std::move(connector)});
    return true;
  }

  std::vector<DeferredWallpaperChange> FullscreenHold::release() {
    std::vector<DeferredWallpaperChange> released = std::move(m_deferred);
    m_deferred.clear();
    return released;
  }

} // namespace wallpaper
