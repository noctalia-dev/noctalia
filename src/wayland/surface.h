#pragma once

#include "render/core/render_styles.h"
#include "render/render_target.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct wl_callback;
struct wl_output;
struct wl_surface;
struct ext_background_effect_surface_v1;
struct wp_fractional_scale_v1;
struct wp_viewport;

class AnimationManager;
class Node;
class RenderContext;
class WaylandConnection;

struct InputRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct SurfaceIdleProfileEntry {
  std::string label;
  std::uint64_t requestUpdate = 0;
  std::uint64_t requestUpdateOnly = 0;
  std::uint64_t requestLayout = 0;
  std::uint64_t requestRedraw = 0;
  std::uint64_t queuedFrameWork = 0;
  std::uint64_t processedFrameWork = 0;
  std::uint64_t queuedRenders = 0;
  std::uint64_t processedQueuedRenders = 0;
  std::uint64_t prepareCallbacks = 0;
  std::uint64_t frameTicks = 0;
  std::uint64_t animationTicks = 0;
  std::uint64_t updateCallbacks = 0;
  std::uint64_t renders = 0;
  double prepareMs = 0.0;
  double frameTickMs = 0.0;
  double animationMs = 0.0;
  double updateMs = 0.0;
  double renderMs = 0.0;
};

struct SurfaceIdleProfileSnapshot {
  SurfaceIdleProfileEntry total;
  std::vector<SurfaceIdleProfileEntry> surfaces;
};

class Surface {
public:
  using ConfigureCallback = std::function<void(std::uint32_t width, std::uint32_t height)>;
  using PrepareFrameCallback = std::function<void(bool needsUpdate, bool needsLayout)>;
  using UpdateCallback = std::function<void()>;
  using FrameTickCallback = std::function<void(float deltaMs)>;

  explicit Surface(WaylandConnection& connection);
  virtual ~Surface();

  Surface(const Surface&) = delete;
  Surface& operator=(const Surface&) = delete;

  virtual bool initialize() = 0;

  [[nodiscard]] bool isRunning() const noexcept;

  void setConfigureCallback(ConfigureCallback callback);
  void setPrepareFrameCallback(PrepareFrameCallback callback);
  void setUpdateCallback(UpdateCallback callback);
  void setFrameTickCallback(FrameTickCallback callback);
  void setInputRegion(const std::vector<InputRect>& rects);
  void setBlurRegion(const std::vector<InputRect>& rects);
  void clearBlurRegion();
  void setDebugName(std::string name);

  // Approximates a rounded rectangle as a stack of horizontal axis-aligned strips
  // suitable for `wl_region` (which has no curve primitives). The four corner radii
  // are applied independently. `stripPx` is the strip height through the corner
  // bands; smaller is smoother but produces more rects.
  static std::vector<InputRect> tessellateRoundedRect(
      int x, int y, int w, int h, float tlRadius, float trRadius, float brRadius, float blRadius, int stripPx = 1
  );
  static std::vector<InputRect> tessellateRoundedRect(int x, int y, int w, int h, float radius, int stripPx = 1) {
    return tessellateRoundedRect(x, y, w, h, radius, radius, radius, radius, stripPx);
  }
  // Tessellates the same shape the rect shader rasterizes: per-corner Concave/Convex
  // curves around a logical body. (x, y, w, h) describes the BODY rect; the visual
  // rect is the body expanded outward by logicalInset (so callers don't need to
  // pre-compute the bounding box that hosts concave-corner bulges). Concave corners
  // extend outward into the inset margin; convex corners contract the body inward.
  static std::vector<InputRect> tessellateShape(
      int x, int y, int w, int h, const CornerShapes& corners, const RectInsets& logicalInset, const Radii& radii,
      int stripPx = 1
  );
  // Tessellates a rounded rectangle rotated by `rotationRad` around its center
  // (`centerX`, `centerY`) into axis-aligned horizontal strips. When `rotationRad`
  // is zero this reduces to a plain tessellateRoundedRect call.
  static std::vector<InputRect> tessellateRotatedRoundedRect(
      float centerX, float centerY, float width, float height, float radius, float rotationRad, int stripPx = 1
  );
  void requestUpdate();
  void requestUpdateOnly();
  void requestLayout();
  void requestRedraw();
  void requestFrameTick();
  void renderNow();
  void setAnimationManager(AnimationManager* manager) noexcept { m_animationManager = manager; }
  void setSceneRoot(Node* root);
  void setRenderContext(RenderContext* ctx);
  [[nodiscard]] RenderContext* renderContext() const noexcept { return m_renderContext; }
  [[nodiscard]] RenderTarget& renderTarget() noexcept { return m_renderTarget; }
  [[nodiscard]] wl_surface* wlSurface() const noexcept { return m_surface; }
  [[nodiscard]] const std::string& debugName() const noexcept { return m_debugName; }
  [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
  [[nodiscard]] std::int32_t bufferScale() const noexcept { return m_bufferScale; }
  [[nodiscard]] float effectiveBufferScale() const noexcept;
  [[nodiscard]] std::uint32_t bufferWidthFor(std::uint32_t logicalWidth) const noexcept;
  [[nodiscard]] std::uint32_t bufferHeightFor(std::uint32_t logicalHeight) const noexcept;

  static void handleFrameDone(void* data, wl_callback* callback, std::uint32_t callbackData);
  void onSurfaceOutputEnter(wl_surface* surface, wl_output* output);
  void onSurfaceOutputLeave(wl_surface* surface, wl_output* output);
  [[nodiscard]] static bool hasPendingFrameWork();
  static void drainPendingFrameWork();
  [[nodiscard]] static bool hasPendingRenders();
  static void drainPendingRenders();
  [[nodiscard]] static SurfaceIdleProfileSnapshot takeIdleProfileSnapshot(bool reset);
  void onPreferredFractionalScale(std::uint32_t numerator);

protected:
  virtual bool createWlSurface();
  virtual void onConfigure(std::uint32_t width, std::uint32_t height);
  virtual void render();
  virtual void onScaleChanged();
  bool prepareBlurEffect();
  void initializeSurfaceScaleProtocol();
  void applySurfaceScaleState();
  void requestFrame();
  void destroySurface();

  void setRunning(bool running) noexcept { m_running = running; }
  void setBufferScale(std::int32_t bufferScale) noexcept { m_bufferScale = bufferScale; }

  WaylandConnection& m_connection;
  wl_surface* m_surface = nullptr;

private:
  struct InvalidationToken {};

  void preparePendingFrame();
  void kickFrameLoop();
  void queueFrameWork(bool runFrameTick = false, float deltaMs = 0.0f);
  void cancelQueuedFrameWork();
  void processQueuedFrameWork();
  void queueRenderIfNeeded();
  void continueAnimationFrameLoop();
  void queueRender();
  void cancelQueuedRender();
  void renderQueuedFrame();
  bool ensureRenderTargetReady();
  void resizeRenderTarget();

  RenderContext* m_renderContext = nullptr;
  RenderTarget m_renderTarget;
  AnimationManager* m_animationManager = nullptr;
  Node* m_sceneRoot = nullptr;
  std::string m_debugName;
  std::shared_ptr<InvalidationToken> m_invalidationToken = std::make_shared<InvalidationToken>();
  ConfigureCallback m_configureCallback;
  PrepareFrameCallback m_prepareFrameCallback;
  UpdateCallback m_updateCallback;
  FrameTickCallback m_frameTickCallback;
  wl_callback* m_frameCallback = nullptr;
  ext_background_effect_surface_v1* m_backgroundEffect = nullptr;
  wp_viewport* m_viewport = nullptr;
  wp_fractional_scale_v1* m_fractionalScale = nullptr;
  std::optional<std::chrono::steady_clock::time_point> m_lastFrameAt;
  bool m_updateRequested = false;
  bool m_layoutRequested = false;
  bool m_redrawRequested = false;
  bool m_running = false;
  bool m_configured = false;
  bool m_inFrameHandler = false;
  bool m_inPrepareFrame = false;
  bool m_frameWorkQueued = false;
  bool m_frameTickPending = false;
  bool m_frameCallbackShouldTick = false;
  bool m_nextFrameCallbackShouldTick = false;
  bool m_renderQueued = false;
  float m_pendingFrameDeltaMs = 0.0f;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  std::int32_t m_bufferScale = 1;
  std::uint32_t m_fractionalScaleNumerator = 0;
};
