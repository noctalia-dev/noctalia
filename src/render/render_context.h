#pragma once

#include "render/core/renderer.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/cairo_text_renderer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

class GlSharedContext;
class Node;
class RenderBackend;
class RenderTarget;
enum class RenderGraphicsResetStatus;
struct Mat3;

class RenderContext : public Renderer {
public:
  RenderContext();
  ~RenderContext() override;

  RenderContext(const RenderContext&) = delete;
  RenderContext& operator=(const RenderContext&) = delete;

  void initialize(GlSharedContext& shared);
  void cleanup();
  void prepareForGraphicsReset();
  void restoreAfterGraphicsReset(GlSharedContext& shared);
  void finishGraphicsResetRecovery() noexcept { m_graphicsResetPending = false; }

  void renderScene(RenderTarget& target, Node* sceneRoot);
  void setGraphicsResetCallback(std::function<void(RenderGraphicsResetStatus)> callback) {
    m_graphicsResetCallback = std::move(callback);
  }
  // Returns false if the surface could not be made current (e.g. teardown);
  // best-effort callers may ignore it, render paths must skip the frame.
  bool makeCurrent(RenderTarget& target);
  // Sync text/glyph renderer content scale to the given target's
  // buffer-to-logical ratio. Must be called before any measureText /
  // measureGlyph performed on behalf of this target, because those
  // results depend on the rasterization scale and get baked into node
  // positions during layout.
  void syncContentScale(RenderTarget& target);
  void setTextFontFamily(std::string family);
  void notifyFontConfigChanged() override;

  // Request that uploaded text- and icon-glyph textures be dropped and
  // re-rasterized. The drop is deferred to the next renderScene so it runs with
  // the context current. Used to recover from GPU memory loss across
  // suspend/resume.
  void invalidateGlyphTexturesNextFrame() noexcept { m_glyphTexturesDirty = true; }
  void invalidateGpuResourcesNextFrame() noexcept;

  [[nodiscard]] RenderBackend& backend() noexcept { return *m_backend; }
  [[nodiscard]] const RenderBackend& backend() const noexcept { return *m_backend; }

  // Renderer interface — used by widgets for measurement and textures
  [[nodiscard]] TextMetrics measureText(
      std::string_view text, float fontSize, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {},
      TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  ) override;
  [[nodiscard]] TextMetrics measureFont(float fontSize, FontWeight fontWeight) override;
  void measureTextCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      FontWeight fontWeight = FontWeight::Normal
  ) override;
  void measureTextCursorStopsWrapped(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, FontWeight fontWeight = FontWeight::Normal
  ) override;
  [[nodiscard]] TextMetrics measureGlyph(char32_t codepoint, float fontSize) override;
  [[nodiscard]] TextureManager& textureManager() override;
  [[nodiscard]] float renderScale() const noexcept override { return m_renderScale; }
  [[nodiscard]] std::uint64_t textMetricsGeneration() const noexcept override { return m_textMetricsGeneration; }

private:
  bool makeCurrentNoSurface();
  void handleGraphicsReset(RenderGraphicsResetStatus status);
  void renderNode(
      const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh, float bw, float bh,
      float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip, bool ignoreNodeOpacity = false
  );

  std::unique_ptr<RenderBackend> m_backend;
  CairoTextRenderer m_textRenderer;
  CairoGlyphRenderer m_glyphRenderer;
  std::string m_textFontFamily = "sans-serif";
  float m_renderScale = 1.0f;
  std::uint64_t m_textMetricsGeneration = 1;
  std::uint64_t m_gpuResourceGeneration = 0;
  bool m_glyphTexturesDirty = false;
  bool m_graphicsResetPending = false;
  std::function<void(RenderGraphicsResetStatus)> m_graphicsResetCallback;
};
