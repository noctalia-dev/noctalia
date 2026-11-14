// Locks the settings GUI's "lane is overridden" signal across capsule-group edits: adding a widget
// to a lane and then folding it into one of the lane's groups must both keep the lane reporting as
// overridden, otherwise the lane loses its [Override] badge while settings.toml still changes it.

#include "config/config_service.h"
#include "config/config_types.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  std::filesystem::path g_root;

  void removeRoot() {
    std::error_code ec;
    std::filesystem::remove_all(g_root, ec);
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-lane-override-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  // TEST_CHECK exits on failure, so the temp tree is removed at exit.
  g_root = root;
  std::atexit(removeRoot);
  writeFile(root / "config" / "noctalia" / "config.toml", R"(
[bar.default]
start = [ "clock", "weather" ]
)");

  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  const std::vector<std::string> lanePath{"bar", "default", "start"};
  const std::vector<std::string> groupPath{"bar", "default", "capsule_group"};

  {
    ConfigService config;

    // Add a widget to the lane: the lane is now overridden.
    TEST_CHECK(config.setOverride(lanePath, std::vector<std::string>{"clock", "weather", "volume"}));
    TEST_CHECK(config.hasOverride(lanePath));
    TEST_CHECK(config.hasEffectiveOverride(lanePath));

    // Fold the added widget into a capsule group with its neighbour: the lane keeps a group token
    // instead of the two widget names, so it is still nothing like the config file's lane.
    BarCapsuleGroupStyle group;
    group.id = "g1";
    group.members = {"weather", "volume"};
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> grouping;
    grouping.emplace_back(lanePath, std::vector<std::string>{"clock", "group:g1"});
    grouping.emplace_back(groupPath, std::vector<BarCapsuleGroupStyle>{group});
    TEST_CHECK(config.setOverrides(grouping));

    TEST_CHECK(config.hasOverride(lanePath));
    TEST_CHECK(config.hasEffectiveOverride(lanePath));
    TEST_CHECK(config.hasEffectiveOverride(groupPath));

    const auto& bars = config.config().bars;
    TEST_CHECK(bars.size() == 1);
    TEST_CHECK(bars[0].startWidgets == std::vector<std::string>{"clock", "group:g1"});
  }

  // Same edit, but the group already exists in the config file: moving the added widget into it
  // returns the lane list to its file value while the group's membership stays overridden.
  {
    writeFile(root / "config" / "noctalia" / "config.toml", R"(
[bar.default]
start = [ "clock", "group:g1" ]

[[bar.default.capsule_group]]
id = "g1"
members = [ "network", "bluetooth" ]
)");
    std::filesystem::remove(root / "state" / "noctalia" / "settings.toml");
    ConfigService config;

    TEST_CHECK(config.setOverride(lanePath, std::vector<std::string>{"clock", "group:g1", "volume"}));
    TEST_CHECK(config.hasEffectiveOverride(lanePath));
    TEST_CHECK(config.hasEffectiveBarLaneOverride(lanePath));

    BarCapsuleGroupStyle group;
    group.id = "g1";
    group.members = {"network", "bluetooth", "volume"};
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> grouping;
    grouping.emplace_back(lanePath, std::vector<std::string>{"clock", "group:g1"});
    grouping.emplace_back(groupPath, std::vector<BarCapsuleGroupStyle>{group});
    TEST_CHECK(config.setOverrides(grouping));

    // The lane list is back to its file value, so only the group array still overrides the lane's
    // content. The lane must keep reporting as overridden.
    TEST_CHECK(!config.hasEffectiveOverride(lanePath));
    TEST_CHECK(config.hasEffectiveOverride(groupPath));
    TEST_CHECK(config.hasEffectiveBarLaneOverride(lanePath));

    // A lane whose groups and list both match the file is not overridden.
    TEST_CHECK(!config.hasEffectiveBarLaneOverride({"bar", "default", "end"}));
  }

  // Resetting a lane reverts its list and the groups it holds, leaving another lane's group edit
  // alone even though both live in the same scope-wide capsule_group array.
  {
    writeFile(root / "config" / "noctalia" / "config.toml", R"(
[bar.default]
start = [ "clock", "group:g1" ]
end = [ "group:g2" ]

[[bar.default.capsule_group]]
id = "g1"
members = [ "network", "bluetooth" ]

[[bar.default.capsule_group]]
id = "g2"
members = [ "battery", "clock" ]
)");
    std::filesystem::remove(root / "state" / "noctalia" / "settings.toml");
    ConfigService config;

    BarCapsuleGroupStyle first;
    first.id = "g1";
    first.members = {"network", "bluetooth", "volume"};
    BarCapsuleGroupStyle second;
    second.id = "g2";
    second.members = {"battery", "clock", "cpu"};
    // g3 exists only because the overridden start lane references it.
    BarCapsuleGroupStyle third;
    third.id = "g3";
    third.members = {"cpu", "memory"};
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> edits;
    edits.emplace_back(lanePath, std::vector<std::string>{"clock", "group:g1", "group:g3"});
    edits.emplace_back(groupPath, std::vector<BarCapsuleGroupStyle>{first, second, third});
    TEST_CHECK(config.setOverrides(edits));
    TEST_CHECK(config.hasEffectiveBarLaneOverride(lanePath));
    TEST_CHECK(config.hasEffectiveBarLaneOverride({"bar", "default", "end"}));

    bool changed = false;
    TEST_CHECK(config.resetBarLaneOverride(lanePath, &changed) && changed);
    TEST_CHECK(!config.hasEffectiveOverride(lanePath));
    TEST_CHECK(!config.hasEffectiveBarLaneOverride(lanePath));
    TEST_CHECK(config.hasEffectiveBarLaneOverride({"bar", "default", "end"}));

    const auto& groups = config.config().bars.at(0).widgetCapsuleGroups;
    const auto g1 = std::ranges::find(groups, "g1", &BarCapsuleGroupStyle::id);
    const auto g2 = std::ranges::find(groups, "g2", &BarCapsuleGroupStyle::id);
    TEST_CHECK(g1 != groups.end());
    TEST_CHECK(g1->members == std::vector<std::string>{"network", "bluetooth"});
    TEST_CHECK(g2 != groups.end());
    TEST_CHECK(g2->members == std::vector<std::string>{"battery", "clock", "cpu"});
    TEST_CHECK(std::ranges::find(groups, "g3", &BarCapsuleGroupStyle::id) == groups.end());
  }

  return 0;
}
