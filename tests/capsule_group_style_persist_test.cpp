// Locks that every capsule group style field edited in the settings GUI survives a restart. Group
// overrides are written to settings.toml by a hand-listed serializer, so a field it forgets reads
// back as its default the next time the config loads.

#include "config/color_spec.h"
#include "config/config_service.h"
#include "config/config_types.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "capsule_group_style_persist: FAIL: {}", message);
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
      std::filesystem::temp_directory_path() / ("noctalia-group-style-persist-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "noctalia" / "config.toml", R"(
[bar.default]
start = [ "clock", "weather" ]
)");

  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  // Every style field differs from its default so a dropped key cannot pass by accident.
  BarCapsuleGroupStyle group;
  group.id = "g1";
  group.members = {"clock", "weather"};
  group.enabled = false;
  group.fill = colorSpecFromConfigString("#223344");
  group.borderSpecified = true;
  group.border = colorSpecFromConfigString("primary");
  group.borderWidth = 4.5F;
  group.foreground = colorSpecFromConfigString("#556677");
  group.padding = 12.0F;
  group.radius = 9.0F;
  group.opacity = 0.5F;
  group.accordion = true;
  group.accordionDirection = BarAccordionDirection::Start;
  group.widgetSpacing = 3;

  {
    ConfigService config;
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> edits;
    edits.emplace_back(std::vector<std::string>{"bar", "default", "start"}, std::vector<std::string>{"group:g1"});
    edits.emplace_back(
        std::vector<std::string>{"bar", "default", "capsule_group"}, std::vector<BarCapsuleGroupStyle>{group}
    );
    expect(config.setOverrides(std::move(edits)), "group override writes");
  }

  {
    ConfigService config;
    const auto& bars = config.config().bars;
    expect(bars.size() == 1, "one bar after reload");
    if (bars.size() == 1) {
      const auto& groups = bars[0].widgetCapsuleGroups;
      const auto it = std::ranges::find(groups, "g1", &BarCapsuleGroupStyle::id);
      expect(it != groups.end(), "group present after reload");
      if (it != groups.end()) {
        expect(it->borderWidth == group.borderWidth, "border_width persists");
        expect(*it == group, "every group style field persists");
      }
    }
  }

  std::filesystem::remove_all(root);
  if (g_failures == 0) {
    std::println("capsule_group_style_persist: OK");
  }
  return g_failures == 0 ? 0 : 1;
}
