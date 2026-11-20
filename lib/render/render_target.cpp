#include "render/render_target.h"

#include "render/backend/render_backend.h"
#include "render/render_context.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

// ── ScaledRenderer ──────────────────────────────────────────────────────────

ScaledRenderer::ScaledRenderer(RenderContext& context, float fixedScale, std::optional<float> fixedTextScale)
    : m_context(&context), m_fixedScale(fixedScale > 0.0F && std::isfinite(fixedScale) ? fixedScale : 1.0F),
      m_fixedTextScale(fixedTextScale.transform(RenderContext::clampTextScale)) {}

float ScaledRenderer::currentScale() const noexcept {
  return m_target != nullptr ? m_target->contentScale() : m_fixedScale;
}

float ScaledRenderer::currentTextScale() const noexcept {
  const std::optional<float> own = m_target != nullptr ? m_target->textScale() : m_fixedTextScale;
  return own.value_or(m_context->textScale());
}

TextMetrics ScaledRenderer::measureText(
    std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines, TextAlign align,
    TextEllipsize ellipsize, bool useMarkup
) {
  assert(m_context != nullptr && "ScaledRenderer measure without a bound RenderContext");
  return m_context->measureTextScaled(
      currentScale(), text, fontSize * currentTextScale(), font, maxWidth, maxLines, align, ellipsize, useMarkup
  );
}

TextMetrics ScaledRenderer::measureFont(float fontSize, const TextFont& font) {
  assert(m_context != nullptr && "ScaledRenderer measureFont without a bound RenderContext");
  return m_context->measureFontScaled(currentScale(), fontSize * currentTextScale(), font);
}

void ScaledRenderer::measureTextCursorStops(
    std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
    const TextFont& font
) {
  assert(m_context != nullptr && "ScaledRenderer cursor stops without a bound RenderContext");
  m_context->measureTextCursorStopsScaled(
      currentScale(), text, fontSize * currentTextScale(), byteOffsets, outStops, font
  );
}

void ScaledRenderer::measureTextCursorStopsWrapped(
    std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
    std::vector<TextCursorStop>& outStops, const TextFont& font
) {
  assert(m_context != nullptr && "ScaledRenderer wrapped cursor stops without a bound RenderContext");
  m_context->measureTextCursorStopsWrappedScaled(
      currentScale(), text, fontSize * currentTextScale(), byteOffsets, maxWidth, outStops, font
  );
}

void ScaledRenderer::measureTextCharSpans(
    std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
    std::vector<TextCharSpan>& outSpans, const TextFont& font
) {
  assert(m_context != nullptr && "ScaledRenderer char spans without a bound RenderContext");
  m_context->measureTextCharSpansScaled(
      currentScale(), text, fontSize * currentTextScale(), byteOffsets, outSpans, font
  );
}

TextMetrics ScaledRenderer::measureGlyph(char32_t codepoint, float fontSize) {
  assert(m_context != nullptr && "ScaledRenderer measureGlyph without a bound RenderContext");
  return m_context->measureGlyphScaled(currentScale(), codepoint, fontSize);
}

TextureManager& ScaledRenderer::textureManager() {
  assert(m_context != nullptr && "ScaledRenderer textureManager without a bound RenderContext");
  return m_context->textureManager();
}

AsyncTextureCache* ScaledRenderer::asyncTextureCache() noexcept {
  return m_context != nullptr ? m_context->asyncTextureCache() : nullptr;
}

float ScaledRenderer::renderScale() const noexcept { return currentScale(); }

Vec2 ScaledRenderer::bufferScale() const noexcept {
  if (m_target == nullptr
      || m_target->logicalWidth() == 0
      || m_target->logicalHeight() == 0
      || m_target->bufferWidth() == 0
      || m_target->bufferHeight() == 0) {
    return Renderer::bufferScale();
  }
  return {
      static_cast<float>(m_target->bufferWidth()) / static_cast<float>(m_target->logicalWidth()),
      static_cast<float>(m_target->bufferHeight()) / static_cast<float>(m_target->logicalHeight())
  };
}

std::uint64_t ScaledRenderer::textMetricsGeneration() const noexcept {
  if (m_context == nullptr) {
    return 0;
  }
  // Both counters only grow, so the sum changes whenever either does.
  return m_context->textMetricsGeneration() + (m_target != nullptr ? m_target->textScaleGeneration() : 0);
}

void ScaledRenderer::notifyFontConfigChanged() {
  if (m_context != nullptr) {
    m_context->notifyFontConfigChanged();
  }
}

// ── RenderTarget ────────────────────────────────────────────────────────────

RenderTarget::RenderTarget() { m_renderer.attachTarget(*this); }

RenderTarget::~RenderTarget() { destroy(); }

void RenderTarget::create(wl_surface* surface, RenderContext& context) {
  m_renderer.setContext(&context);
  create(surface, context.backend());
}

void RenderTarget::create(wl_surface* surface, RenderBackend& backend) {
  destroy();
  m_surfaceTarget = backend.createSurfaceTarget(surface);
  if (m_surfaceTarget == nullptr) {
    throw std::runtime_error("render backend failed to create a surface target");
  }
}

void RenderTarget::setContentScale(float scale) noexcept {
  m_contentScale = scale > 0.0F && std::isfinite(scale) ? scale : 1.0F;
}

void RenderTarget::setTextScale(std::optional<float> scale) noexcept {
  scale = scale.transform(RenderContext::clampTextScale);
  if (m_textScale == scale) {
    return;
  }
  m_textScale = scale;
  ++m_textScaleGeneration;
}

void RenderTarget::resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) {
  if (bufferWidth == 0 || bufferHeight == 0) {
    return;
  }

  m_bufferWidth = bufferWidth;
  m_bufferHeight = bufferHeight;

  if (m_surfaceTarget != nullptr) {
    m_surfaceTarget->resize(bufferWidth, bufferHeight);
  }
}

bool RenderTarget::isReady() const noexcept { return m_surfaceTarget != nullptr && m_surfaceTarget->isReady(); }

void RenderTarget::destroy() {
  if (m_surfaceTarget != nullptr) {
    m_surfaceTarget->destroy();
    m_surfaceTarget.reset();
  }
  m_bufferWidth = 0;
  m_bufferHeight = 0;
  m_logicalWidth = 0;
  m_logicalHeight = 0;
}
