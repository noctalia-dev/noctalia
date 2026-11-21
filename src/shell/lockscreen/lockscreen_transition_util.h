#pragma once

#include "shell/render/lockscreen_transition_types.h"

#include <cstdint>
#include <functional>
#include <vector>

struct ScreencopyImage;
struct WaylandOutput;
class WaylandConnection;
enum class LockscreenTransition : std::uint8_t;

namespace lockscreen {

  LockscreenTransitionKind renderTransitionKind(LockscreenTransition transition);

  // Randomized per-lock variation (direction, center, cell size, ...) for the given effect.
  LockscreenTransitionParams randomTransitionParams(LockscreenTransition transition, float edgeSmoothness);

  // Screencopies each target over an isolated sibling connection and hands every successful capture to
  // `onCapture`, still in the compositor's buffer orientation. Returns false only when `cancelled` stopped it.
  bool captureOutputs(
      WaylandConnection& wayland, const std::vector<WaylandOutput>& targets, const std::function<bool()>& cancelled,
      const std::function<void(const WaylandOutput& target, ScreencopyImage&& image)>& onCapture
  );

} // namespace lockscreen
