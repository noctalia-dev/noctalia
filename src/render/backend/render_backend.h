#pragma once

#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/core/texture_handle.h"

#include <cstdint>
#include <memory>
#include <span>

class GlSharedContext;
class RenderTarget;
class TextureManager;
struct wl_surface;
enum class WallpaperSourceKind : std::uint8_t;
enum class WallpaperTransition : std::uint8_t;
struct AudioSpectrumStyle;
struct EffectStyle;
struct GraphStyle;
struct RoundedRectStyle;
struct ScreenCornerStyle;
struct SpinnerStyle;
struct TransitionParams;

class RenderFramebuffer {
public:
  virtual ~RenderFramebuffer() = default;

  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual TextureId colorTexture() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
};

enum class RenderBlendMode {
  Disabled,
  StraightAlpha,
  PremultipliedAlpha,
};

enum class RenderImageFitMode : std::uint8_t {
  Stretch,
  Cover,
  Contain,
};

struct RenderImageDraw {
  TextureId texture;
  float surfaceWidth = 0.0f;
  float surfaceHeight = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  Color tint = rgba(1.0f, 1.0f, 1.0f, 1.0f);
  float opacity = 1.0f;
  float radius = 0.0f;
  Color borderColor = rgba(0.0f, 0.0f, 0.0f, 0.0f);
  float borderWidth = 0.0f;
  RenderImageFitMode fitMode = RenderImageFitMode::Stretch;
  float textureWidth = 0.0f;
  float textureHeight = 0.0f;
  Mat3 transform = Mat3::identity();
};

struct RenderGlyphDraw {
  TextureId texture;
  float surfaceWidth = 0.0f;
  float surfaceHeight = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 1.0f;
  float v1 = 1.0f;
  float opacity = 1.0f;
  Color tint = rgba(1.0f, 1.0f, 1.0f, 1.0f);
  bool tinted = false;
  Mat3 transform = Mat3::identity();
};

struct RenderScissor {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
};

class RenderSurfaceTarget {
public:
  virtual ~RenderSurfaceTarget() = default;

  virtual void resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) = 0;
  virtual void destroy() = 0;

  [[nodiscard]] virtual bool isReady() const noexcept = 0;
};

class RenderBackend {
public:
  virtual ~RenderBackend() = default;

  virtual void initialize(GlSharedContext& shared) = 0;
  virtual void cleanup() = 0;

  virtual void makeCurrent(RenderTarget& target) = 0;
  virtual void makeCurrentNoSurface() = 0;
  virtual void beginFrame(RenderTarget& target) = 0;
  virtual void endFrame(RenderTarget& target) = 0;

  [[nodiscard]] virtual std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface* surface) = 0;
  [[nodiscard]] virtual std::unique_ptr<RenderFramebuffer>
  createFramebuffer(std::uint32_t width, std::uint32_t height) = 0;
  virtual void bindFramebuffer(const RenderFramebuffer& framebuffer) = 0;
  virtual void bindDefaultFramebuffer() = 0;
  virtual void setViewport(std::uint32_t width, std::uint32_t height) = 0;
  virtual void clear(Color color) = 0;
  virtual void setBlendMode(RenderBlendMode mode) = 0;
  [[nodiscard]] virtual int maxTextureSize() = 0;
  virtual void setScissor(RenderScissor scissor) = 0;
  virtual void disableScissor() = 0;
  virtual void drawRect(
      float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
      const Mat3& transform
  ) = 0;
  virtual void drawImage(const RenderImageDraw& draw) = 0;
  virtual void drawGlyph(const RenderGlyphDraw& draw) = 0;
  virtual void drawSpinner(
      float surfaceWidth, float surfaceHeight, float width, float height, const SpinnerStyle& style,
      const Mat3& transform
  ) = 0;
  virtual void drawScreenCorner(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const ScreenCornerStyle& style, const Mat3& transform
  ) = 0;
  virtual void drawAudioSpectrum(
      float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
      const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
  ) = 0;
  virtual void drawEffect(
      float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style,
      const Mat3& transform
  ) = 0;
  virtual void drawGraph(
      TextureId dataTexture, int textureWidth, float surfaceWidth, float surfaceHeight, float width, float height,
      const GraphStyle& style, const Mat3& transform
  ) = 0;
  virtual void drawWallpaper(
      WallpaperTransition transition, WallpaperSourceKind sourceKind1, TextureId texture1, const Color& sourceColor1,
      WallpaperSourceKind sourceKind2, TextureId texture2, const Color& sourceColor2, float surfaceWidth,
      float surfaceHeight, float width, float height, float imageWidth1, float imageHeight1, float imageWidth2,
      float imageHeight2, float progress, float fillMode, const TransitionParams& params, const Color& fillColor,
      const Mat3& transform
  ) = 0;
  virtual void drawFullscreenTexture(TextureId texture, bool flipY) = 0;
  virtual void drawFullscreenTint(Color color) = 0;
  virtual void drawFramebufferBlur(
      TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY,
      float radius
  ) = 0;

  [[nodiscard]] virtual TextureManager& textureManager() = 0;
};

[[nodiscard]] std::unique_ptr<RenderBackend> createDefaultRenderBackend();
[[nodiscard]] std::unique_ptr<TextureManager> createDefaultTextureManager();
