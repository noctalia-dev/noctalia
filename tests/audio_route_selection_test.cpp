#include "pipewire/audio_route_selection.h"

#include <print>
#include <vector>

namespace {
  bool expectRoute(
      const PipeWireService::DeviceRouteData* actual, std::int32_t expectedDevice, bool expectedMuted,
      const char* message
  ) {
    if (actual != nullptr && actual->device == expectedDevice && actual->muted == expectedMuted) {
      return true;
    }
    std::println(
        stderr, "audio_route_selection_test: {}: expected device {} muted={}, got device {} muted={}", message,
        expectedDevice, expectedMuted, actual != nullptr ? actual->device : -1, actual != nullptr && actual->muted
    );
    return false;
  }
} // namespace

int main() {
  using Route = PipeWireService::DeviceRouteData;
  const std::vector<Route> outputRoutes = {
      Route{.index = 0, .device = 0, .direction = SPA_DIRECTION_OUTPUT, .priority = 100, .muted = false},
      Route{.index = 1, .device = 1, .direction = SPA_DIRECTION_OUTPUT, .priority = 200, .muted = true},
  };

  bool ok = true;
  ok = expectRoute(activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, 0), 0, false, "speaker route") && ok;
  ok = expectRoute(activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, 1), 1, true, "headphone route") && ok;
  ok = expectRoute(
           activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, kAnyProfileDevice), 1, true, "unbound fallback"
       )
      && ok;

  if (activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, 9) != nullptr) {
    std::println(stderr, "audio_route_selection_test: unmatched profile device must not inherit another route");
    ok = false;
  }
  if (activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_INPUT, 0) != nullptr) {
    std::println(stderr, "audio_route_selection_test: direction mismatch must not select a route");
    ok = false;
  }

  // Within one profile device, an available route beats a higher-priority one of unknown availability.
  const std::vector<Route> sameDeviceRoutes = {
      Route{
          .index = 4,
          .device = 0,
          .direction = SPA_DIRECTION_OUTPUT,
          .priority = 100,
          .available = SPA_PARAM_AVAILABILITY_yes,
          .muted = true
      },
      Route{
          .index = 5,
          .device = 0,
          .direction = SPA_DIRECTION_OUTPUT,
          .priority = 200,
          .available = SPA_PARAM_AVAILABILITY_unknown,
          .muted = false
      },
  };
  ok = expectRoute(
           activeAudioDeviceRoute(sameDeviceRoutes, SPA_DIRECTION_OUTPUT, 0), 0, true, "available beats priority"
       )
      && ok;

  const std::vector<Route> unrelatedDeviceRoutes = {
      Route{.index = 2, .device = 1, .direction = SPA_DIRECTION_OUTPUT, .available = SPA_PARAM_AVAILABILITY_yes},
  };
  if (!audioNodeRouteAvailable({}, unrelatedDeviceRoutes, SPA_DIRECTION_OUTPUT, 0)) {
    std::println(stderr, "audio_route_selection_test: an unrelated device route must not hide the node");
    ok = false;
  }

  const std::vector<Route> unavailableMatchingRoutes = {
      Route{.index = 3, .device = 0, .direction = SPA_DIRECTION_OUTPUT, .available = SPA_PARAM_AVAILABILITY_no},
  };
  if (audioNodeRouteAvailable({}, unavailableMatchingRoutes, SPA_DIRECTION_OUTPUT, 0)) {
    std::println(stderr, "audio_route_selection_test: an unavailable matching device route must hide the node");
    ok = false;
  }

  return ok ? 0 : 1;
}
