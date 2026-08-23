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
struct WallpaperMaskDrawParams;

class RenderContext {
public:
  RenderContext();
  ~RenderContext();

  RenderContext(const RenderContext&) = delete;
  RenderContext& operator=(const RenderContext&) = delete;

  void initialize(GlSharedContext& shared);
  void cleanup();
  void prepareForGraphicsReset();
  void restoreAfterGraphicsReset(GlSharedContext& shared);
  void finishGraphicsResetRecovery() noexcept { m_graphicsResetPending = false; }

  void renderScene(RenderTarget& target, Node* sceneRoot, const WallpaperMaskDrawParams* wallpaperMask = nullptr);
  void setGraphicsResetCallback(std::function<void(RenderGraphicsResetStatus)> callback) {
    m_graphicsResetCallback = std::move(callback);
  }
  // Returns false if the surface could not be made current (e.g. teardown);
  // best-effort callers may ignore it, render paths must skip the frame.
  bool makeCurrent(RenderTarget& target);
  void setTextFontFamily(std::string family);
  void setTextBaseDirection(bool rtl);
  void notifyFontConfigChanged();

  // Request that uploaded text- and icon-glyph textures be dropped and
  // re-rasterized. The drop is deferred to the next renderScene so it runs with
  // the context current. Used to recover from GPU memory loss across
  // suspend/resume.
  void invalidateGlyphTexturesNextFrame() noexcept { m_glyphTexturesDirty = true; }
  void invalidateGpuResourcesNextFrame() noexcept;

  [[nodiscard]] RenderBackend& backend() noexcept { return *m_backend; }
  [[nodiscard]] const RenderBackend& backend() const noexcept { return *m_backend; }

  // Texture/text-generation access for ScaledRenderer views and graphics-lifecycle owners.
  [[nodiscard]] TextureManager& textureManager();
  [[nodiscard]] std::uint64_t textMetricsGeneration() const noexcept { return m_textMetricsGeneration; }

private:
  friend class ScaledRenderer;

  // Scale-parameterized measurement/text ops. ScaledRenderer forwards here with
  // an explicit scale; no call mutates any shared render scale.
  [[nodiscard]] TextMetrics measureTextScaled(
      float scale, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines,
      TextAlign align, std::string_view fontFamily, TextEllipsize ellipsize, bool useMarkup
  );
  [[nodiscard]] TextMetrics measureFontScaled(float scale, float fontSize, FontWeight fontWeight);
  void measureTextCursorStopsScaled(
      float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
      std::vector<float>& outStops, FontWeight fontWeight
  );
  void measureTextCursorStopsWrappedScaled(
      float scale, std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, FontWeight fontWeight
  );
  [[nodiscard]] TextMetrics measureGlyphScaled(float scale, char32_t codepoint, float fontSize);

  bool makeCurrentNoSurface();
  void handleGraphicsReset(RenderGraphicsResetStatus status);
  void renderNode(
      float renderScale, const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh,
      float bw, float bh, float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip,
      bool ignoreNodeOpacity, bool parentPaintContained
  );

  std::unique_ptr<RenderBackend> m_backend;
  CairoTextRenderer m_textRenderer;
  CairoGlyphRenderer m_glyphRenderer;
  std::string m_textFontFamily = "sans-serif";
  bool m_textBaseDirRtl = false;
  std::uint64_t m_textMetricsGeneration = 1;
  std::uint64_t m_gpuResourceGeneration = 0;
  bool m_glyphTexturesDirty = false;
  bool m_graphicsResetPending = false;
  std::function<void(RenderGraphicsResetStatus)> m_graphicsResetCallback;
};
