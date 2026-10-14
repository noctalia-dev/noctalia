#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Process names holding non-metadata video devices open, as in the v4 privacy plugin.
// Reads sysfs and procfs without opening the camera. Inaccessible processes are skipped.
[[nodiscard]] std::vector<std::string> v4l2CameraApps(
    const std::filesystem::path& videoClass = "/sys/class/video4linux", const std::filesystem::path& proc = "/proc"
);

// Best-effort match of a V4L2 /proc comm name to a PipeWire application label or binary
// (basename, case-insensitive, including 15-character comm truncation).
[[nodiscard]] bool v4l2CameraSameApp(std::string_view pipewireLabel, std::string_view comm);

// Milliseconds until deadline, clamped to [0, INT_MAX]. Expired deadlines (including epoch)
// return zero; the caller must advance the deadline when handling the pending work.
[[nodiscard]] int v4l2CameraPollTimeoutMs(
    std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now
) noexcept;
