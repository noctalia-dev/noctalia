#pragma once

#include "config/config_types.h"
#include "render/core/color.h"
#include "render/core/texture_handle.h"
#include "shell/render/wallpaper_types.h"

#include <cstdint>
#include <memory>

struct wl_surface;

class GlSharedContext;
class RenderBackend;
class RenderFramebuffer;
class RenderTarget;

struct BackdropPostProcessOptions {
  float blurRadius = 0.0F;
  int blurRounds = 0;
  Color tintColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  float tintIntensity = 0.0F;

  // Mirrors the blur/tint activation thresholds in WallpaperRenderer (0.5px radius, ~0 alpha).
  [[nodiscard]] bool needsPostProcess() const noexcept { return blurRadius >= 0.5F || tintIntensity > 0.001F; }
};

class WallpaperRenderer {
public:
  WallpaperRenderer();
  ~WallpaperRenderer();

  WallpaperRenderer(const WallpaperRenderer&) = delete;
  WallpaperRenderer& operator=(const WallpaperRenderer&) = delete;

  void bind(GlSharedContext& shared, wl_surface* surface);
  void makeCurrent();
  void resize(
      std::uint32_t bufferWidth, std::uint32_t bufferHeight, std::uint32_t logicalWidth, std::uint32_t logicalHeight
  );
  void render();
  void
  renderBackdropFrame(RenderFramebuffer& target, RenderFramebuffer& scratch, const BackdropPostProcessOptions& options);
  void renderBackdropContent(
      RenderFramebuffer& target, RenderFramebuffer& scratch, const BackdropPostProcessOptions& options
  );
  void presentTexture(TextureId texture);
  void invalidateGpuResources();
  void prepareForGraphicsReset() noexcept;
  void restoreAfterGraphicsReset(GlSharedContext& shared);
  void finishGraphicsResetRecovery() noexcept { m_graphicsResetPending = false; }
  [[nodiscard]] std::unique_ptr<RenderFramebuffer> createFramebuffer(std::uint32_t width, std::uint32_t height);

  void setTransitionState(
      TextureId tex1, TextureId tex2, float imgW1, float imgH1, float imgW2, float imgH2, float progress,
      WallpaperTransition transition, WallpaperFillMode fillMode, const TransitionParams& params
  );

  RenderBackend* backend() const noexcept { return m_graphicsResetPending ? nullptr : m_backend.get(); }

private:
  void cleanup();
  void renderToFramebuffer(const RenderFramebuffer& target);
  void renderFadeBlurredSources(RenderFramebuffer& target, const BackdropPostProcessOptions& options);
  void ensureBlurredSource(
      std::unique_ptr<RenderFramebuffer>& cache, std::uint32_t& cacheKey, TextureId source, float imgW, float imgH,
      const BackdropPostProcessOptions& options
  );
  void blur(RenderFramebuffer& target, RenderFramebuffer& scratch, float radius, int rounds);
  void tint(RenderFramebuffer& target, Color color, float intensity);
  void blitToSurface(TextureId texture);
  void swapBuffers();
  void checkGraphicsReset();

  // Per-source pre-blurred frames for fade transitions: blur(A) and blur(B) are cached and
  // cross-faded directly, instead of re-blurring the blended frame every vsync. Keyed by
  // (source texture, radius, rounds, size) packed into cacheKey's texture id compare.
  std::unique_ptr<RenderFramebuffer> m_blurFbA;
  std::unique_ptr<RenderFramebuffer> m_blurFbB;
  std::uint32_t m_blurKeyA = 0;
  std::uint32_t m_blurKeyB = 0;
  float m_blurRadiusCached = 0.0F;
  int m_blurRoundsCached = 0;
  std::uint32_t m_blurSizeW = 0;
  std::uint32_t m_blurSizeH = 0;
  std::unique_ptr<RenderFramebuffer> m_blurScratch;

  wl_surface* m_surface = nullptr;
  GlSharedContext* m_shared = nullptr;
  std::unique_ptr<RenderBackend> m_backend;
  std::unique_ptr<RenderTarget> m_target;

  std::uint32_t m_bufferWidth = 0;
  std::uint32_t m_bufferHeight = 0;
  std::uint32_t m_logicalWidth = 0;
  std::uint32_t m_logicalHeight = 0;

  TextureId m_tex1;
  TextureId m_tex2;
  float m_imgW1 = 0.0F;
  float m_imgH1 = 0.0F;
  float m_imgW2 = 0.0F;
  float m_imgH2 = 0.0F;
  float m_progress = 0.0F;
  WallpaperTransition m_transition = WallpaperTransition::Fade;
  WallpaperFillMode m_fillMode = WallpaperFillMode::Crop;
  Color m_fillColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  TransitionParams m_params;
  bool m_graphicsResetPending = false;
};
