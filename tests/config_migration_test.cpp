#include "config/config_migrations.h"
#include "config/config_service.h"
#include "config/toml.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace {

  int g_syntheticMigrationApplications = 0;

  void countSyntheticMigration(toml::table&, const toml::table&, noctalia::config::schema::Diagnostics&) {
    ++g_syntheticMigrationApplications;
  }

  bool hasIssuePath(const noctalia::config::LegacyConfigIssues& issues, std::string_view path) {
    return std::ranges::any_of(issues, [path](const auto& issue) { return issue.path == path; });
  }

  void checkNegativeRadiusMigration() {
    toml::table root = toml::parse(R"(
[bar.main]
radius = -12
radius_top_left = -20
radius_top_right = 8

[bar.main.monitor.dp1]
match = "DP-1"
radius = -16

[dock]
radius = -7
)");

    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(root, issues);

    TEST_CHECK(root["bar"]["main"]["radius"].value<std::int64_t>() == 12);
    TEST_CHECK(root["bar"]["main"]["radius_top_left"].value<std::int64_t>() == 20);
    TEST_CHECK(root["bar"]["main"]["radius_top_right"].value<std::int64_t>() == 8);
    TEST_CHECK(root["bar"]["main"]["concave_edge_corners"].value<bool>() == true);
    TEST_CHECK(root["bar"]["main"]["monitor"]["dp1"]["radius"].value<std::int64_t>() == 16);
    TEST_CHECK(root["bar"]["main"]["monitor"]["dp1"]["concave_edge_corners"].value<bool>() == true);
    TEST_CHECK(root["dock"]["radius"].value<std::int64_t>() == -7);
    TEST_CHECK(issues.size() == 3);
    TEST_CHECK(hasIssuePath(issues, "bar.main.radius"));
    TEST_CHECK(hasIssuePath(issues, "bar.main.radius_top_left"));
    TEST_CHECK(hasIssuePath(issues, "bar.main.monitor.dp1.radius"));

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(root, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkExtremeNegativeRadius() {
    toml::table root;
    toml::table bar;
    bar.insert_or_assign("radius", std::numeric_limits<std::int64_t>::min());
    toml::table bars;
    bars.insert_or_assign("main", std::move(bar));
    root.insert_or_assign("bar", std::move(bars));

    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(root, issues);
    TEST_CHECK(root["bar"]["main"]["radius"].value<std::int64_t>() == 500);
  }

  void checkCustomScheduleMigration() {
    toml::table legacy = toml::parse(R"(
[location]
sunset = "20:30"
sunrise = "07:30"
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(legacy, issues);
    TEST_CHECK(legacy["location"]["custom_schedule"].value<bool>() == true);
    TEST_CHECK(issues.size() == 1);
    TEST_CHECK(issues.front().path == "location.sunset");

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(legacy, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());

    // Coordinates won under the old rules, so these configs must keep using them.
    for (const std::string_view source :
         {"auto_locate = true", "address = \"Toronto, ON\"", "latitude = 52.52\nlongitude = 13.405"}) {
      toml::table coords =
          toml::parse(std::format("[location]\nsunset = \"20:30\"\nsunrise = \"07:30\"\n{}\n", source));
      noctalia::config::LegacyConfigIssues coordIssues;
      noctalia::config::normalizeLegacyConfig(coords, coordIssues);
      TEST_CHECK(!coords["location"]["custom_schedule"].value<bool>().has_value());
      TEST_CHECK(coordIssues.empty());
    }

    toml::table explicitOff = toml::parse(R"(
[location]
custom_schedule = false
sunset = "20:30"
sunrise = "07:30"
)");
    noctalia::config::LegacyConfigIssues offIssues;
    noctalia::config::normalizeLegacyConfig(explicitOff, offIssues);
    TEST_CHECK(explicitOff["location"]["custom_schedule"].value<bool>() == false);

    // Times written by Settings mean what they say: without custom_schedule the schedule stays off.
    const toml::table settingsTimes = toml::parse(R"(
config_version = 17
[location]
sunset = "20:30"
sunrise = "07:30"
)");
    toml::table mergedSettings = settingsTimes;
    noctalia::config::LegacyConfigIssues settingsIssues;
    noctalia::config::normalizeLegacyConfig(mergedSettings, settingsIssues, &settingsTimes);
    TEST_CHECK(!mergedSettings["location"]["custom_schedule"].value<bool>().has_value());
    TEST_CHECK(settingsIssues.empty());

    // Older settings files relied on that implicit schedule; the upgrade keeps it unless config.toml
    // supplies coordinates, which used to win.
    toml::table olderSettings =
        toml::parse("config_version = 16\n[location]\nsunset = \"20:30\"\nsunrise = \"07:30\"\n");
    noctalia::config::schema::Diagnostics upgradeDiag;
    (void)noctalia::config::applyPendingConfigMigrations(olderSettings, {}, 16, upgradeDiag);
    TEST_CHECK(olderSettings["location"]["custom_schedule"].value<bool>() == true);

    toml::table coordinateSettings =
        toml::parse("config_version = 16\n[location]\nsunset = \"20:30\"\nsunrise = \"07:30\"\n");
    const toml::table coordinateBase = toml::parse("[location]\nlatitude = 52.52\nlongitude = 13.405\n");
    (void)noctalia::config::applyPendingConfigMigrations(coordinateSettings, coordinateBase, 16, upgradeDiag);
    TEST_CHECK(!coordinateSettings["location"]["custom_schedule"].value<bool>().has_value());
  }

  void checkWidgetActionsMigration() {
    // The setting is now the built-in `middle` binding, so an enabled config just drops the key.
    toml::table enabled = toml::parse(R"(
[shell]
middle_click_opens_widget_settings = true
)");
    noctalia::config::LegacyConfigIssues enabledIssues;
    noctalia::config::normalizeLegacyConfig(enabled, enabledIssues);
    TEST_CHECK(!enabled["shell"]["middle_click_opens_widget_settings"].value<bool>().has_value());
    TEST_CHECK(enabled["bar"].as_table() == nullptr);
    TEST_CHECK(enabledIssues.size() == 1 && enabledIssues.front().path == "shell.middle_click_opens_widget_settings");

    // A disabled config has to keep behaving the same, which now means unbinding the gesture.
    toml::table disabled = toml::parse(R"(
[shell]
middle_click_opens_widget_settings = false

[bar.default]
position = "top"

[bar.secondary]
position = "bottom"
)");
    noctalia::config::LegacyConfigIssues disabledIssues;
    noctalia::config::normalizeLegacyConfig(disabled, disabledIssues);
    TEST_CHECK(!disabled["shell"]["middle_click_opens_widget_settings"].value<bool>().has_value());
    for (const std::string_view barName : {"default", "secondary"}) {
      TEST_CHECK(
          disabled["bar"][barName]["actions"]["middle"].value<std::string>() == std::optional<std::string>{"none"}
      );
    }
    TEST_CHECK(disabledIssues.size() == 1 && disabledIssues.front().path == "shell.middle_click_opens_widget_settings");

    // With no [bar] table the built-in default bar is still in play, so it must be seeded.
    toml::table noBars = toml::parse(R"(
[shell]
middle_click_opens_widget_settings = false
)");
    noctalia::config::LegacyConfigIssues noBarIssues;
    noctalia::config::normalizeLegacyConfig(noBars, noBarIssues);
    TEST_CHECK(
        noBars["bar"]["default"]["actions"]["middle"].value<std::string>() == std::optional<std::string>{"none"}
    );

    // An existing explicit binding wins over the migration.
    toml::table explicitBinding = toml::parse(R"(
[shell]
middle_click_opens_widget_settings = false

[bar.default.actions]
middle = "media toggle"
)");
    noctalia::config::LegacyConfigIssues explicitIssues;
    noctalia::config::normalizeLegacyConfig(explicitBinding, explicitIssues);
    TEST_CHECK(
        explicitBinding["bar"]["default"]["actions"]["middle"].value<std::string>()
        == std::optional<std::string>{"media toggle"}
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(disabled, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkWidgetGestureSettingsMigration() {
    toml::table config = toml::parse(R"(
[widget.workspaces]
enable_scroll = false

[widget.taskbar]
enable_scroll = true

[widget.my_profile]
type = "power_profile"
enable_scroll = false

[widget.keyboard_layout]
cycle_command = "hyprctl switchxkblayout all next"

[widget.plugin_thing]
type = "someone/plugin:entry"
enable_scroll = false
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    // Disabled scroll becomes an explicit unbind on both scroll gestures.
    for (const std::string_view gesture : {"scroll_up", "scroll_down"}) {
      TEST_CHECK(
          config["widget"]["workspaces"]["actions"][gesture].value<std::string>() == std::optional<std::string>{"none"}
      );
    }
    TEST_CHECK(!config["widget"]["workspaces"]["enable_scroll"].value<bool>().has_value());

    // Enabled scroll is the default, so the key just goes away.
    TEST_CHECK(!config["widget"]["taskbar"]["enable_scroll"].value<bool>().has_value());
    TEST_CHECK(config["widget"]["taskbar"]["actions"].as_table() == nullptr);

    // The type comes from `type` when the widget is named something else.
    TEST_CHECK(
        config["widget"]["my_profile"]["actions"]["scroll_up"].value<std::string>()
        == std::optional<std::string>{"none"}
    );

    TEST_CHECK(
        config["widget"]["keyboard_layout"]["actions"]["left"].value<std::string>()
        == std::optional<std::string>{"exec hyprctl switchxkblayout all next"}
    );
    TEST_CHECK(!config["widget"]["keyboard_layout"]["cycle_command"].value<std::string>().has_value());

    // Plugin widgets still gate their own onScroll handler, so the key survives for them.
    TEST_CHECK(config["widget"]["plugin_thing"]["enable_scroll"].value<bool>() == std::optional<bool>{false});

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkRemainingWidgetGesturesMigration() {
    toml::table config = toml::parse(R"(
[widget.media]
enable_scroll = false

[widget.volume]
scroll_step = 10

[widget.mic]
type = "volume"
device = "input"
scroll_step = 2

[widget.brightness]
enable_scroll = true
scroll_step = 5

[widget.screenshot]
primary_click = "fullscreen"

[widget.shot2]
type = "screenshot"
primary_click = "region"
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    for (const std::string_view gesture : {"scroll_up", "scroll_down"}) {
      TEST_CHECK(
          config["widget"]["media"]["actions"][gesture].value<std::string>() == std::optional<std::string>{"none"}
      );
    }

    // A non-default step survives as the verb's argument, picking the verbs the device implies.
    TEST_CHECK(
        config["widget"]["volume"]["actions"]["scroll_up"].value<std::string>()
        == std::optional<std::string>{"volume-up 10%"}
    );
    TEST_CHECK(
        config["widget"]["mic"]["actions"]["scroll_down"].value<std::string>()
        == std::optional<std::string>{"mic-volume-down 2%"}
    );

    // The default step matches the verbs' own default, so it leaves nothing behind.
    TEST_CHECK(!config["widget"]["brightness"]["scroll_step"].value<std::int64_t>().has_value());
    TEST_CHECK(config["widget"]["brightness"]["actions"].as_table() == nullptr);

    TEST_CHECK(
        config["widget"]["screenshot"]["actions"]["left"].value<std::string>()
        == std::optional<std::string>{"screenshot-fullscreen"}
    );
    // `region` is already the declared default, so it needs no binding.
    TEST_CHECK(config["widget"]["shot2"]["actions"].as_table() == nullptr);
    TEST_CHECK(!config["widget"]["screenshot"]["primary_click"].value<std::string>().has_value());

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkCustomButtonCommandsMigration() {
    toml::table config = toml::parse(R"(
[widget.custom_button]
command = "notify-send 'hello world'"
right_command = "playerctl next"
scroll_up_command = "brightnessctl set +5%"

[widget.dead_scroll]
type = "custom_button"
enable_scroll = false
scroll_up_command = "echo up"

[widget.explicit]
type = "custom_button"
command = "echo old"

[widget.explicit.actions]
left = "media toggle"
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    // Quoting survives verbatim: the widget ran these through the same call `exec` uses.
    TEST_CHECK(
        config["widget"]["custom_button"]["actions"]["left"].value<std::string>()
        == std::optional<std::string>{"exec notify-send 'hello world'"}
    );
    TEST_CHECK(
        config["widget"]["custom_button"]["actions"]["right"].value<std::string>()
        == std::optional<std::string>{"exec playerctl next"}
    );
    TEST_CHECK(
        config["widget"]["custom_button"]["actions"]["scroll_up"].value<std::string>()
        == std::optional<std::string>{"exec brightnessctl set +5%"}
    );
    for (const std::string_view key : {"command", "right_command", "scroll_up_command"}) {
      TEST_CHECK(!config["widget"]["custom_button"][key].value<std::string>().has_value());
    }

    // enable_scroll = false used to beat the scroll commands, and still does.
    TEST_CHECK(
        config["widget"]["dead_scroll"]["actions"]["scroll_up"].value<std::string>()
        == std::optional<std::string>{"none"}
    );

    // An explicit binding always wins over a migrated key.
    TEST_CHECK(
        config["widget"]["explicit"]["actions"]["left"].value<std::string>()
        == std::optional<std::string>{"media toggle"}
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkDeadZoneActionsMigration() {
    toml::table config = toml::parse(R"(
[bar.default.dead_zone]
command = "notify-send left"
right_command = "notify-send right"

[bar.default.monitor.DP-1.dead_zone]
scroll_up_command = "notify-send up"

[bar.other.dead_zone]
middle_command = ""
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    TEST_CHECK(
        config["bar"]["default"]["dead_zone"]["actions"]["left"].value<std::string>()
        == std::optional<std::string>{"exec notify-send left"}
    );
    TEST_CHECK(
        config["bar"]["default"]["dead_zone"]["actions"]["right"].value<std::string>()
        == std::optional<std::string>{"exec notify-send right"}
    );
    TEST_CHECK(!config["bar"]["default"]["dead_zone"]["command"].value<std::string>().has_value());

    // Monitor overrides carry their own dead zone table.
    TEST_CHECK(
        config["bar"]["default"]["monitor"]["DP-1"]["dead_zone"]["actions"]["scroll_up"].value<std::string>()
        == std::optional<std::string>{"exec notify-send up"}
    );

    // An empty command bound nothing before and binds nothing now.
    TEST_CHECK(config["bar"]["other"]["dead_zone"]["actions"].as_table() == nullptr);

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkSysmonPresentationMigration() {
    toml::table config = toml::parse(R"(
[widget.gauge]
type = "sysmon"
display = "gauge"
show_label = false
show_icon = false
label_show_units = false
label_min_width = 42

[widget.text]
type = "sysmon"
display = "text"
show_label = false

[widget.none]
type = "sysmon"
display = "none"
show_label = true

[widget.sysmon]
display = "graph"

[widget.canonical]
type = "sysmon"
display = "text"
visualization = "graph"
show_label = true
show_value = false
show_icon = false
show_glyph = true

[widget.clock]
display = "text"
show_label = false
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    TEST_CHECK(config["widget"]["gauge"]["visualization"].value<std::string>() == std::optional<std::string>{"gauge"});
    TEST_CHECK(config["widget"]["gauge"]["show_value"].value<bool>() == std::optional<bool>{false});
    TEST_CHECK(config["widget"]["gauge"]["show_glyph"].value<bool>() == std::optional<bool>{false});
    TEST_CHECK(
        config["widget"]["gauge"]["label_show_units"].value<bool>() == std::optional<bool>{false}
        && config["widget"]["gauge"]["label_min_width"].value<std::int64_t>() == std::optional<std::int64_t>{42}
    );
    TEST_CHECK(
        config["widget"]["text"]["visualization"].value<std::string>() == std::optional<std::string>{"none"}
        && config["widget"]["text"]["show_value"].value<bool>() == std::optional<bool>{true}
    );
    TEST_CHECK(
        config["widget"]["none"]["visualization"].value<std::string>() == std::optional<std::string>{"none"}
        && config["widget"]["none"]["show_value"].value<bool>() == std::optional<bool>{false}
    );
    TEST_CHECK(
        config["widget"]["sysmon"]["visualization"].value<std::string>() == std::optional<std::string>{"graph"}
        && config["widget"]["sysmon"]["show_value"].value<bool>() == std::optional<bool>{true}
    );
    TEST_CHECK(
        config["widget"]["canonical"]["visualization"].value<std::string>() == std::optional<std::string>{"graph"}
        && config["widget"]["canonical"]["show_value"].value<bool>() == std::optional<bool>{false}
        && config["widget"]["canonical"]["show_glyph"].value<bool>() == std::optional<bool>{true}
    );
    TEST_CHECK(
        !config["widget"]["canonical"].as_table()->contains("display")
        && !config["widget"]["canonical"].as_table()->contains("show_label")
        && !config["widget"]["canonical"].as_table()->contains("show_icon")
    );
    TEST_CHECK(config["widget"]["clock"]["display"].value<std::string>() == std::optional<std::string>{"text"});
    TEST_CHECK(issues.size() == 11);
    TEST_CHECK(hasIssuePath(issues, "widget.gauge.display"));
    TEST_CHECK(hasIssuePath(issues, "widget.gauge.show_label"));
    TEST_CHECK(hasIssuePath(issues, "widget.gauge.show_icon"));

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkWorkspacesDisplayMigration() {
    toml::table config = toml::parse(R"(
[widget.none]
type = "workspaces"
display = "none"
labels_only_when_occupied = true
max_label_chars = 8

[widget.named]
type = "workspaces"
display = "name"

[widget.numbered]
type = "workspaces"
display = "id"

[widget.canonical-labels]
type = "workspaces"
display = "none"
show_labels = true

[widget.canonical-source]
type = "workspaces"
display = "name"
label_source = "id"

[widget.lock_keys]
type = "lock_keys"
display = "short"

[widget.invalid]
type = "workspaces"
display = "icon"

# A bare [widget.workspaces] has no `type`; the widget name is the type.
[widget.workspaces]
display = "id"
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    TEST_CHECK(
        config["widget"]["none"]["show_labels"].value<bool>() == std::optional<bool>{false}
        && !config["widget"]["none"].as_table()->contains("display")
    );
    TEST_CHECK(
        config["widget"]["none"]["labels_only_when_occupied"].value<bool>() == std::optional<bool>{true}
        && config["widget"]["none"]["max_label_chars"].value<std::int64_t>() == std::optional<std::int64_t>{8}
    );
    TEST_CHECK(
        config["widget"]["named"]["label_source"].value<std::string>() == std::optional<std::string>{"name"}
        && !config["widget"]["named"].as_table()->contains("display")
    );
    TEST_CHECK(
        config["widget"]["numbered"]["label_source"].value<std::string>() == std::optional<std::string>{"id"}
        && !config["widget"]["numbered"].as_table()->contains("display")
    );
    TEST_CHECK(
        config["widget"]["canonical-labels"]["show_labels"].value<bool>() == std::optional<bool>{true}
        && !config["widget"]["canonical-labels"].as_table()->contains("display")
    );
    TEST_CHECK(
        config["widget"]["canonical-source"]["label_source"].value<std::string>() == std::optional<std::string>{"id"}
        && !config["widget"]["canonical-source"].as_table()->contains("display")
    );
    TEST_CHECK(config["widget"]["lock_keys"]["display"].value<std::string>() == std::optional<std::string>{"short"});
    TEST_CHECK(config["widget"]["invalid"]["display"].value<std::string>() == std::optional<std::string>{"icon"});
    TEST_CHECK(
        config["widget"]["workspaces"]["label_source"].value<std::string>() == std::optional<std::string>{"id"}
        && !config["widget"]["workspaces"].as_table()->contains("display")
    );
    TEST_CHECK(issues.size() == 6);

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkKeyboardLayoutShowGlyphMigration() {
    toml::table config = toml::parse(R"(
[widget.keyboard_layout]
show_icon = false

[widget.named-layout]
type = "keyboard_layout"
show_icon = true
show_glyph = false

[widget.clock]
show_icon = false
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    TEST_CHECK(
        config["widget"]["keyboard_layout"]["show_glyph"].value<bool>() == std::optional<bool>{false}
        && !config["widget"]["keyboard_layout"].as_table()->contains("show_icon")
    );
    TEST_CHECK(
        config["widget"]["named-layout"]["show_glyph"].value<bool>() == std::optional<bool>{false}
        && !config["widget"]["named-layout"].as_table()->contains("show_icon")
    );
    TEST_CHECK(config["widget"]["clock"]["show_icon"].value<bool>() == std::optional<bool>{false});
    TEST_CHECK(issues.size() == 2);

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkWorkspacesMonitorsMigration() {
    toml::table config = toml::parse(R"(
[widget.workspaces]
show_all_outputs = true

[widget.bar-only]
type = "workspaces"
show_all_outputs = false

[widget.canonical]
type = "workspaces"
show_all_outputs = true
monitors = ["DP-3"]

[widget.taskbar]
show_all_outputs = true
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    const auto* allMonitors = config["widget"]["workspaces"]["monitors"].as_array();
    TEST_CHECK(
        allMonitors != nullptr
        && allMonitors->size() == 1
        && (*allMonitors)[0].value<std::string>() == std::optional<std::string>{"*"}
        && !config["widget"]["workspaces"].as_table()->contains("show_all_outputs")
    );
    TEST_CHECK(
        !config["widget"]["bar-only"].as_table()->contains("monitors")
        && !config["widget"]["bar-only"].as_table()->contains("show_all_outputs")
    );
    const auto* canonical = config["widget"]["canonical"]["monitors"].as_array();
    TEST_CHECK(
        canonical != nullptr
        && canonical->size() == 1
        && (*canonical)[0].value<std::string>() == std::optional<std::string>{"DP-3"}
        && !config["widget"]["canonical"].as_table()->contains("show_all_outputs")
    );
    TEST_CHECK(config["widget"]["taskbar"]["show_all_outputs"].value<bool>() == std::optional<bool>{true});
    TEST_CHECK(issues.size() == 3);

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkKeyboardLayoutCustomLabelsMigration() {
    toml::table config = toml::parse(R"toml(
[shell.keyboard_layout.custom_labels]
German = "Global"

[widget.keyboard_layout]

[widget.keyboard_layout.custom_labels]
"English (US)" = "US"
German = "Legacy"

[widget.named-layout]
type = "keyboard_layout"

[widget.named-layout.custom_labels]
French = "FR"

[widget.clock]
type = "clock"

[widget.clock.custom_labels]
German = "Clock"
)toml");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(config, issues);

    TEST_CHECK(
        config["shell"]["keyboard_layout"]["custom_labels"]["English (US)"].value<std::string>()
            == std::optional<std::string>{"US"}
        && config["shell"]["keyboard_layout"]["custom_labels"]["German"].value<std::string>()
            == std::optional<std::string>{"Global"}
        && config["shell"]["keyboard_layout"]["custom_labels"]["French"].value<std::string>()
            == std::optional<std::string>{"FR"}
    );
    TEST_CHECK(
        !config["widget"]["keyboard_layout"].as_table()->contains("custom_labels")
        && !config["widget"]["named-layout"].as_table()->contains("custom_labels")
    );
    TEST_CHECK(
        config["widget"]["clock"]["custom_labels"]["German"].value<std::string>() == std::optional<std::string>{"Clock"}
    );
    TEST_CHECK(issues.size() == 2);
    TEST_CHECK(std::ranges::any_of(issues, [](const auto& issue) {
      return issue.message.contains("conflicting canonical");
    }));

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkPluginAutoUpdateModeMigration() {
    for (const bool enabled : {true, false}) {
      toml::table config = toml::parse(std::format("[plugins]\nauto_update = {}", enabled));
      noctalia::config::LegacyConfigIssues issues;
      noctalia::config::normalizeLegacyConfig(config, issues);

      const std::string_view expected = enabled ? "all" : "none";
      TEST_CHECK(config["plugins"]["auto_update"].value<std::string_view>() == expected);
      TEST_CHECK(issues.size() == 1 && hasIssuePath(issues, "plugins.auto_update"));
    }

    toml::table sidecar = toml::parse("config_version = 12\n[plugins]\nauto_update = false");
    noctalia::config::schema::Diagnostics diagnostics;
    const int applied = noctalia::config::applyPendingConfigMigrations(sidecar, {}, 12, diagnostics);
    TEST_CHECK(applied == noctalia::config::currentConfigVersion());
    TEST_CHECK(sidecar["plugins"]["auto_update"].value<std::string_view>() == std::optional<std::string_view>{"none"});
  }

  void checkCalendarEventFormatsMigration() {
    toml::table sidecar = toml::parse(R"(
config_version = 13
[control_center.calendar]
event_date_format = "%Y-%m-%d"
event_time_format = "%I:%M %p"
)");
    noctalia::config::schema::Diagnostics diagnostics;
    const int applied = noctalia::config::applyPendingConfigMigrations(sidecar, {}, 13, diagnostics);
    TEST_CHECK(applied == noctalia::config::currentConfigVersion());
    TEST_CHECK(
        sidecar["calendar"]["event_date_format"].value<std::string_view>()
        == std::optional<std::string_view>{"%Y-%m-%d"}
    );
    TEST_CHECK(
        sidecar["calendar"]["event_time_format"].value<std::string_view>()
        == std::optional<std::string_view>{"%I:%M %p"}
    );
    const auto* legacyCalendar = sidecar["control_center"]["calendar"].as_table();
    TEST_CHECK(
        legacyCalendar != nullptr
        && legacyCalendar->get("event_date_format") == nullptr
        && legacyCalendar->get("event_time_format") == nullptr
    );

    toml::table legacyConfig = toml::parse(R"(
[control_center.calendar]
event_date_format = "%d.%m.%Y"
event_time_format = "%H:%M"
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(legacyConfig, issues);
    TEST_CHECK(
        legacyConfig["calendar"]["event_date_format"].value<std::string_view>()
        == std::optional<std::string_view>{"%d.%m.%Y"}
    );
    TEST_CHECK(
        issues.size() == 2
        && hasIssuePath(issues, "control_center.calendar.event_date_format")
        && hasIssuePath(issues, "control_center.calendar.event_time_format")
    );

    toml::table conflict = toml::parse(R"(
config_version = 13
[calendar]
event_date_format = "%d"
[control_center.calendar]
event_date_format = "%A"
event_time_format = "%H:%M"
)");
    noctalia::config::schema::Diagnostics conflictDiagnostics;
    (void)noctalia::config::applyPendingConfigMigrations(conflict, {}, 13, conflictDiagnostics);
    TEST_CHECK(
        conflict["calendar"]["event_date_format"].value<std::string_view>() == std::optional<std::string_view>{"%d"}
    );
    TEST_CHECK(
        conflict["calendar"]["event_time_format"].value<std::string_view>() == std::optional<std::string_view>{"%H:%M"}
    );
  }

  void checkDockMonitorAllowListMigration() {
    toml::table sidecar = toml::parse(R"(
config_version = 14
[dock]
enabled = true
monitors = ["DP-1", "Dell Inc."]
)");
    noctalia::config::schema::Diagnostics diagnostics;
    const int applied = noctalia::config::applyPendingConfigMigrations(sidecar, {}, 14, diagnostics);
    TEST_CHECK(applied == noctalia::config::currentConfigVersion());
    TEST_CHECK(sidecar["dock"]["monitors"].node() == nullptr);
    TEST_CHECK(sidecar["dock"]["enabled"].value<bool>() == false);
    TEST_CHECK(sidecar["dock"]["monitor"]["DP-1"]["enabled"].value<bool>() == true);
    TEST_CHECK(sidecar["dock"]["monitor"]["Dell Inc."]["enabled"].value<bool>() == true);

    toml::table legacyConfig = toml::parse(R"(
[dock]
enabled = true
monitors = ["eDP-1"]
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(legacyConfig, issues);
    TEST_CHECK(
        legacyConfig["dock"]["enabled"].value<bool>() == false
        && legacyConfig["dock"]["monitor"]["eDP-1"]["enabled"].value<bool>() == true
    );
    TEST_CHECK(issues.size() == 1 && hasIssuePath(issues, "dock.monitors"));

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(legacyConfig, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());

    toml::table emptyList = toml::parse("[dock]\nenabled = true\nmonitors = []");
    noctalia::config::LegacyConfigIssues emptyIssues;
    noctalia::config::normalizeLegacyConfig(emptyList, emptyIssues);
    TEST_CHECK(emptyList["dock"]["enabled"].value<bool>() == true);
    TEST_CHECK(emptyList["dock"]["monitors"].node() == nullptr);
    TEST_CHECK(emptyList["dock"]["monitor"].node() == nullptr);

    toml::table disabled = toml::parse("[dock]\nenabled = false\nmonitors = [\"DP-1\"]");
    noctalia::config::LegacyConfigIssues disabledIssues;
    noctalia::config::normalizeLegacyConfig(disabled, disabledIssues);
    TEST_CHECK(disabled["dock"]["enabled"].value<bool>() == false);
    TEST_CHECK(disabled["dock"]["monitor"].node() == nullptr);
  }

  void checkPaletteRoleNamesMigration() {
    toml::table sidecar = toml::parse(R"(
config_version = 15
[bar.main]
capsule_fill = "surface_variant"
capsule_border = " Outline "
[[session.actions]]
variant = "outline"
[desktop_widgets.widget.login]
settings = { background_color = "surface_variant", track_color = "on_surface_variant" }
[theme.templates.custom_colors]
accent = "outline"
)");
    noctalia::config::schema::Diagnostics diagnostics;
    const int applied = noctalia::config::applyPendingConfigMigrations(sidecar, {}, 15, diagnostics);
    TEST_CHECK(applied == noctalia::config::currentConfigVersion());
    TEST_CHECK(
        sidecar["bar"]["main"]["capsule_fill"].value<std::string>() == std::optional<std::string>{"surface_container"}
    );
    TEST_CHECK(
        sidecar["bar"]["main"]["capsule_border"].value<std::string>() == std::optional<std::string>{"outline_variant"}
    );
    TEST_CHECK(
        sidecar["session"]["actions"][0]["variant"].value<std::string>() == std::optional<std::string>{"outline"}
    );
    TEST_CHECK(
        sidecar["desktop_widgets"]["widget"]["login"]["settings"]["background_color"].value<std::string>()
            == std::optional<std::string>{"surface_container"}
        && sidecar["desktop_widgets"]["widget"]["login"]["settings"]["track_color"].value<std::string>()
            == std::optional<std::string>{"on_surface_variant"}
    );
    TEST_CHECK(
        sidecar["theme"]["templates"]["custom_colors"]["accent"].value<std::string>()
        == std::optional<std::string>{"outline"}
    );

    toml::table legacyConfig = toml::parse("[bar.main]\ncapsule_fill = \"surface_variant\"\n");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(legacyConfig, issues);
    TEST_CHECK(issues.size() == 1 && hasIssuePath(issues, "bar.main.capsule_fill"));
    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(legacyConfig, secondPassIssues);
    TEST_CHECK(secondPassIssues.empty());
  }

  void checkAppIconColorizationMigration() {
    for (const bool enabled : {false, true}) {
      toml::table root = toml::parse(
          std::format(
              R"(
[shell]
app_icon_colorize = {}
app_icon_color = "primary"
[widget.tags]
type = "workspaces"
[widget.apps]
type = "taskbar"
[widget.status]
type = "tray"
[widget.title]
type = "active_window"
[widget.clock]
format = "%H:%M"
)",
              enabled
          )
      );
      noctalia::config::LegacyConfigIssues issues;
      noctalia::config::normalizeLegacyConfig(root, issues);
      TEST_CHECK(!root["shell"].as_table()->contains("app_icon_colorize"));
      TEST_CHECK(root["shell"]["app_icon_color"].value<std::string>() == "primary");
      TEST_CHECK(root["dock"]["colorize_icons"].value<bool>() == enabled);
      TEST_CHECK(root["notification"]["colorize_icons"].value<bool>() == enabled);
      TEST_CHECK(root["audio"]["colorize_icons"].value<bool>() == enabled);
      TEST_CHECK(root["shell"]["screen_time_colorize_icons"].value<bool>() == enabled);
      TEST_CHECK(root["shell"]["polkit_colorize_icons"].value<bool>() == enabled);
      TEST_CHECK(root["shell"]["launcher"]["colorize_icons"].value<bool>() == enabled);
      TEST_CHECK(root["shell"]["window_switcher"]["colorize_icons"].value<bool>() == enabled);
      for (const auto* name : {"workspaces", "taskbar", "tray", "active_window", "tags", "apps", "status", "title"}) {
        TEST_CHECK(root["widget"][name]["colorize_icons"].value<bool>() == enabled);
      }
      TEST_CHECK(!root["widget"]["clock"]["colorize_icons"]);
      TEST_CHECK(issues.size() == 1 && hasIssuePath(issues, "shell.app_icon_colorize"));
      const toml::table firstPass = root;
      noctalia::config::LegacyConfigIssues secondPass;
      noctalia::config::normalizeLegacyConfig(root, secondPass);
      TEST_CHECK(root == firstPass && secondPass.empty());
    }

    const toml::table base = toml::parse(R"(
[dock]
colorize_icons = false
[widget.workspaces]
type = "clock"
[widget.custom]
type = "taskbar"
[widget.tray]
colorize_icons = false
)");
    toml::table sidecar = toml::parse(R"(
config_version = 18
[shell]
app_icon_colorize = true
[shell.launcher]
colorize_icons = false
[widget.custom]
icon_scale = 0.8
[widget.other]
type = "active_window"
)");
    noctalia::config::schema::Diagnostics diagnostics;
    const int version = noctalia::config::applyPendingConfigMigrations(sidecar, base, 18, diagnostics);
    TEST_CHECK(version == noctalia::config::currentConfigVersion());
    TEST_CHECK(!sidecar["shell"]["app_icon_colorize"]);
    TEST_CHECK(!sidecar["dock"]["colorize_icons"]);
    TEST_CHECK(sidecar["shell"]["launcher"]["colorize_icons"].value<bool>() == false);
    TEST_CHECK(!sidecar["widget"]["tray"]["colorize_icons"]);
    TEST_CHECK(!sidecar["widget"]["workspaces"]["colorize_icons"]);
    TEST_CHECK(sidecar["widget"]["custom"]["colorize_icons"].value<bool>() == true);
    TEST_CHECK(sidecar["widget"]["other"]["colorize_icons"].value<bool>() == true);

    toml::table disabled = toml::parse("[shell]\napp_icon_colorize = false\n");
    const toml::table enabledBase = toml::parse("[shell]\napp_icon_colorize = true\n");
    (void)noctalia::config::applyPendingConfigMigrations(disabled, enabledBase, 18, diagnostics);
    TEST_CHECK(disabled["dock"]["colorize_icons"].value<bool>() == false);
    TEST_CHECK(disabled["widget"]["taskbar"]["colorize_icons"].value<bool>() == false);

    toml::table invalid = toml::parse("[shell]\napp_icon_colorize = \"yes\"\n");
    noctalia::config::LegacyConfigIssues invalidIssues;
    noctalia::config::normalizeLegacyConfig(invalid, invalidIssues);
    TEST_CHECK(invalid["shell"]["app_icon_colorize"].is_string());

    toml::table absent;
    noctalia::config::LegacyConfigIssues absentIssues;
    noctalia::config::normalizeLegacyConfig(absent, absentIssues);
    TEST_CHECK(absent.empty() && absentIssues.empty());
  }

  void checkCircularAvatarMigration() {
    for (const bool circular : {false, true}) {
      toml::table legacy = toml::parse(std::format("[control_center]\ncircular_avatar = {}\n", circular));
      noctalia::config::LegacyConfigIssues issues;
      noctalia::config::normalizeLegacyConfig(legacy, issues);
      TEST_CHECK(legacy["shell"]["circular_avatar"].value<bool>() == circular);
      TEST_CHECK(!legacy["control_center"]["circular_avatar"]);
      TEST_CHECK(
          issues.size() == 1
          && issues.front().migrationVersion == 20
          && issues.front().path == "control_center.circular_avatar"
      );
      const auto firstPass = legacy;
      noctalia::config::LegacyConfigIssues secondIssues;
      noctalia::config::normalizeLegacyConfig(legacy, secondIssues);
      TEST_CHECK(legacy == firstPass && secondIssues.empty());
    }

    toml::table conflict = toml::parse(R"(
[shell]
circular_avatar = false
[control_center]
circular_avatar = true
)");
    noctalia::config::LegacyConfigIssues conflictIssues;
    noctalia::config::normalizeLegacyConfig(conflict, conflictIssues);
    TEST_CHECK(conflict["shell"]["circular_avatar"].value<bool>() == false);
    TEST_CHECK(!conflict["control_center"]["circular_avatar"]);

    const toml::table canonicalBase = toml::parse("[shell]\ncircular_avatar = true\n");
    toml::table sidecar = toml::parse("config_version = 19\n[control_center]\ncircular_avatar = false\n");
    noctalia::config::schema::Diagnostics sidecarDiagnostics;
    const int version = noctalia::config::applyPendingConfigMigrations(sidecar, canonicalBase, 19, sidecarDiagnostics);
    TEST_CHECK(version == noctalia::config::currentConfigVersion());
    toml::table merged = canonicalBase;
    ConfigService::deepMerge(merged, sidecar);
    noctalia::config::LegacyConfigIssues mergedIssues;
    noctalia::config::normalizeLegacyConfig(merged, mergedIssues, &sidecar);
    TEST_CHECK(merged["shell"]["circular_avatar"].value<bool>() == false);

    const toml::table legacyBase = toml::parse("[control_center]\ncircular_avatar = true\n");
    toml::table canonicalSidecar = toml::parse("config_version = 19\n[shell]\ncircular_avatar = false\n");
    (void)noctalia::config::applyPendingConfigMigrations(canonicalSidecar, legacyBase, 19, sidecarDiagnostics);
    merged = legacyBase;
    ConfigService::deepMerge(merged, canonicalSidecar);
    mergedIssues.clear();
    noctalia::config::normalizeLegacyConfig(merged, mergedIssues, &canonicalSidecar);
    TEST_CHECK(merged["shell"]["circular_avatar"].value<bool>() == false);

    toml::table invalid = toml::parse("[control_center]\ncircular_avatar = \"invalid\"\n");
    noctalia::config::LegacyConfigIssues invalidIssues;
    noctalia::config::normalizeLegacyConfig(invalid, invalidIssues);
    TEST_CHECK(invalid["shell"]["circular_avatar"].value<std::string>() == "invalid");
    TEST_CHECK(!invalid["control_center"]["circular_avatar"]);

    toml::table invalidCanonical =
        toml::parse("[shell]\ncircular_avatar = \"invalid\"\n[control_center]\ncircular_avatar = false\n");
    noctalia::config::LegacyConfigIssues invalidCanonicalIssues;
    noctalia::config::normalizeLegacyConfig(invalidCanonical, invalidCanonicalIssues);
    TEST_CHECK(
        invalidCanonical["shell"]["circular_avatar"].value<std::string>() == "invalid"
        && !invalidCanonical["control_center"]["circular_avatar"]
    );

    toml::table malformedShell = toml::parse("shell = \"invalid\"\n[control_center]\ncircular_avatar = false\n");
    noctalia::config::LegacyConfigIssues malformedIssues;
    noctalia::config::normalizeLegacyConfig(malformedShell, malformedIssues);
    TEST_CHECK(
        malformedShell["shell"].is_string()
        && malformedShell["control_center"]["circular_avatar"].value<bool>() == false
    );
  }

  void checkVersionGating() {
    toml::table legacy = toml::parse(R"(
[bar.main]
radius = -10
)");
    noctalia::config::schema::Diagnostics diag;
    const auto stored = noctalia::config::storedConfigVersion(legacy, diag);
    TEST_CHECK(stored == 0);
    const int applied = noctalia::config::applyPendingConfigMigrations(legacy, {}, stored.value_or(0), diag);
    TEST_CHECK(applied == noctalia::config::currentConfigVersion());
    TEST_CHECK(legacy["bar"]["main"]["radius"].value<std::int64_t>() == 10);

    toml::table current = toml::parse(R"(
config_version = 1
[bar.main]
radius = -10
)");
    noctalia::config::schema::Diagnostics currentDiag;
    const auto currentStored = noctalia::config::storedConfigVersion(current, currentDiag);
    TEST_CHECK(currentStored == 1);
    (void)noctalia::config::applyPendingConfigMigrations(current, {}, currentStored.value_or(0), currentDiag);
    TEST_CHECK(current["bar"]["main"]["radius"].value<std::int64_t>() == -10);

    toml::table invalid = toml::parse("config_version = \"one\"");
    noctalia::config::schema::Diagnostics invalidDiag;
    TEST_CHECK(!noctalia::config::storedConfigVersion(invalid, invalidDiag).has_value());
    TEST_CHECK(invalidDiag.hasErrors());
    TEST_CHECK(invalidDiag.hasFatalErrors());

    toml::table future = toml::parse("config_version = 999");
    noctalia::config::schema::Diagnostics futureDiag;
    TEST_CHECK(!noctalia::config::storedConfigVersion(future, futureDiag).has_value());
    TEST_CHECK(futureDiag.hasErrors());
    TEST_CHECK(futureDiag.hasFatalErrors());

    noctalia::config::schema::Diagnostics baseline;
    baseline.componentError("widget.clock.timezone", "widget.clock", "unknown timezone", "clock.timezone.unknown");
    noctalia::config::schema::Diagnostics candidate = baseline;
    candidate.error("accessibility.ui_scale", "expected a number", "config.type.number");
    const auto introduced = candidate.introducedErrorsComparedTo(baseline);
    TEST_CHECK(introduced.entries.size() == 1);
    TEST_CHECK(introduced.entries.front().path == "accessibility.ui_scale");
  }

  void checkReminderFingerprint() {
    const noctalia::config::LegacyConfigIssues first = {{1, "bar.main", "message"}};
    const noctalia::config::LegacyConfigIssues reordered = {
        {1, "bar.second", "message"},
        {1, "bar.main", "different display message"},
    };
    const noctalia::config::LegacyConfigIssues sameReordered = {
        {1, "bar.main", "message"},
        {1, "bar.second", "message"},
    };

    const std::string firstFingerprint = noctalia::config::legacyConfigIssueFingerprint(first);
    const std::string expandedFingerprint = noctalia::config::legacyConfigIssueFingerprint(reordered);
    TEST_CHECK(expandedFingerprint == noctalia::config::legacyConfigIssueFingerprint(sameReordered));
    TEST_CHECK(noctalia::config::legacyConfigFingerprintHasNewIssues(expandedFingerprint, firstFingerprint));
    TEST_CHECK(!noctalia::config::legacyConfigFingerprintHasNewIssues(firstFingerprint, expandedFingerprint));

    constexpr std::int64_t kStart = 1'000'000;
    TEST_CHECK(!noctalia::config::legacyConfigReminderIntervalElapsed(
        kStart + noctalia::config::kLegacyConfigReminderIntervalSeconds - 1, kStart
    ));
    TEST_CHECK(
        noctalia::config::legacyConfigReminderIntervalElapsed(
            kStart + noctalia::config::kLegacyConfigReminderIntervalSeconds, kStart
        )
    );
    TEST_CHECK(noctalia::config::legacyConfigReminderIntervalElapsed(kStart - 1, kStart));
  }

  void checkRegistryOrdering() {
    int expectedVersion = 1;
    for (const auto& migration : noctalia::config::configMigrations()) {
      TEST_CHECK(migration.toVersion == expectedVersion);
      TEST_CHECK(!migration.summary.empty());
      TEST_CHECK(migration.apply != nullptr);
      ++expectedVersion;
    }
    TEST_CHECK(expectedVersion - 1 == noctalia::config::currentConfigVersion());
  }

  void checkLargeCurrentRegistrySkipsBodies() {
    std::vector<noctalia::config::ConfigMigration> migrations;
    migrations.reserve(100);
    for (int version = 1; version <= 100; ++version) {
      migrations.push_back({
          .toVersion = version,
          .summary = "synthetic migration",
          .apply = countSyntheticMigration,
      });
    }

    toml::table root;
    noctalia::config::schema::Diagnostics diag;
    g_syntheticMigrationApplications = 0;
    const int current = noctalia::config::applyPendingConfigMigrations(root, {}, 100, diag, migrations);
    TEST_CHECK(current == 100);
    TEST_CHECK(g_syntheticMigrationApplications == 0);

    const int upgraded = noctalia::config::applyPendingConfigMigrations(root, {}, 99, diag, migrations);
    TEST_CHECK(upgraded == 100);
    TEST_CHECK(g_syntheticMigrationApplications == 1);
  }

} // namespace

int main() {
  checkNegativeRadiusMigration();
  checkExtremeNegativeRadius();
  checkCustomScheduleMigration();
  checkWidgetActionsMigration();
  checkWidgetGestureSettingsMigration();
  checkRemainingWidgetGesturesMigration();
  checkCustomButtonCommandsMigration();
  checkDeadZoneActionsMigration();
  checkCalendarEventFormatsMigration();
  checkDockMonitorAllowListMigration();
  checkSysmonPresentationMigration();
  checkKeyboardLayoutShowGlyphMigration();
  checkWorkspacesMonitorsMigration();
  checkKeyboardLayoutCustomLabelsMigration();
  checkWorkspacesDisplayMigration();
  checkPaletteRoleNamesMigration();
  checkPluginAutoUpdateModeMigration();
  checkAppIconColorizationMigration();
  checkCircularAvatarMigration();
  checkVersionGating();
  checkReminderFingerprint();
  checkRegistryOrdering();
  checkLargeCurrentRegistrySkipsBodies();

  return 0;
}
