#include "config/config_export.h"
#include "config/config_origins.h"
#include "config/config_service.h"
#include "config/config_validate.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "config/schema/greeter_schema.h"
#include "core/timer_manager.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/greeter/greeter_config_draft.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <poll.h>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace schema = noctalia::config::schema;

namespace {
  constexpr std::string_view kProfile = R"(
[layout]
login_mode = "minimal"
[layout.login]
width = 420
rotation = 0.25
flip_y = true
scale = 1.25
[layout.branding]
visible = false
[widgets]
widget_order = ["clock1", "clock2", "caption", "metrics", "power1", "art"]
[widgets.widget.clock1]
type = "clock"
placement = "anchor"
anchor = "center-left"
x = 10
y = -20
[widgets.widget.clock1.settings]
format = "{:%H:%M}"
show_seconds = false
[widgets.widget.clock2]
type = "clock"
placement = "relative"
cx = 1280
cy = 720
placement_width = 2560
placement_height = 1440
output = "DP-2"
box_width = 512
box_height = 256
rotation = 0.261799
flip_x = true
flip_y = true
enabled = false
[widgets.widget.clock2.settings]
clock_style = "analog"
[widgets.widget.caption]
type = "label"
placement = "anchor"
anchor = "bottom-center"
[widgets.widget.caption.settings]
title = "Hello"
opacity = 0.7
background = false
[widgets.widget.metrics]
type = "sysmon"
placement = "anchor"
anchor = "top-right"
[widgets.widget.metrics.settings]
stat = "cpu_usage"
stat2 = "ram_pct"
display = "graph"
[widgets.widget.power1]
type = "button"
placement = "anchor"
anchor = "bottom-right"
[widgets.widget.power1.settings]
action = "power"
variant = "primary"
color = ""
[widgets.widget.art]
type = "sticker"
placement = "anchor"
anchor = "center"
[widgets.widget.art.settings]
image_path = "/tmp/example.png"
)";

  ShellGreeterConfig read(const toml::table& table) {
    schema::Diagnostics diagnostics;
    auto result = schema::readGreeterConfig(table, "shell.greeter", diagnostics);
    for (const auto& entry : diagnostics.entries) {
      std::println(stderr, "{}", entry.describe());
    }
    TEST_CHECK(result.has_value());
    TEST_CHECK(diagnostics.entries.empty());
    return *result;
  }

  std::string text(const toml::table& table) {
    std::ostringstream output;
    output << toml::toml_formatter{table};
    return output.str();
  }

  void invalid(const toml::table& profile, std::string_view path) {
    schema::Diagnostics diagnostics;
    const auto parsed = schema::readGreeterConfig(profile, "shell.greeter", diagnostics);
    if (parsed) {
      std::println(stderr, "Unexpected valid profile for {}:\n{}", path, text(profile));
    }
    TEST_CHECK(!parsed);
    TEST_CHECK(diagnostics.hasErrors());
    TEST_CHECK(std::ranges::any_of(diagnostics.entries, [&](const auto& entry) { return entry.path == path; }));
    const toml::table root{{"shell", toml::table{{"greeter", profile}}}};
    TEST_CHECK(noctalia::config::validateMergedConfig(root, {}).hasErrors());
  }

  void checkSchema() {
    const auto profile = read(toml::parse(kProfile));
    TEST_CHECK(read(toml::parse(text(schema::writeGreeterConfig(profile)))) == profile);
    ShellConfig shell;
    schema::Diagnostics diagnostics;
    schema::readInto(
        toml::table{{"greeter", toml::parse(kProfile)}}, shell, schema::shellSchema(), "shell", diagnostics
    );
    TEST_CHECK(shell.greeter == profile);
    TEST_CHECK(!diagnostics.hasErrors());
    const auto serialized = schema::writeTable(shell, schema::shellSchema());
    TEST_CHECK(read(*serialized["greeter"].as_table()) == profile);
    TEST_CHECK(!profile.layout->components.at("login").visible.has_value());
    TEST_CHECK(!profile.widgets->instances->at(0).output.has_value());
    TEST_CHECK(profile.widgets->instances->at(1).flipX && profile.widgets->instances->at(1).flipY);
    TEST_CHECK(!profile.widgets->instances->at(1).enabled);
    TEST_CHECK(read(toml::table{}) == ShellGreeterConfig{});
    const auto defaults = read(toml::parse("[widgets]"));
    const auto empty = read(toml::parse("[widgets]\nwidget_order=[]"));
    TEST_CHECK(defaults.widgets && !defaults.widgets->instances);
    TEST_CHECK(empty.widgets && empty.widgets->instances && empty.widgets->instances->empty());
    TEST_CHECK(read(toml::parse(text(schema::writeGreeterConfig(defaults)))) == defaults);
    TEST_CHECK(read(toml::parse(text(schema::writeGreeterConfig(empty)))) == empty);

    auto rotated = schema::writeGreeterConfig(profile);
    rotated["widgets"]["widget"]["clock1"].as_table()->insert_or_assign("rotation", 3 * std::numbers::pi);
    rotated["layout"]["login"].as_table()->insert_or_assign("rotation", -3 * std::numbers::pi);
    const auto normalized = read(rotated);
    TEST_CHECK(std::abs(normalized.widgets->instances->front().rotation + std::numbers::pi) < 1e-12);
    TEST_CHECK(std::abs(*normalized.layout->components.at("login").rotation + std::numbers::pi) < 1e-12);

    for (const auto& [field, value] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"output", "\"\""},
             {"rotation", "nan"},
             {"flip_x", "1"},
             {"enabled", "1"},
             {"rotation", "true"},
             {"box_width", "15"},
             {"box_height", "4097"},
             {"anchor", "\"left\""},
             {"scale", "1"},
             {"cx", "50"},
             {"placement", "\"other\""},
             {"enabled", "\"yes\""},
             {"type", "\"weather\""},
             {"type", "\"volume\""},
             {"type", "\"login_box\""},
             {"type", "\"me/plugin:widget\""},
         }) {
      auto input = schema::writeGreeterConfig(profile);
      auto valueTable = toml::parse("value=" + std::string(value));
      input["widgets"]["widget"]["clock1"].as_table()->insert_or_assign(field, *valueTable.get("value"));
      invalid(input, "shell.greeter.widgets.widget.clock1." + std::string(field));
    }
    for (const auto& [source, path] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"[layout.nope]", "shell.greeter.layout.nope"},
             {"[layout.login]\nvisible=false", "shell.greeter.layout.login.visible"},
             {"[layout.login]\noutput='DP-1'", "shell.greeter.layout.login.output"},
             {"[layout.login]\nwidth=279", "shell.greeter.layout.login.width"},
             {"[layout.power]\nscale=4.1", "shell.greeter.layout.power.scale"},
             {"[layout.power]\nscale=true", "shell.greeter.layout.power.scale"},
             {"[layout]\nlogin_mode='compact'", "shell.greeter.layout.login_mode"},
             {"[widgets]\nwidget_order=['missing']", "shell.greeter.widgets.widget_order"},
             {"[widgets.widget.foo]", "shell.greeter.widgets.widget_order"},
             {"[widgets]\nwidget_order=[]\n[widgets.widget.login]", "shell.greeter.widgets.widget.login"},
         }) {
      invalid(toml::parse(source), path);
    }
    for (const auto& [id, setting, value] :
         std::initializer_list<std::tuple<std::string_view, std::string_view, std::string_view>>{
             {"clock1", "clock_style", "'other'"},
             {"clock1", "timezone", "'Not/A_Timezone'"},
             {"clock1", "color", "'invalid-color'"},
             {"clock1", "background_opacity", "2"},
             {"clock1", "background_opacity", "true"},
             {"clock1", "background_padding", "1.5"},
             {"power1", "command", "'shutdown now'"},
             {"power1", "action", "'exec'"},
             {"metrics", "stat", "'net_rx'"},
             {"metrics", "interface", "'eth0'"},
         }) {
      auto input = schema::writeGreeterConfig(profile);
      auto v = toml::parse("value=" + std::string(value));
      input["widgets"]["widget"][id]["settings"].as_table()->insert_or_assign(setting, *v.get("value"));
      invalid(input, "shell.greeter.widgets.widget." + std::string(id) + ".settings." + std::string(setting));
    }
    TEST_CHECK(schema::greeterWidgetSettingSchema("calendar").empty());
    TEST_CHECK(!desktop_settings::desktopWidgetSettingSchema("calendar").empty());
    auto calendar = schema::writeGreeterConfig(profile);
    calendar["widgets"]["widget"]["caption"].as_table()->insert_or_assign("type", "calendar");
    invalid(calendar, "shell.greeter.widgets.widget.caption.type");
    auto input = schema::writeGreeterConfig(profile);
    input["widgets"]["widget_order"].as_array()->push_back("clock1");
    invalid(input, "shell.greeter.widgets.widget_order");
    input = schema::writeGreeterConfig(profile);
    input["widgets"]["widget_order"].as_array()->erase(input["widgets"]["widget_order"].as_array()->begin());
    invalid(input, "shell.greeter.widgets.widget_order");
    input = schema::writeGreeterConfig(profile);
    input["widgets"]["widget"]["clock2"].as_table()->erase("placement_width");
    invalid(input, "shell.greeter.widgets.widget.clock2.placement_width");
    auto tooMany = profile;
    for (int index = 0; index < 65; ++index) {
      auto widget = profile.widgets->instances->front();
      widget.id = "widget-" + std::to_string(index);
      tooMany.widgets->instances->push_back(std::move(widget));
    }
    invalid(schema::writeGreeterConfig(tooMany), "shell.greeter.widgets.widget_order");
  }

  void writeFile(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value;
    TEST_CHECK(output.good());
  }
  std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    TEST_CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void reloadFromWatch(ConfigService& config, const int& reloads) {
    const int before = reloads;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (reloads == before && std::chrono::steady_clock::now() < deadline) {
      pollfd fd{.fd = config.watchFd(), .events = POLLIN};
      const int ready = ::poll(&fd, 1, 10);
      TEST_CHECK(ready >= 0 || errno == EINTR);
      if (ready > 0) {
        config.checkReload();
      }
      TimerManager::instance().tick();
    }
    TEST_CHECK(reloads > before);
  }

  void checkPersistence(const std::filesystem::path& root) {
    const auto configPath = root / "config/noctalia/config.toml";
    const auto settingsPath = root / "state/noctalia/settings.toml";
    const auto adminPath = root / "greeter/greeter.toml";
    const auto installedPath = root / "greeter/sync.toml";
    const auto base = read(toml::parse(kProfile));
    const auto baseText = text(toml::table{{"shell", toml::table{{"greeter", schema::writeGreeterConfig(base)}}}});
    writeFile(configPath, baseText);
    writeFile(settingsPath, "config_version = " + std::to_string(noctalia::config::currentConfigVersion()) + "\n");
    writeFile(adminPath, "[layout.login]\nwidth=500\n");
    writeFile(installedPath, "[appearance]\nscheme='Synced'\n");
    int reloads = 0;
    ConfigService config;
    config.addReloadCallback([&reloads] { ++reloads; }, "greeter-config-test");
    TEST_CHECK(config.config().shell.greeter == base);
    const auto desktop = config.config().desktopWidgets;
    const auto lockscreen = config.config().lockscreenWidgets;
    GreeterConfigDraft draft(config);
    const auto before = readFile(settingsPath);
    draft.profile().widgets->instances->erase(draft.profile().widgets->instances->begin());
    draft.profile().layout->components.erase("login");
    TEST_CHECK(draft.dirty());
    draft.discard();
    TEST_CHECK(!draft.dirty());
    TEST_CHECK(readFile(settingsPath) == before);
    TEST_CHECK(draft.save());
    TEST_CHECK(readFile(settingsPath) == before);
    draft.profile().widgets->instances->erase(draft.profile().widgets->instances->begin());
    TEST_CHECK(draft.save());
    TEST_CHECK(config.config().shell.greeter.widgets->instances->size() == 5);
    TEST_CHECK(config.config().shell.greeter.widgets->instances->front().id == "clock2");
    config.forceReload();
    TEST_CHECK(config.config().shell.greeter.widgets->instances->size() == 5);
    const auto saved = readFile(settingsPath);
    draft.profile().widgets->instances->front().type = "weather";
    TEST_CHECK(!draft.save());
    TEST_CHECK(!draft.error().empty());
    TEST_CHECK(readFile(settingsPath) == saved);
    draft.discard();
    draft.profile().widgets = GreeterWidgetCollection{};
    TEST_CHECK(draft.save());
    TEST_CHECK(config.config().shell.greeter.widgets && !config.config().shell.greeter.widgets->instances);
    TEST_CHECK(toml::parse_file(settingsPath.string())["shell"]["greeter"]["widgets"].as_table()->empty());
    draft.profile().widgets->instances.emplace();
    TEST_CHECK(draft.save());
    TEST_CHECK(config.config().shell.greeter.widgets->instances->empty());
    config.forceReload();
    TEST_CHECK(config.config().shell.greeter.widgets->instances->empty());
    draft.profile().widgets.reset();
    TEST_CHECK(draft.save());
    TEST_CHECK(config.config().shell.greeter.widgets->instances->size() == 6);

    // Resetting a GUI field restores the declarative Noctalia value, without changing that file.
    draft.profile().layout->components.at("login").width = 600;
    TEST_CHECK(draft.save());
    draft.profile().layout->components.at("login").width.reset();
    TEST_CHECK(draft.save());
    TEST_CHECK(config.config().shell.greeter.layout->components.at("login").width == 420);

    GreeterConfigDraft stale(config);
    stale.profile().layout->loginMode = GreeterLoginMode::Full;
    auto external = config.config().shell.greeter;
    external.layout->loginMode = GreeterLoginMode::Auto;
    TEST_CHECK(config.setGreeterConfig(external));
    const auto externalText = readFile(settingsPath);
    stale.refresh();
    TEST_CHECK(stale.backingChanged());
    TEST_CHECK(stale.profile().layout->loginMode == GreeterLoginMode::Full);
    TEST_CHECK(!stale.save());
    TEST_CHECK(readFile(settingsPath) == externalText);
    stale.keepDraft();
    TEST_CHECK(!stale.backingChanged() && stale.dirty());
    TEST_CHECK(stale.save());
    TEST_CHECK(config.config().shell.greeter.layout->loginMode == GreeterLoginMode::Full);

    GreeterConfigDraft reload(config);
    reload.profile().layout->loginMode = GreeterLoginMode::Minimal;
    auto disk = toml::parse_file(settingsPath.string());
    disk["shell"]["greeter"]["layout"].as_table()->insert_or_assign("login_mode", "auto");
    writeFile(settingsPath, text(disk));
    reloadFromWatch(config, reloads);
    const auto latest = readFile(settingsPath);
    // Save itself checks even if the editor has not yet processed a reload callback/update.
    TEST_CHECK(!reload.save());
    reload.discard();
    TEST_CHECK(!reload.backingChanged() && !reload.dirty());
    TEST_CHECK(reload.profile().layout->loginMode == GreeterLoginMode::Auto);
    TEST_CHECK(readFile(settingsPath) == latest);

    const auto validProfile = config.config().shell.greeter;
    auto badDisk = toml::parse_file(settingsPath.string());
    badDisk["shell"]["greeter"]["widgets"]["widget"]["clock1"].as_table()->insert_or_assign("type", "weather");
    writeFile(settingsPath, text(badDisk));
    reloadFromWatch(config, reloads);
    TEST_CHECK(config.config().shell.greeter == validProfile);
    TEST_CHECK(readFile(settingsPath) == text(badDisk));
    writeFile(settingsPath, latest);
    reloadFromWatch(config, reloads);
    TEST_CHECK(config.config().desktopWidgets == desktop);
    TEST_CHECK(config.config().lockscreenWidgets == lockscreen);
    TEST_CHECK(readFile(configPath) == baseText);
    TEST_CHECK(readFile(adminPath) == "[layout.login]\nwidth=500\n");
    TEST_CHECK(readFile(installedPath) == "[appearance]\nscheme='Synced'\n");
  }
} // namespace

int main() {
  static const auto root =
      std::filesystem::temp_directory_path() / ("noctalia-greeter-config-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::atexit([] {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
  TEST_CHECK(::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1) == 0);
  TEST_CHECK(::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1) == 0);
  TEST_CHECK(::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1) == 0);
  TEST_CHECK(::setenv("XDG_CONFIG_DIRS", (root / "system").c_str(), 1) == 0);
  TEST_CHECK(::setenv("NOCTALIA_GREETER_STATE_DIR", (root / "greeter").c_str(), 1) == 0);
  TEST_CHECK(::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1) == 0);
  checkSchema();
  checkPersistence(root);
}
