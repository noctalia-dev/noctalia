#pragma once

#include "render/core/cached_layer.h"
#include "render/wallpaper_renderer.h"
#include "wayland/layer_surface.h"

#include <cstdint>

class GlSharedContext;

class BackdropSurface : public LayerSurface {
public:
  using LayerSurface::LayerSurface;
  ~BackdropSurface() override;

  void setSharedGl(GlSharedContext* shared) noexcept { m_shared = shared; }
  void setBlurIntensity(float v) noexcept;
  void setTintIntensity(float v) noexcept;
  void setTintColor(float r, float g, float b) noexcept;
  void setWallpaperState(TextureId tex, float imgW, float imgH, WallpaperFillMode fillMode);
  void onGpuResourcesInvalidated();

  [[nodiscard]] WallpaperRenderer* wallpaperRenderer() noexcept { return &m_wallpaperRenderer; }

protected:
  bool createWlSurface() override;
  void onConfigure(std::uint32_t width, std::uint32_t height) override;
  void render() override;
  void onScaleChanged() override;

private:
  WallpaperRenderer m_wallpaperRenderer;
  CachedLayer m_layer;

  std::uint32_t m_bufW = 0;
  std::uint32_t m_bufH = 0;

  GlSharedContext* m_shared = nullptr;
  float m_blurIntensity = 0.5f;
  float m_tintIntensity = 0.3f;
  float m_tintR = 0.0f;
  float m_tintG = 0.0f;
  float m_tintB = 0.0f;
};
