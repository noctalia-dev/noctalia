#include "pipewire/audio_route_selection.h"
#include "tests/test_check.h"

#include <vector>

int main() {
  using Route = PipeWireService::DeviceRouteData;
  const std::vector<Route> outputRoutes = {
      Route{.index = 0, .device = 0, .direction = SPA_DIRECTION_OUTPUT, .priority = 100, .muted = false},
      Route{.index = 1, .device = 1, .direction = SPA_DIRECTION_OUTPUT, .priority = 200, .muted = true},
  };

  {
    const Route* route = activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, 0);
    TEST_CHECK(route != nullptr && route->device == 0 && !route->muted);
  }
  {
    const Route* route = activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, 1);
    TEST_CHECK(route != nullptr && route->device == 1 && route->muted);
  }
  {
    const Route* route = activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, kAnyProfileDevice);
    TEST_CHECK(route != nullptr && route->device == 1 && route->muted);
  }

  TEST_CHECK(activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_OUTPUT, 9) == nullptr);
  TEST_CHECK(activeAudioDeviceRoute(outputRoutes, SPA_DIRECTION_INPUT, 0) == nullptr);

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
  {
    const Route* route = activeAudioDeviceRoute(sameDeviceRoutes, SPA_DIRECTION_OUTPUT, 0);
    TEST_CHECK(route != nullptr && route->device == 0 && route->muted);
  }

  const std::vector<Route> unrelatedDeviceRoutes = {
      Route{.index = 2, .device = 1, .direction = SPA_DIRECTION_OUTPUT, .available = SPA_PARAM_AVAILABILITY_yes},
  };
  TEST_CHECK(audioNodeRouteAvailable({}, unrelatedDeviceRoutes, SPA_DIRECTION_OUTPUT, 0));

  const std::vector<Route> unavailableMatchingRoutes = {
      Route{.index = 3, .device = 0, .direction = SPA_DIRECTION_OUTPUT, .available = SPA_PARAM_AVAILABILITY_no},
  };
  TEST_CHECK(!audioNodeRouteAvailable({}, unavailableMatchingRoutes, SPA_DIRECTION_OUTPUT, 0));

  return 0;
}
