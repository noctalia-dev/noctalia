#include "shell/backdrop/backdrop_surface.h"

#include "render/backend/render_backend.h"
#include "wayland/wayland_connection.h"

#include <stdexcept>
#include <wayland-client.h>

BackdropSurface::~BackdropSurface() {
  m_wallpaperRenderer.makeCurrent();
  m_layer.destroy();
}

bool BackdropSurface::createWlSurface() {
  m_surface = wl_compositor_create_surface(m_connection.compositor());
  if (m_surface == nullptr) {
    return false;
  }

  initializeSurfaceScaleProtocol();

  if (m_shared == nullptr) {
    throw std::runtime_error("BackdropSurface requires a GlSharedContext");
  }
  m_wallpaperRenderer.bind(*m_shared, m_surface);
  return true;
}

void BackdropSurface::onConfigure(std::uint32_t width, std::uint32_t height) {
  const auto bw = bufferWidthFor(width);
  const auto bh = bufferHeightFor(height);

  m_bufW = bw;
  m_bufH = bh;

  m_wallpaperRenderer.resize(bw, bh, width, height);
  m_layer.invalidate();

  Surface::onConfigure(width, height);
}

void BackdropSurface::onScaleChanged() {
  if (width() == 0 || height() == 0) {
    return;
  }
  onConfigure(width(), height());
}

void BackdropSurface::render() {
  if (m_surface == nullptr) {
    return;
  }

  m_wallpaperRenderer.makeCurrent();
  m_layer.resize(*m_wallpaperRenderer.backend(), m_bufW, m_bufH);

  if (!m_layer.valid()) {
    return;
  }

  static constexpr int kBlurRounds = 3;
  const auto options = BackdropPostProcessOptions{
      .blurRadius = m_blurIntensity * 40.0f,
      .blurRounds = kBlurRounds,
      .tintColor = rgba(m_tintR, m_tintG, m_tintB, 1.0f),
      .tintIntensity = m_tintIntensity,
  };

  if (!m_layer.dirty()) {
    return;
  }

  m_layer.ensure([&](RenderFramebuffer& target) {
    auto* scratch = m_layer.scratch();
    if (scratch == nullptr) {
      return;
    }
    m_wallpaperRenderer.renderBackdropContent(target, *scratch, options);
  });

  requestFrame();
  m_wallpaperRenderer.presentTexture(m_layer.texture());
}

void BackdropSurface::setBlurIntensity(float v) noexcept {
  if (m_blurIntensity == v) {
    return;
  }
  m_blurIntensity = v;
  m_layer.invalidate();
}

void BackdropSurface::setTintIntensity(float v) noexcept {
  if (m_tintIntensity == v) {
    return;
  }
  m_tintIntensity = v;
  m_layer.invalidate();
}

void BackdropSurface::setTintColor(float r, float g, float b) noexcept {
  if (m_tintR == r && m_tintG == g && m_tintB == b) {
    return;
  }
  m_tintR = r;
  m_tintG = g;
  m_tintB = b;
  m_layer.invalidate();
}

void BackdropSurface::setWallpaperState(TextureId tex, float imgW, float imgH, WallpaperFillMode fillMode) {
  m_wallpaperRenderer.setTransitionState(
      tex, {}, imgW, imgH, 0.0f, 0.0f, 0.0f, WallpaperTransition::Fade, fillMode, TransitionParams{}
  );
  m_layer.invalidate();
}
