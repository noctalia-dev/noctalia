// The validator and the settings GUI must agree on which keys a plugin widget accepts.
// Generic bar-widget settings (scale, color, anchor, capsule_*, ...) are applied to plugin
// widgets at runtime and written by the GUI, so the validator schema has to list them too,
// or `noctalia config validate` warns "unknown setting" about its own output.
#include "scripting/plugin_registry.h"
#include "shell/settings/widget_settings_registry.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-widget-schema-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* result = ::mkdtemp(buffer.data());
    return result != nullptr ? std::filesystem::path(result) : std::filesystem::path{};
  }

  bool writeManifest(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << text;
    return out.good();
  }

  bool hasKey(const noctalia::config::schema::WidgetSettingSchema& schema, std::string_view key) {
    return std::ranges::any_of(schema, [&](const noctalia::config::schema::WidgetSettingField& field) {
      return field.key == key;
    });
  }

} // namespace

int main() {
  const auto root = makeTempDir();
  TEST_CHECK(!root.empty());

  const bool written = writeManifest(
      root / "monitor/plugin.toml",
      "id = \"me/monitor\"\n"
      "name = \"Monitor\"\n"
      "version = \"1.0.0\"\n"
      "plugin_api = 3\n"
      "[[widget]]\n"
      "id = \"summary\"\n"
      "entry = \"summary.luau\"\n"
      "[[widget.setting]]\n"
      "key = \"metric\"\n"
      "type = \"string\"\n"
      "label_key = \"settings.metric.label\"\n"
  );
  scripting::PluginRegistry registry;
  registry.setSources({root});
  registry.ensureScanned();

  const auto schema = settings::widgetSettingSchema("me/monitor:summary", nullptr, &registry);
  std::filesystem::remove_all(root);

  TEST_CHECK(written);
  // manifest setting should stay in the schema
  TEST_CHECK(hasKey(schema, "metric"));
  // host scroll gate should stay in the schema
  TEST_CHECK(hasKey(schema, "enable_scroll"));
  for (const std::string_view key :
       {"enabled", "anchor", "interactive", "scale", "font_scale", "color", "icon_color", "capsule", "capsule_fill",
        "capsule_padding", "actions"}) {
    // common widget setting missing from the plugin widget schema
    TEST_CHECK(hasKey(schema, key));
  }

  return 0;
}
