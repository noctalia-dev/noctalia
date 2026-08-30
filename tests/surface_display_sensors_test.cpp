#include "system/surface_display_sensors.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

  class TempTree {
  public:
    TempTree() {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      root = std::filesystem::temp_directory_path() / ("noctalia-surface-display-" + std::to_string(stamp));
      std::filesystem::create_directories(root / "aggregator");
      std::filesystem::create_directories(root / "iio");
      std::filesystem::create_directories(root / "params");
    }

    ~TempTree() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }

    void addTabletSwitch(std::string_view state) const {
      const auto device = root / "aggregator" / "01:26:01:00:01";
      std::filesystem::create_directories(device);
      std::filesystem::create_directories(root / "drivers" / "surface_aggregator_tablet_mode_switch");
      std::filesystem::create_directory_symlink(
          "../../drivers/surface_aggregator_tablet_mode_switch", device / "driver"
      );
      std::ofstream(device / "state") << state << '\n';
    }

    void addIio(std::string_view dirName, std::string_view name) const {
      const auto dir = root / "iio" / dirName;
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "name") << name << '\n';
    }

    void writeIio(std::string_view dirName, std::string_view fileName, std::string_view value) const {
      std::ofstream(root / "iio" / dirName / fileName) << value << '\n';
    }

    [[nodiscard]] std::filesystem::path slateParamPath() const {
      return root / "params" / "tablet_mode_in_slate_state";
    }

    void writeSlateParam(std::string_view value) const {
      std::ofstream(slateParamPath()) << value << '\n';
    }

    void addTypeCoverKeyboard(std::string_view eventName = "event15") const {
      const auto dir = root / "input" / eventName / "device";
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "name") << "Microsoft Surface 045E:09B0 Keyboard\n";
    }

    void addModule(std::string_view name) const {
      std::filesystem::create_directories(root / "module" / name);
    }

    std::filesystem::path root;
  };

} // namespace

int main() {
  using noctalia::system::surface::classifyOrientation;
  using noctalia::system::surface::classifyScreenRotation;
  using noctalia::system::surface::DeviceOrientation;
  using noctalia::system::surface::ScreenRotation;
  using noctalia::system::surface::readAccel;
  using noctalia::system::surface::readAlsLux;
  using noctalia::system::surface::readTabletModeInSlateState;
  using noctalia::system::surface::readTabletModeState;
  using noctalia::system::surface::readTypeCoverAttached;
  using noctalia::system::surface::writeTabletModeInSlateState;

  TempTree empty;
  assert(!readTabletModeState(empty.root / "aggregator").has_value());
  assert(!readAlsLux(empty.root / "iio").has_value());
  assert(!readAccel(empty.root / "iio").has_value());
  assert(!readTypeCoverAttached(empty.root / "input", empty.root / "module").has_value());

  TempTree tree;
  tree.addTabletSwitch("laptop");
  auto mode = readTabletModeState(tree.root / "aggregator");
  assert(mode.has_value());
  assert(*mode == "laptop");

  tree.addModule("surface_hid");
  auto cover = readTypeCoverAttached(tree.root / "input", tree.root / "module");
  assert(cover.has_value());
  assert(!*cover);
  tree.addTypeCoverKeyboard();
  cover = readTypeCoverAttached(tree.root / "input", tree.root / "module");
  assert(cover.has_value());
  assert(*cover);

  tree.addIio("iio:device4", "als");
  tree.writeIio("iio:device4", "in_illuminance_raw", "250");
  tree.writeIio("iio:device4", "in_illuminance_scale", "0.01");
  auto lux = readAlsLux(tree.root / "iio");
  assert(lux.has_value());
  assert(std::abs(*lux - 2.5) < 0.001);

  // Stuck-at-zero ALS still samples (HID reports 0 between frames); callers hold last-good.
  TempTree zeroAls;
  zeroAls.addIio("iio:device4", "als");
  zeroAls.writeIio("iio:device4", "in_illuminance_raw", "0");
  zeroAls.writeIio("iio:device4", "in_illuminance_scale", "0.01");
  zeroAls.writeIio("iio:device4", "in_intensity_both_raw", "0");
  zeroAls.writeIio("iio:device4", "in_intensity_scale", "0.01");
  auto luxZero = readAlsLux(zeroAls.root / "iio");
  assert(luxZero.has_value());
  assert(*luxZero == 0.0);
  auto alsZero = noctalia::system::surface::readAls(zeroAls.root / "iio");
  assert(alsZero.devicePresent);
  assert(alsZero.reporting);

  tree.addIio("iio:device1", "accel_3d");
  tree.writeIio("iio:device1", "in_accel_x_raw", "0");
  tree.writeIio("iio:device1", "in_accel_y_raw", "-1000");
  tree.writeIio("iio:device1", "in_accel_z_raw", "0");
  tree.writeIio("iio:device1", "in_accel_scale", "0.00980665");
  auto accel = readAccel(tree.root / "iio");
  assert(accel.has_value());
  assert(classifyOrientation(*accel) == DeviceOrientation::Landscape);

  tree.writeSlateParam("Y");
  auto slate = readTabletModeInSlateState(tree.slateParamPath());
  assert(slate.has_value());
  assert(*slate);
  assert(noctalia::system::surface::tabletModeInSlateStateWritable(tree.slateParamPath()));
  // Same value is a no-op success.
  assert(writeTabletModeInSlateState(true, tree.slateParamPath()));
  assert(writeTabletModeInSlateState(false, tree.slateParamPath()));
  slate = readTabletModeInSlateState(tree.slateParamPath());
  assert(slate.has_value());
  assert(!*slate);

  assert(classifyOrientation({.x = 0.0, .y = 0.0, .z = -9.8}) == DeviceOrientation::Flat);
  assert(classifyOrientation({.x = 9.8, .y = 0.0, .z = 0.0}) == DeviceOrientation::Portrait);
  assert(classifyOrientation({.x = 0.0, .y = 0.0, .z = 0.1}) == DeviceOrientation::Flat);
  assert(classifyScreenRotation({.x = 0.0, .y = -9.8, .z = 0.0}) == ScreenRotation::Normal);
  assert(classifyScreenRotation({.x = 0.0, .y = 9.8, .z = 0.0}) == ScreenRotation::Rotate180);

  return 0;
}
