#include "config/config_migrations.h"
#include "core/toml.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <print>
#include <string>
#include <string_view>

namespace {

  int g_failures = 0;
  int g_syntheticMigrationApplications = 0;

  void countSyntheticMigration(toml::table&, noctalia::config::schema::Diagnostics&) {
    ++g_syntheticMigrationApplications;
  }

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "config_migration_test: FAIL: {}", message);
      ++g_failures;
    }
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

    expect(root["bar"]["main"]["radius"].value<std::int64_t>() == 12, "base radius was not made positive");
    expect(
        root["bar"]["main"]["radius_top_left"].value<std::int64_t>() == 20, "per-corner radius was not made positive"
    );
    expect(root["bar"]["main"]["radius_top_right"].value<std::int64_t>() == 8, "positive radius was changed");
    expect(root["bar"]["main"]["concave_edge_corners"].value<bool>() == true, "base concave flag was not set");
    expect(
        root["bar"]["main"]["monitor"]["dp1"]["radius"].value<std::int64_t>() == 16, "monitor radius was not migrated"
    );
    expect(
        root["bar"]["main"]["monitor"]["dp1"]["concave_edge_corners"].value<bool>() == true,
        "monitor concave flag was not set"
    );
    expect(root["dock"]["radius"].value<std::int64_t>() == -7, "dock radius was incorrectly migrated");
    expect(issues.size() == 3, "expected one migration issue per negative radius");
    expect(hasIssuePath(issues, "bar.main.radius"), "base radius issue did not identify its source key");
    expect(hasIssuePath(issues, "bar.main.radius_top_left"), "corner radius issue did not identify its source key");
    expect(hasIssuePath(issues, "bar.main.monitor.dp1.radius"), "monitor radius issue did not identify its source key");

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(root, secondPassIssues);
    expect(secondPassIssues.empty(), "normalization was not idempotent");
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
    expect(
        root["bar"]["main"]["radius"].value<std::int64_t>() == 500, "extreme negative radius did not normalize safely"
    );
  }

  void checkCustomScheduleMigration() {
    toml::table legacy = toml::parse(R"(
[location]
sunset = "20:30"
sunrise = "07:30"
)");
    noctalia::config::LegacyConfigIssues issues;
    noctalia::config::normalizeLegacyConfig(legacy, issues);
    expect(
        legacy["location"]["custom_schedule"].value<bool>() == true,
        "a times-only location did not opt into custom scheduling"
    );
    expect(issues.size() == 1, "times-only location did not report a legacy issue");
    expect(issues.front().path == "location.sunset", "custom schedule issue did not identify a source key");

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(legacy, secondPassIssues);
    expect(secondPassIssues.empty(), "custom scheduling normalization was not idempotent");

    // Coordinates won under the old rules, so these configs must keep using them.
    for (const std::string_view source :
         {"auto_locate = true", "address = \"Toronto, ON\"", "latitude = 52.52\nlongitude = 13.405"}) {
      toml::table coords =
          toml::parse(std::format("[location]\nsunset = \"20:30\"\nsunrise = \"07:30\"\n{}\n", source));
      noctalia::config::LegacyConfigIssues coordIssues;
      noctalia::config::normalizeLegacyConfig(coords, coordIssues);
      expect(
          !coords["location"]["custom_schedule"].value<bool>().has_value(),
          "a location with coordinates was switched to custom scheduling"
      );
      expect(coordIssues.empty(), "a location with coordinates reported a legacy issue");
    }

    toml::table explicitOff = toml::parse(R"(
[location]
custom_schedule = false
sunset = "20:30"
sunrise = "07:30"
)");
    noctalia::config::LegacyConfigIssues offIssues;
    noctalia::config::normalizeLegacyConfig(explicitOff, offIssues);
    expect(
        explicitOff["location"]["custom_schedule"].value<bool>() == false,
        "an explicit custom_schedule = false was overwritten"
    );
  }

  void checkWidgetActionsMigration() {
    // The setting is now the built-in `middle` binding, so an enabled config just drops the key.
    toml::table enabled = toml::parse(R"(
[shell]
middle_click_opens_widget_settings = true
)");
    noctalia::config::LegacyConfigIssues enabledIssues;
    noctalia::config::normalizeLegacyConfig(enabled, enabledIssues);
    expect(
        !enabled["shell"]["middle_click_opens_widget_settings"].value<bool>().has_value(),
        "an enabled middle_click_opens_widget_settings was not dropped"
    );
    expect(enabled["bar"].as_table() == nullptr, "an enabled config gained a spurious bar actions table");
    expect(
        enabledIssues.size() == 1 && enabledIssues.front().path == "shell.middle_click_opens_widget_settings",
        "enabled widget action issue did not identify the removed source key"
    );

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
    expect(
        !disabled["shell"]["middle_click_opens_widget_settings"].value<bool>().has_value(),
        "a disabled middle_click_opens_widget_settings was not dropped"
    );
    for (const std::string_view barName : {"default", "secondary"}) {
      expect(
          disabled["bar"][barName]["actions"]["middle"].value<std::string>() == std::optional<std::string>{"none"},
          "a disabled config did not unbind middle on every bar"
      );
    }
    expect(
        disabledIssues.size() == 1 && disabledIssues.front().path == "shell.middle_click_opens_widget_settings",
        "disabled widget action reported generated bar actions instead of the source key"
    );

    // With no [bar] table the built-in default bar is still in play, so it must be seeded.
    toml::table noBars = toml::parse(R"(
[shell]
middle_click_opens_widget_settings = false
)");
    noctalia::config::LegacyConfigIssues noBarIssues;
    noctalia::config::normalizeLegacyConfig(noBars, noBarIssues);
    expect(
        noBars["bar"]["default"]["actions"]["middle"].value<std::string>() == std::optional<std::string>{"none"},
        "a disabled config without a [bar] table did not seed the default bar"
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
    expect(
        explicitBinding["bar"]["default"]["actions"]["middle"].value<std::string>()
            == std::optional<std::string>{"media toggle"},
        "the migration overwrote an explicit middle binding"
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(disabled, secondPassIssues);
    expect(secondPassIssues.empty(), "widget action normalization was not idempotent");
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
      expect(
          config["widget"]["workspaces"]["actions"][gesture].value<std::string>() == std::optional<std::string>{"none"},
          "enable_scroll = false did not unbind a workspaces scroll gesture"
      );
    }
    expect(
        !config["widget"]["workspaces"]["enable_scroll"].value<bool>().has_value(),
        "enable_scroll was not dropped from workspaces"
    );

    // Enabled scroll is the default, so the key just goes away.
    expect(
        !config["widget"]["taskbar"]["enable_scroll"].value<bool>().has_value(),
        "enable_scroll = true was not dropped from taskbar"
    );
    expect(
        config["widget"]["taskbar"]["actions"].as_table() == nullptr,
        "enable_scroll = true seeded a spurious taskbar actions table"
    );

    // The type comes from `type` when the widget is named something else.
    expect(
        config["widget"]["my_profile"]["actions"]["scroll_up"].value<std::string>()
            == std::optional<std::string>{"none"},
        "a renamed power_profile widget was not migrated"
    );

    expect(
        config["widget"]["keyboard_layout"]["actions"]["left"].value<std::string>()
            == std::optional<std::string>{"exec hyprctl switchxkblayout all next"},
        "cycle_command did not become an exec binding"
    );
    expect(
        !config["widget"]["keyboard_layout"]["cycle_command"].value<std::string>().has_value(),
        "cycle_command was not dropped"
    );

    // Plugin widgets still gate their own onScroll handler, so the key survives for them.
    expect(
        config["widget"]["plugin_thing"]["enable_scroll"].value<bool>() == std::optional<bool>{false},
        "enable_scroll was dropped from a widget that still uses it"
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "widget gesture setting normalization was not idempotent");
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
      expect(
          config["widget"]["media"]["actions"][gesture].value<std::string>() == std::optional<std::string>{"none"},
          "media enable_scroll = false did not unbind a scroll gesture"
      );
    }

    // A non-default step survives as the verb's argument, picking the verbs the device implies.
    expect(
        config["widget"]["volume"]["actions"]["scroll_up"].value<std::string>()
            == std::optional<std::string>{"volume-up 10%"},
        "volume scroll_step did not become a step argument"
    );
    expect(
        config["widget"]["mic"]["actions"]["scroll_down"].value<std::string>()
            == std::optional<std::string>{"mic-volume-down 2%"},
        "a microphone volume widget did not migrate to the mic verbs"
    );

    // The default step matches the verbs' own default, so it leaves nothing behind.
    expect(
        !config["widget"]["brightness"]["scroll_step"].value<std::int64_t>().has_value(),
        "a default scroll_step was not dropped"
    );
    expect(
        config["widget"]["brightness"]["actions"].as_table() == nullptr,
        "a default scroll_step seeded a spurious actions table"
    );

    expect(
        config["widget"]["screenshot"]["actions"]["left"].value<std::string>()
            == std::optional<std::string>{"screenshot-fullscreen"},
        "primary_click = fullscreen did not become a left binding"
    );
    // `region` is already the declared default, so it needs no binding.
    expect(
        config["widget"]["shot2"]["actions"].as_table() == nullptr, "primary_click = region wrote a redundant binding"
    );
    expect(
        !config["widget"]["screenshot"]["primary_click"].value<std::string>().has_value(),
        "primary_click was not dropped"
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "remaining widget gesture normalization was not idempotent");
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
    expect(
        config["widget"]["custom_button"]["actions"]["left"].value<std::string>()
            == std::optional<std::string>{"exec notify-send 'hello world'"},
        "command did not become a left exec binding"
    );
    expect(
        config["widget"]["custom_button"]["actions"]["right"].value<std::string>()
            == std::optional<std::string>{"exec playerctl next"},
        "right_command did not become a right exec binding"
    );
    expect(
        config["widget"]["custom_button"]["actions"]["scroll_up"].value<std::string>()
            == std::optional<std::string>{"exec brightnessctl set +5%"},
        "scroll_up_command did not become a scroll_up exec binding"
    );
    for (const std::string_view key : {"command", "right_command", "scroll_up_command"}) {
      expect(
          !config["widget"]["custom_button"][key].value<std::string>().has_value(),
          "a custom_button command key was not dropped"
      );
    }

    // enable_scroll = false used to beat the scroll commands, and still does.
    expect(
        config["widget"]["dead_scroll"]["actions"]["scroll_up"].value<std::string>()
            == std::optional<std::string>{"none"},
        "a disabled scroll command was migrated as if it were live"
    );

    // An explicit binding always wins over a migrated key.
    expect(
        config["widget"]["explicit"]["actions"]["left"].value<std::string>()
            == std::optional<std::string>{"media toggle"},
        "the migration overwrote an explicit left binding"
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "custom_button command normalization was not idempotent");
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

    expect(
        config["bar"]["default"]["dead_zone"]["actions"]["left"].value<std::string>()
            == std::optional<std::string>{"exec notify-send left"},
        "a dead zone command did not become a left exec binding"
    );
    expect(
        config["bar"]["default"]["dead_zone"]["actions"]["right"].value<std::string>()
            == std::optional<std::string>{"exec notify-send right"},
        "a dead zone right_command did not migrate"
    );
    expect(
        !config["bar"]["default"]["dead_zone"]["command"].value<std::string>().has_value(),
        "the old dead zone key was not dropped"
    );

    // Monitor overrides carry their own dead zone table.
    expect(
        config["bar"]["default"]["monitor"]["DP-1"]["dead_zone"]["actions"]["scroll_up"].value<std::string>()
            == std::optional<std::string>{"exec notify-send up"},
        "a monitor override dead zone command did not migrate"
    );

    // An empty command bound nothing before and binds nothing now.
    expect(
        config["bar"]["other"]["dead_zone"]["actions"].as_table() == nullptr,
        "an empty dead zone command seeded a binding"
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "dead zone normalization was not idempotent");
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

    expect(
        config["widget"]["gauge"]["visualization"].value<std::string>() == std::optional<std::string>{"gauge"},
        "gauge display was not migrated"
    );
    expect(
        config["widget"]["gauge"]["show_value"].value<bool>() == std::optional<bool>{false},
        "gauge show_label was not migrated"
    );
    expect(
        config["widget"]["gauge"]["show_glyph"].value<bool>() == std::optional<bool>{false},
        "show_icon was not migrated"
    );
    expect(
        config["widget"]["gauge"]["label_show_units"].value<bool>() == std::optional<bool>{false}
            && config["widget"]["gauge"]["label_min_width"].value<std::int64_t>() == std::optional<std::int64_t>{42},
        "unchanged label detail settings were not preserved"
    );
    expect(
        config["widget"]["text"]["visualization"].value<std::string>() == std::optional<std::string>{"none"}
            && config["widget"]["text"]["show_value"].value<bool>() == std::optional<bool>{true},
        "text display did not preserve its always-visible value"
    );
    expect(
        config["widget"]["none"]["visualization"].value<std::string>() == std::optional<std::string>{"none"}
            && config["widget"]["none"]["show_value"].value<bool>() == std::optional<bool>{false},
        "none display did not preserve its hidden value"
    );
    expect(
        config["widget"]["sysmon"]["visualization"].value<std::string>() == std::optional<std::string>{"graph"}
            && config["widget"]["sysmon"]["show_value"].value<bool>() == std::optional<bool>{true},
        "implicit sysmon defaults were not preserved"
    );
    expect(
        config["widget"]["canonical"]["visualization"].value<std::string>() == std::optional<std::string>{"graph"}
            && config["widget"]["canonical"]["show_value"].value<bool>() == std::optional<bool>{false}
            && config["widget"]["canonical"]["show_glyph"].value<bool>() == std::optional<bool>{true},
        "legacy sysmon settings overwrote canonical settings"
    );
    expect(
        !config["widget"]["canonical"].as_table()->contains("display")
            && !config["widget"]["canonical"].as_table()->contains("show_label")
            && !config["widget"]["canonical"].as_table()->contains("show_icon"),
        "legacy sysmon keys were retained"
    );
    expect(
        config["widget"]["clock"]["display"].value<std::string>() == std::optional<std::string>{"text"},
        "another widget type was migrated as sysmon"
    );
    expect(issues.size() == 11, "expected one migration issue per legacy sysmon key");
    expect(hasIssuePath(issues, "widget.gauge.display"), "sysmon display issue did not identify its source key");
    expect(hasIssuePath(issues, "widget.gauge.show_label"), "sysmon label issue did not identify its source key");
    expect(hasIssuePath(issues, "widget.gauge.show_icon"), "sysmon glyph issue did not identify its source key");

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "sysmon presentation normalization was not idempotent");
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

    expect(
        config["widget"]["none"]["show_labels"].value<bool>() == std::optional<bool>{false}
            && !config["widget"]["none"].as_table()->contains("display"),
        "none display was not migrated to show_labels"
    );
    expect(
        config["widget"]["none"]["labels_only_when_occupied"].value<bool>() == std::optional<bool>{true}
            && config["widget"]["none"]["max_label_chars"].value<std::int64_t>() == std::optional<std::int64_t>{8},
        "unchanged workspaces label settings were not preserved"
    );
    expect(
        config["widget"]["named"]["label_source"].value<std::string>() == std::optional<std::string>{"name"}
            && !config["widget"]["named"].as_table()->contains("display"),
        "name display was not migrated to label_source"
    );
    expect(
        config["widget"]["numbered"]["label_source"].value<std::string>() == std::optional<std::string>{"id"}
            && !config["widget"]["numbered"].as_table()->contains("display"),
        "id display was not migrated to label_source"
    );
    expect(
        config["widget"]["canonical-labels"]["show_labels"].value<bool>() == std::optional<bool>{true}
            && !config["widget"]["canonical-labels"].as_table()->contains("display"),
        "legacy workspaces display overwrote canonical show_labels"
    );
    expect(
        config["widget"]["canonical-source"]["label_source"].value<std::string>() == std::optional<std::string>{"id"}
            && !config["widget"]["canonical-source"].as_table()->contains("display"),
        "legacy workspaces display overwrote canonical label_source"
    );
    expect(
        config["widget"]["lock_keys"]["display"].value<std::string>() == std::optional<std::string>{"short"},
        "another widget type was migrated as workspaces"
    );
    expect(
        config["widget"]["invalid"]["display"].value<std::string>() == std::optional<std::string>{"icon"},
        "unknown workspaces display was migrated"
    );
    expect(
        config["widget"]["workspaces"]["label_source"].value<std::string>() == std::optional<std::string>{"id"}
            && !config["widget"]["workspaces"].as_table()->contains("display"),
        "untyped [widget.workspaces] was not migrated"
    );
    expect(issues.size() == 6, "expected one migration issue per workspaces widget");

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "workspaces display normalization was not idempotent");
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

    expect(
        config["widget"]["keyboard_layout"]["show_glyph"].value<bool>() == std::optional<bool>{false}
            && !config["widget"]["keyboard_layout"].as_table()->contains("show_icon"),
        "implicit keyboard layout show_icon was not migrated"
    );
    expect(
        config["widget"]["named-layout"]["show_glyph"].value<bool>() == std::optional<bool>{false}
            && !config["widget"]["named-layout"].as_table()->contains("show_icon"),
        "legacy keyboard layout setting overwrote canonical show_glyph"
    );
    expect(
        config["widget"]["clock"]["show_icon"].value<bool>() == std::optional<bool>{false},
        "another widget type was migrated as keyboard layout"
    );
    expect(issues.size() == 2, "expected one migration issue per keyboard layout widget");

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "keyboard layout show_glyph normalization was not idempotent");
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

    expect(
        config["shell"]["keyboard_layout"]["custom_labels"]["English (US)"].value<std::string>()
                == std::optional<std::string>{"US"}
            && config["shell"]["keyboard_layout"]["custom_labels"]["German"].value<std::string>()
                == std::optional<std::string>{"Global"}
            && config["shell"]["keyboard_layout"]["custom_labels"]["French"].value<std::string>()
                == std::optional<std::string>{"FR"},
        "keyboard layout custom labels were not merged into shell configuration"
    );
    expect(
        !config["widget"]["keyboard_layout"].as_table()->contains("custom_labels")
            && !config["widget"]["named-layout"].as_table()->contains("custom_labels"),
        "legacy keyboard layout custom_labels were not removed"
    );
    expect(
        config["widget"]["clock"]["custom_labels"]["German"].value<std::string>()
            == std::optional<std::string>{"Clock"},
        "another widget type's custom_labels were migrated"
    );
    expect(issues.size() == 2, "expected one migration issue per keyboard layout widget");
    expect(
        std::ranges::any_of(issues, [](const auto& issue) { return issue.message.contains("conflicting canonical"); }),
        "conflicting canonical keyboard layout label was not reported"
    );

    noctalia::config::LegacyConfigIssues secondPassIssues;
    noctalia::config::normalizeLegacyConfig(config, secondPassIssues);
    expect(secondPassIssues.empty(), "keyboard layout custom_labels normalization was not idempotent");
  }

  void checkPluginAutoUpdateModeMigration() {
    for (const bool enabled : {true, false}) {
      toml::table config = toml::parse(std::format("[plugins]\nauto_update = {}", enabled));
      noctalia::config::LegacyConfigIssues issues;
      noctalia::config::normalizeLegacyConfig(config, issues);

      const std::string_view expected = enabled ? "all" : "none";
      expect(
          config["plugins"]["auto_update"].value<std::string_view>() == expected,
          "legacy plugin auto-update boolean was not converted to its matching scope"
      );
      expect(
          issues.size() == 1 && hasIssuePath(issues, "plugins.auto_update"),
          "plugin auto-update migration did not identify its source key"
      );
    }

    toml::table sidecar = toml::parse("config_version = 12\n[plugins]\nauto_update = false");
    noctalia::config::schema::Diagnostics diagnostics;
    const int applied = noctalia::config::applyPendingConfigMigrations(sidecar, 12, diagnostics);
    expect(applied == noctalia::config::currentConfigVersion(), "plugin auto-update sidecar migration was not applied");
    expect(
        sidecar["plugins"]["auto_update"].value<std::string_view>() == std::optional<std::string_view>{"none"},
        "plugin auto-update sidecar migration did not preserve false as none"
    );
  }

  void checkVersionGating() {
    toml::table legacy = toml::parse(R"(
[bar.main]
radius = -10
)");
    noctalia::config::schema::Diagnostics diag;
    const auto stored = noctalia::config::storedConfigVersion(legacy, diag);
    expect(stored == 0, "missing config_version was not treated as legacy version 0");
    const int applied = noctalia::config::applyPendingConfigMigrations(legacy, stored.value_or(0), diag);
    expect(applied == noctalia::config::currentConfigVersion(), "pending migration did not reach current version");
    expect(legacy["bar"]["main"]["radius"].value<std::int64_t>() == 10, "sidecar migration did not run");

    toml::table current = toml::parse(R"(
config_version = 1
[bar.main]
radius = -10
)");
    noctalia::config::schema::Diagnostics currentDiag;
    const auto currentStored = noctalia::config::storedConfigVersion(current, currentDiag);
    expect(currentStored == 1, "current config_version was not read");
    (void)noctalia::config::applyPendingConfigMigrations(current, currentStored.value_or(0), currentDiag);
    expect(
        current["bar"]["main"]["radius"].value<std::int64_t>() == -10, "current sidecar replayed a historical migration"
    );

    toml::table invalid = toml::parse("config_version = \"one\"");
    noctalia::config::schema::Diagnostics invalidDiag;
    expect(
        !noctalia::config::storedConfigVersion(invalid, invalidDiag).has_value(), "invalid config_version was accepted"
    );
    expect(invalidDiag.hasErrors(), "invalid config_version did not produce an error");
    expect(invalidDiag.hasFatalErrors(), "invalid config_version was not document-fatal");

    toml::table future = toml::parse("config_version = 999");
    noctalia::config::schema::Diagnostics futureDiag;
    expect(
        !noctalia::config::storedConfigVersion(future, futureDiag).has_value(), "future config_version was accepted"
    );
    expect(futureDiag.hasErrors(), "future config_version did not produce an error");
    expect(futureDiag.hasFatalErrors(), "future config_version was not document-fatal");

    noctalia::config::schema::Diagnostics baseline;
    baseline.componentError("widget.clock.timezone", "widget.clock", "unknown timezone", "clock.timezone.unknown");
    noctalia::config::schema::Diagnostics candidate = baseline;
    candidate.error("accessibility.ui_scale", "expected a number", "config.type.number");
    const auto introduced = candidate.introducedErrorsComparedTo(baseline);
    expect(introduced.entries.size() == 1, "diagnostic comparison did not isolate the new error");
    expect(
        introduced.entries.front().path == "accessibility.ui_scale", "diagnostic comparison returned the wrong error"
    );
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
    expect(
        expandedFingerprint == noctalia::config::legacyConfigIssueFingerprint(sameReordered),
        "fingerprint depends on issue ordering or display message"
    );
    expect(
        noctalia::config::legacyConfigFingerprintHasNewIssues(expandedFingerprint, firstFingerprint),
        "new issue was not detected"
    );
    expect(
        !noctalia::config::legacyConfigFingerprintHasNewIssues(firstFingerprint, expandedFingerprint),
        "removing an issue was treated as introducing one"
    );

    constexpr std::int64_t kStart = 1'000'000;
    expect(
        !noctalia::config::legacyConfigReminderIntervalElapsed(
            kStart + noctalia::config::kLegacyConfigReminderIntervalSeconds - 1, kStart
        ),
        "reminder became due before three days"
    );
    expect(
        noctalia::config::legacyConfigReminderIntervalElapsed(
            kStart + noctalia::config::kLegacyConfigReminderIntervalSeconds, kStart
        ),
        "reminder was not due at three days"
    );
    expect(
        noctalia::config::legacyConfigReminderIntervalElapsed(kStart - 1, kStart),
        "backward clock change did not make the reminder due"
    );
  }

  void checkRegistryOrdering() {
    int expectedVersion = 1;
    for (const auto& migration : noctalia::config::configMigrations()) {
      expect(migration.toVersion == expectedVersion, "migration registry has a gap or is out of order");
      expect(!migration.summary.empty(), "migration registry entry has no summary");
      expect(migration.apply != nullptr, "migration registry entry has no apply function");
      ++expectedVersion;
    }
    expect(
        expectedVersion - 1 == noctalia::config::currentConfigVersion(),
        "current config version does not match the registry"
    );
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
    const int current = noctalia::config::applyPendingConfigMigrations(root, 100, diag, migrations);
    expect(current == 100, "synthetic current version changed");
    expect(g_syntheticMigrationApplications == 0, "current sidecar executed historical migration bodies");

    const int upgraded = noctalia::config::applyPendingConfigMigrations(root, 99, diag, migrations);
    expect(upgraded == 100, "synthetic upgrade did not reach the current version");
    expect(g_syntheticMigrationApplications == 1, "synthetic upgrade did not execute exactly one pending body");
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
  checkSysmonPresentationMigration();
  checkKeyboardLayoutShowGlyphMigration();
  checkKeyboardLayoutCustomLabelsMigration();
  checkWorkspacesDisplayMigration();
  checkPluginAutoUpdateModeMigration();
  checkVersionGating();
  checkReminderFingerprint();
  checkRegistryOrdering();
  checkLargeCurrentRegistrySkipsBodies();

  if (g_failures == 0) {
    std::println("config_migration_test: all checks passed");
  }
  return g_failures == 0 ? 0 : 1;
}
