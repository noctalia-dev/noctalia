#pragma once

#include "render/backend/gles_texture_manager.h"
#include "render/backend/render_backend.h"
#include "render/core/shader_program.h"
#include "render/programs/audio_spectrum_program.h"
#include "render/programs/blur_program.h"
#include "render/programs/effect_program.h"
#include "render/programs/glyph_program.h"
#include "render/programs/graph_program.h"
#include "render/programs/image_program.h"
#include "render/programs/rect_program.h"
#include "render/programs/screen_corner_program.h"
#include "render/programs/spinner_program.h"
#include "render/programs/wallpaper_program.h"

#include <EGL/egl.h>

class GlesRenderBackend final : public RenderBackend {
public:
  GlesRenderBackend() = default;
  ~GlesRenderBackend() override;

  GlesRenderBackend(const GlesRenderBackend&) = delete;
  GlesRenderBackend& operator=(const GlesRenderBackend&) = delete;

  void initialize(GlSharedContext& shared) override;
  void cleanup() override;

  void makeCurrent(RenderTarget& target) override;
  void makeCurrentNoSurface() override;
  void beginFrame(RenderTarget& target) override;
  void endFrame(RenderTarget& target) override;

  [[nodiscard]] std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface* surface) override;
  [[nodiscard]] std::unique_ptr<RenderFramebuffer>
  createFramebuffer(std::uint32_t width, std::uint32_t height) override;
  void bindFramebuffer(const RenderFramebuffer& framebuffer) override;
  void bindDefaultFramebuffer() override;
  void setViewport(std::uint32_t width, std::uint32_t height) override;
  void clear(Color color) override;
  void setBlendMode(RenderBlendMode mode) override;
  [[nodiscard]] int maxTextureSize() override;
  void setScissor(RenderScissor scissor) override;
  void disableScissor() override;
  void drawRect(
      float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
      const Mat3& transform
  ) override;
  void drawImage(const RenderImageDraw& draw) override;
  void drawGlyph(const RenderGlyphDraw& draw) override;
  void drawSpinner(
      float surfaceWidth, float surfaceHeight, float width, float height, const SpinnerStyle& style,
      const Mat3& transform
  ) override;
  void drawScreenCorner(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const ScreenCornerStyle& style, const Mat3& transform
  ) override;
  void drawAudioSpectrum(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
  ) override;
  void drawEffect(
      float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style,
      const Mat3& transform
  ) override;
  void drawGraph(
      TextureId dataTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
      const GraphStyle& style, const Mat3& transform
  ) override;
  void drawWallpaper(
      WallpaperTransition transition, WallpaperSourceKind sourceKind1, TextureId texture1, const Color& sourceColor1,
      WallpaperSourceKind sourceKind2, TextureId texture2, const Color& sourceColor2, float surfaceWidth,
      float surfaceHeight, float width, float height, float imageWidth1, float imageHeight1, float imageWidth2,
      float imageHeight2, float progress, float fillMode, const TransitionParams& params, const Color& fillColor,
      const Mat3& transform
  ) override;
  void drawFullscreenTexture(TextureId texture, bool flipY) override;
  void drawFullscreenTint(Color color) override;
  void drawFramebufferBlur(
      TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY,
      float radius
  ) override;

  [[nodiscard]] TextureManager& textureManager() override { return m_textureManager; }

private:
  void drawFullscreenQuad(const ShaderProgram& program);
  void ensureFullscreenTextureProgram();
  void ensureFullscreenTintProgram();

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLConfig m_config = nullptr;
  EGLContext m_context = EGL_NO_CONTEXT;
  int m_maxTextureSize = 0;
  GlesTextureManager m_textureManager;
  RectProgram m_rectProgram;
  ImageProgram m_imageProgram;
  GlyphProgram m_glyphProgram;
  SpinnerProgram m_spinnerProgram;
  ScreenCornerProgram m_screenCornerProgram;
  AudioSpectrumProgram m_audioSpectrumProgram;
  EffectProgram m_effectProgram;
  GraphProgram m_graphProgram;
  WallpaperProgram m_wallpaperProgram;
  BlurProgram m_blurProgram;
  ShaderProgram m_fullscreenTextureProgram;
  ShaderProgram m_fullscreenTintProgram;
};
