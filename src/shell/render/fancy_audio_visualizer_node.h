#pragma once

#include "render/core/texture_handle.h"
#include "render/scene/custom_draw_node.h"
#include "shell/render/shell_render_styles.h"

#include <cstdint>
#include <span>
#include <vector>

class TextureManager;

class FancyAudioVisualizerNode : public CustomDrawNode {
public:
  static constexpr int kBandCount = 32;

  FancyAudioVisualizerNode() = default;
  ~FancyAudioVisualizerNode() override;

  FancyAudioVisualizerNode(const FancyAudioVisualizerNode&) = delete;
  FancyAudioVisualizerNode& operator=(const FancyAudioVisualizerNode&) = delete;

  const FancyAudioVisualizerStyle& style() const noexcept { return m_style; }
  TextureId textureId() const noexcept { return m_texture.id; }

  void setStyle(const FancyAudioVisualizerStyle& style) {
    if (m_style == style) {
      return;
    }
    m_style = style;
    markPaintDirty();
  }

  bool setSpectrumValues(TextureManager& textures, std::span<const float> values);

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  void doInvalidateGpuResources(Renderer& renderer) override;

  FancyAudioVisualizerStyle m_style;
  TextureManager* m_textureManager = nullptr;
  TextureHandle m_texture;
  std::vector<std::uint8_t> m_pixels;
  int m_texWidth = 0;
};
