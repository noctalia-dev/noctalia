#pragma once

#include <string>
#include <string_view>

// PipeWire marks helper endpoints that users must not see, such as the hidden sink a split-sink
// filter chain feeds, with an `/Internal` media.class suffix (e.g. `Audio/Sink/Internal`).
[[nodiscard]] bool isInternalAudioMediaClass(std::string_view mediaClass);

// PipeWire exposes virtual endpoints (e.g. EasyEffects) with a suffix such as `Audio/Sink/Virtual`;
// collapse them to the base class so downstream tracking treats them like normal sinks/sources.
// `/Internal` endpoints keep their full class so they stay untracked.
void normalizeAudioMediaClass(std::string& mediaClass);

// Node classes the service keeps in its graph: audio sinks/sources, audio streams, and every video
// class the privacy indicator needs. Internal endpoints are never tracked.
[[nodiscard]] bool isTrackedNodeClass(std::string_view mediaClass);
