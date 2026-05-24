#include "render/render_context.h"

#include "core/log.h"
#include "core/resource_paths.h"
#include "core/ui_phase.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_handle.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_target.h"
#include "render/scene/audio_spectrum_node.h"
#include "render/scene/effect_node.h"
#include "render/scene/glyph_node.h"
#include "render/scene/graph_node.h"
#include "render/scene/image_node.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "render/scene/screen_corner_node.h"
#include "render/scene/spinner_node.h"
#include "render/scene/text_node.h"
#include "render/scene/wallpaper_node.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("render");
  constexpr float kSlowRenderOperationDebugMs = 50.0f;
  constexpr float kSlowRenderOperationWarnMs = 1000.0f;

} // namespace

namespace {

  float elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename... Args> void logSlowRenderOperation(float ms, std::format_string<Args...> fmt, Args&&... args) {
    if (ms >= kSlowRenderOperationWarnMs) {
      kLog.warn(fmt, std::forward<Args>(args)...);
    } else if (ms >= kSlowRenderOperationDebugMs) {
      kLog.debug(fmt, std::forward<Args>(args)...);
    }
  }

  RenderScissor
  scissorForClip(float sw, float sh, float bw, float bh, float left, float top, float right, float bottom) {
    const float scaleX = sw > 0.0f ? bw / sw : 1.0f;
    const float scaleY = sh > 0.0f ? bh / sh : 1.0f;
    const auto clampX = [bw](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bw)));
    };
    const auto clampY = [bh](std::int32_t value) {
      return std::clamp(value, std::int32_t{0}, static_cast<std::int32_t>(std::ceil(bh)));
    };

    // Round clip edges independently in buffer space. Rounding the size after
    // flooring the origin can drop the final column/row for fractional origins.
    const std::int32_t x0 = clampX(static_cast<std::int32_t>(std::floor(left * scaleX)));
    const std::int32_t x1 = clampX(static_cast<std::int32_t>(std::ceil(right * scaleX)));
    const std::int32_t y0 = clampY(static_cast<std::int32_t>(std::floor((sh - bottom) * scaleY)));
    const std::int32_t y1 = clampY(static_cast<std::int32_t>(std::ceil((sh - top) * scaleY)));
    return RenderScissor{
        .x = x0,
        .y = y0,
        .width = std::max(std::int32_t{0}, x1 - x0),
        .height = std::max(std::int32_t{0}, y1 - y0),
    };
  }

  Mat3 nodeLocalTransform(const Node* node) {
    const float cx = node->width() * 0.5f;
    const float cy = node->height() * 0.5f;
    return Mat3::translation(node->x(), node->y()) * Mat3::translation(cx, cy) * Mat3::rotation(node->rotation()) *
           Mat3::scale(node->scale(), node->scale()) * Mat3::translation(-cx, -cy);
  }

} // namespace

RenderContext::RenderContext() = default;

RenderContext::~RenderContext() { cleanup(); }

void RenderContext::initialize(GlSharedContext& shared) {
  cleanup();
  m_backend = createDefaultRenderBackend();
  m_backend->initialize(shared);

  // Pango handles font fallback via Fontconfig automatically — no explicit chain.
  m_backend->textureManager().probeExtensions();
  m_textRenderer.initialize(m_backend.get(), &m_backend->textureManager());
  m_glyphRenderer.initialize(
      paths::assetPath("fonts/tabler.ttf").string(), m_backend.get(), &m_backend->textureManager()
  );
  m_textFontFamily = "sans-serif";
  ++m_textMetricsGeneration;
}

void RenderContext::makeCurrentNoSurface() {
  if (m_backend == nullptr) {
    return;
  }
  m_backend->makeCurrentNoSurface();
}

void RenderContext::makeCurrent(RenderTarget& target) {
  if (m_backend == nullptr) {
    throw std::runtime_error("RenderContext has no initialized backend");
  }
  m_backend->makeCurrent(target);
  // Sync the shared text/glyph renderer to this target's buffer/logical ratio
  // unconditionally on every makeCurrent. The text and glyph renderers are
  // process-singletons; if this is left out, layout/measure on one surface can
  // run at a stale scale set by the last-rendered surface (visible as label
  // jitter on multi-monitor setups with mixed fractional scales). renderScene
  // also goes through this path indirectly via beginFrame.
  syncContentScale(target);
}

void RenderContext::syncContentScale(RenderTarget& target) {
  const auto sw = static_cast<float>(target.logicalWidth());
  const auto bw = static_cast<float>(target.bufferWidth());
  m_renderScale = sw > 0.0f ? std::max(1.0f, bw / sw) : 1.0f;
  m_textRenderer.setContentScale(m_renderScale);
  m_glyphRenderer.setContentScale(m_renderScale);
}

void RenderContext::setTextFontFamily(std::string family) {
  if (family.empty()) {
    family = "sans-serif";
  }
  if (m_textFontFamily == family) {
    return;
  }
  makeCurrentNoSurface();
  m_textFontFamily = std::move(family);
  m_textRenderer.setFontFamily(m_textFontFamily);
  ++m_textMetricsGeneration;
}

void RenderContext::notifyFontConfigChanged() {
  m_textRenderer.notifyFontConfigChanged();
  ++m_textMetricsGeneration;
}

void RenderContext::renderScene(RenderTarget& target, Node* sceneRoot) {
  UiPhaseScope renderPhase(UiPhase::Render);
  if (m_backend == nullptr) {
    return;
  }
  const auto totalStart = std::chrono::steady_clock::now();
  m_backend->beginFrame(target);
  syncContentScale(target);

  const auto drawStart = std::chrono::steady_clock::now();
  if (sceneRoot != nullptr) {
    const auto sw = static_cast<float>(target.logicalWidth());
    const auto sh = static_cast<float>(target.logicalHeight());
    const auto bw = static_cast<float>(target.bufferWidth());
    const auto bh = static_cast<float>(target.bufferHeight());
    renderNode(sceneRoot, Mat3::identity(), 1.0f, sw, sh, bw, bh, 0.0f, 0.0f, sw, sh, false);
  }
  float ms = elapsedSince(drawStart);
  logSlowRenderOperation(
      ms, "scene draw took {:.1f}ms ({}x{} logical, {}x{} buffer)", ms, target.logicalWidth(), target.logicalHeight(),
      target.bufferWidth(), target.bufferHeight()
  );

  m_backend->endFrame(target);
  ms = elapsedSince(totalStart);
  logSlowRenderOperation(ms, "renderScene took {:.1f}ms total", ms);
}

TextMetrics RenderContext::measureText(
    std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines, TextAlign align,
    std::string_view fontFamily
) {
  auto m = m_textRenderer.measure(text, fontSize, fontWeight, maxWidth, maxLines, align, fontFamily);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight
  };
}

TextMetrics RenderContext::measureFont(float fontSize, FontWeight fontWeight) {
  auto m = m_textRenderer.measureFont(fontSize, fontWeight);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.inkTop,
      .inkBottom = m.inkBottom,
      .inkLeft = m.inkLeft,
      .inkRight = m.inkRight
  };
}

void RenderContext::measureTextCursorStops(
    std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
    FontWeight fontWeight
) {
  m_textRenderer.measureCursorStops(text, fontSize, byteOffsets, outStops, fontWeight);
}

TextMetrics RenderContext::measureGlyph(char32_t codepoint, float fontSize) {
  auto m = m_glyphRenderer.measureGlyph(codepoint, fontSize);
  return TextMetrics{
      .width = m.width,
      .left = m.left,
      .right = m.right,
      .top = m.top,
      .bottom = m.bottom,
      .inkTop = m.top,
      .inkBottom = m.bottom,
      .inkLeft = m.left,
      .inkRight = m.right
  };
}

TextureManager& RenderContext::textureManager() {
  makeCurrentNoSurface();
  return m_backend->textureManager();
}

void RenderContext::renderNode(
    const Node* node, const Mat3& parentTransform, float parentOpacity, float sw, float sh, float bw, float bh,
    float clipLeft, float clipTop, float clipRight, float clipBottom, bool hasClip
) {
  if (!node->visible()) {
    return;
  }

  const Mat3 worldTransform = parentTransform * nodeLocalTransform(node);
  const float effectiveOpacity = parentOpacity * node->opacity();
  float boundsLeft = 0.0f;
  float boundsTop = 0.0f;
  float boundsRight = 0.0f;
  float boundsBottom = 0.0f;
  Node::transformedBounds(node, worldTransform, boundsLeft, boundsTop, boundsRight, boundsBottom);

  if (hasClip) {
    m_backend->setScissor(scissorForClip(sw, sh, bw, bh, clipLeft, clipTop, clipRight, clipBottom));
  } else {
    m_backend->disableScissor();
  }

  switch (node->type()) {
  case NodeType::Rect: {
    const auto* rect = static_cast<const RectNode*>(node);
    auto style = rect->style();
    style.fill.a *= effectiveOpacity;
    style.border.a *= effectiveOpacity;
    for (auto& stop : style.gradientStops) {
      stop.color.a *= effectiveOpacity;
    }
    m_backend->drawRect(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::Text: {
    const auto* text = static_cast<const TextNode*>(node);
    if (!text->text().empty()) {
      const auto& font = text->fontFamily();
      if (text->hasShadow()) {
        auto shadowColor = text->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(text->shadowOffsetX(), text->shadowOffsetY());
        m_textRenderer.draw(
            sw, sh, 0.0f, 0.0f, text->text(), text->fontSize(), shadowColor, shadowTransform, text->fontWeight(),
            text->maxWidth(), text->maxLines(), text->textAlign(), font
        );
      }
      auto color = text->color();
      color.a *= effectiveOpacity;
      m_textRenderer.draw(
          sw, sh, 0.0f, 0.0f, text->text(), text->fontSize(), color, worldTransform, text->fontWeight(),
          text->maxWidth(), text->maxLines(), text->textAlign(), font
      );
    }
    break;
  }
  case NodeType::Image: {
    const auto* img = static_cast<const ImageNode*>(node);
    if (img->textureId() != 0) {
      auto tint = img->tint();
      tint.a *= effectiveOpacity;
      m_backend->drawImage(
          RenderImageDraw{
              .texture = img->textureId(),
              .surfaceWidth = sw,
              .surfaceHeight = sh,
              .width = node->width(),
              .height = node->height(),
              .tint = tint,
              .opacity = effectiveOpacity,
              .radius = img->radius(),
              .borderColor = img->borderColor(),
              .borderWidth = img->borderWidth(),
              .fitMode = static_cast<RenderImageFitMode>(img->fitMode()),
              .textureWidth = static_cast<float>(img->textureWidth()),
              .textureHeight = static_cast<float>(img->textureHeight()),
              .transform = worldTransform,
          }
      );
    }
    break;
  }
  case NodeType::Glyph: {
    const auto* icon = static_cast<const GlyphNode*>(node);
    if (icon->codepoint() != 0) {
      if (icon->hasShadow()) {
        auto shadowColor = icon->shadowColor();
        shadowColor.a *= effectiveOpacity;
        const Mat3 shadowTransform = worldTransform * Mat3::translation(icon->shadowOffsetX(), icon->shadowOffsetY());
        m_glyphRenderer.drawGlyph(
            sw, sh, 0.0f, 0.0f, icon->codepoint(), icon->fontSize(), shadowColor, shadowTransform
        );
      }
      auto color = icon->color();
      color.a *= effectiveOpacity;
      m_glyphRenderer.drawGlyph(sw, sh, 0.0f, 0.0f, icon->codepoint(), icon->fontSize(), color, worldTransform);
    }
    break;
  }
  case NodeType::Spinner: {
    const auto* spinner = static_cast<const SpinnerNode*>(node);
    auto style = spinner->style();
    style.color.a *= effectiveOpacity;
    m_backend->drawSpinner(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::ScreenCorner: {
    const auto* corner = static_cast<const ScreenCornerNode*>(node);
    auto style = corner->style();
    style.color.a *= effectiveOpacity;
    const float pixelScaleX = sw > 0.0f ? bw / sw : 1.0f;
    const float pixelScaleY = sh > 0.0f ? bh / sh : 1.0f;
    m_backend->drawScreenCorner(sw, sh, pixelScaleX, pixelScaleY, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::AudioSpectrum: {
    const auto* spectrum = static_cast<const AudioSpectrumNode*>(node);
    auto style = spectrum->style();
    style.lowColor.a *= effectiveOpacity;
    style.highColor.a *= effectiveOpacity;
    const float pixelScaleX = sw > 0.0f ? bw / sw : 1.0f;
    const float pixelScaleY = sh > 0.0f ? bh / sh : 1.0f;
    m_backend->drawAudioSpectrum(
        sw, sh, pixelScaleX, pixelScaleY, node->width(), node->height(), style, spectrum->values(), worldTransform
    );
    break;
  }
  case NodeType::Effect: {
    const auto* effect = static_cast<const EffectNode*>(node);
    auto style = effect->style();
    style.bgColor.a *= effectiveOpacity;
    m_backend->drawEffect(sw, sh, node->width(), node->height(), style, worldTransform);
    break;
  }
  case NodeType::Graph: {
    const auto* graph = static_cast<const GraphNode*>(node);
    if (graph->textureId() != 0) {
      auto style = graph->style();
      style.lineColor1.a *= effectiveOpacity;
      style.lineColor2.a *= effectiveOpacity;
      style.graphFillOpacity *= effectiveOpacity;
      m_backend->drawGraph(
          graph->textureId(), graph->textureWidth(), sw, sh, node->width(), node->height(), style, worldTransform
      );
    }
    break;
  }
  case NodeType::Wallpaper: {
    const auto* wallpaper = static_cast<const WallpaperNode*>(node);
    const bool hasSource1 = wallpaper->sourceKind1() == WallpaperSourceKind::Color || wallpaper->texture1() != 0;
    if (hasSource1) {
      const bool hasSource2 = wallpaper->sourceKind2() == WallpaperSourceKind::Color || wallpaper->texture2() != 0;
      const WallpaperSourceKind sourceKind2 = hasSource2 ? wallpaper->sourceKind2() : wallpaper->sourceKind1();
      const TextureId texture2 = hasSource2 ? wallpaper->texture2() : wallpaper->texture1();
      const Color& sourceColor2 = hasSource2 ? wallpaper->sourceColor2() : wallpaper->sourceColor1();
      const float imageWidth2 = hasSource2 ? wallpaper->imageWidth2() : wallpaper->imageWidth1();
      const float imageHeight2 = hasSource2 ? wallpaper->imageHeight2() : wallpaper->imageHeight1();
      const float progress = hasSource2 ? wallpaper->progress() : 0.0f;
      m_backend->drawWallpaper(
          wallpaper->transition(), wallpaper->sourceKind1(), wallpaper->texture1(), wallpaper->sourceColor1(),
          sourceKind2, texture2, sourceColor2, sw, sh, node->width(), node->height(), wallpaper->imageWidth1(),
          wallpaper->imageHeight1(), imageWidth2, imageHeight2, progress, static_cast<float>(wallpaper->fillMode()),
          wallpaper->transitionParams(), wallpaper->fillColor(), worldTransform
      );
    }
    break;
  }
  case NodeType::Base:
    break;
  }

  // Fast path: children are already in zIndex order (the common case — most
  // callers never touch zIndex, or set it identically across siblings). Skip
  // allocating/sorting a side vector and iterate the child list directly.
  // Only fall back to the sorted copy when there's an actual out-of-order
  // pair, which removes a per-node heap allocation from every rendered frame.
  const auto& children = node->children();
  bool childrenSorted = true;
  for (std::size_t i = 1; i < children.size(); ++i) {
    if (children[i]->zIndex() < children[i - 1]->zIndex()) {
      childrenSorted = false;
      break;
    }
  }

  std::vector<const Node*> orderedChildren;
  if (!childrenSorted) {
    orderedChildren.reserve(children.size());
    for (const auto& child : children) {
      orderedChildren.push_back(child.get());
    }
    std::stable_sort(orderedChildren.begin(), orderedChildren.end(), [](const Node* a, const Node* b) {
      return a->zIndex() < b->zIndex();
    });
  }

  float childClipLeft = clipLeft;
  float childClipTop = clipTop;
  float childClipRight = clipRight;
  float childClipBottom = clipBottom;
  bool childHasClip = hasClip;

  if (node->clipChildren()) {
    childClipLeft = hasClip ? std::max(childClipLeft, boundsLeft) : boundsLeft;
    childClipTop = hasClip ? std::max(childClipTop, boundsTop) : boundsTop;
    childClipRight = hasClip ? std::min(childClipRight, boundsRight) : boundsRight;
    childClipBottom = hasClip ? std::min(childClipBottom, boundsBottom) : boundsBottom;
    childHasClip = true;
  }

  if (childHasClip && (childClipRight <= childClipLeft || childClipBottom <= childClipTop)) {
    return;
  }

  if (childrenSorted) {
    for (const auto& child : children) {
      renderNode(
          child.get(), worldTransform, effectiveOpacity, sw, sh, bw, bh, childClipLeft, childClipTop, childClipRight,
          childClipBottom, childHasClip
      );
    }
  } else {
    for (const auto* child : orderedChildren) {
      renderNode(
          child, worldTransform, effectiveOpacity, sw, sh, bw, bh, childClipLeft, childClipTop, childClipRight,
          childClipBottom, childHasClip
      );
    }
  }
}

void RenderContext::cleanup() {
  if (m_backend != nullptr) {
    // Need a current context to destroy GL resources, but we may not have a surface.
    m_backend->makeCurrentNoSurface();
  }

  // Text renderers first — they destroy GL textures and need a current context.
  m_textRenderer.cleanup();
  m_glyphRenderer.cleanup();

  if (m_backend != nullptr) {
    m_backend->cleanup();
    m_backend.reset();
  }
}
