#pragma once

#include "render/core/cached_layer.h"
#include "render/core/texture_manager.h"

#include <cstdint>

class RenderBackend;

class BlurCache {
public:
  BlurCache() = default;
  ~BlurCache();

  BlurCache(const BlurCache&) = delete;
  BlurCache& operator=(const BlurCache&) = delete;

  TextureHandle
  get(RenderBackend& backend, TextureHandle source, std::uint32_t width, std::uint32_t height, float radius = 20.0f,
      int rounds = 3);

  void invalidate() { m_layer.invalidate(); }
  void destroy();

private:
  CachedLayer m_layer;
  RenderBackend* m_backend = nullptr;
  TextureId m_lastSourceTex;
  float m_lastRadius = 0.0f;
  int m_lastRounds = 0;
};
