#include "render/render_target.h"

#include "render/backend/render_backend.h"
#include "render/render_context.h"

#include <stdexcept>

RenderTarget::RenderTarget() = default;

RenderTarget::~RenderTarget() { destroy(); }

void RenderTarget::create(wl_surface* surface, RenderContext& context) { create(surface, context.backend()); }

void RenderTarget::create(wl_surface* surface, RenderBackend& backend) {
  destroy();
  m_surfaceTarget = backend.createSurfaceTarget(surface);
  if (m_surfaceTarget == nullptr) {
    throw std::runtime_error("render backend failed to create a surface target");
  }
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
