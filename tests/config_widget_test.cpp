#include "config/toml.h"
#include "config/widget_config.h"
#include "tests/test_check.h"

#include <string>
#include <variant>

namespace {

  void checkStringSetting(const WidgetConfig& widget, const std::string& key, const std::string& expected) {
    const auto it = widget.settings.find(key);
    TEST_CHECK(it != widget.settings.end());
    const auto* actual = std::get_if<std::string>(&it->second);
    TEST_CHECK(actual != nullptr && *actual == expected);
  }

  void checkBoolSetting(const WidgetConfig& widget, const std::string& key, bool expected) {
    const auto it = widget.settings.find(key);
    TEST_CHECK(it != widget.settings.end());
    const auto* actual = std::get_if<bool>(&it->second);
    TEST_CHECK(actual != nullptr && *actual == expected);
  }

  void checkStringMapEntry(
      const WidgetConfig& widget, const std::string& tableKey, const std::string& key, const std::string& expected
  ) {
    const auto tableIt = widget.tables.find(tableKey);
    TEST_CHECK(tableIt != widget.tables.end());
    const auto valueIt = tableIt->second.find(key);
    TEST_CHECK(valueIt != tableIt->second.end() && valueIt->second == expected);
  }

} // namespace

int main() {
  Config base;
  noctalia::config::seedBuiltinWidgets(base);

  const auto parsed = toml::parse("[widget.temp]\nshow_label = false\n");
  const auto* widgetRoot = parsed["widget"].as_table();
  const auto* tempTable = widgetRoot != nullptr ? (*widgetRoot)["temp"].as_table() : nullptr;
  TEST_CHECK(tempTable != nullptr);

  const WidgetConfig temp = noctalia::config::readBarWidgetConfig("temp", *tempTable, base);
  TEST_CHECK(temp.type == "sysmon");
  checkStringSetting(temp, "stat", "cpu_temp");
  checkBoolSetting(temp, "show_label", false);

  const auto customParsed = toml::parse("[widget.my_clock]\nformat = \"{:%H:%M}\"\n");
  const auto* customRoot = customParsed["widget"].as_table();
  const auto* customTable = customRoot != nullptr ? (*customRoot)["my_clock"].as_table() : nullptr;
  TEST_CHECK(customTable != nullptr);

  const WidgetConfig custom = noctalia::config::readBarWidgetConfig("my_clock", *customTable, base);
  // Unknown widget name resolves to its own type.
  TEST_CHECK(custom.type == "my_clock");
  checkStringSetting(custom, "format", "{:%H:%M}");

  const auto layoutParsed = toml::parse(
      "[widget.keyboard_layout]\n"
      "show_label = true\n"
      "[widget.keyboard_layout.custom_labels]\n"
      "\"English (US)\" = \"EN\"\n"
  );
  const auto* layoutRoot = layoutParsed["widget"].as_table();
  const auto* layoutTable = layoutRoot != nullptr ? (*layoutRoot)["keyboard_layout"].as_table() : nullptr;
  TEST_CHECK(layoutTable != nullptr);

  const WidgetConfig layout = noctalia::config::readBarWidgetConfig("keyboard_layout", *layoutTable, base);
  TEST_CHECK(layout.type == "keyboard_layout");
  checkBoolSetting(layout, "hide_when_single_layout", false);
  checkStringMapEntry(layout, "custom_labels", "English (US)", "EN");

  const auto mapValueParsed = toml::parse("[output_glyphs]\n\"eDP-1\" = \"laptop\"\n\"DP-1\" = \"monitor\"\n");
  const auto* mapValueNode = mapValueParsed.get("output_glyphs");
  TEST_CHECK(mapValueNode != nullptr);
  const auto mapValue = noctalia::config::readWidgetSettingValue(*mapValueNode);
  TEST_CHECK(mapValue.has_value());
  const auto* values = std::get_if<WidgetSettingStringMap>(&*mapValue);
  TEST_CHECK(values != nullptr);
  TEST_CHECK(values->size() == 2);
  TEST_CHECK(values->at("eDP-1") == "laptop");
  TEST_CHECK(values->at("DP-1") == "monitor");

  const auto invalidMapValueParsed = toml::parse("[output_glyphs]\n\"eDP-1\" = 1\n");
  const auto* invalidMapValueNode = invalidMapValueParsed.get("output_glyphs");
  TEST_CHECK(invalidMapValueNode != nullptr);
  TEST_CHECK(!noctalia::config::readWidgetSettingValue(*invalidMapValueNode).has_value());

  BarConfig bar;
  bar.widgetCapsuleRadius = 12.0;
  WidgetConfig launcher;
  launcher.settings["capsule"] = true;
  launcher.settings["capsule_radius"] = std::string("auto");
  const WidgetBarCapsuleSpec automaticCapsule = resolveWidgetBarCapsuleSpec(bar, &launcher);
  TEST_CHECK(automaticCapsule.enabled);
  TEST_CHECK(automaticCapsule.radius.has_value() && *automaticCapsule.radius == 12.0F);

  launcher.settings["capsule_radius"] = 7.0;
  const WidgetBarCapsuleSpec explicitCapsule = resolveWidgetBarCapsuleSpec(bar, &launcher);
  TEST_CHECK(explicitCapsule.radius.has_value() && *explicitCapsule.radius == 7.0F);

  bar.widgetCapsuleBorderWidth = 3.0F;
  const WidgetBarCapsuleSpec inheritedBorderWidth = resolveWidgetBarCapsuleSpec(bar, &launcher);
  TEST_CHECK(inheritedBorderWidth.borderWidth == 3.0F);

  launcher.settings["capsule_border_width"] = 1.5;
  const WidgetBarCapsuleSpec explicitBorderWidth = resolveWidgetBarCapsuleSpec(bar, &launcher);
  TEST_CHECK(explicitBorderWidth.borderWidth == 1.5F);

  launcher.settings["capsule_border_width"] = 99.0;
  const WidgetBarCapsuleSpec clampedBorderWidth = resolveWidgetBarCapsuleSpec(bar, &launcher);
  TEST_CHECK(clampedBorderWidth.borderWidth == 8.0F);

  BarCapsuleGroupStyle group;
  group.id = "g1";
  group.borderSpecified = true;
  group.border = colorSpecFromRole(ColorRole::OutlineVariant);
  group.borderWidth = 2.5F;
  const WidgetBarCapsuleSpec groupSpec = capsuleSpecFromGroup(bar, group);
  TEST_CHECK(groupSpec.borderWidth == 2.5F);

  return 0;
}
