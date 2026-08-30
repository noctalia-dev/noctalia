#include "system/surface_thermal_reader.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

  class TempTree {
  public:
    TempTree() {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      root = std::filesystem::temp_directory_path() / ("noctalia-surface-thermal-" + std::to_string(stamp));
      std::filesystem::create_directories(root);
    }

    ~TempTree() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }

    void addHwmon(std::string_view dirName, std::string_view name) const {
      const auto dir = root / dirName;
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "name") << name << '\n';
    }

    void write(std::string_view dirName, std::string_view fileName, std::string_view value) const {
      std::ofstream(root / dirName / fileName) << value << '\n';
    }

    std::filesystem::path root;
  };

} // namespace

int main() {
  using noctalia::system::surface::readThermal;

  TempTree empty;
  auto snap = readThermal(empty.root);
  assert(!snap.fan.has_value());
  assert(snap.temperatures.empty());

  TempTree tree;
  tree.addHwmon("hwmon2", "surface_fan");
  tree.write("hwmon2", "fan1_input", "2750");
  tree.addHwmon("hwmon4", "surface_thermal");
  tree.write("hwmon4", "temp1_input", "42100");
  tree.write("hwmon4", "temp1_label", "Skin");
  tree.write("hwmon4", "temp2_input", "51000");
  tree.write("hwmon4", "temp2_label", "SOC");

  snap = readThermal(tree.root);
  assert(snap.fan.has_value());
  assert(snap.fan->rpm == 2750U);
  assert(snap.temperatures.size() == 2);
  assert(snap.temperatures[0].label == "Skin" || snap.temperatures[1].label == "Skin");
  bool foundSkin = false;
  for (const auto& t : snap.temperatures) {
    if (t.label == "Skin") {
      foundSkin = true;
      assert(std::abs(t.celsius - 42.1) < 0.01);
    }
  }
  assert(foundSkin);

  return 0;
}
