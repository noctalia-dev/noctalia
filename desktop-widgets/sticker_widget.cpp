#include "sticker_widget.h"

#include "core/ui_phase.h"
#include "ui/controls/image.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
  constexpr float kDefaultSize = 200.0F;
} // namespace

StickerWidget::StickerWidget() {
  setHitTestVisible(false);
  addChild(
      ui::image({
          .out = &m_image,
          .fit = ImageFit::Contain,
          .configure = [this](Image& image) {
            image.setHitTestVisible(false);
            image.setAsyncReadyCallback([this] {
              markLayoutDirty();
              if (m_asyncReadyCallback) {
                m_asyncReadyCallback();
              }
            });
          },
      })
  );
}

bool StickerWidget::setSourceFile(Renderer& renderer, const std::string& path) {
  uiAssertNotRendering("StickerWidget::setSourceFile");
  const float previousAspect = aspectRatio();
  m_externalAspect.reset();
  const bool ready = m_image->setSourceFile(renderer, path);
  if (aspectRatio() != previousAspect) {
    markLayoutDirty();
  }
  return ready;
}

void StickerWidget::setExternalTexture(Renderer& renderer, TextureHandle texture, int imageWidth, int imageHeight) {
  uiAssertNotRendering("StickerWidget::setExternalTexture");
  if (imageWidth <= 0 || imageHeight <= 0) {
    throw std::invalid_argument("Sticker image dimensions must be positive");
  }
  const float previousAspect = aspectRatio();
  m_externalAspect = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
  m_image->setExternalTexture(renderer, texture);
  if (aspectRatio() != previousAspect) {
    markLayoutDirty();
  }
}

void StickerWidget::clear(Renderer& renderer) {
  uiAssertNotRendering("StickerWidget::clear");
  const float previousAspect = aspectRatio();
  m_externalAspect.reset();
  m_image->clear(renderer);
  if (aspectRatio() != previousAspect) {
    markLayoutDirty();
  }
}

void StickerWidget::setAsyncReadyCallback(std::function<void()> callback) {
  uiAssertNotRendering("StickerWidget::setAsyncReadyCallback");
  m_asyncReadyCallback = std::move(callback);
}

void StickerWidget::setContentScale(float scale) {
  uiAssertNotRendering("StickerWidget::setContentScale");
  if (!std::isfinite(scale) || scale <= 0.0F) {
    throw std::invalid_argument("Sticker content scale must be positive and finite");
  }
  if (m_contentScale == scale) {
    return;
  }
  m_contentScale = scale;
  markLayoutDirty();
}

bool StickerWidget::hasImage() const noexcept { return m_image->hasImage(); }
bool StickerWidget::hasPendingContent() const noexcept { return m_image->hasPendingContent(); }

float StickerWidget::aspectRatio() const noexcept {
  return m_externalAspect.value_or(m_image->hasImage() ? m_image->aspectRatio() : 1.0F);
}

void StickerWidget::doLayout(Renderer& /*renderer*/) {
  UiPhaseScope phase(UiPhase::Layout);
  const float size = kDefaultSize * m_contentScale;
  const float aspect = aspectRatio();
  const float width = aspect >= 1.0F ? size : size * aspect;
  const float height = aspect >= 1.0F ? size / aspect : size;
  m_image->setSize(width, height);
  setSize(width, height);
}
