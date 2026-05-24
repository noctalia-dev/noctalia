#pragma once

#include "render/backend/render_backend.h"
#include "render/core/mat3.h"
#include "render/core/renderer.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/cairo_text_renderer.h"

#include <cstdint>
#include <memory>
#include <string>

class GlSharedContext;
class Node;
class RenderTarget;

class RenderContext : public Renderer {
public:
  RenderContext();
  ~RenderContext() override;

  RenderContext(const RenderContext&) = delete;
  RenderContext& operator=(const RenderContext&) = delete;

  void initialize(GlSharedContext& shared);
  void cleanup();

  void renderScene(RenderTarget& target, Node* sceneRoot);
  void makeCurrent(RenderTarget& target);
  // Sync text/glyph renderer content scale to the given target's
  // buffer-to-logical ratio. Must be called before any measureText /
  // measureGlyph performed on behalf of this target, because those
  // results depend on the rasterization scale and get baked into node
  // positions during layout.
  void syncContentScale(RenderTarget& target);
  void setTextFontFamily(std::string family);
  void notifyFontConfigChanged() override;

  [[nodiscard]] RenderBackend& backend() noexcept { return *m_backend; }
  [[nodiscard]] const RenderBackend& backend() const noexcept { return *m_backend; }

  // Renderer interface — used by widgets for measurement and textures
  [[nodiscard]] TextMetrics measureText(
      std::string_view text, float fontSize, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {}
  ) override;
  [[nodiscard]] TextMetrics measureFont(float fontSize, FontWeight fontWeight) override;
  void measureTextCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      FontWeight fontWeight = FontWeight::Normal
  ) override;
  [[nodiscard]] TextMetrics measureGlyph(char32_t codepoint, float fontSize) override;
  [[nodiscard]] TextureManager& textureManager() override;
  [[nodiscard]] float renderScale() const noexcept override { return m_renderScale; }
  [[nodiscard]] std::uint64_t textMetricsGeneration() const noexcept override { return m_textMetricsGeneration; }

private:
  void makeCurrentNoSurface();
  void renderNode(
      const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh, float bw, float bh,
      float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip
  );

  std::unique_ptr<RenderBackend> m_backend;
  CairoTextRenderer m_textRenderer;
  CairoGlyphRenderer m_glyphRenderer;
  std::string m_textFontFamily = "sans-serif";
  float m_renderScale = 1.0f;
  std::uint64_t m_textMetricsGeneration = 1;
};
