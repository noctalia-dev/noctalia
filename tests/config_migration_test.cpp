#include "config/config_migrations.h"
#include "core/toml.h"

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
    expect(issues.size() == 2, "expected one issue for the bar and one for its monitor override");

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
  checkVersionGating();
  checkReminderFingerprint();
  checkRegistryOrdering();
  checkLargeCurrentRegistrySkipsBodies();

  if (g_failures == 0) {
    std::println("config_migration_test: all checks passed");
  }
  return g_failures == 0 ? 0 : 1;
}
