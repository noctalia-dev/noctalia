#include "system/surface_capability_probe.h"

#include "system/surface_display_sensors.h"
#include "util/file_utils.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

namespace noctalia::system::surface {
  namespace {

    [[nodiscard]] bool modulePresent(const std::filesystem::path& moduleRoot, std::string_view name) {
      std::error_code error;
      return std::filesystem::is_directory(moduleRoot / name, error) && !error;
    }

    [[nodiscard]] bool aggregatorBusPresent(const std::filesystem::path& aggregatorBusRoot) {
      std::error_code error;
      // Require the devices/ child so an empty injectable root directory is not a false positive.
      return std::filesystem::is_directory(aggregatorBusRoot / "devices", error) && !error;
    }

    [[nodiscard]] std::string trimTrailingWhitespace(std::string value) {
      while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
      }
      return value;
    }

    [[nodiscard]] bool hwmonNamed(
        const std::filesystem::path& hwmonRoot, std::string_view expectedName,
        const std::function<bool(const std::filesystem::path&)>& hasRequiredFile
    ) {
      std::error_code error;
      if (!std::filesystem::is_directory(hwmonRoot, error) || error) {
        return false;
      }

      for (const auto& entry : std::filesystem::directory_iterator(hwmonRoot, error)) {
        if (error || !entry.is_directory(error) || error) {
          continue;
        }
        const auto name = FileUtils::readSmallTextFile(entry.path() / "name");
        if (!name.has_value()) {
          continue;
        }
        if (trimTrailingWhitespace(*name) != expectedName) {
          continue;
        }
        if (hasRequiredFile(entry.path())) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  Capabilities probe(
      const std::filesystem::path& moduleRoot, const std::filesystem::path& aggregatorBusRoot,
      const std::filesystem::path& hwmonRoot, const std::filesystem::path& iioRoot,
      const std::filesystem::path& inputClassRoot
  ) {
    (void)inputClassRoot;
    Capabilities caps;
    const bool aggregatorModule = modulePresent(moduleRoot, "surface_aggregator");
    caps.sectionGate = aggregatorModule || aggregatorBusPresent(aggregatorBusRoot);
    caps.platformProfileDriver = modulePresent(moduleRoot, "surface_platform_profile");
    caps.surfaceBatteryModule = modulePresent(moduleRoot, "surface_battery");
    caps.tabletModeSwitch = modulePresent(moduleRoot, "surface_aggregator_tabletsw")
        || readTabletModeState(aggregatorBusRoot / "devices").has_value();
    {
      const auto slateParam =
          moduleRoot / "surface_aggregator_tabletsw" / "parameters" / "tablet_mode_in_slate_state";
      caps.slateAsTabletParam = tabletModeInSlateStateWritable(slateParam);
    }
    caps.typeCoverHotplug =
        modulePresent(moduleRoot, "surface_hid") || modulePresent(moduleRoot, "surface_hotplug");
    caps.alsSensor = readAlsLux(iioRoot).has_value()
        || [&] {
             std::error_code error;
             if (!std::filesystem::is_directory(iioRoot, error) || error) {
               return false;
             }
             for (const auto& entry : std::filesystem::directory_iterator(iioRoot, error)) {
               if (error || !entry.is_directory(error) || error) {
                 continue;
               }
               auto name = FileUtils::readSmallTextFile(entry.path() / "name");
               if (name.has_value() && trimTrailingWhitespace(*name) == "als") {
                 return true;
               }
             }
             return false;
           }();
    caps.accelSensor = readAccel(iioRoot).has_value()
        || [&] {
             std::error_code error;
             if (!std::filesystem::is_directory(iioRoot, error) || error) {
               return false;
             }
             for (const auto& entry : std::filesystem::directory_iterator(iioRoot, error)) {
               if (error || !entry.is_directory(error) || error) {
                 continue;
               }
               auto name = FileUtils::readSmallTextFile(entry.path() / "name");
               if (name.has_value() && trimTrailingWhitespace(*name) == "accel_3d") {
                 return true;
               }
             }
             return false;
           }();

    caps.fanRpm = hwmonNamed(hwmonRoot, "surface_fan", [](const std::filesystem::path& dir) {
      std::error_code error;
      return std::filesystem::is_regular_file(dir / "fan1_input", error) && !error;
    });

    caps.thermalSensors = hwmonNamed(hwmonRoot, "surface_thermal", [](const std::filesystem::path& dir) {
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file(error) || error) {
          continue;
        }
        const auto fileName = entry.path().filename().string();
        if (fileName.starts_with("temp") && fileName.ends_with("_input")) {
          return true;
        }
      }
      return false;
    });

    return caps;
  }

} // namespace noctalia::system::surface
