#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace noctalia::system::surface {

  /// Coarse device orientation derived from the IIO accelerometer.
  enum class DeviceOrientation : std::uint8_t {
    Unknown,
    Flat,
    Landscape,
    Portrait,
  };

  /// Screen transform suitable for compositor output rotation (Niri).
  enum class ScreenRotation : std::uint8_t {
    Flat, // do not change transform
    Normal,
    Rotate90,
    Rotate180,
    Rotate270,
  };

  struct AccelSample {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
  };

  /// SAM tablet-mode switch state: typically "laptop", "tablet", or "slate".
  [[nodiscard]] std::optional<std::string> readTabletModeState(
      const std::filesystem::path& aggregatorDevicesRoot = "/sys/bus/surface_aggregator/devices"
  );

  /// Kernel module parameter: treat slate posture as tablet mode (SW_TABLET_MODE).
  [[nodiscard]] std::optional<bool> readTabletModeInSlateState(
      const std::filesystem::path& paramPath =
          "/sys/module/surface_aggregator_tabletsw/parameters/tablet_mode_in_slate_state"
  );

  /// True when the slate→tablet-mode module parameter exists and is writable by this process.
  [[nodiscard]] bool tabletModeInSlateStateWritable(
      const std::filesystem::path& paramPath =
          "/sys/module/surface_aggregator_tabletsw/parameters/tablet_mode_in_slate_state"
  );

  /// Write the slate→tablet-mode module parameter. Returns false on I/O failure.
  /// No-ops (returns true) when the current value already matches `enabled`.
  [[nodiscard]] bool writeTabletModeInSlateState(
      bool enabled, const std::filesystem::path& paramPath =
                        "/sys/module/surface_aggregator_tabletsw/parameters/tablet_mode_in_slate_state"
  );

  /// Ambient light reading from IIO `als`.
  struct AlsReading {
    bool devicePresent = false;
    std::optional<double> lux;
    /// True when a channel sample was read (lux may still be 0 between HID reports).
    bool reporting = false;
  };

  /// Ambient light in lux from an IIO `als` device, when present and reporting.
  [[nodiscard]] std::optional<double> readAlsLux(const std::filesystem::path& iioRoot = "/sys/bus/iio/devices");

  /// Full ALS probe: presence + lux + whether the channel appears live.
  [[nodiscard]] AlsReading readAls(const std::filesystem::path& iioRoot = "/sys/bus/iio/devices");

  /// Accelerometer sample in m/s² from IIO `accel_3d`, when present.
  [[nodiscard]] std::optional<AccelSample> readAccel(const std::filesystem::path& iioRoot = "/sys/bus/iio/devices");

  /// Type Cover attach state from input devices.
  /// - `true` when a Microsoft Surface Keyboard input node is present
  /// - `false` when this host supports Type Cover hotplug but no keyboard is present
  /// - nullopt when cover attach cannot be determined
  [[nodiscard]] std::optional<bool> readTypeCoverAttached(
      const std::filesystem::path& inputClassRoot = "/sys/class/input",
      const std::filesystem::path& moduleRoot = "/sys/module"
  );

  /// True when a supported Wayland OSK binary is on PATH (squeekboard or wvkbd-mobintl).
  [[nodiscard]] bool waylandOskAvailable();

  /// Arm (true) or disarm (false) the Wayland OSK for Cover-detach sessions.
  /// Arm starts squeekboard + enables a11y OSK without forcing the panel open.
  /// Disarm hides, restores prior a11y setting, and stops only processes we launched.
  void setWaylandOskArmed(bool armed);

  /// Map an accelerometer sample to a coarse orientation label.
  [[nodiscard]] DeviceOrientation classifyOrientation(const AccelSample& sample) noexcept;

  /// Map accelerometer sample to a compositor screen rotation.
  [[nodiscard]] ScreenRotation classifyScreenRotation(const AccelSample& sample) noexcept;

  [[nodiscard]] inline std::string_view orientationLabel(DeviceOrientation orientation) noexcept {
    switch (orientation) {
    case DeviceOrientation::Flat:
      return "flat";
    case DeviceOrientation::Landscape:
      return "landscape";
    case DeviceOrientation::Portrait:
      return "portrait";
    case DeviceOrientation::Unknown:
    default:
      return "unknown";
    }
  }

  [[nodiscard]] inline std::string_view screenRotationLabel(ScreenRotation rotation) noexcept {
    switch (rotation) {
    case ScreenRotation::Normal:
      return "normal";
    case ScreenRotation::Rotate90:
      return "90";
    case ScreenRotation::Rotate180:
      return "180";
    case ScreenRotation::Rotate270:
      return "270";
    case ScreenRotation::Flat:
    default:
      return "flat";
    }
  }

} // namespace noctalia::system::surface
