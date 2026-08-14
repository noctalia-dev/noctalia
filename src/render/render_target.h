#pragma once

#include "render/core/renderer.h"

#include <cstdint>
#include <memory>

struct wl_surface;

class RenderBackend;
class RenderContext;
class RenderSurfaceTarget;
class RenderTarget;

// A Renderer view whose scale is not ambient process-wide state.
//
// Two modes share one forwarding implementation:
//  - Target-bound: embedded by value in a RenderTarget; renderScale() reads the
//    owning target's contentScale() on every call.
//  - Fixed-scale: a stack/local object carrying an explicit scale for
//    measurement that must happen before a surface target exists.
//
// All Renderer methods forward to the shared RenderContext with an explicit
// scale, so measuring/drawing through one view never mutates another view's
// scale. The view holds no cache/backend of its own.
class ScaledRenderer final : public Renderer {
public:
  ScaledRenderer() = default;
  // Fixed-scale mode: measurement before any surface target exists.
  ScaledRenderer(RenderContext& context, float fixedScale);

  ScaledRenderer(const ScaledRenderer&) = delete;
  ScaledRenderer& operator=(const ScaledRenderer&) = delete;

  // Target-bound wiring, driven by the owning RenderTarget.
  void attachTarget(const RenderTarget& target) noexcept { m_target = &target; }
  void setContext(RenderContext* context) noexcept { m_context = context; }

  [[nodiscard]] TextMetrics measureText(
      std::string_view text, float fontSize, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0F,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {},
      TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  ) override;
  [[nodiscard]] TextMetrics measureFont(float fontSize, FontWeight fontWeight = FontWeight::Normal) override;
  void measureTextCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      FontWeight fontWeight = FontWeight::Normal
  ) override;
  void measureTextCursorStopsWrapped(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, FontWeight fontWeight = FontWeight::Normal
  ) override;
  [[nodiscard]] TextMetrics measureGlyph(char32_t codepoint, float fontSize) override;
  [[nodiscard]] TextureManager& textureManager() override;
  [[nodiscard]] float renderScale() const noexcept override;
  [[nodiscard]] std::uint64_t textMetricsGeneration() const noexcept override;
  void notifyFontConfigChanged() override;

private:
  [[nodiscard]] float currentScale() const noexcept;

  RenderContext* m_context = nullptr;
  const RenderTarget* m_target = nullptr;
  float m_fixedScale = 1.0F;
};

class RenderTarget {
public:
  RenderTarget();
  ~RenderTarget();

  RenderTarget(const RenderTarget&) = delete;
  RenderTarget& operator=(const RenderTarget&) = delete;

  void create(wl_surface* surface, RenderContext& context);
  void create(wl_surface* surface, RenderBackend& backend);
  void resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight);
  void destroy();

  [[nodiscard]] std::uint32_t bufferWidth() const noexcept { return m_bufferWidth; }
  [[nodiscard]] std::uint32_t bufferHeight() const noexcept { return m_bufferHeight; }
  [[nodiscard]] std::uint32_t logicalWidth() const noexcept { return m_logicalWidth; }
  [[nodiscard]] std::uint32_t logicalHeight() const noexcept { return m_logicalHeight; }
  [[nodiscard]] bool isReady() const noexcept;
  [[nodiscard]] RenderSurfaceTarget* surfaceTarget() noexcept { return m_surfaceTarget.get(); }
  [[nodiscard]] const RenderSurfaceTarget* surfaceTarget() const noexcept { return m_surfaceTarget.get(); }

  // This target's stable Renderer view. Retained callbacks and components keep
  // this reference for the target's lifetime; destroying the GPU surface target
  // does not replace the view object.
  [[nodiscard]] Renderer& renderer() noexcept { return m_renderer; }
  [[nodiscard]] const Renderer& renderer() const noexcept { return m_renderer; }

  // Physical render scale = buffer-to-logical ratio. Set by the owning surface
  // from its resolved scale; the renderer view reads it on every call.
  void setContentScale(float scale) noexcept;
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }

  void setLogicalSize(std::uint32_t w, std::uint32_t h) noexcept {
    m_logicalWidth = w;
    m_logicalHeight = h;
  }

private:
  std::unique_ptr<RenderSurfaceTarget> m_surfaceTarget;
  ScaledRenderer m_renderer;
  float m_contentScale = 1.0F;
  std::uint32_t m_bufferWidth = 0;
  std::uint32_t m_bufferHeight = 0;
  std::uint32_t m_logicalWidth = 0;
  std::uint32_t m_logicalHeight = 0;
};
