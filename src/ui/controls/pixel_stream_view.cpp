#include "ui/controls/pixel_stream_view.h"

#include "ipc/pixel_stream_service.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/controls/image.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace {
  bool insideImage(float x, float y) { return x >= 0.0F && x <= 1.0F && y >= 0.0F && y <= 1.0F; }
} // namespace

PixelStreamView::PixelStreamView() {
  setClipChildren(true);
  setAcceptedButtons(0);
  m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
  m_image->setFit(ImageFit::Contain);

  m_client.setChangeCallback([this] { onStreamChanged(); });
  m_service = PixelStreamService::current();
  if (m_service != nullptr) {
    m_service->add(m_client);
  }

  const auto hover = [this](const PointerData& pointer) {
    const auto position = imagePosition(pointer.localX, pointer.localY);
    if (!position.has_value()) {
      return;
    }
    const auto [x, y] = *position;
    if (m_dragging) {
      // A drag keeps reporting past the image edge, pinned to it.
      emit(PointerKind::Move, std::clamp(x, 0.0F, 1.0F), std::clamp(y, 0.0F, 1.0F));
    } else if (insideImage(x, y)) {
      emit(PointerKind::Move, x, y);
    }
  };
  setOnEnter(hover);
  setOnMotion(hover);

  setOnPress([this](const PointerData& pointer) {
    const auto position = imagePosition(pointer.localX, pointer.localY);
    if (pointer.pressed) {
      if (!position.has_value() || !insideImage(position->first, position->second)) {
        return;
      }
      if (pointer.button == BTN_LEFT) {
        if ((m_buttons & buttonMask(BTN_LEFT)) != 0 && !m_dragging) {
          m_dragging = true;
          emit(PointerKind::Down, position->first, position->second);
        }
      } else if ((m_buttons & buttonMask(pointer.button)) != 0) {
        m_clickButton = pointer.button;
        m_clickPosition = *position;
      }
      return;
    }
    if (pointer.button != BTN_LEFT || !m_dragging) {
      return;
    }
    if (!position.has_value()) {
      cancelDrag();
      return;
    }
    m_dragging = false;
    emit(PointerKind::Up, std::clamp(position->first, 0.0F, 1.0F), std::clamp(position->second, 0.0F, 1.0F));
  });

  // Right/middle report a click: pressed on the image and released over the view.
  setOnClick([this](const PointerData& pointer) {
    if (pointer.button == BTN_LEFT || pointer.button != std::exchange(m_clickButton, 0)) {
      return;
    }
    emit(
        pointer.button == BTN_RIGHT ? PointerKind::RightClick : PointerKind::MiddleClick, m_clickPosition.first,
        m_clickPosition.second
    );
  });

  setOnCancel([this] {
    m_clickButton = 0;
    cancelDrag();
  });

  setOnAxisHandler([this](const PointerData& pointer) {
    if (!m_scroll || m_dragging || pointer.axis > 1) {
      return false;
    }
    const auto position = imagePosition(pointer.localX, pointer.localY);
    const float steps = pointer.scrollSteps();
    if (!position.has_value() || !insideImage(position->first, position->second) || !std::isfinite(steps)) {
      return false;
    }
    if (steps != 0.0F) {
      // wl_pointer axis 0 is vertical, 1 horizontal.
      emit(
          PointerKind::Scroll, position->first, position->second, pointer.axis == 1 ? steps : 0.0F,
          pointer.axis == 0 ? steps : 0.0F
      );
    }
    return true;
  });

  const auto key = [this](const KeyData& data) {
    if (m_keyHandler) {
      m_keyHandler(data);
    }
  };
  setOnKeyDown(key);
  setOnKeyUp(key);
}

PixelStreamView::~PixelStreamView() {
  // Teardown reports nothing: the host ends any open drag session itself.
  m_client.setChangeCallback({});
  if (m_service != nullptr && PixelStreamService::current() == m_service) {
    m_service->remove(m_client);
  }
  if (m_renderer != nullptr && m_texture.valid()) {
    m_renderer->textureManager().unload(m_texture);
  }
}

void PixelStreamView::setPath(const std::string& path) {
  if (!m_client.setPath(path)) {
    return;
  }
  cancelDrag();
  m_clearPending = true;
  markLayoutDirty();
}

void PixelStreamView::setRadius(float radius) { m_image->setRadius(radius); }

void PixelStreamView::setPointerHandler(PointerHandler handler, std::uint32_t buttons, bool scroll) {
  if (!handler) {
    cancelDrag();
  }
  m_pointerHandler = std::move(handler);
  m_buttons = m_pointerHandler ? buttons & buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) : 0;
  m_scroll = m_pointerHandler && scroll;
  if ((m_buttons & buttonMask(BTN_LEFT)) == 0) {
    cancelDrag();
  }
  updateAcceptedButtons();
}

void PixelStreamView::setKeyHandler(KeyHandler handler) {
  m_keyHandler = std::move(handler);
  const bool keyboard = static_cast<bool>(m_keyHandler);
  setFocusable(keyboard);
  setRetainsFocusOnPointerRelease(keyboard);
  updateAcceptedButtons();
}

void PixelStreamView::updateAcceptedButtons() {
  // A left click focuses a keyboard-enabled view even without pointer callbacks.
  setAcceptedButtons(m_buttons | (m_keyHandler ? buttonMask(BTN_LEFT) : 0));
}

void PixelStreamView::cancelDrag() {
  if (!m_dragging) {
    return;
  }
  m_dragging = false;
  emit(PointerKind::Cancel);
}

void PixelStreamView::emit(PointerKind kind, float x, float y, float dx, float dy) {
  if (!m_pointerHandler) {
    return;
  }
  if (kind != PointerKind::Move) {
    ++m_segment;
  }
  m_pointerHandler({.kind = kind, .x = x, .y = y, .dx = dx, .dy = dy, .segment = m_segment});
}

void PixelStreamView::onStreamChanged() {
  if (!m_client.connected()) {
    cancelDrag();
  }
  markLayoutDirty();
}

std::optional<std::pair<float, float>> PixelStreamView::imagePosition(float localX, float localY) const {
  if (m_displayWidth <= 0 || m_displayHeight <= 0 || width() <= 0.0F || height() <= 0.0F) {
    return std::nullopt;
  }
  const auto sourceWidth = static_cast<float>(m_displayWidth);
  const auto sourceHeight = static_cast<float>(m_displayHeight);
  const float scale = std::min(width() / sourceWidth, height() / sourceHeight);
  const float imageWidth = sourceWidth * scale;
  const float imageHeight = sourceHeight * scale;
  const float x = (localX - (width() - imageWidth) * 0.5F) / imageWidth;
  const float y = (localY - (height() - imageHeight) * 0.5F) / imageHeight;
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return std::nullopt;
  }
  return std::pair{x, y};
}

void PixelStreamView::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  auto& textures = renderer.textureManager();
  if (std::exchange(m_clearPending, false)) {
    m_image->clear(renderer);
    textures.unload(m_texture);
    m_texture = {};
    m_displayWidth = 0;
    m_displayHeight = 0;
  }
  if (const auto frame = m_client.frame(); frame.has_value()) {
    const bool sameSize = m_texture.valid() && m_texture.width == frame->width && m_texture.height == frame->height;
    const bool uploaded = (sameSize
                           && textures.updateSubImage(
                               m_texture, frame->pixels, 0, 0, frame->width, frame->height, TextureDataFormat::Rgba
                           ))
        || textures.replace(m_texture, frame->pixels, frame->width, frame->height, TextureDataFormat::Rgba);
    if (uploaded) {
      m_image->setExternalTexture(renderer, m_texture);
      m_image->markPaintDirty();
      if (frame->width != m_displayWidth || frame->height != m_displayHeight) {
        // Positions in the old frame no longer mean the same place.
        cancelDrag();
        m_displayWidth = frame->width;
        m_displayHeight = frame->height;
      }
    }
    // The pixels are on the GPU now (or the upload failed); either way the buffer goes back.
    m_client.releaseFrame();
  }
  m_image->setSize(width(), height());
  InputArea::doLayout(renderer);
}

void PixelStreamView::doInvalidateGpuResources(Renderer& renderer) {
  m_renderer = &renderer;
  // The old texture died with the context and the companion may already be reusing the buffer
  // it came from; a fresh connection is answered with the current frame.
  m_texture = {};
  m_client.reconnect();
  markLayoutDirty();
}
