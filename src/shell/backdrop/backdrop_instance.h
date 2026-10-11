#pragma once

#include "render/core/shared_texture_cache.h"

#include <cstdint>
#include <memory>
#include <string>

struct wl_output;
class BackdropSurface;

struct BackdropInstance {
  std::uint32_t outputName = 0;
  struct wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::string connectorName;

  std::unique_ptr<BackdropSurface> surface;

  // Declared after surface so the leases release before the upload backend they may reference goes away.
  std::string currentPath;
  SharedTextureCache::Lease currentLease;
  // A newer wallpaper still decoding; currentLease stays visible until it is ready.
  std::string pendingPath;
  SharedTextureCache::Lease pendingLease;
};
