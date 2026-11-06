#include "shell/lockscreen/lockscreen_transition_util.h"

#include "capture/screencopy_capture.h"
#include "capture/screencopy_util.h"
#include "config/config_types.h"
#include "core/log.h"
#include "core/random.h"
#include "shell/wayland/capture_globals.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace {

  constexpr Logger kLog("lockscreen");

} // namespace

namespace lockscreen {

  LockscreenTransitionKind renderTransitionKind(LockscreenTransition transition) {
    switch (transition) {
    case LockscreenTransition::Fade:
      return LockscreenTransitionKind::Fade;
    case LockscreenTransition::Wipe:
      return LockscreenTransitionKind::Wipe;
    case LockscreenTransition::Disc:
      return LockscreenTransitionKind::Disc;
    case LockscreenTransition::Stripes:
      return LockscreenTransitionKind::Stripes;
    case LockscreenTransition::Zoom:
      return LockscreenTransitionKind::Zoom;
    case LockscreenTransition::Honeycomb:
      return LockscreenTransitionKind::Honeycomb;
    case LockscreenTransition::Vortex:
      return LockscreenTransitionKind::Vortex;
    case LockscreenTransition::Pixel:
      return LockscreenTransitionKind::Pixel;
    case LockscreenTransition::Diamond:
      return LockscreenTransitionKind::Diamond;
    case LockscreenTransition::Golden:
      return LockscreenTransitionKind::Golden;
    case LockscreenTransition::Melt:
      return LockscreenTransitionKind::Melt;
    }
    std::unreachable();
  }

  LockscreenTransitionParams randomTransitionParams(LockscreenTransition transition, float edgeSmoothness) {
    LockscreenTransitionParams params;
    params.smoothness = edgeSmoothness;

    switch (transition) {
    case LockscreenTransition::Wipe:
      params.direction = std::floor(Random::randomFloat(0.0F, 4.0F));
      break;
    case LockscreenTransition::Disc:
      params.centerX = Random::randomFloat(0.2F, 0.8F);
      params.centerY = Random::randomFloat(0.2F, 0.8F);
      break;
    case LockscreenTransition::Stripes:
      params.stripeCount = std::round(Random::randomFloat(4.0F, 24.0F));
      params.angle = Random::randomFloat(0.0F, 360.0F);
      break;
    case LockscreenTransition::Honeycomb:
      params.cellSize = Random::randomFloat(0.02F, 0.06F);
      params.centerX = Random::randomFloat(0.2F, 0.8F);
      params.centerY = Random::randomFloat(0.2F, 0.8F);
      break;
    case LockscreenTransition::Vortex:
      params.centerX = Random::randomFloat(0.2F, 0.8F);
      params.centerY = Random::randomFloat(0.2F, 0.8F);
      break;
    case LockscreenTransition::Pixel:
      params.cellSize = Random::randomFloat(0.02F, 0.05F);
      params.centerX = Random::randomFloat(0.2F, 0.8F);
      params.centerY = Random::randomFloat(0.2F, 0.8F);
      break;
    case LockscreenTransition::Diamond:
      params.cellSize = Random::randomFloat(0.0345F, 0.069F);
      params.centerX = Random::randomFloat(0.2F, 0.8F);
      params.centerY = Random::randomFloat(0.2F, 0.8F);
      break;
    case LockscreenTransition::Golden:
      params.centerX = Random::randomFloat(0.2F, 0.8F);
      params.centerY = Random::randomFloat(0.2F, 0.8F);
      params.angle = Random::randomFloat(0.0F, 360.0F);
      break;
    case LockscreenTransition::Melt:
      params.angle = Random::randomFloat(0.0F, 360.0F);
      break;
    case LockscreenTransition::Fade:
    case LockscreenTransition::Zoom:
      break;
    }
    return params;
  }

  bool captureOutputs(
      WaylandConnection& wayland, const std::vector<WaylandOutput>& targets, const std::function<bool()>& cancelled,
      const std::function<void(const WaylandOutput& target, ScreencopyImage&& image)>& onCapture
  ) {
    if (targets.empty()) {
      return true;
    }

    const auto firstCaptureDeadline = std::chrono::steady_clock::now() + screencopy::kBlockingCaptureTimeout;
    WaylandConnection captureWayland(WaylandConnection::Purpose::Sibling);
    captureWayland.extension<ScreencopyGlobals>();
    std::string setupError;
    if (!captureWayland.connectUntil(firstCaptureDeadline, &wayland, setupError)) {
      kLog.warn("lockscreen desktop capture setup failed: {}", setupError);
      return true;
    }

    ScreencopyCapture capture(captureWayland);
    if (!capture.available()) {
      kLog.warn("lockscreen desktop capture requested but isolated screencopy is unavailable");
      return true;
    }

    bool firstCapture = true;
    for (const auto& target : targets) {
      if (cancelled()) {
        return false;
      }

      const auto& captureOutputs = captureWayland.outputs();
      const auto captureIt = std::ranges::find_if(captureOutputs, [&target](const WaylandOutput& candidate) {
        return candidate.name == target.name
            && (target.connectorName.empty() || candidate.connectorName == target.connectorName);
      });
      if (captureIt == captureOutputs.end() || captureIt->output == nullptr) {
        kLog.debug("skipping lockscreen desktop capture for disappeared output {}", target.connectorName);
        continue;
      }

      ScreencopyImage image;
      std::string error;
      const auto deadline =
          firstCapture ? std::optional<std::chrono::steady_clock::time_point>(firstCaptureDeadline) : std::nullopt;
      firstCapture = false;
      const screencopy::CaptureOutputStatus captureStatus = screencopy::captureOutputBlocking(
          capture, captureWayland, captureIt->output, image, error, false, &wayland, deadline
      );
      if (captureStatus != screencopy::CaptureOutputStatus::Success) {
        if (cancelled()) {
          return false;
        }
        kLog.warn("lockscreen desktop capture failed for {}: {}", target.connectorName, error);
        if (captureStatus == screencopy::CaptureOutputStatus::EventLoopFailed) {
          break;
        }
        continue;
      }

      if (cancelled()) {
        return false;
      }
      onCapture(target, std::move(image));
    }
    return !cancelled();
  }

} // namespace lockscreen
