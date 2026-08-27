#pragma once

#include "pipewire/pipewire_service.h"

#include <cstdint>
#include <span>

using AudioDeviceRoutes = std::span<const PipeWireService::DeviceRouteData>;

// Passed as profileDevice when a route list is not bound to a card.profile.device.
inline constexpr std::int32_t kAnyProfileDevice = -1;

// Select the best available route for one audio node. A non-negative profileDevice binds the
// selection to that card.profile.device; kAnyProfileDevice selects direction-wide.
[[nodiscard]] const PipeWireService::DeviceRouteData*
activeAudioDeviceRoute(AudioDeviceRoutes routes, std::uint32_t wantDirection, std::int32_t profileDevice);

// Node-local routes are direction-wide. Card routes only affect a node when their device matches
// card.profile.device; an explicitly unavailable matching route hides the node.
[[nodiscard]] bool audioNodeRouteAvailable(
    AudioDeviceRoutes nodeRoutes, AudioDeviceRoutes deviceRoutes, std::uint32_t wantDirection,
    std::int32_t profileDevice
);
