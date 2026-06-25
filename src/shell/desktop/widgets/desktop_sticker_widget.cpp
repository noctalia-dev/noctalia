#include "shell/desktop/widgets/desktop_sticker_widget.h"

#include "core/log.h"
#include "render/core/image_decoder.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <utility>

namespace {

  constexpr Logger kLog("desktop");
  constexpr float kDefaultStickerSize = 200.0f;
  constexpr int kMaxStickerGifFrames = 512;
  constexpr std::size_t kMaxStickerGifBytes = 96ull * 1024 * 1024;

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
    : m_imagePath(std::move(imagePath)), m_opacity(std::clamp(opacity, 0.0f, 1.0f)) {}

DesktopStickerWidget::~DesktopStickerWidget() { unloadFrames(); }

void DesktopStickerWidget::create() {
  auto rootNode = std::make_unique<Node>();
  rootNode->setOpacity(m_opacity);

  auto image = ui::image({
      .out = &m_image,
      .fit = ImageFit::Contain,
  });
  rootNode->addChild(std::move(image));
  setRoot(std::move(rootNode));
}

bool DesktopStickerWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "opacity") {
    if (const auto* v = std::get_if<double>(&value)) {
      m_opacity = std::clamp(static_cast<float>(*v), 0.0f, 1.0f);
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
  if (m_image == nullptr || root() == nullptr) {
    return;
  }

  if (!m_loaded && !m_imagePath.empty()) {
    m_renderer = &renderer;
    bool animated = false;
    if (endsWithIgnoreCase(m_imagePath, ".gif")) {
      animated = tryLoadAnimated(renderer);
    }
    if (!animated) {
      m_image->setSourceFile(renderer, m_imagePath);
    }
    m_loaded = true;
    if (animated && m_frames.size() > 1) {
      scheduleNextFrame();
    }
  }

  const float baseSize = kDefaultStickerSize * contentScale();
  float width = baseSize;
  float height = baseSize;

  if (m_image->hasImage()) {
    const float ar = m_image->aspectRatio();
    if (ar >= 1.0f) {
      height = baseSize / ar;
    } else {
      width = baseSize * ar;
    }
  }

  m_image->setSize(width, height);
  root()->setSize(width, height);
}

bool DesktopStickerWidget::tryLoadAnimated(Renderer& renderer) {
  std::vector<std::uint8_t> bytes = FileUtils::readBinaryFile(m_imagePath);
  if (bytes.empty()) {
    return false;
  }

  auto decoded = decodeAnimatedGif(bytes.data(), bytes.size(), kMaxStickerGifFrames, kMaxStickerGifBytes);
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
  m_image->setExternalTexture(renderer, m_frames[0].handle);
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
  m_image->setExternalTexture(*m_renderer, m_frames[m_currentFrame].handle);
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
