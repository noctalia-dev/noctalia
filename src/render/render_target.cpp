#include "render/render_target.h"

#include "render/backend/render_backend.h"
#include "render/render_context.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

// ── ScaledRenderer ──────────────────────────────────────────────────────────

ScaledRenderer::ScaledRenderer(RenderContext& context, float fixedScale)
    : m_context(&context), m_fixedScale(fixedScale > 0.0F && std::isfinite(fixedScale) ? fixedScale : 1.0F) {}

float ScaledRenderer::currentScale() const noexcept {
  return m_target != nullptr ? m_target->contentScale() : m_fixedScale;
}

TextMetrics ScaledRenderer::measureText(
    std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines, TextAlign align,
    std::string_view fontFamily, TextEllipsize ellipsize, bool useMarkup
) {
  assert(m_context != nullptr && "ScaledRenderer measure without a bound RenderContext");
  return m_context->measureTextScaled(
      currentScale(), text, fontSize, fontWeight, maxWidth, maxLines, align, fontFamily, ellipsize, useMarkup
  );
}

TextMetrics ScaledRenderer::measureFont(float fontSize, FontWeight fontWeight) {
  assert(m_context != nullptr && "ScaledRenderer measureFont without a bound RenderContext");
  return m_context->measureFontScaled(currentScale(), fontSize, fontWeight);
}

void ScaledRenderer::measureTextCursorStops(
    std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
    FontWeight fontWeight
) {
  assert(m_context != nullptr && "ScaledRenderer cursor stops without a bound RenderContext");
  m_context->measureTextCursorStopsScaled(currentScale(), text, fontSize, byteOffsets, outStops, fontWeight);
}

void ScaledRenderer::measureTextCursorStopsWrapped(
    std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
    std::vector<TextCursorStop>& outStops, FontWeight fontWeight
) {
  assert(m_context != nullptr && "ScaledRenderer wrapped cursor stops without a bound RenderContext");
  m_context->measureTextCursorStopsWrappedScaled(
      currentScale(), text, fontSize, byteOffsets, maxWidth, outStops, fontWeight
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

float ScaledRenderer::renderScale() const noexcept { return currentScale(); }

std::uint64_t ScaledRenderer::textMetricsGeneration() const noexcept {
  return m_context != nullptr ? m_context->textMetricsGeneration() : 0;
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
