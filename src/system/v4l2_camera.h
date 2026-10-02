#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Process names holding non-metadata video devices open, as in the v4 privacy plugin.
// Reads sysfs and procfs without opening the camera. Inaccessible processes are skipped.
[[nodiscard]] std::vector<std::string> v4l2CameraApps(
    const std::filesystem::path& videoClass = "/sys/class/video4linux", const std::filesystem::path& proc = "/proc"
);
