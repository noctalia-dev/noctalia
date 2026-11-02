#include "render/backend/render_backend.h"

#include "render/backend/gles_render_backend.h"
#include "render/backend/gles_texture_manager.h"

void RenderBackend::destroyExtensions() {
  for (auto& entry : m_extensions) {
    entry.second->destroy();
  }
}

void RenderBackend::abandonExtensions() noexcept {
  for (auto& entry : m_extensions) {
    entry.second->abandon();
  }
}

std::unique_ptr<RenderBackend> createDefaultRenderBackend() { return std::make_unique<GlesRenderBackend>(); }

std::unique_ptr<TextureManager> createDefaultTextureManager() { return std::make_unique<GlesTextureManager>(); }
