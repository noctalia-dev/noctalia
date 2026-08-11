#pragma once

#include "pipewire/pipewire_service.h"

#include <cstdint>
#include <vector>

// Select the best available route for one audio node. A non-negative profileDevice binds the
// selection to that card.profile.device; -1 preserves the direction-wide fallback for nodes that
// do not expose a profile-device binding.
[[nodiscard]] const PipeWireService::DeviceRouteData* activeAudioDeviceRoute(
    const std::vector<PipeWireService::DeviceRouteData>& routes, std::uint32_t wantDirection, std::int32_t profileDevice
);
