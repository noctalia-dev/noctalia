#include "render/backend/render_backend.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/image_node.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <memory>
#include <vector>

class RenderContextTestAccess {
public:
  static void setBackend(RenderContext& context, std::unique_ptr<RenderBackend> backend) {
    context.m_backend = std::move(backend);
  }
};

namespace {
  class RecordingBackend final : public RenderBackend {
  public:
    std::vector<RenderImageDraw> draws;
    void initialize(GlSharedContext&) override { std::abort(); }
    void cleanup() override {}
    bool makeCurrent(RenderTarget&) override { std::abort(); }
    bool makeCurrentNoSurface() override { return true; }
    bool beginFrame(RenderTarget&) override { return true; }
    void endFrame(RenderTarget&) override {}
    RenderGraphicsResetStatus graphicsResetStatus() override { return RenderGraphicsResetStatus::NoError; }
    void invalidateGpuResources() override { std::abort(); }
    void abandonAfterGraphicsReset() noexcept override { std::abort(); }
    std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface*) override { std::abort(); }
    std::unique_ptr<RenderFramebuffer> createFramebuffer(std::uint32_t, std::uint32_t) override { std::abort(); }
    void bindFramebuffer(const RenderFramebuffer&) override { std::abort(); }
    void bindDefaultFramebuffer() override { std::abort(); }
    void setViewport(std::uint32_t, std::uint32_t) override { std::abort(); }
    void clear(Color) override { std::abort(); }
    void setBlendMode(RenderBlendMode) override { std::abort(); }
    int maxTextureSize() override { std::abort(); }
    void setScissor(RenderScissor) override { std::abort(); }
    void disableScissor() override {}
    void drawRect(float, float, float, float, const RoundedRectStyle&, const Mat3&) override { std::abort(); }
    void drawImage(const RenderImageDraw& draw) override { draws.push_back(draw); }
    void drawGlyph(const RenderGlyphDraw&) override { std::abort(); }
    void drawSpinner(float, float, float, float, const SpinnerStyle&, const Mat3&) override { std::abort(); }
    void drawCountdownRing(float, float, float, float, const CountdownRingStyle&, const Mat3&) override {
      std::abort();
    }
    void drawScreenCorner(float, float, float, float, const ScreenCornerStyle&, const Mat3&) override { std::abort(); }
    void drawAudioSpectrum(
        float, float, float, float, float, float, const AudioSpectrumStyle&, std::span<const float>, const Mat3&
    ) override {
      std::abort();
    }
    void drawFancyAudioVisualizer(
        TextureId, int, float, float, float, float, const FancyAudioVisualizerStyle&, const Mat3&
    ) override {
      std::abort();
    }
    void drawEffect(float, float, float, float, const EffectStyle&, const Mat3&) override { std::abort(); }
    void drawGraph(TextureId, int, float, float, float, float, const GraphStyle&, const Mat3&) override {
      std::abort();
    }
    void drawWallpaper(const WallpaperDrawParams&) override { std::abort(); }
    void drawWallpaperMask(const WallpaperMaskDrawParams&) override { std::abort(); }
    void drawLockscreenTransition(const LockscreenTransitionDrawParams&) override { std::abort(); }
    void drawFullscreenTexture(TextureId, bool) override { std::abort(); }
    void drawFullscreenTint(Color) override { std::abort(); }
    void drawFramebufferBlur(TextureId, std::uint32_t, std::uint32_t, float, float, float) override { std::abort(); }
    TextureManager& textureManager() override { std::abort(); }
  };
} // namespace

int main() {
  RenderContext context;
  auto backend = std::make_unique<RecordingBackend>();
  auto* recorder = backend.get();
  RenderContextTestAccess::setBackend(context, std::move(backend));
  RenderTarget target;
  target.setLogicalSize(32, 32);
  Node root;
  root.setSize(32, 32);
  auto image = std::make_unique<ImageNode>();
  auto* imageNode = image.get();
  image->setSize(32, 32);
  image->setTextureId(TextureId{1});
  image->setBorder(rgba(1.0F, 1.0F, 1.0F, 0.6F), 2.0F);
  root.addChild(std::move(image));

  for (int mode = 0; mode < 3; ++mode) {
    imageNode->setMonochromeTint(mode != 0);
    imageNode->setAlphaMaskTint(mode == 2);
    for (float parentOpacity : {0.5F, 1.0F}) {
      root.setOpacity(parentOpacity);
      for (float imageOpacity : {0.25F, 0.5F, 1.0F}) {
        imageNode->setOpacity(imageOpacity);
        for (float tintAlpha : {0.4F, 1.0F}) {
          imageNode->setTint(rgba(1.0F, 1.0F, 1.0F, tintAlpha));
          recorder->draws.clear();
          context.renderScene(target, &root);
          TEST_CHECK(recorder->draws.size() == 1);
          const auto& draw = recorder->draws.front();
          TEST_CHECK(draw.tint.a == tintAlpha);
          TEST_CHECK(draw.opacity == parentOpacity * imageOpacity);
          TEST_CHECK(draw.borderColor.a == 0.6F);
          TEST_CHECK(draw.borderWidth == 2.0F);
          TEST_CHECK(draw.monochromeTint == (mode != 0));
          TEST_CHECK(draw.alphaMaskTint == (mode == 2));
        }
      }
    }
  }
}
