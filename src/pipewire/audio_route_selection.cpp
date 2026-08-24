#include "pipewire/audio_route_selection.h"

#include <algorithm>

namespace {
  [[nodiscard]] bool routeIsSelectable(
      const PipeWireService::DeviceRouteData& route, std::uint32_t wantDirection, std::int32_t profileDevice
  ) {
    return route.index >= 0
        && route.direction == wantDirection
        && route.available != SPA_PARAM_AVAILABILITY_no
        && (profileDevice < 0 || route.device == profileDevice);
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

const PipeWireService::DeviceRouteData* activeAudioDeviceRoute(
    const std::vector<PipeWireService::DeviceRouteData>& routes, std::uint32_t wantDirection, std::int32_t profileDevice
) {
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
    const std::vector<PipeWireService::DeviceRouteData>& nodeRoutes,
    const std::vector<PipeWireService::DeviceRouteData>& deviceRoutes, std::uint32_t wantDirection,
    std::int32_t profileDevice
) {
  if (activeAudioDeviceRoute(nodeRoutes, wantDirection, -1) != nullptr
      || activeAudioDeviceRoute(deviceRoutes, wantDirection, profileDevice) != nullptr) {
    return true;
  }

  const auto matchesDirection = [wantDirection](const PipeWireService::DeviceRouteData& route) {
    return route.direction == wantDirection;
  };
  const auto matchesDevice = [wantDirection, profileDevice](const PipeWireService::DeviceRouteData& route) {
    return route.direction == wantDirection && (profileDevice < 0 || route.device == profileDevice);
  };
  return !std::ranges::any_of(nodeRoutes, matchesDirection) && !std::ranges::any_of(deviceRoutes, matchesDevice);
}
