#include "render/scene/fancy_audio_visualizer_node.h"

#include "render/core/texture_manager.h"

#include <algorithm>
#include <cstdint>
#include <utility>

FancyAudioVisualizerNode::~FancyAudioVisualizerNode() {
  if (m_textureManager != nullptr) {
    m_textureManager->unload(m_texture);
  }
}

bool FancyAudioVisualizerNode::setSpectrumValues(TextureManager& textures, std::span<const float> values) {
  if (m_textureManager != nullptr && m_textureManager != &textures) {
    m_textureManager->unload(m_texture);
  }
  m_textureManager = &textures;

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kBandCount) * 4U, 0);
  for (int i = 0; i < kBandCount; ++i) {
    const float value = i < static_cast<int>(values.size()) ? values[static_cast<std::size_t>(i)] : 0.0F;
    const auto amplitude = static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F);
    const auto base = static_cast<std::size_t>(i) * 4U;
    pixels[base] = amplitude;
    pixels[base + 1] = amplitude;
    pixels[base + 2] = amplitude;
    pixels[base + 3] = 255;
  }

  if (m_texture.valid() && m_pixels == pixels && m_texWidth == kBandCount) {
    return false;
  }

  m_pixels = std::move(pixels);
  if (m_texture.id == 0) {
    if (!textures.replace(m_texture, m_pixels.data(), kBandCount, 1, TextureDataFormat::Rgba, TextureFilter::Nearest)) {
      return false;
    }
  } else if (!textures.updateSubImage(m_texture, m_pixels.data(), 0, 0, kBandCount, 1, TextureDataFormat::Rgba)) {
    return false;
  }

  m_texWidth = kBandCount;
  markPaintDirty();
  return true;
}

void FancyAudioVisualizerNode::doInvalidateGpuResources(Renderer& renderer) {
  (void)renderer;
  if (m_textureManager == nullptr) {
    m_texture = {};
    m_texWidth = 0;
    return;
  }

  if (m_texture.id != 0) {
    m_textureManager->unload(m_texture);
  }
  m_texture = {};
  m_texWidth = 0;

  if (m_pixels.empty()) {
    markPaintDirty();
    return;
  }

  if (m_textureManager->replace(
          m_texture, m_pixels.data(), kBandCount, 1, TextureDataFormat::Rgba, TextureFilter::Nearest
      )) {
    m_texWidth = kBandCount;
  }
  markPaintDirty();
}
