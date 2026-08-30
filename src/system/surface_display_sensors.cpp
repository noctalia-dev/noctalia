#include "system/surface_display_sensors.h"

#include "core/process/process.h"
#include "util/file_utils.h"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace noctalia::system::surface {
  namespace {

    [[nodiscard]] std::string trimTrailing(std::string value) {
      while (!value.empty()
             && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
      }
      return value;
    }

    [[nodiscard]] std::optional<double> parseDouble(std::string_view text) {
      double value = 0.0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
      }
      return value;
    }

    [[nodiscard]] std::optional<std::filesystem::path>
    findIioDevice(const std::filesystem::path& iioRoot, std::string_view expectedName) {
      // Cache resolved device dirs briefly — /sys walks every tick were noticeable, but a
      // forever-sticky miss would ignore sensors that appear after boot (module load / hotplug).
      constexpr auto kCacheTtl = std::chrono::seconds(30);
      struct CacheEntry {
        std::filesystem::path root;
        std::string name;
        std::optional<std::filesystem::path> device;
        std::chrono::steady_clock::time_point resolvedAt{};
        bool resolved = false;
      };
      static CacheEntry alsCache;
      static CacheEntry accelCache;
      CacheEntry* cache = nullptr;
      if (expectedName == "als") {
        cache = &alsCache;
      } else if (expectedName == "accel_3d") {
        cache = &accelCache;
      }
      const auto now = std::chrono::steady_clock::now();
      if (cache != nullptr && cache->resolved && cache->root == iioRoot && cache->name == expectedName
          && (now - cache->resolvedAt) < kCacheTtl) {
        return cache->device;
      }

      std::error_code error;
      std::optional<std::filesystem::path> found;
      if (std::filesystem::is_directory(iioRoot, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(iioRoot, error)) {
          if (error || !entry.is_directory(error) || error) {
            continue;
          }
          auto name = FileUtils::readSmallTextFile(entry.path() / "name");
          if (!name.has_value() || trimTrailing(*name) != expectedName) {
            continue;
          }
          found = entry.path();
          break;
        }
      }

      if (cache != nullptr) {
        cache->root = iioRoot;
        cache->name = std::string(expectedName);
        cache->device = found;
        cache->resolvedAt = now;
        cache->resolved = true;
      }
      return found;
    }

    [[nodiscard]] std::optional<double>
    readScaledAxis(const std::filesystem::path& deviceDir, std::string_view rawName, std::string_view scaleName) {
      auto rawText = FileUtils::readSmallTextFile(deviceDir / rawName);
      if (!rawText.has_value()) {
        return std::nullopt;
      }
      const auto raw = parseDouble(trimTrailing(*rawText));
      if (!raw.has_value()) {
        return std::nullopt;
      }
      double scale = 1.0;
      if (auto scaleText = FileUtils::readSmallTextFile(deviceDir / scaleName)) {
        if (const auto parsed = parseDouble(trimTrailing(*scaleText))) {
          scale = *parsed;
        }
      }
      return *raw * scale;
    }

  } // namespace

  std::optional<std::string> readTabletModeState(const std::filesystem::path& aggregatorDevicesRoot) {
    std::error_code error;
    if (!std::filesystem::is_directory(aggregatorDevicesRoot, error) || error) {
      return std::nullopt;
    }

    for (const auto& entry : std::filesystem::directory_iterator(aggregatorDevicesRoot, error)) {
      if (error || !entry.is_directory(error) || error) {
        continue;
      }
      const auto driverLink = entry.path() / "driver";
      std::error_code linkStatError;
      const auto linkStatus = std::filesystem::symlink_status(driverLink, linkStatError);
      if (linkStatError || !std::filesystem::is_symlink(linkStatus)) {
        continue;
      }
      std::error_code linkError;
      const auto target = std::filesystem::read_symlink(driverLink, linkError);
      if (linkError) {
        continue;
      }
      if (target.filename() != "surface_aggregator_tablet_mode_switch") {
        continue;
      }
      auto state = FileUtils::readSmallTextFile(entry.path() / "state");
      if (!state.has_value()) {
        return std::nullopt;
      }
      return trimTrailing(std::move(*state));
    }
    return std::nullopt;
  }

  std::optional<bool> readTabletModeInSlateState(const std::filesystem::path& paramPath) {
    auto text = FileUtils::readSmallTextFile(paramPath);
    if (!text.has_value()) {
      return std::nullopt;
    }
    const auto value = trimTrailing(*text);
    if (value == "Y" || value == "y" || value == "1") {
      return true;
    }
    if (value == "N" || value == "n" || value == "0") {
      return false;
    }
    return std::nullopt;
  }

  bool tabletModeInSlateStateWritable(const std::filesystem::path& paramPath) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(paramPath, error) || error) {
      return false;
    }
    return ::access(paramPath.c_str(), W_OK) == 0;
  }

  bool writeTabletModeInSlateState(bool enabled, const std::filesystem::path& paramPath) {
    const auto current = readTabletModeInSlateState(paramPath);
    if (current.has_value() && *current == enabled) {
      return true;
    }
    if (!tabletModeInSlateStateWritable(paramPath)) {
      return false;
    }
    std::ofstream out(paramPath, std::ios::trunc);
    if (!out) {
      return false;
    }
    out << (enabled ? 'Y' : 'N');
    return static_cast<bool>(out);
  }

  std::optional<double> readAlsLux(const std::filesystem::path& iioRoot) {
    const auto reading = readAls(iioRoot);
    if (!reading.reporting) {
      return std::nullopt;
    }
    return reading.lux;
  }

  AlsReading readAls(const std::filesystem::path& iioRoot) {
    AlsReading reading;
    const auto device = findIioDevice(iioRoot, "als");
    if (!device.has_value()) {
      return reading;
    }
    reading.devicePresent = true;

    // Prefer illuminance; fall back to intensity (some HID ALS firmware only fills one).
    auto lux = readScaledAxis(*device, "in_illuminance_raw", "in_illuminance_scale");
    if (!lux.has_value() || *lux <= 0.0) {
      lux = readScaledAxis(*device, "in_intensity_both_raw", "in_intensity_scale");
    }
    if (!lux.has_value()) {
      return reading;
    }
    reading.lux = *lux;
    // HID ALS sometimes returns 0 between input reports; treat any successful
    // channel read as "present and sampled". Callers hold last-good lux across
    // transient zeros and only back off after a sustained run of zeros.
    reading.reporting = true;
    return reading;
  }

  std::optional<AccelSample> readAccel(const std::filesystem::path& iioRoot) {
    const auto device = findIioDevice(iioRoot, "accel_3d");
    if (!device.has_value()) {
      return std::nullopt;
    }
    const auto x = readScaledAxis(*device, "in_accel_x_raw", "in_accel_scale");
    const auto y = readScaledAxis(*device, "in_accel_y_raw", "in_accel_scale");
    const auto z = readScaledAxis(*device, "in_accel_z_raw", "in_accel_scale");
    if (!x.has_value() || !y.has_value() || !z.has_value()) {
      return std::nullopt;
    }
    return AccelSample{.x = *x, .y = *y, .z = *z};
  }

  std::optional<bool> readTypeCoverAttached(
      const std::filesystem::path& inputClassRoot, const std::filesystem::path& moduleRoot
  ) {
    std::error_code error;
    bool keyboardPresent = false;
    if (std::filesystem::is_directory(inputClassRoot, error) && !error) {
      for (const auto& entry : std::filesystem::directory_iterator(inputClassRoot, error)) {
        if (error || !entry.is_directory(error) || error) {
          continue;
        }
        const auto nameFile = entry.path() / "device" / "name";
        auto name = FileUtils::readSmallTextFile(nameFile);
        if (!name.has_value()) {
          continue;
        }
        const auto trimmed = trimTrailing(std::move(*name));
        // Type Cover / keyboard folio nodes look like "Microsoft Surface 045E:09B0 Keyboard".
        if (trimmed.find("Microsoft Surface") != std::string::npos
            && trimmed.find("Keyboard") != std::string::npos) {
          keyboardPresent = true;
          break;
        }
      }
    }

    if (keyboardPresent) {
      return true;
    }

    // Without a keyboard node, "detached" is only meaningful on hosts that hotplug covers.
    std::error_code moduleError;
    const bool surfaceHid = std::filesystem::is_directory(moduleRoot / "surface_hid", moduleError) && !moduleError;
    const bool surfaceHotplug =
        std::filesystem::is_directory(moduleRoot / "surface_hotplug", moduleError) && !moduleError;
    if (surfaceHid || surfaceHotplug) {
      return false;
    }
    return std::nullopt;
  }

  bool waylandOskAvailable() {
    return process::commandExists("squeekboard") || process::commandExists("wvkbd-mobintl");
  }

  void setWaylandOskArmed(bool armed) {
    struct OskSession {
      std::optional<int> squeekPid;
      std::optional<int> wvkbdPid;
      std::optional<std::string> priorA11yOsk;
      bool a11yChanged = false;
    };
    static OskSession session;

    const auto killOwned = [](std::optional<int>& pid) {
      if (!pid.has_value() || *pid <= 0) {
        pid.reset();
        return;
      }
      // Non-blocking: never waitpid on the shell timer thread.
      (void)process::runAsync({"kill", "-TERM", std::to_string(*pid)});
      pid.reset();
    };

    if (process::commandExists("squeekboard")) {
      if (armed) {
        if (process::commandExists("gsettings") && !session.a11yChanged) {
          const auto prior = process::runSync(
              {"gsettings", "get", "org.gnome.desktop.a11y.applications", "screen-keyboard-enabled"}
          );
          if (prior) {
            auto value = prior.out;
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
              value.pop_back();
            }
            session.priorA11yOsk = std::move(value);
          }
          (void)process::runAsync(
              {"gsettings", "set", "org.gnome.desktop.a11y.applications", "screen-keyboard-enabled", "true"}
          );
          session.a11yChanged = true;
        }
        if (!session.squeekPid.has_value()) {
          session.squeekPid = process::launchDetachedTracked({"squeekboard"});
        }
        return;
      }

      if (process::commandExists("busctl")) {
        (void)process::runAsync(
            {"busctl", "call", "--user", "sm.puri.OSK0", "/sm/puri/OSK0", "sm.puri.OSK0", "SetVisible", "b", "false"}
        );
      }
      killOwned(session.squeekPid);
      if (session.a11yChanged && process::commandExists("gsettings")) {
        const std::string restore = session.priorA11yOsk.value_or("false");
        (void)process::runAsync(
            {"gsettings", "set", "org.gnome.desktop.a11y.applications", "screen-keyboard-enabled", restore}
        );
      }
      session.priorA11yOsk.reset();
      session.a11yChanged = false;
      return;
    }

    if (!process::commandExists("wvkbd-mobintl")) {
      return;
    }
    if (armed) {
      if (!session.wvkbdPid.has_value()) {
        session.wvkbdPid = process::launchDetachedTracked({"wvkbd-mobintl"});
      }
      return;
    }
    killOwned(session.wvkbdPid);
  }

  DeviceOrientation classifyOrientation(const AccelSample& sample) noexcept {
    switch (classifyScreenRotation(sample)) {
    case ScreenRotation::Flat:
      return DeviceOrientation::Flat;
    case ScreenRotation::Normal:
    case ScreenRotation::Rotate180:
      return DeviceOrientation::Landscape;
    case ScreenRotation::Rotate90:
    case ScreenRotation::Rotate270:
      return DeviceOrientation::Portrait;
    }
    return DeviceOrientation::Unknown;
  }

  ScreenRotation classifyScreenRotation(const AccelSample& sample) noexcept {
    const double absX = std::abs(sample.x);
    const double absY = std::abs(sample.y);
    const double absZ = std::abs(sample.z);
    constexpr double kMinG = 4.0; // ~0.4 g
    const double dominant = std::max({absX, absY, absZ});
    if (dominant < kMinG) {
      return ScreenRotation::Flat;
    }
    // Face-up / face-down: keep transform unchanged.
    if (absZ >= absX && absZ >= absY) {
      return ScreenRotation::Flat;
    }
    // Landscape (Y gravity axis dominant on Surface Pro when upright in laptop/tablet).
    if (absY >= absX) {
      return sample.y < 0.0 ? ScreenRotation::Normal : ScreenRotation::Rotate180;
    }
    // Portrait — X dominant. Sign mapping may need device-specific tweak later.
    return sample.x > 0.0 ? ScreenRotation::Rotate90 : ScreenRotation::Rotate270;
  }

} // namespace noctalia::system::surface
