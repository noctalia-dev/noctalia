#include "render/scene/graph_node.h"

#include "render/core/texture_manager.h"

#include <algorithm>
#include <cstdint>
#include <vector>

GraphNode::~GraphNode() {
  if (m_textureManager != nullptr) {
    m_textureManager->unload(m_texture);
  }
}

void GraphNode::setData(
    TextureManager& textures, const float* primary, int primaryCount, const float* secondary, int secondaryCount,
    const float* tertiary, int tertiaryCount
) {
  if (m_textureManager != nullptr && m_textureManager != &textures) {
    m_textureManager->unload(m_texture);
    m_texCapacity = 0;
  }
  m_textureManager = &textures;

  const int newWidth = std::max({primaryCount + 1, secondaryCount + 1, tertiaryCount + 1, 4});

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(newWidth) * 4, 0);
  for (int i = 0; i < newWidth; ++i) {
    float r = (i < primaryCount) ? primary[i] : (primaryCount > 0 ? primary[primaryCount - 1] : 0.0f);
    float g = (i < secondaryCount) ? secondary[i] : (secondaryCount > 0 ? secondary[secondaryCount - 1] : 0.0f);
    float b = (i < tertiaryCount) ? tertiary[i] : (tertiaryCount > 0 ? tertiary[tertiaryCount - 1] : 0.0f);
    const auto base = static_cast<std::size_t>(i) * 4;
    pixels[base] = static_cast<std::uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
    pixels[base + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
    pixels[base + 2] = static_cast<std::uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
    pixels[base + 3] = 255;
  }

  if (m_texture.id == 0 || newWidth > m_texCapacity) {
    if (!textures.replace(m_texture, pixels.data(), newWidth, 1, TextureDataFormat::Rgba, TextureFilter::Nearest)) {
      return;
    }
    m_texCapacity = newWidth;
  } else if (!textures.updateSubImage(m_texture, pixels.data(), 0, 0, newWidth, 1, TextureDataFormat::Rgba)) {
    return;
  }

  m_texWidth = newWidth;
  markPaintDirty();
}
