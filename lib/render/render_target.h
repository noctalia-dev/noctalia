#pragma once

#include "render/core/renderer.h"

#include <cstdint>
#include <memory>
#include <optional>

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
  // Fixed-scale mode: measurement before any surface target exists. fixedTextScale is the text scale
  // of the surface being measured for; nullopt uses the RenderContext default.
  ScaledRenderer(RenderContext& context, float fixedScale, std::optional<float> fixedTextScale = std::nullopt);

  ScaledRenderer(const ScaledRenderer&) = delete;
  ScaledRenderer& operator=(const ScaledRenderer&) = delete;

  // Target-bound wiring, driven by the owning RenderTarget.
  void attachTarget(const RenderTarget& target) noexcept { m_target = &target; }
  void setContext(RenderContext* context) noexcept { m_context = context; }

  TextMetrics measureText(
      std::string_view text, float fontSize, const TextFont& font = {}, float maxWidth = 0.0F, int maxLines = 0,
      TextAlign align = TextAlign::Start, TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  ) override;
  TextMetrics measureFont(float fontSize, const TextFont& font = {}) override;
  void measureTextCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      const TextFont& font = {}
  ) override;
  void measureTextCursorStopsWrapped(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, const TextFont& font = {}
  ) override;
  void measureTextCharSpans(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
      std::vector<TextCharSpan>& outSpans, const TextFont& font = {}
  ) override;
  TextMetrics measureGlyph(char32_t codepoint, float fontSize) override;
  TextureManager& textureManager() override;
  AsyncTextureCache* asyncTextureCache() noexcept override;
  float renderScale() const noexcept override;
  Vec2 bufferScale() const noexcept override;
  std::uint64_t textMetricsGeneration() const noexcept override;
  void notifyFontConfigChanged() override;

private:
  float currentScale() const noexcept;
  // Text size multiplier: the target's own, else the fixed one, else the RenderContext default.
  float currentTextScale() const noexcept;

  RenderContext* m_context = nullptr;
  const RenderTarget* m_target = nullptr;
  float m_fixedScale = 1.0F;
  std::optional<float> m_fixedTextScale;
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

  std::uint32_t bufferWidth() const noexcept { return m_bufferWidth; }
  std::uint32_t bufferHeight() const noexcept { return m_bufferHeight; }
  std::uint32_t logicalWidth() const noexcept { return m_logicalWidth; }
  std::uint32_t logicalHeight() const noexcept { return m_logicalHeight; }
  bool isReady() const noexcept;
  RenderSurfaceTarget* surfaceTarget() noexcept { return m_surfaceTarget.get(); }
  const RenderSurfaceTarget* surfaceTarget() const noexcept { return m_surfaceTarget.get(); }

  // This target's stable Renderer view. Retained callbacks and components keep
  // this reference for the target's lifetime; destroying the GPU surface target
  // does not replace the view object.
  Renderer& renderer() noexcept { return m_renderer; }
  const Renderer& renderer() const noexcept { return m_renderer; }

  // Physical render scale = buffer-to-logical ratio. Set by the owning surface
  // from its resolved scale; the renderer view reads it on every call.
  void setContentScale(float scale) noexcept;
  float contentScale() const noexcept { return m_contentScale; }

  // Text size multiplier for this target only; nullopt follows RenderContext::setTextScale.
  // Measurement and drawing through this target both apply it, so labels re-measure on change.
  void setTextScale(std::optional<float> scale) noexcept;
  std::optional<float> textScale() const noexcept { return m_textScale; }
  std::uint64_t textScaleGeneration() const noexcept { return m_textScaleGeneration; }

  void setLogicalSize(std::uint32_t w, std::uint32_t h) noexcept {
    m_logicalWidth = w;
    m_logicalHeight = h;
  }

private:
  std::unique_ptr<RenderSurfaceTarget> m_surfaceTarget;
  ScaledRenderer m_renderer;
  float m_contentScale = 1.0F;
  std::optional<float> m_textScale;
  std::uint64_t m_textScaleGeneration = 0;
  std::uint32_t m_bufferWidth = 0;
  std::uint32_t m_bufferHeight = 0;
  std::uint32_t m_logicalWidth = 0;
  std::uint32_t m_logicalHeight = 0;
};
