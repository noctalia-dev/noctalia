#include "shell/desktop/widgets/desktop_sticker_widget.h"

#include "core/log.h"
#include "render/codecs/codecs.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "sticker_widget.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <utility>

namespace {

  constexpr Logger kLog("desktop");
  constexpr int kMaxStickerGifFrames = 512;
  constexpr std::size_t kMaxStickerGifBytes = 96ULL * 1024 * 1024;

  bool endsWithIgnoreCase(const std::string& s, const char* suffix) {
    const std::size_t n = std::strlen(suffix);
    if (s.size() < n) {
      return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
      const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - n + i])));
      const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
      if (a != b) {
        return false;
      }
    }
    return true;
  }

} // namespace

DesktopStickerWidget::DesktopStickerWidget(std::string imagePath, float opacity)
    : m_imagePath(std::move(imagePath)), m_opacity(std::clamp(opacity, 0.0F, 1.0F)) {}

DesktopStickerWidget::~DesktopStickerWidget() { unloadFrames(); }

void DesktopStickerWidget::create() {
  auto sticker = std::make_unique<StickerWidget>();
  m_sticker = sticker.get();
  sticker->setOpacity(m_opacity);
  sticker->setContentScale(contentScale());
  sticker->setAsyncReadyCallback([this] { requestLayout(); });
  setRoot(std::move(sticker));
}

bool DesktopStickerWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "opacity") {
    if (const auto* v = std::get_if<double>(&value)) {
      m_opacity = std::clamp(static_cast<float>(*v), 0.0F, 1.0F);
      if (root() != nullptr) {
        root()->setOpacity(m_opacity);
      }
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopStickerWidget::doLayout(Renderer& renderer) {
  if (m_sticker == nullptr || root() == nullptr) {
    return;
  }

  if (!m_loaded && !m_imagePath.empty()) {
    m_renderer = &renderer;
    bool animated = false;
    if (endsWithIgnoreCase(m_imagePath, ".gif")) {
      animated = tryLoadAnimated(renderer);
    }
    if (!animated) {
      m_sticker->setSourceFile(renderer, m_imagePath);
    }
    m_loaded = true;
    if (animated && m_frames.size() > 1) {
      scheduleNextFrame();
    }
  }

  m_sticker->setContentScale(contentScale());
  m_sticker->layout(renderer);
}

bool DesktopStickerWidget::tryLoadAnimated(Renderer& renderer) {
  std::vector<std::uint8_t> bytes = FileUtils::readBinaryFile(m_imagePath);
  if (bytes.empty()) {
    return false;
  }

  auto decoded = codecs::decodeAnimatedGif(bytes.data(), bytes.size(), kMaxStickerGifFrames, kMaxStickerGifBytes);
  if (!decoded) {
    kLog.warn("sticker: failed to decode GIF \"{}\": {}", m_imagePath, decoded.error());
    return false;
  }
  if (decoded->frames.size() <= 1) {
    return false; // single-frame GIF — fall through to setSourceFile (cheap, no extra textures held)
  }
  if (decoded->truncated) {
    kLog.warn("sticker: GIF \"{}\" truncated at {} frames (resource cap)", m_imagePath, decoded->frames.size());
  }

  m_frames.reserve(decoded->frames.size());
  for (const auto& frame : decoded->frames) {
    TextureHandle handle =
        renderer.textureManager().loadFromRgba(frame.rgba.data(), decoded->width, decoded->height, /*mipmap=*/false);
    if (handle.id == 0) {
      kLog.warn("sticker: GPU upload failed for GIF \"{}\"", m_imagePath);
      unloadFrames();
      return false;
    }
    m_frames.push_back(AnimatedFrame{handle, std::chrono::milliseconds(frame.durationMs)});
  }

  m_currentFrame = 0;
  const auto handle = m_frames[0].handle;
  m_sticker->setExternalTexture(renderer, handle, handle.width, handle.height);
  return true;
}

void DesktopStickerWidget::scheduleNextFrame() {
  if (m_frames.size() <= 1) {
    return;
  }
  const auto delay = m_frames[m_currentFrame].duration;
  m_frameTimer.start(delay, [this] { onFrameTimer(); });
}

void DesktopStickerWidget::onFrameTimer() {
  if (m_frames.size() <= 1 || m_renderer == nullptr) {
    return;
  }
  m_currentFrame = (m_currentFrame + 1) % m_frames.size();
  const auto handle = m_frames[m_currentFrame].handle;
  m_sticker->setExternalTexture(*m_renderer, handle, handle.width, handle.height);
  requestRedraw();
  scheduleNextFrame();
}

void DesktopStickerWidget::unloadFrames() {
  if (m_renderer == nullptr) {
    m_frames.clear();
    return;
  }
  for (auto& frame : m_frames) {
    m_renderer->textureManager().unload(frame.handle);
  }
  m_frames.clear();
}
