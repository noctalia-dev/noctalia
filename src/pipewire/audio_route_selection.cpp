#include "pipewire/audio_route_selection.h"

#include <algorithm>

namespace {
  [[nodiscard]] bool routeMatchesDevice(const PipeWireService::DeviceRouteData& route, std::int32_t profileDevice) {
    return profileDevice == kAnyProfileDevice || route.device == profileDevice;
  }

  [[nodiscard]] bool routeIsSelectable(
      const PipeWireService::DeviceRouteData& route, std::uint32_t wantDirection, std::int32_t profileDevice
  ) {
    return route.index >= 0
        && route.direction == wantDirection
        && route.available != SPA_PARAM_AVAILABILITY_no
        && routeMatchesDevice(route, profileDevice);
  }

  [[nodiscard]] bool routeIsBetterCandidate(
      const PipeWireService::DeviceRouteData& candidate, const PipeWireService::DeviceRouteData& current
  ) {
    const bool candidateAvailable = candidate.available == SPA_PARAM_AVAILABILITY_yes;
    const bool currentAvailable = current.available == SPA_PARAM_AVAILABILITY_yes;
    if (candidateAvailable != currentAvailable) {
      return candidateAvailable;
    }
    return candidate.priority > current.priority;
  }
} // namespace

const PipeWireService::DeviceRouteData*
activeAudioDeviceRoute(AudioDeviceRoutes routes, std::uint32_t wantDirection, std::int32_t profileDevice) {
  const PipeWireService::DeviceRouteData* best = nullptr;
  for (const auto& route : routes) {
    if (!routeIsSelectable(route, wantDirection, profileDevice)) {
      continue;
    }
    if (best == nullptr || routeIsBetterCandidate(route, *best)) {
      best = &route;
    }
  }
  return best;
}

bool audioNodeRouteAvailable(
    AudioDeviceRoutes nodeRoutes, AudioDeviceRoutes deviceRoutes, std::uint32_t wantDirection,
    std::int32_t profileDevice
) {
  if (activeAudioDeviceRoute(nodeRoutes, wantDirection, kAnyProfileDevice) != nullptr
      || activeAudioDeviceRoute(deviceRoutes, wantDirection, profileDevice) != nullptr) {
    return true;
  }

  const auto matchesDirection = [wantDirection](const PipeWireService::DeviceRouteData& route) {
    return route.direction == wantDirection;
  };
  const auto matchesDevice = [wantDirection, profileDevice](const PipeWireService::DeviceRouteData& route) {
    return route.direction == wantDirection && routeMatchesDevice(route, profileDevice);
  };
  return !std::ranges::any_of(nodeRoutes, matchesDirection) && !std::ranges::any_of(deviceRoutes, matchesDevice);
}
