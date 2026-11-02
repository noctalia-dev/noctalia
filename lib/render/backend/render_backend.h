#pragma once

#include "render/backend/render_extension.h"
#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/core/render_styles.h"
#include "render/core/texture_handle.h"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

class GlSharedContext;
class RenderTarget;
class TextureManager;
struct wl_surface;

class RenderFramebuffer {
public:
  virtual ~RenderFramebuffer() = default;

  virtual bool valid() const noexcept = 0;
  virtual TextureId colorTexture() const noexcept = 0;
  virtual std::uint32_t width() const noexcept = 0;
  virtual std::uint32_t height() const noexcept = 0;
  virtual void abandon() noexcept = 0;
};

enum class RenderGraphicsResetStatus {
  NoError,
  Guilty,
  Innocent,
  Unknown,
  Purged,
  Other,
};

enum class RenderBlendMode {
  Disabled,
  StraightAlpha,
  PremultipliedAlpha,
  DestinationOut,
};

enum class RenderImageFitMode : std::uint8_t {
  Stretch,
  Cover,
  Contain,
};

struct RenderImageDraw {
  TextureId texture;
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  Color tint = rgba(1.0F, 1.0F, 1.0F, 1.0F);
  float saturation = 1.0F;
  bool monochromeTint = false;
  bool alphaMaskTint = false;
  float opacity = 1.0F;
  float radius = 0.0F;
  Color borderColor = rgba(0.0F, 0.0F, 0.0F, 0.0F);
  float borderWidth = 0.0F;
  RenderImageFitMode fitMode = RenderImageFitMode::Stretch;
  float textureWidth = 0.0F;
  float textureHeight = 0.0F;
  Mat3 transform = Mat3::identity();
  ImageScrim scrim{};
};

struct RenderGlyphDraw {
  TextureId texture;
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  float u0 = 0.0F;
  float v0 = 0.0F;
  float u1 = 1.0F;
  float v1 = 1.0F;
  float opacity = 1.0F;
  Color tint = rgba(1.0F, 1.0F, 1.0F, 1.0F);
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

  virtual bool isReady() const noexcept = 0;
};

class RenderBackend {
public:
  virtual ~RenderBackend() = default;

  virtual void initialize(GlSharedContext& shared) = 0;
  virtual void cleanup() = 0;

  // Returns false if the surface could not be made current (e.g. invalidated
  // during compositor teardown); callers must skip the frame, not treat it as fatal.
  virtual bool makeCurrent(RenderTarget& target) = 0;
  // Returns false if the surfaceless context could not be made current (e.g. lost on resume);
  // best-effort callers may ignore it, GPU paths must skip the work, not treat it as fatal.
  virtual bool makeCurrentNoSurface() = 0;
  // Returns false if the frame could not begin; callers must skip drawing and endFrame.
  virtual bool beginFrame(RenderTarget& target) = 0;
  virtual void endFrame(RenderTarget& target) = 0;
  virtual RenderGraphicsResetStatus graphicsResetStatus() = 0;
  virtual void invalidateGpuResources() = 0;
  // Tear down a lost context without attempting to preserve its invalid GL objects.
  virtual void abandonAfterGraphicsReset() noexcept = 0;

  [[nodiscard]] virtual std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface* surface) = 0;
  [[nodiscard]] virtual std::unique_ptr<RenderFramebuffer>
  createFramebuffer(std::uint32_t width, std::uint32_t height) = 0;
  virtual void bindFramebuffer(const RenderFramebuffer& framebuffer) = 0;
  virtual void bindDefaultFramebuffer() = 0;
  virtual void setViewport(std::uint32_t width, std::uint32_t height) = 0;
  virtual void clear(Color color) = 0;
  virtual void setBlendMode(RenderBlendMode mode) = 0;
  virtual int maxTextureSize() = 0;
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
  virtual void drawFullscreenTexture(TextureId texture, bool flipY) = 0;
  virtual void drawFullscreenTint(Color color) = 0;
  virtual void drawFramebufferBlur(
      TextureId sourceTexture, std::uint32_t width, std::uint32_t height, float directionX, float directionY,
      float radius
  ) = 0;

  virtual TextureManager& textureManager() = 0;

  // Returns this backend's instance of T, creating it on first use. The backend destroys or
  // abandons every extension together with its own GPU objects.
  template <typename T> T& extension() {
    static_assert(std::is_base_of_v<RenderExtension, T>);
    const std::type_index key(typeid(T));
    for (auto& [type, instance] : m_extensions) {
      if (type == key) {
        return static_cast<T&>(*instance);
      }
    }
    return static_cast<T&>(*m_extensions.emplace_back(key, std::make_unique<T>()).second);
  }

protected:
  void destroyExtensions();
  void abandonExtensions() noexcept;

private:
  std::vector<std::pair<std::type_index, std::unique_ptr<RenderExtension>>> m_extensions;
};

[[nodiscard]] std::unique_ptr<RenderBackend> createDefaultRenderBackend();
[[nodiscard]] std::unique_ptr<TextureManager> createDefaultTextureManager();
