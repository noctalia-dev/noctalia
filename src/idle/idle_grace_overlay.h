#pragma once

#include "render/animation/animation_manager.h"
#include "wayland/layer_surface.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Box;
class RenderContext;
class WaylandConnection;

/// Fullscreen overlay layer: fades from transparent to the shell surface color, then signals completion.
/// Click-through so input still reaches the compositor and `ext_idle_notification_v1` can emit `resumed` during the
/// fade.
class IdleGraceOverlay {
public:
  IdleGraceOverlay() = default;
  ~IdleGraceOverlay() = default;

  IdleGraceOverlay(const IdleGraceOverlay&) = delete;
  IdleGraceOverlay& operator=(const IdleGraceOverlay&) = delete;

  void initialize(WaylandConnection& wayland, RenderContext* renderContext);
  void onOutputChange();

  /// Shows overlay on all outputs and fades the fullscreen surface tint 0 → 1 over `fadeIn`. Invokes
  /// `onFadeComplete` once every output has finished fading (or immediately if there are no outputs).
  void show(std::chrono::milliseconds fadeIn, std::function<void()> onFadeComplete = {});
  /// Tears down overlay surfaces (running fade animations are cancelled).
  void hide();

private:
  struct Instance {
    wl_output* output = nullptr;
    std::int32_t scale = 1;
    std::unique_ptr<LayerSurface> surface;
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    Box* dim = nullptr;
    AnimationManager::Id fadeAnimId = 0;
    bool visible = false;
    std::optional<std::chrono::milliseconds> pendingFadeIn;
  };

  void ensureSurfaces();
  [[nodiscard]] bool surfacesMatchOutputs() const;
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, std::uint32_t width, std::uint32_t height);
  void startFadeIn(Instance& inst, std::chrono::milliseconds fadeIn);
  void onFadeInstanceComplete();

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::vector<std::unique_ptr<Instance>> m_instances;
  bool m_showRequested = false;
  std::function<void()> m_onAllFadesComplete;
  std::uint32_t m_fadeCompletionTarget = 0;
  std::uint32_t m_fadeCompletionsReceived = 0;
};
