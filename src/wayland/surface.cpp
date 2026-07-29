#include "wayland/surface.h"

#include "core/log.h"
#include "core/ui_phase.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "util/sys_utils.h"
#include "viewporter-client-protocol.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace {

  constexpr Logger kLog("surface");
  constexpr float kSlowSurfaceOperationDebugMs = 50.0f;
  constexpr float kSlowSurfaceOperationWarnMs = 1000.0f;

  const wl_callback_listener kFrameListener = {
      .done = &Surface::handleFrameDone,
  };

  void surfaceEnter(void* data, wl_surface* surface, wl_output* output) {
    auto* self = static_cast<Surface*>(data);
    self->onSurfaceOutputEnter(surface, output);
  }

  void surfaceLeave(void* data, wl_surface* surface, wl_output* output) {
    auto* self = static_cast<Surface*>(data);
    self->onSurfaceOutputLeave(surface, output);
  }

  const wl_surface_listener kSurfaceListener = {
      .enter = surfaceEnter,
      .leave = surfaceLeave,
      .preferred_buffer_scale = nullptr,
      .preferred_buffer_transform = nullptr,
  };

  void preferredFractionalScale(void* data, wp_fractional_scale_v1* /*fractionalScale*/, std::uint32_t scale) {
    auto* self = static_cast<Surface*>(data);
    self->onPreferredFractionalScale(scale);
  }

  const wp_fractional_scale_v1_listener kFractionalScaleListener = {
      .preferred_scale = preferredFractionalScale,
  };

  std::uint32_t scaledExtent(std::uint32_t logical, float scale) noexcept {
    if (logical == 0) {
      return 0;
    }
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(static_cast<float>(logical) * scale)));
  }

  std::vector<Surface*>& pendingRenderQueue() {
    static std::vector<Surface*> queue;
    return queue;
  }

  std::vector<Surface*>& pendingFrameWorkQueue() {
    static std::vector<Surface*> queue;
    return queue;
  }

  bool idleProfileEnabled() {
    static const bool enabled = SysUtils::isEnvFlagOn("NOCTALIA_IDLE_PROFILE");
    return enabled;
  }

  bool blurTraceEnabled() {
    static const bool enabled = SysUtils::isEnvFlagOn("NOCTALIA_BLUR_TRACE");
    return enabled;
  }

  std::string_view surfaceTraceName(const Surface& surface) {
    const auto& name = surface.debugName();
    if (name.empty()) {
      return "surface";
    }
    return name;
  }

  InputRect boundsForRects(const std::vector<InputRect>& rects) {
    if (rects.empty()) {
      return {};
    }

    int minX = rects.front().x;
    int minY = rects.front().y;
    int maxX = rects.front().x + rects.front().width;
    int maxY = rects.front().y + rects.front().height;
    for (const auto& rect : rects) {
      minX = std::min(minX, rect.x);
      minY = std::min(minY, rect.y);
      maxX = std::max(maxX, rect.x + rect.width);
      maxY = std::max(maxY, rect.y + rect.height);
    }

    return InputRect{minX, minY, maxX - minX, maxY - minY};
  }

  std::string rectListPreview(const std::vector<InputRect>& rects) {
    if (rects.empty()) {
      return "[]";
    }

    constexpr std::size_t kPreviewCount = 6;
    const std::size_t previewCount = std::min(kPreviewCount, rects.size());
    std::string out = "[";
    for (std::size_t i = 0; i < previewCount; ++i) {
      const auto& rect = rects[i];
      if (i > 0) {
        out += " ";
      }
      out += std::format("{}:{}+{}x{}", rect.x, rect.y, rect.width, rect.height);
    }
    if (rects.size() > previewCount) {
      out += std::format(" +{}", rects.size() - previewCount);
    }
    out += "]";
    return out;
  }

  void traceSurfaceEvent(const Surface& surface, std::string_view event) {
    if (!blurTraceEnabled()) {
      return;
    }

    kLog.debug(
        "blur-trace {} name={} self={} wl={} phase={} running={} logical={}x{} buffer={}x{} scale={:.3f}", event,
        surfaceTraceName(surface), static_cast<const void*>(&surface), static_cast<const void*>(surface.wlSurface()),
        uiPhaseName(currentUiPhase()), surface.isRunning(), surface.width(), surface.height(),
        surface.bufferWidthFor(surface.width()), surface.bufferHeightFor(surface.height()),
        surface.effectiveBufferScale()
    );
  }

  void traceBlurRegionEvent(const Surface& surface, std::string_view event, const std::vector<InputRect>& rects) {
    if (!blurTraceEnabled()) {
      return;
    }

    const InputRect bounds = boundsForRects(rects);
    const int right = bounds.x + bounds.width;
    const int bottom = bounds.y + bounds.height;
    const bool fullSurface = !rects.empty()
        && surface.width() > 0
        && surface.height() > 0
        && bounds.x <= 0
        && bounds.y <= 0
        && right >= static_cast<int>(surface.width())
        && bottom >= static_cast<int>(surface.height());
    kLog.debug(
        "blur-trace {} name={} self={} wl={} phase={} logical={}x{} scale={:.3f} rects={} bounds={}:{}+{}x{} "
        "full_surface={} sample={}",
        event, surfaceTraceName(surface), static_cast<const void*>(&surface),
        static_cast<const void*>(surface.wlSurface()), uiPhaseName(currentUiPhase()), surface.width(), surface.height(),
        surface.effectiveBufferScale(), rects.size(), bounds.x, bounds.y, bounds.width, bounds.height, fullSurface,
        rectListPreview(rects)
    );
  }

  struct SurfaceProfileState {
    SurfaceIdleProfileEntry total;
    std::unordered_map<const Surface*, SurfaceIdleProfileEntry> surfaces;
  };

  SurfaceProfileState& surfaceProfileState() {
    static SurfaceProfileState state;
    return state;
  }

  enum class SurfaceProfileEvent : std::uint8_t {
    RequestUpdate,
    RequestUpdateOnly,
    RequestLayout,
    RequestRedraw,
    QueueFrameWork,
    ProcessFrameWork,
    QueueRender,
    ProcessQueuedRender,
    PrepareCallback,
    FrameTick,
    AnimationTick,
    UpdateCallback,
    Render,
  };

  void addSurfaceProfileEvent(SurfaceIdleProfileEntry& entry, SurfaceProfileEvent event, double ms) {
    switch (event) {
    case SurfaceProfileEvent::RequestUpdate:
      ++entry.requestUpdate;
      break;
    case SurfaceProfileEvent::RequestUpdateOnly:
      ++entry.requestUpdateOnly;
      break;
    case SurfaceProfileEvent::RequestLayout:
      ++entry.requestLayout;
      break;
    case SurfaceProfileEvent::RequestRedraw:
      ++entry.requestRedraw;
      break;
    case SurfaceProfileEvent::QueueFrameWork:
      ++entry.queuedFrameWork;
      break;
    case SurfaceProfileEvent::ProcessFrameWork:
      ++entry.processedFrameWork;
      break;
    case SurfaceProfileEvent::QueueRender:
      ++entry.queuedRenders;
      break;
    case SurfaceProfileEvent::ProcessQueuedRender:
      ++entry.processedQueuedRenders;
      break;
    case SurfaceProfileEvent::PrepareCallback:
      ++entry.prepareCallbacks;
      entry.prepareMs += ms;
      break;
    case SurfaceProfileEvent::FrameTick:
      ++entry.frameTicks;
      entry.frameTickMs += ms;
      break;
    case SurfaceProfileEvent::AnimationTick:
      ++entry.animationTicks;
      entry.animationMs += ms;
      break;
    case SurfaceProfileEvent::UpdateCallback:
      ++entry.updateCallbacks;
      entry.updateMs += ms;
      break;
    case SurfaceProfileEvent::Render:
      ++entry.renders;
      entry.renderMs += ms;
      break;
    }
  }

  void recordSurfaceProfileEvent(const Surface& surface, SurfaceProfileEvent event, double ms = 0.0) {
    if (!idleProfileEnabled()) {
      return;
    }

    auto& state = surfaceProfileState();
    addSurfaceProfileEvent(state.total, event, ms);

    auto& entry = state.surfaces[&surface];
    if (entry.label.empty()) {
      entry.label = std::format(
          "{}@{:x} {}x{}", typeid(surface).name(), reinterpret_cast<std::uintptr_t>(&surface), surface.width(),
          surface.height()
      );
    }
    addSurfaceProfileEvent(entry, event, ms);
  }

  template <typename Fn> float elapsedMs(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  template <typename... Args> void logSlowSurfaceOperation(float ms, std::format_string<Args...> fmt, Args&&... args) {
    if (ms >= kSlowSurfaceOperationWarnMs) {
      kLog.warn(fmt, std::forward<Args>(args)...);
    } else if (ms >= kSlowSurfaceOperationDebugMs) {
      kLog.debug(fmt, std::forward<Args>(args)...);
    }
  }

  std::string outputLabelForSurface(const WaylandConnection& connection, wl_surface* surface) {
    wl_output* wlOutput = connection.outputForSurface(surface);
    const WaylandOutput* output = connection.findOutputByWl(wlOutput);
    if (output == nullptr) {
      return "unknown";
    }
    if (!output->connectorName.empty()) {
      return output->connectorName;
    }
    return std::format("#{}", output->name);
  }

  class ScopedBoolFlag {
  public:
    explicit ScopedBoolFlag(bool& flag) noexcept : m_flag(flag) { m_flag = true; }
    ~ScopedBoolFlag() { m_flag = false; }

    ScopedBoolFlag(const ScopedBoolFlag&) = delete;
    ScopedBoolFlag& operator=(const ScopedBoolFlag&) = delete;

  private:
    bool& m_flag;
  };

} // namespace

Surface::Surface(WaylandConnection& connection) : m_connection(connection) {}

Surface::~Surface() {
  cancelQueuedFrameWork();
  cancelQueuedRender();
  m_invalidationToken.reset();
  setSceneRoot(nullptr);
  destroySurface();
}

bool Surface::isRunning() const noexcept { return m_running; }

void Surface::setDebugName(std::string name) { m_debugName = std::move(name); }

float Surface::effectiveBufferScale() const noexcept {
  if (m_fractionalScale != nullptr && m_viewport != nullptr) {
    if (m_fractionalScaleNumerator > 0) {
      return std::max(1.0f, static_cast<float>(m_fractionalScaleNumerator) / 120.0f);
    }
    return static_cast<float>(std::max(1, m_bufferScale));
  }
  return static_cast<float>(std::max(1, m_bufferScale));
}

std::uint32_t Surface::bufferWidthFor(std::uint32_t logicalWidth) const noexcept {
  return scaledExtent(logicalWidth, effectiveBufferScale());
}

std::uint32_t Surface::bufferHeightFor(std::uint32_t logicalHeight) const noexcept {
  return scaledExtent(logicalHeight, effectiveBufferScale());
}

void Surface::handleFrameDone(void* data, wl_callback* callback, std::uint32_t callbackData) {
  auto* self = static_cast<Surface*>(data);
  (void)callbackData;

  if (callback != nullptr) {
    wl_callback_destroy(callback);
  }

  self->m_frameCallback = nullptr;

  float deltaMs = 0.0f;
  const auto now = std::chrono::steady_clock::now();
  if (self->m_lastFrameAt.has_value()) {
    deltaMs = std::chrono::duration<float, std::milli>(now - *self->m_lastFrameAt).count();
  }
  self->m_lastFrameAt = now;

  const bool activeAnimations = self->m_animationManager != nullptr && self->m_animationManager->hasActive();
  const bool runFrameTick = self->m_frameCallbackShouldTick || self->m_frameTickPending || activeAnimations;
  self->m_frameCallbackShouldTick = false;

  const bool invalidated =
      self->m_sceneRoot != nullptr && (self->m_sceneRoot->paintDirty() || self->m_sceneRoot->layoutDirty());
  const bool hasPendingWork =
      runFrameTick || self->m_updateRequested || self->m_layoutRequested || self->m_redrawRequested || invalidated;
  if (hasPendingWork) {
    self->queueFrameWork(runFrameTick, deltaMs);
  }
}

void Surface::onSurfaceOutputEnter(wl_surface* surface, wl_output* output) {
  if (surface != m_surface || output == nullptr) {
    return;
  }

  m_connection.notifySurfaceOutputEnter(surface, output);

  const WaylandOutput* outputInfo = m_connection.findOutputByWl(output);
  if (outputInfo == nullptr) {
    return;
  }

  const std::int32_t nextScale = std::max(1, outputInfo->scale);
  if (nextScale == m_bufferScale) {
    return;
  }

  m_bufferScale = nextScale;
  if ((m_fractionalScale == nullptr || m_viewport == nullptr || m_fractionalScaleNumerator == 0) && m_configured) {
    onScaleChanged();
  }
}

void Surface::onSurfaceOutputLeave(wl_surface* surface, wl_output* output) {
  if (surface != m_surface || output == nullptr) {
    return;
  }
  m_connection.notifySurfaceOutputLeave(surface, output);
}

bool Surface::createWlSurface() {
  m_surface = wl_compositor_create_surface(m_connection.compositor());
  if (m_surface == nullptr) {
    return false;
  }
  traceSurfaceEvent(*this, "create-wl-surface");
  wl_surface_add_listener(m_surface, &kSurfaceListener, this);

  initializeSurfaceScaleProtocol();

  if (m_renderContext != nullptr) {
    m_renderTarget.create(m_surface, *m_renderContext);
  }
  return true;
}

void Surface::onConfigure(std::uint32_t width, std::uint32_t height) {
  m_width = width;
  m_height = height;
  m_configured = true;
  traceSurfaceEvent(*this, "configure");

  const float resizeMs = elapsedMs([this] {
    applySurfaceScaleState();
    resizeRenderTarget();
    if (m_renderContext != nullptr) {
      m_renderContext->syncContentScale(m_renderTarget);
    }
  });
  logSlowSurfaceOperation(
      resizeMs, "surface configure resize took {:.1f}ms ({}, {}x{} logical)", resizeMs, static_cast<const void*>(this),
      m_width, m_height
  );

  if (m_configureCallback) {
    const float callbackMs = elapsedMs([this] { m_configureCallback(m_width, m_height); });
    logSlowSurfaceOperation(
        callbackMs, "surface configure callback took {:.1f}ms ({}, {}x{} logical)", callbackMs,
        static_cast<const void*>(this), m_width, m_height
    );
  }
  m_redrawRequested = true;
  queueFrameWork();
}

void Surface::setConfigureCallback(ConfigureCallback callback) { m_configureCallback = std::move(callback); }

void Surface::setPrepareFrameCallback(PrepareFrameCallback callback) { m_prepareFrameCallback = std::move(callback); }

void Surface::setUpdateCallback(UpdateCallback callback) { m_updateCallback = std::move(callback); }

void Surface::setFrameTickCallback(FrameTickCallback callback) { m_frameTickCallback = std::move(callback); }

void Surface::setSceneRoot(Node* root) {
  if (m_sceneRoot == root) {
    return;
  }
  m_sceneRoot = root;
  if (m_sceneRoot == nullptr) {
    return;
  }

  const std::weak_ptr<InvalidationToken> token = m_invalidationToken;
  m_sceneRoot->setInvalidationCallback([this, token](NodeInvalidation invalidation) {
    if (token.expired()) {
      return;
    }
    const UiPhase phase = currentUiPhase();
    if (m_inPrepareFrame || phase != UiPhase::Idle) {
      return;
    }

    if (invalidation == NodeInvalidation::Layout) {
      requestLayout();
    } else {
      requestRedraw();
    }
  });
}

void Surface::setRenderContext(RenderContext* ctx) {
  if (m_renderContext == ctx && (ctx == nullptr || m_surface == nullptr || m_renderTarget.surfaceTarget() != nullptr)) {
    return;
  }

  m_renderContext = ctx;
  m_renderTarget.destroy();

  if (m_surface != nullptr && m_renderContext != nullptr) {
    m_renderTarget.create(m_surface, *m_renderContext);
    resizeRenderTarget();
  }
}

void Surface::initializeSurfaceScaleProtocol() {
  if (m_surface == nullptr || !m_connection.hasFractionalScale()) {
    return;
  }

  auto* viewporter = m_connection.viewporter();
  auto* fractionalScaleManager = m_connection.fractionalScaleManager();
  if (viewporter == nullptr || fractionalScaleManager == nullptr) {
    return;
  }

  m_viewport = wp_viewporter_get_viewport(viewporter, m_surface);
  m_fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(fractionalScaleManager, m_surface);
  if (m_fractionalScale != nullptr) {
    wp_fractional_scale_v1_add_listener(m_fractionalScale, &kFractionalScaleListener, this);
  }

  // Fractional-scale-v1 requires wl_surface buffer scale to remain 1; viewport
  // destination keeps the surface-local size equal to the role configure size.
  wl_surface_set_buffer_scale(m_surface, 1);
}

void Surface::applySurfaceScaleState() {
  if (m_surface == nullptr) {
    return;
  }

  if (m_fractionalScale != nullptr && m_viewport != nullptr) {
    wl_surface_set_buffer_scale(m_surface, 1);
    if (m_width > 0 && m_height > 0) {
      constexpr auto kMaxViewportExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
      if (m_width > kMaxViewportExtent || m_height > kMaxViewportExtent) {
        kLog.warn("skipping viewport destination with out-of-range size {}x{}", m_width, m_height);
      } else {
        wp_viewport_set_destination(
            m_viewport, static_cast<std::int32_t>(m_width), static_cast<std::int32_t>(m_height)
        );
      }
    }
    return;
  }

  wl_surface_set_buffer_scale(m_surface, std::max(1, m_bufferScale));
}

void Surface::resizeRenderTarget() {
  if (m_renderContext == nullptr || m_width == 0 || m_height == 0) {
    return;
  }

  if (m_surface != nullptr && m_renderTarget.surfaceTarget() == nullptr) {
    m_renderTarget.create(m_surface, *m_renderContext);
  }

  const auto bufferWidth = bufferWidthFor(m_width);
  const auto bufferHeight = bufferHeightFor(m_height);

  m_renderTarget.setLogicalSize(m_width, m_height);
  if (m_renderTarget.bufferWidth() == bufferWidth
      && m_renderTarget.bufferHeight() == bufferHeight
      && m_renderTarget.isReady()) {
    return;
  }
  m_renderTarget.resize(bufferWidth, bufferHeight);
}

void Surface::onPreferredFractionalScale(std::uint32_t numerator) {
  if (numerator == 0 || numerator == m_fractionalScaleNumerator) {
    return;
  }

  m_fractionalScaleNumerator = numerator;
  const float preferredScale = std::max(1.0f, static_cast<float>(numerator) / 120.0f);
  kLog.debug(
      "fractional scale preferred output={} surface={} scale={:.3f} raw={}/120 logical={}x{} buffer={}x{}",
      outputLabelForSurface(m_connection, m_surface), static_cast<const void*>(m_surface), preferredScale, numerator,
      m_width, m_height, bufferWidthFor(m_width), bufferHeightFor(m_height)
  );
  if (!m_configured) {
    return;
  }

  onScaleChanged();
}

void Surface::onScaleChanged() {
  applySurfaceScaleState();
  resizeRenderTarget();
  if (m_renderContext != nullptr) {
    m_renderContext->syncContentScale(m_renderTarget);
  }
  requestLayout();
  requestRedraw();
}

void Surface::setInputRegion(const std::vector<InputRect>& rects) {
  if (m_surface == nullptr) {
    return;
  }

  wl_region* region = wl_compositor_create_region(m_connection.compositor());
  if (region == nullptr) {
    return;
  }

  for (const auto& r : rects) {
    wl_region_add(region, r.x, r.y, r.width, r.height);
  }

  wl_surface_set_input_region(m_surface, region);
  wl_region_destroy(region);
}

bool Surface::prepareBlurEffect() {
  if (m_surface == nullptr) {
    traceSurfaceEvent(*this, "blur-effect-skip-no-surface");
    return false;
  }
  if (!m_connection.hasBackgroundEffectBlur()) {
    traceSurfaceEvent(*this, "blur-effect-skip-no-protocol");
    return false;
  }
  if (m_backgroundEffect != nullptr) {
    return true;
  }

  auto* manager = m_connection.backgroundEffectManager();
  if (manager == nullptr) {
    traceSurfaceEvent(*this, "blur-effect-skip-no-manager");
    return false;
  }
  m_backgroundEffect = ext_background_effect_manager_v1_get_background_effect(manager, m_surface);
  if (m_backgroundEffect == nullptr) {
    traceSurfaceEvent(*this, "blur-effect-skip-create-failed");
    return false;
  }
  traceSurfaceEvent(*this, "blur-effect-create");
  return true;
}

void Surface::setBlurRegion(const std::vector<InputRect>& rects) {
  if (!prepareBlurEffect()) {
    return;
  }

  wl_region* region = nullptr;
  if (!rects.empty()) {
    region = wl_compositor_create_region(m_connection.compositor());
    if (region == nullptr) {
      traceSurfaceEvent(*this, "blur-set-skip-region-failed");
      return;
    }
    for (const auto& r : rects) {
      wl_region_add(region, r.x, r.y, r.width, r.height);
    }
  }
  traceBlurRegionEvent(*this, rects.empty() ? "blur-set-empty" : "blur-set", rects);
  ext_background_effect_surface_v1_set_blur_region(m_backgroundEffect, region);
  if (region != nullptr) {
    wl_region_destroy(region);
  }
}

std::vector<InputRect> Surface::tessellateRoundedRect(
    int x, int y, int w, int h, float tlRadius, float trRadius, float brRadius, float blRadius, int stripPx
) {
  std::vector<InputRect> out;
  if (w <= 0 || h <= 0) {
    return out;
  }

  stripPx = std::max(stripPx, 1);

  const float halfW = static_cast<float>(w) * 0.5f;
  const float halfH = static_cast<float>(h) * 0.5f;
  const float maxRadius = std::min(halfW, halfH);
  const auto clampR = [maxRadius](float r) {
    if (r < 0.0f)
      return 0.0f;
    return std::min(r, maxRadius);
  };
  const float tl = clampR(tlRadius);
  const float tr = clampR(trRadius);
  const float br = clampR(brRadius);
  const float bl = clampR(blRadius);

  const int topBand = static_cast<int>(std::ceil(std::max(tl, tr)));
  const int bottomBand = static_cast<int>(std::ceil(std::max(bl, br)));
  const int middleY = y + topBand;
  const int middleH = h - topBand - bottomBand;

  const auto inset = [](float r, float distFromCornerEdge) -> float {
    if (r <= 0.0f)
      return 0.0f;
    const float dy = r - distFromCornerEdge;
    if (dy <= 0.0f)
      return 0.0f;
    const float ry = std::sqrt(std::max(0.0f, r * r - dy * dy));
    return r - ry;
  };

  out.reserve(static_cast<std::size_t>((topBand + bottomBand) / stripPx + 2));

  // Top corner band: strips run from y..y+topBand, distFromTop grows downward.
  for (int row = 0; row < topBand; row += stripPx) {
    const int rowH = std::min(stripPx, topBand - row);
    // Use the strip's bottom edge for the inset sample so the polygon stays inside the curve.
    const auto sample = static_cast<float>(row + rowH);
    const float leftInset = inset(tl, sample);
    const float rightInset = inset(tr, sample);
    const int rx = x + static_cast<int>(std::ceil(leftInset));
    const int rw = w - static_cast<int>(std::ceil(leftInset)) - static_cast<int>(std::ceil(rightInset));
    if (rw > 0) {
      out.push_back({rx, y + row, rw, rowH});
    }
  }

  // Middle full-width band.
  if (middleH > 0) {
    out.push_back({x, middleY, w, middleH});
  }

  // Bottom corner band: distFromBottom shrinks as we move down.
  for (int row = 0; row < bottomBand; row += stripPx) {
    const int rowFromTop = row;
    const int rowH = std::min(stripPx, bottomBand - rowFromTop);
    // Sample at the strip's top edge (distance from bottom edge of the rect).
    const auto sample = static_cast<float>(bottomBand - rowFromTop);
    const float leftInset = inset(bl, sample);
    const float rightInset = inset(br, sample);
    const int rx = x + static_cast<int>(std::ceil(leftInset));
    const int rw = w - static_cast<int>(std::ceil(leftInset)) - static_cast<int>(std::ceil(rightInset));
    if (rw > 0) {
      out.push_back({rx, middleY + middleH + rowFromTop, rw, rowH});
    }
  }

  return out;
}

std::vector<InputRect> Surface::tessellateShape(
    int x, int y, int w, int h, const CornerShapes& corners, const RectInsets& logicalInset, const Radii& radii,
    int stripPx
) {
  std::vector<InputRect> out;
  if (w <= 0 || h <= 0) {
    return out;
  }
  stripPx = std::max(stripPx, 1);

  // All-convex with no inset reduces to a plain rounded rect; take the cheap path
  // (corner bands + one middle rect) instead of walking every row. This is the
  // common case on per-frame blur updates.
  const bool anyConcave = corners.tl == CornerShape::Concave
      || corners.tr == CornerShape::Concave
      || corners.br == CornerShape::Concave
      || corners.bl == CornerShape::Concave;
  if (!anyConcave
      && logicalInset.left <= 0.0f
      && logicalInset.top <= 0.0f
      && logicalInset.right <= 0.0f
      && logicalInset.bottom <= 0.0f) {
    return tessellateRoundedRect(x, y, w, h, radii.tl, radii.tr, radii.br, radii.bl, stripPx);
  }

  // (x, y, w, h) is the body rect. Expand outward by logicalInset to obtain the
  // visual rect that hosts concave-corner bulges; the body sits inside it offset
  // by logicalInset.left / .top.
  const float insetL = std::max(0.0f, logicalInset.left);
  const float insetT = std::max(0.0f, logicalInset.top);
  const float insetR = std::max(0.0f, logicalInset.right);
  const float insetB = std::max(0.0f, logicalInset.bottom);
  const int visualX = x - static_cast<int>(std::lround(insetL));
  const int visualY = y - static_cast<int>(std::lround(insetT));
  const int visualW = w + static_cast<int>(std::lround(insetL)) + static_cast<int>(std::lround(insetR));
  const int visualH = h + static_cast<int>(std::lround(insetT)) + static_cast<int>(std::lround(insetB));

  const auto W = static_cast<float>(visualW);
  const auto H = static_cast<float>(visualH);
  const float bodyMinX = std::clamp(insetL, 0.0f, W);
  const float bodyMaxX = std::clamp(W - insetR, bodyMinX, W);
  const float bodyMinY = std::clamp(insetT, 0.0f, H);
  const float bodyMaxY = std::clamp(H - insetB, bodyMinY, H);
  const float bodyW = bodyMaxX - bodyMinX;
  const float bodyH = bodyMaxY - bodyMinY;
  const float maxR = std::max(0.0f, std::min(bodyW, bodyH) * 0.5f);
  const auto clampR = [maxR](float r) { return std::clamp(r, 0.0f, maxR); };
  const float rTl = clampR(radii.tl);
  const float rTr = clampR(radii.tr);
  const float rBr = clampR(radii.br);
  const float rBl = clampR(radii.bl);

  const auto extent = [](float r, float dy) -> float {
    if (r <= 0.0f) {
      return 0.0f;
    }
    const float d2 = r * r - dy * dy;
    return d2 > 0.0f ? std::sqrt(d2) : 0.0f;
  };

  // Per-row coverage of the shape as a set of [left, right] segments (clipped to
  // the visual rect), mirroring the rect shader. Rows in the top/bottom inset
  // strips (yf outside the body's vertical span) are covered only where a concave
  // corner bulge crosses the row; two concave corners on the same edge (e.g. a bar
  // with both inner corners concave) produce two disjoint segments with an empty
  // middle — so this returns a list, not a single span.
  const auto rowSegments = [&](float yf) -> std::vector<std::pair<float, float>> {
    std::vector<std::pair<float, float>> segs;
    const auto pushSeg = [&](float l, float r) {
      l = std::clamp(l, 0.0f, W);
      r = std::clamp(r, l, W);
      if (r > l) {
        segs.emplace_back(l, r);
      }
    };

    if (yf < bodyMinY) {
      const float dy = bodyMinY - yf;
      if (corners.tl == CornerShape::Concave && rTl > 0.0f && dy <= rTl) {
        const float chord = std::sqrt(std::max(0.0f, dy * (2.0f * rTl - dy)));
        pushSeg(bodyMinX, bodyMinX + rTl - chord);
      }
      if (corners.tr == CornerShape::Concave && rTr > 0.0f && dy <= rTr) {
        const float chord = std::sqrt(std::max(0.0f, dy * (2.0f * rTr - dy)));
        pushSeg(bodyMaxX - rTr + chord, bodyMaxX);
      }
      return segs;
    }
    if (yf > bodyMaxY) {
      const float dy = yf - bodyMaxY;
      if (corners.bl == CornerShape::Concave && rBl > 0.0f && dy <= rBl) {
        const float chord = std::sqrt(std::max(0.0f, dy * (2.0f * rBl - dy)));
        pushSeg(bodyMinX, bodyMinX + rBl - chord);
      }
      if (corners.br == CornerShape::Concave && rBr > 0.0f && dy <= rBr) {
        const float chord = std::sqrt(std::max(0.0f, dy * (2.0f * rBr - dy)));
        pushSeg(bodyMaxX - rBr + chord, bodyMaxX);
      }
      return segs;
    }

    // Body rows: the body fills the middle, so a single span (possibly widened by
    // concave corners reaching outward, or narrowed by convex rounding) is correct.
    float left = bodyMinX;
    float right = bodyMaxX;
    if (rTl > 0.0f && yf < bodyMinY + rTl) {
      const float sy = std::clamp(yf, bodyMinY, bodyMinY + rTl);
      const float e = extent(rTl, sy - (bodyMinY + rTl));
      if (corners.tl == CornerShape::Concave) {
        left = std::min(left, bodyMinX - rTl + e);
      } else {
        left = std::max(left, bodyMinX + rTl - e);
      }
    }
    if (rBl > 0.0f && yf > bodyMaxY - rBl) {
      const float sy = std::clamp(yf, bodyMaxY - rBl, bodyMaxY);
      const float e = extent(rBl, sy - (bodyMaxY - rBl));
      if (corners.bl == CornerShape::Concave) {
        left = std::min(left, bodyMinX - rBl + e);
      } else {
        left = std::max(left, bodyMinX + rBl - e);
      }
    }
    if (rTr > 0.0f && yf < bodyMinY + rTr) {
      const float sy = std::clamp(yf, bodyMinY, bodyMinY + rTr);
      const float e = extent(rTr, sy - (bodyMinY + rTr));
      if (corners.tr == CornerShape::Concave) {
        right = std::max(right, bodyMaxX + rTr - e);
      } else {
        right = std::min(right, bodyMaxX - rTr + e);
      }
    }
    if (rBr > 0.0f && yf > bodyMaxY - rBr) {
      const float sy = std::clamp(yf, bodyMaxY - rBr, bodyMaxY);
      const float e = extent(rBr, sy - (bodyMaxY - rBr));
      if (corners.br == CornerShape::Concave) {
        right = std::max(right, bodyMaxX + rBr - e);
      } else {
        right = std::min(right, bodyMaxX - rBr + e);
      }
    }
    pushSeg(left, right);
    return segs;
  };

  out.reserve(static_cast<std::size_t>(visualH / stripPx + 2));

  for (int row = 0; row < visualH; row += stripPx) {
    const int rowH = std::min(stripPx, visualH - row);
    // Conservative inside coverage = intersection of the segment sets at the
    // strip's top and bottom edges, so the polygon stays inside the actual shape
    // regardless of corner kind. Each set has at most two segments.
    const auto segsTop = rowSegments(static_cast<float>(row));
    const auto segsBot = rowSegments(static_cast<float>(row + rowH));
    for (const auto& a : segsTop) {
      for (const auto& b : segsBot) {
        const float left = std::max(a.first, b.first);
        const float right = std::min(a.second, b.second);
        const int rx = visualX + static_cast<int>(std::ceil(left));
        const int rRight = visualX + static_cast<int>(std::floor(right));
        const int rw = rRight - rx;
        if (rw <= 0) {
          continue;
        }
        const int ry = visualY + row;
        // Vertically merge with a matching strip from the previous row. With two
        // spike columns the match may not be the very last rect, so scan back over
        // the few rects that end at this row.
        bool merged = false;
        for (auto& rect : std::views::reverse(out)) {
          if (rect.y + rect.height < ry) {
            break;
          }
          if (rect.x == rx && rect.width == rw && rect.y + rect.height == ry) {
            rect.height += rowH;
            merged = true;
            break;
          }
        }
        if (!merged) {
          out.push_back({rx, ry, rw, rowH});
        }
      }
    }
  }

  return out;
}

std::vector<InputRect> Surface::tessellateRotatedRoundedRect(
    float centerX, float centerY, float width, float height, float radius, float rotationRad, int stripPx
) {
  constexpr float kRotationEpsilon = 0.001f;
  if (std::abs(rotationRad) < kRotationEpsilon) {
    const int ix = static_cast<int>(std::lround(centerX - width * 0.5f));
    const int iy = static_cast<int>(std::lround(centerY - height * 0.5f));
    const int iw = static_cast<int>(std::lround(width));
    const int ih = static_cast<int>(std::lround(height));
    return tessellateRoundedRect(ix, iy, iw, ih, radius, stripPx);
  }

  const float cosA = std::cos(rotationRad);
  const float sinA = std::sin(rotationRad);
  const float halfW = width * 0.5f;
  const float halfH = height * 0.5f;
  const float r = std::clamp(radius, 0.0f, std::min(halfW, halfH));

  const float aabbH = std::abs(width * sinA) + std::abs(height * cosA);
  const int aabbIH = static_cast<int>(std::ceil(aabbH));
  const float aabbTop = centerY - aabbH * 0.5f;

  stripPx = std::max(stripPx, 1);

  // Corner inset in local space: how far the rounded corner narrows the rect
  // at a given distance from the edge.
  const auto cornerInset = [](float cr, float distFromEdge) -> float {
    if (cr <= 0.0f || distFromEdge >= cr) {
      return 0.0f;
    }
    const float dy = cr - distFromEdge;
    return cr - std::sqrt(std::max(0.0f, cr * cr - dy * dy));
  };

  // For a given local-space Y (origin at rect center), compute the left and
  // right X extents of the rounded rect.
  const auto localExtents = [&](float ly) -> std::pair<float, float> {
    if (ly < -halfH || ly > halfH) {
      return {0.0f, 0.0f};
    }
    float left = -halfW;
    float right = halfW;
    const float distFromTop = ly + halfH;
    const float distFromBottom = halfH - ly;
    if (distFromTop < r) {
      const float inset = cornerInset(r, distFromTop);
      left += inset;
      right -= inset;
    }
    if (distFromBottom < r) {
      const float inset = cornerInset(r, distFromBottom);
      left += inset;
      right -= inset;
    }
    if (left >= right) {
      return {0.0f, 0.0f};
    }
    return {left, right};
  };

  std::vector<InputRect> out;
  out.reserve(static_cast<std::size_t>(aabbIH / stripPx + 2));

  for (int row = 0; row < aabbIH; row += stripPx) {
    const int rowH = std::min(stripPx, aabbIH - row);
    float globalMinX = std::numeric_limits<float>::max();
    float globalMaxX = std::numeric_limits<float>::lowest();
    bool anyHit = false;

    // Sample the strip at its top and bottom edges for a conservative bound.
    for (int edge = 0; edge <= 1; ++edge) {
      const float surfaceY = aabbTop + static_cast<float>(row + edge * rowH);
      const float dy = surfaceY - centerY;

      // Scan across the local-Y axis to find what range of local rows this
      // surface-Y touches. A horizontal surface-space line at surfaceY, when
      // inverse-rotated, becomes a line in local space. We need the min/max
      // surface-X of the rounded rect along that line.
      //
      // A point (lx, ly) in local space maps to surface-X = cx + lx*cos - ly*sin.
      // The surface scanline surfaceY corresponds to all (lx, ly) satisfying
      // cy + lx*sin + ly*cos = surfaceY, i.e. lx*sin + ly*cos = dy.
      // For each local ly: lx_on_line = (dy - ly*cos) / sin  (when sin != 0).
      // But lx must also be within the rounded rect's horizontal extent at ly.
      // The leftmost and rightmost surface-X values across all valid (lx,ly) give
      // the strip bounds.
      //
      // Sample the local-Y range that can produce this surface-Y.
      constexpr int kSamples = 32;
      for (int s = 0; s <= kSamples; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(kSamples);
        const float ly = -halfH + (2.0f * halfH) * t;
        const auto [localLeft, localRight] = localExtents(ly);
        if (localLeft >= localRight) {
          continue;
        }
        // lx_on_line = (dy - ly*cos) / sin, but we need the intersection of
        // the horizontal surface-Y line with the row ly in local space projected
        // onto the surface X axis. Surface X = cx + lx*cos - ly*sin.
        // Constraint: lx*sin + ly*cos = dy  =>  lx = (dy - ly*cos) / sin.
        // But when sin ≈ 0, surface-Y ≈ cy + ly*cos, so only ly ≈ dy/cos is
        // valid, and surface-X = cx + lx*cos for any lx in the local extent.
        float sxLeft, sxRight;
        if (std::abs(sinA) > 1e-6f) {
          const float lxOnLine = (dy - ly * cosA) / sinA;
          if (lxOnLine < localLeft - 0.5f || lxOnLine > localRight + 0.5f) {
            continue;
          }
          const float clampedLx = std::clamp(lxOnLine, localLeft, localRight);
          const float sx = centerX + clampedLx * cosA - ly * sinA;
          sxLeft = sx;
          sxRight = sx;
        } else {
          if (std::abs(ly * cosA - dy) > 1.0f) {
            continue;
          }
          sxLeft = centerX + localLeft * cosA - ly * sinA;
          sxRight = centerX + localRight * cosA - ly * sinA;
          if (sxLeft > sxRight) {
            std::swap(sxLeft, sxRight);
          }
        }
        globalMinX = std::min(globalMinX, sxLeft);
        globalMaxX = std::max(globalMaxX, sxRight);
        anyHit = true;
      }
    }

    if (!anyHit || globalMinX >= globalMaxX) {
      continue;
    }

    const int rx = static_cast<int>(std::floor(globalMinX));
    const int rRight = static_cast<int>(std::ceil(globalMaxX));
    const int rw = rRight - rx;
    const int ry = static_cast<int>(std::lround(aabbTop)) + row;
    if (rw <= 0) {
      continue;
    }

    if (!out.empty()) {
      auto& prev = out.back();
      if (prev.x == rx && prev.width == rw && prev.y + prev.height == ry) {
        prev.height += rowH;
        continue;
      }
    }
    out.push_back({rx, ry, rw, rowH});
  }

  return out;
}

void Surface::clearBlurRegion() {
  if (m_backgroundEffect == nullptr) {
    traceSurfaceEvent(*this, "blur-clear-no-effect");
    return;
  }
  traceSurfaceEvent(*this, "blur-clear-destroy");
  ext_background_effect_surface_v1_destroy(m_backgroundEffect);
  m_backgroundEffect = nullptr;
}

void Surface::requestUpdate() {
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::RequestUpdate);
  m_updateRequested = true;
  m_layoutRequested = true;
  kickFrameLoop();
}

void Surface::requestUpdateOnly() {
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::RequestUpdateOnly);
  m_updateRequested = true;
  kickFrameLoop();
}

void Surface::requestLayout() {
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::RequestLayout);
  m_layoutRequested = true;
  kickFrameLoop();
}

void Surface::requestRedraw() {
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::RequestRedraw);
  if (m_frameTickCallback != nullptr) {
    m_nextFrameCallbackShouldTick = true;
  }
  m_redrawRequested = true;
  kickFrameLoop();
}

void Surface::requestFrameTick() {
  if (!m_running || !m_configured) {
    return;
  }

  float deltaMs = 0.0f;
  const auto now = std::chrono::steady_clock::now();
  if (m_lastFrameAt.has_value()) {
    deltaMs = std::chrono::duration<float, std::milli>(now - *m_lastFrameAt).count();
  }

  if (m_frameCallback != nullptr || m_inFrameHandler || m_inPrepareFrame) {
    // Defer to handleFrameDone / the next drain, but keep the tick intent so
    // coalesced spectrum callbacks are not lost while a frame is in flight.
    m_frameTickPending = true;
    m_pendingFrameDeltaMs = deltaMs;
    return;
  }

  m_lastFrameAt = now;
  queueFrameWork(true, deltaMs);
}

void Surface::renderNow() {
  if (m_running && m_configured) {
    cancelQueuedFrameWork();
    cancelQueuedRender();
    preparePendingFrame();
    render();
  }
}

void Surface::render() {
  if (m_surface == nullptr || m_renderContext == nullptr || !m_renderTarget.isReady()) {
    return;
  }

  requestFrame();
  traceSurfaceEvent(*this, "render-begin");
  const float renderMs = elapsedMs([this] { m_renderContext->renderScene(m_renderTarget, m_sceneRoot); });
  traceSurfaceEvent(*this, "render-end");
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::Render, renderMs);
  logSlowSurfaceOperation(
      renderMs, "surface render took {:.1f}ms ({}x{} logical, {}x{} buffer)", renderMs, m_width, m_height,
      m_renderTarget.bufferWidth(), m_renderTarget.bufferHeight()
  );

  if (m_sceneRoot != nullptr) {
    m_sceneRoot->clearDirty();
  }
  m_redrawRequested = false;
}

void Surface::requestFrame() {
  if (m_frameCallback != nullptr) {
    m_frameCallbackShouldTick = m_frameCallbackShouldTick || m_nextFrameCallbackShouldTick;
    m_nextFrameCallbackShouldTick = false;
    return;
  }

  const bool activeAnimations = m_animationManager != nullptr && m_animationManager->hasActive();
  m_frameCallbackShouldTick = m_nextFrameCallbackShouldTick || activeAnimations;
  m_nextFrameCallbackShouldTick = false;

  m_frameCallback = wl_surface_frame(m_surface);
  if (m_frameCallback != nullptr) {
    wl_callback_add_listener(m_frameCallback, &kFrameListener, this);
  }
}

void Surface::destroySurface() {
  cancelQueuedFrameWork();
  cancelQueuedRender();

  if (m_frameCallback != nullptr) {
    wl_callback_destroy(m_frameCallback);
    m_frameCallback = nullptr;
  }

  m_renderTarget.destroy();

  if (m_backgroundEffect != nullptr) {
    ext_background_effect_surface_v1_destroy(m_backgroundEffect);
    m_backgroundEffect = nullptr;
  }

  if (m_fractionalScale != nullptr) {
    wp_fractional_scale_v1_destroy(m_fractionalScale);
    m_fractionalScale = nullptr;
  }

  if (m_viewport != nullptr) {
    wp_viewport_destroy(m_viewport);
    m_viewport = nullptr;
  }

  if (m_surface != nullptr) {
    wl_surface_destroy(m_surface);
    m_surface = nullptr;
  }

  m_running = false;
  m_configured = false;
}

void Surface::preparePendingFrame() {
  if (m_prepareFrameCallback == nullptr || (!m_updateRequested && !m_layoutRequested)) {
    return;
  }
  if (!ensureRenderTargetReady()) {
    return;
  }

  UiPhaseScope preparePhase(UiPhase::PrepareFrame);
  const bool needsUpdate = m_updateRequested;
  const bool needsLayout = m_layoutRequested;
  m_updateRequested = false;
  m_layoutRequested = false;
  ScopedBoolFlag preparing{m_inPrepareFrame};
  const float callbackMs =
      elapsedMs([this, needsUpdate, needsLayout] { m_prepareFrameCallback(needsUpdate, needsLayout); });
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::PrepareCallback, callbackMs);
  logSlowSurfaceOperation(
      callbackMs, "surface prepareFrame callback took {:.1f}ms ({}, {}x{} logical)", callbackMs,
      static_cast<const void*>(this), m_width, m_height
  );
}

void Surface::kickFrameLoop() {
  if (!m_running
      || !m_configured
      || m_frameCallback != nullptr
      || m_inFrameHandler
      || m_inPrepareFrame
      || m_frameWorkQueued) {
    return;
  }

  // Anchor the animation clock at "now" instead of clearing it. On the next
  // handleFrameDone, deltaMs is computed against this anchor, so animations
  // that start while the surface was idle get a real first-tick deltaMs
  // instead of zero (which would waste the first tick at t=0).
  m_lastFrameAt = std::chrono::steady_clock::now();

  if (m_updateRequested || m_layoutRequested) {
    queueFrameWork();
    return;
  }

  queueRenderIfNeeded();
}

void Surface::queueFrameWork(bool runFrameTick, float deltaMs) {
  if (runFrameTick) {
    m_frameTickPending = true;
    m_pendingFrameDeltaMs = deltaMs;
  }
  if (m_frameWorkQueued) {
    return;
  }
  m_frameWorkQueued = true;
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::QueueFrameWork);
  pendingFrameWorkQueue().push_back(this);
}

void Surface::cancelQueuedFrameWork() {
  if (!m_frameWorkQueued) {
    return;
  }
  auto& queue = pendingFrameWorkQueue();
  std::erase(queue, this);
  m_frameWorkQueued = false;
  m_frameTickPending = false;
  m_pendingFrameDeltaMs = 0.0f;
}

void Surface::processQueuedFrameWork() {
  m_frameWorkQueued = false;
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::ProcessFrameWork);
  if (m_surface == nullptr || !m_configured) {
    m_frameTickPending = false;
    m_pendingFrameDeltaMs = 0.0f;
    return;
  }

  const bool runFrameTick = m_frameTickPending;
  const float deltaMs = m_pendingFrameDeltaMs;
  m_frameTickPending = false;
  m_pendingFrameDeltaMs = 0.0f;

  if (runFrameTick) {
    ScopedBoolFlag frameHandler{m_inFrameHandler};

    if (m_animationManager != nullptr) {
      const float tickMs = elapsedMs([this, deltaMs] { m_animationManager->tick(deltaMs); });
      recordSurfaceProfileEvent(*this, SurfaceProfileEvent::AnimationTick, tickMs);
      logSlowSurfaceOperation(
          tickMs, "surface animation tick took {:.1f}ms ({}, {}x{} logical)", tickMs, static_cast<const void*>(this),
          m_width, m_height
      );
    }

    // Frame-tick callbacks make the surface's render target current and do GL
    // work. Skip them until the target is ready; on wlroots compositors the
    // surface can be configured a frame before its EGL surface exists.
    if (m_frameTickCallback && ensureRenderTargetReady()) {
      const float callbackMs = elapsedMs([this, deltaMs] { m_frameTickCallback(deltaMs); });
      recordSurfaceProfileEvent(*this, SurfaceProfileEvent::FrameTick, callbackMs);
      logSlowSurfaceOperation(
          callbackMs, "surface frame tick callback took {:.1f}ms ({}, {}x{} logical)", callbackMs,
          static_cast<const void*>(this), m_width, m_height
      );
    }

    if (m_updateCallback) {
      const float updateMs = elapsedMs([this] { m_updateCallback(); });
      recordSurfaceProfileEvent(*this, SurfaceProfileEvent::UpdateCallback, updateMs);
      logSlowSurfaceOperation(
          updateMs, "surface update callback took {:.1f}ms ({}, {}x{} logical)", updateMs,
          static_cast<const void*>(this), m_width, m_height
      );
    }
  }

  preparePendingFrame();
  queueRenderIfNeeded();
  // Frame loop stops here when idle. Restarted by requestRedraw().
}

void Surface::queueRenderIfNeeded() {
  const bool invalidated = m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty());
  const bool animating = m_animationManager != nullptr && m_animationManager->hasActive();
  if (m_redrawRequested || invalidated) {
    queueRender();
  } else if (animating) {
    continueAnimationFrameLoop();
  }
}

void Surface::continueAnimationFrameLoop() {
  if (m_surface == nullptr || m_frameCallback != nullptr) {
    return;
  }

  // A frame callback becomes active on commit. With no changed pixels, commit
  // only the callback state and retain the current buffer instead of repainting it.
  requestFrame();
  if (m_frameCallback != nullptr) {
    wl_surface_commit(m_surface);
  }
}

void Surface::queueRender() {
  if (m_renderQueued) {
    return;
  }
  m_renderQueued = true;
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::QueueRender);
  pendingRenderQueue().push_back(this);
}

void Surface::cancelQueuedRender() {
  if (!m_renderQueued) {
    return;
  }
  auto& queue = pendingRenderQueue();
  std::erase(queue, this);
  m_renderQueued = false;
}

void Surface::renderQueuedFrame() {
  m_renderQueued = false;
  recordSurfaceProfileEvent(*this, SurfaceProfileEvent::ProcessQueuedRender);
  if (m_surface == nullptr || !m_configured) {
    return;
  }

  preparePendingFrame();
  const bool invalidated = m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty());
  if (m_redrawRequested || invalidated) {
    render();
  }
}

bool Surface::ensureRenderTargetReady() {
  if (m_renderContext == nullptr) {
    return true;
  }
  if (m_surface == nullptr || m_width == 0 || m_height == 0) {
    return false;
  }

  resizeRenderTarget();
  return m_renderTarget.isReady();
}

bool Surface::hasPendingRenders() { return !pendingRenderQueue().empty(); }

bool Surface::hasPendingFrameWork() { return !pendingFrameWorkQueue().empty(); }

SurfaceIdleProfileSnapshot Surface::takeIdleProfileSnapshot(bool reset) {
  SurfaceIdleProfileSnapshot snapshot;
  if (!idleProfileEnabled()) {
    return snapshot;
  }

  auto& state = surfaceProfileState();
  snapshot.total = state.total;
  snapshot.surfaces.reserve(state.surfaces.size());
  for (const auto& [surface, entry] : state.surfaces) {
    (void)surface;
    snapshot.surfaces.push_back(entry);
  }

  if (reset) {
    state = SurfaceProfileState{};
  }
  return snapshot;
}

void Surface::drainPendingFrameWork() {
  auto& queue = pendingFrameWorkQueue();
  if (queue.empty()) {
    return;
  }

  auto pending = std::move(queue);
  queue.clear();
  for (auto* surface : pending) {
    if (surface == nullptr || !surface->m_frameWorkQueued) {
      continue;
    }
    surface->processQueuedFrameWork();
  }
}

void Surface::drainPendingRenders() {
  auto& queue = pendingRenderQueue();
  if (queue.empty()) {
    return;
  }

  auto pending = std::move(queue);
  queue.clear();
  for (auto* surface : pending) {
    if (surface == nullptr || !surface->m_renderQueued) {
      continue;
    }
    surface->renderQueuedFrame();
  }
}
