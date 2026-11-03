#pragma once

#include "capture/screencopy_capture.h"
#include "render/animation/animation_manager.h"
#include "render/core/blur_cache.h"
#include "render/core/texture_handle.h"
#include "shell/render/lockscreen_transition_types.h"
#include "shell/wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <vector>

class Box;
class ConfigService;
class LockscreenTransitionCover;
class Node;
class RenderContext;
class WallpaperNode;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
enum class LockscreenTransition : std::uint8_t;

/// Plays a lock-screen transition on fullscreen overlay layers without taking the session lock: the captured
/// desktop gives way to its blurred, tinted copy, holds, then comes back. Any click or key press ends it early.
class LockscreenTransitionPreview {
public:
  LockscreenTransitionPreview() = default;
  ~LockscreenTransitionPreview();

  LockscreenTransitionPreview(const LockscreenTransitionPreview&) = delete;
  LockscreenTransitionPreview& operator=(const LockscreenTransitionPreview&) = delete;

  void initialize(WaylandConnection& wayland, RenderContext* renderContext, ConfigService* configService);
  void play(LockscreenTransition transition);
  void hide();
  void onOutputChange();

  bool isActive() const noexcept { return !m_instances.empty(); }
  // Swallow all input while the preview is up; a press dismisses it.
  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);

private:
  struct Instance {
    wl_output* output = nullptr;
    ScreencopyImage capture;
    std::unique_ptr<LayerSurface> surface;
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    WallpaperNode* background = nullptr;
    Box* tint = nullptr;
    LockscreenTransitionCover* cover = nullptr;
    TextureHandle captureTexture;
    TextureHandle blurredTexture;
    BlurCache blurCache;
    LockscreenTransitionParams params;
    float progress = 0.0F;
    bool started = false;
  };

  void prepareFrame(Instance& inst);
  bool buildScene(Instance& inst);
  void start(Instance& inst);
  void releaseTextures(Instance& inst);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  ConfigService* m_configService = nullptr;
  std::vector<std::unique_ptr<Instance>> m_instances;
  LockscreenTransitionKind m_transition = LockscreenTransitionKind::Fade;
  float m_durationMs = 1500.0F;
};
