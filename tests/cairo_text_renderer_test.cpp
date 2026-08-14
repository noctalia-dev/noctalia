#include "render/backend/render_backend.h"
#include "render/core/mat3.h"
#include "render/core/texture_manager.h"
#include "render/text/cairo_text_renderer.h"

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <print>
#include <string_view>

namespace {

  class StubTextureManager final : public TextureManager {
  public:
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRgba(const std::uint8_t*, int, int, bool) override { return {}; }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override {
      return {};
    }
    TextureHandle
    loadFromPixels(const std::uint8_t*, int width, int height, TextureDataFormat, TextureFilter, bool) override {
      return TextureHandle{.id = TextureId{1}, .width = width, .height = height, .generation = 1};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  class RecordingBackend final : public RenderBackend {
  public:
    explicit RecordingBackend(TextureManager& textures) : m_textures(textures) {}

    void initialize(GlSharedContext&) override {}
    void cleanup() override {}
    bool makeCurrent(RenderTarget&) override { return true; }
    bool makeCurrentNoSurface() override { return true; }
    bool beginFrame(RenderTarget&) override { return true; }
    void endFrame(RenderTarget&) override {}
    RenderGraphicsResetStatus graphicsResetStatus() override { return RenderGraphicsResetStatus::NoError; }
    void invalidateGpuResources() override {}
    void abandonAfterGraphicsReset() noexcept override {}
    std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface*) override { return nullptr; }
    std::unique_ptr<RenderFramebuffer> createFramebuffer(std::uint32_t, std::uint32_t) override { return nullptr; }
    void bindFramebuffer(const RenderFramebuffer&) override {}
    void bindDefaultFramebuffer() override {}
    void setViewport(std::uint32_t, std::uint32_t) override {}
    void clear(Color) override {}
    void setBlendMode(RenderBlendMode) override {}
    int maxTextureSize() override { return 2048; }
    void setScissor(RenderScissor) override {}
    void disableScissor() override {}
    void drawRect(float, float, float, float, const RoundedRectStyle&, const Mat3&) override {}
    void drawImage(const RenderImageDraw&) override {}
    void drawGlyph(const RenderGlyphDraw& draw) override { lastGlyph = draw; }
    void drawSpinner(float, float, float, float, const SpinnerStyle&, const Mat3&) override {}
    void drawCountdownRing(float, float, float, float, const CountdownRingStyle&, const Mat3&) override {}
    void drawScreenCorner(float, float, float, float, const ScreenCornerStyle&, const Mat3&) override {}
    void drawAudioSpectrum(
        float, float, float, float, float, float, const AudioSpectrumStyle&, std::span<const float>, const Mat3&
    ) override {}
    void drawFancyAudioVisualizer(
        TextureId, int, float, float, float, float, const FancyAudioVisualizerStyle&, const Mat3&
    ) override {}
    void drawEffect(float, float, float, float, const EffectStyle&, const Mat3&) override {}
    void drawGraph(TextureId, int, float, float, float, float, const GraphStyle&, const Mat3&) override {}
    void drawWallpaper(const WallpaperDrawParams&) override {}
    void drawWallpaperMask(const WallpaperMaskDrawParams&) override {}
    void drawFullscreenTexture(TextureId, bool) override {}
    void drawFullscreenTint(Color) override {}
    void drawFramebufferBlur(TextureId, std::uint32_t, std::uint32_t, float, float, float) override {}
    TextureManager& textureManager() override { return m_textures; }

    std::optional<RenderGlyphDraw> lastGlyph;

  private:
    TextureManager& m_textures;
  };

  bool sameMetrics(const CairoTextRenderer::TextMetrics& lhs, const CairoTextRenderer::TextMetrics& rhs) {
    constexpr float epsilon = 0.0001F;
    return std::abs(lhs.width - rhs.width) < epsilon
        && std::abs(lhs.top - rhs.top) < epsilon
        && std::abs(lhs.bottom - rhs.bottom) < epsilon
        && lhs.lineCount == rhs.lineCount;
  }

  // Wrapping-boundary cache correctness: a cached measurement at one width must
  // not reuse metrics computed for a different width.
  int wrappingBoundaryCase(CairoTextRenderer& renderer) {
    constexpr std::string_view text =
        "Error: Error while parsing table header: expected a comment or whitespace, saw 't'";
    constexpr float fontSize = 14.0F;
    const float naturalWidth = renderer.measure(1.0F, text, fontSize).width;
    constexpr std::array deltas{0.0005F, 0.001F, 0.002F, 0.003F, 0.004F, 0.006F};

    for (const float delta : deltas) {
      renderer.notifyFontConfigChanged();
      const auto above = renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth + delta, 3);
      renderer.notifyFontConfigChanged();
      const auto expectedBelow = renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth - delta, 3);
      if (above.lineCount == expectedBelow.lineCount) {
        continue;
      }

      renderer.notifyFontConfigChanged();
      [[maybe_unused]] const auto cachedAbove =
          renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth + delta, 3);
      const auto cachedBelow = renderer.measure(1.0F, text, fontSize, FontWeight::Normal, naturalWidth - delta, 3);
      if (!sameMetrics(cachedBelow, expectedBelow)) {
        std::println(
            stderr,
            "cairo_text_renderer_test: FAIL: cached measurement at width {} reused metrics from width {} "
            "(expected {} lines, got {})",
            naturalWidth - delta, naturalWidth + delta, expectedBelow.lineCount, cachedBelow.lineCount
        );
        return 1;
      }
      return 0;
    }

    std::println(stderr, "cairo_text_renderer_test: FAIL: could not establish a wrapping boundary");
    return 1;
  }

  // The metrics cache is keyed by content scale: interleaving 1.0x and 1.25x
  // measurements of the same text must return per-scale-repeatable results, and
  // an intervening call at the other scale must not corrupt the cached result.
  int scaleKeyedCacheCase(CairoTextRenderer& renderer) {
    constexpr std::string_view text = "The quick brown fox";
    constexpr float fontSize = 16.0F;

    const auto oneA = renderer.measure(1.0F, text, fontSize);
    const auto hiA = renderer.measure(1.25F, text, fontSize);
    const auto oneB = renderer.measure(1.0F, text, fontSize); // cache hit at 1.0x
    const auto hiB = renderer.measure(1.25F, text, fontSize); // cache hit at 1.25x

    if (!sameMetrics(oneA, oneB)) {
      std::println(
          stderr,
          "cairo_text_renderer_test: FAIL: 1.0x measurement not repeatable across a 1.25x call "
          "(width {} vs {}, lines {} vs {})",
          oneA.width, oneB.width, oneA.lineCount, oneB.lineCount
      );
      return 1;
    }
    if (!sameMetrics(hiA, hiB)) {
      std::println(
          stderr,
          "cairo_text_renderer_test: FAIL: 1.25x measurement not repeatable across a 1.0x call "
          "(width {} vs {}, lines {} vs {})",
          hiA.width, hiB.width, hiA.lineCount, hiB.lineCount
      );
      return 1;
    }
    return 0;
  }

  int gradientColorGlyphOpacityCase() {
    StubTextureManager textures;
    RecordingBackend backend(textures);
    CairoTextRenderer renderer;
    renderer.initialize(&backend, &textures);

    TextGradientStyle gradient;
    gradient.enabled = true;
    for (auto& stop : gradient.stops) {
      stop.color = rgba(1.0F, 1.0F, 1.0F, 0.5F);
    }
    renderer.draw(
        1.0F, 200.0F, 100.0F, 0.0F, 0.0F, "😀", 16.0F, rgba(1.0F, 1.0F, 1.0F, 0.25F), Mat3::identity(),
        FontWeight::Normal, 0.0F, 0, TextAlign::Start, {}, TextEllipsize::End, false, &gradient
    );

    if (!backend.lastGlyph.has_value()) {
      std::println(stderr, "cairo_text_renderer_test: FAIL: gradient color glyph emitted no draw");
      return 1;
    }
    if (std::abs(backend.lastGlyph->opacity - 1.0F) > 0.0001F) {
      std::println(
          stderr, "cairo_text_renderer_test: FAIL: gradient color glyph reused solid opacity ({})",
          backend.lastGlyph->opacity
      );
      return 1;
    }
    return 0;
  }

} // namespace

int main() {
  CairoTextRenderer renderer;
  renderer.initialize(nullptr, nullptr);

  if (const int rc = scaleKeyedCacheCase(renderer); rc != 0) {
    return rc;
  }
  if (const int rc = wrappingBoundaryCase(renderer); rc != 0) {
    return rc;
  }
  return gradientColorGlyphOpacityCase();
}
