#pragma once

#include "core/inotify/inotify.h"

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

// Wakes only when camera users may have changed, so idle cameras cost nothing: inotify reports
// opens and closes of readable non-metadata /dev/video* nodes, and attribute changes in /dev
// announce hotplugged nodes once udev grants access to them.
class V4l2CameraWatch {
public:
  V4l2CameraWatch();

  [[nodiscard]] int fd() const noexcept { return m_inotify.fd(); }

  // Drains pending events. Returns true when v4l2CameraApps() should run again.
  [[nodiscard]] bool drain();

private:
  void watchDevices();
  void watchDevice(std::string_view name);

  Inotify m_inotify;
  int m_devWd = -1;
};
