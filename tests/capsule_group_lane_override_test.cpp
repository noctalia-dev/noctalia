// Locks the settings GUI's "lane is overridden" signal across capsule-group edits: adding a widget
// to a lane and then folding it into one of the lane's groups must both keep the lane reporting as
// overridden, otherwise the lane loses its [Override] badge while settings.toml still changes it.

#include "config/config_service.h"
#include "config/config_types.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "capsule_group_lane_override: FAIL: {}", message);
      ++g_failures;
    }
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
    expect(config.setOverride(lanePath, std::vector<std::string>{"clock", "weather", "volume"}), "add widget writes");
    expect(config.hasOverride(lanePath), "lane override stored after add");
    expect(config.hasEffectiveOverride(lanePath), "lane override effective after add");

    // Fold the added widget into a capsule group with its neighbour: the lane keeps a group token
    // instead of the two widget names, so it is still nothing like the config file's lane.
    BarCapsuleGroupStyle group;
    group.id = "g1";
    group.members = {"weather", "volume"};
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> grouping;
    grouping.emplace_back(lanePath, std::vector<std::string>{"clock", "group:g1"});
    grouping.emplace_back(groupPath, std::vector<BarCapsuleGroupStyle>{group});
    expect(config.setOverrides(std::move(grouping)), "grouping writes");

    expect(config.hasOverride(lanePath), "lane override stored after grouping");
    expect(config.hasEffectiveOverride(lanePath), "lane override effective after grouping");
    expect(config.hasEffectiveOverride(groupPath), "group override effective after grouping");

    const auto& bars = config.config().bars;
    expect(
        bars.size() == 1 && bars[0].startWidgets == std::vector<std::string>{"clock", "group:g1"},
        "lane resolves to the grouped token"
    );
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

    expect(
        config.setOverride(lanePath, std::vector<std::string>{"clock", "group:g1", "volume"}),
        "add widget writes (file group)"
    );
    expect(config.hasEffectiveOverride(lanePath), "lane override effective after add (file group)");
    expect(config.hasEffectiveBarLaneOverride(lanePath), "lane content overridden after add (file group)");

    BarCapsuleGroupStyle group;
    group.id = "g1";
    group.members = {"network", "bluetooth", "volume"};
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> grouping;
    grouping.emplace_back(lanePath, std::vector<std::string>{"clock", "group:g1"});
    grouping.emplace_back(groupPath, std::vector<BarCapsuleGroupStyle>{group});
    expect(config.setOverrides(std::move(grouping)), "grouping writes (file group)");

    // The lane list is back to its file value, so only the group array still overrides the lane's
    // content. The lane must keep reporting as overridden.
    expect(!config.hasEffectiveOverride(lanePath), "lane list override dropped once it matches the file");
    expect(config.hasEffectiveOverride(groupPath), "group override effective after grouping (file group)");
    expect(config.hasEffectiveBarLaneOverride(lanePath), "lane content overridden through its group");

    // A lane whose groups and list both match the file is not overridden.
    expect(!config.hasEffectiveBarLaneOverride({"bar", "default", "end"}), "untouched lane is not overridden");
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
    expect(config.setOverrides(std::move(edits)), "lane and groups overridden");
    expect(config.hasEffectiveBarLaneOverride(lanePath), "start lane overridden through its groups");
    expect(config.hasEffectiveBarLaneOverride({"bar", "default", "end"}), "end lane overridden through g2");

    bool changed = false;
    expect(config.resetBarLaneOverride(lanePath, &changed) && changed, "resetting start writes");
    expect(!config.hasEffectiveOverride(lanePath), "start lane list clean after reset");
    expect(!config.hasEffectiveBarLaneOverride(lanePath), "start lane content clean after reset");
    expect(config.hasEffectiveBarLaneOverride({"bar", "default", "end"}), "end lane keeps its group override");

    const auto& groups = config.config().bars.at(0).widgetCapsuleGroups;
    const auto g1 = std::ranges::find(groups, "g1", &BarCapsuleGroupStyle::id);
    const auto g2 = std::ranges::find(groups, "g2", &BarCapsuleGroupStyle::id);
    expect(
        g1 != groups.end() && g1->members == std::vector<std::string>{"network", "bluetooth"},
        "g1 restored to its config file members"
    );
    expect(
        g2 != groups.end() && g2->members == std::vector<std::string>{"battery", "clock", "cpu"},
        "g2 keeps its overridden members"
    );
    expect(
        std::ranges::find(groups, "g3", &BarCapsuleGroupStyle::id) == groups.end(),
        "the lane's GUI-created group is gone after reset"
    );
  }

  std::filesystem::remove_all(root);
  if (g_failures == 0) {
    std::println("capsule_group_lane_override: OK");
  }
  return g_failures == 0 ? 0 : 1;
}
