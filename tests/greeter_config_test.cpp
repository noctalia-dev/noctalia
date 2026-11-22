#include "config/config_service.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "config/schema/greeter_schema.h"
#include "core/files/file_watcher.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/greeter/greeter_config_draft.h"
#include "shell/greeter/greeter_profile_store.h"
#include "tests/test_check.h"

#include <algorithm>
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
#include <sys/stat.h>
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
font_size = 112.0
font_weight = 300
tight_text = true
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

  GreeterProfile read(const toml::table& table) {
    schema::Diagnostics diagnostics;
    auto result = schema::readGreeterConfig(table, {}, diagnostics);
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
    const auto parsed = schema::readGreeterConfig(profile, {}, diagnostics);
    if (parsed) {
      std::println(stderr, "Unexpected valid profile for {}:\n{}", path, text(profile));
    }
    TEST_CHECK(!parsed);
    TEST_CHECK(diagnostics.hasErrors());
    TEST_CHECK(std::ranges::any_of(diagnostics.entries, [&](const auto& entry) { return entry.path == path; }));
  }

  void checkSchema() {
    const auto profile = read(toml::parse(kProfile));
    TEST_CHECK(read(toml::parse(text(schema::writeGreeterConfig(profile)))) == profile);
    const auto perOutput = read(toml::parse(R"(
[layout.login]
anchor='center'
[layout.outputs.HEADLESS-1.login]
x=100
[layout.outputs.HEADLESS-2]
login_mode='minimal'
[layout.outputs.HEADLESS-2.login]
x=-80
[layout.outputs.HEADLESS-2.keyboard]
visible=false
)"));
    TEST_CHECK(perOutput.layout->outputs.size() == 2);
    TEST_CHECK(perOutput.layout->outputs.at("HEADLESS-1").components.at("login").x == 100);
    TEST_CHECK(perOutput.layout->outputs.at("HEADLESS-2").loginMode == GreeterLoginMode::Minimal);
    TEST_CHECK(read(toml::parse(text(schema::writeGreeterConfig(perOutput)))) == perOutput);
    for (const auto* invalidOutput :
         {"[layout.outputs.X.login]\nvisible=false", "[layout.outputs.X.outputs.Y.login]\nx=0",
          "[layout.outputs.X.login]\nwidth=20", "[layout.outputs.X]\nsession='injected'"}) {
      schema::Diagnostics errors;
      TEST_CHECK(!schema::readGreeterConfig(toml::parse(invalidOutput), {}, errors));
    }
    const auto serialized = schema::writeTable(ShellConfig{}, schema::shellSchema());
    TEST_CHECK(!serialized.contains("greeter"));
    TEST_CHECK(!profile.layout->components.at("login").visible.has_value());
    TEST_CHECK(!profile.widgets->instances->at(0).output.has_value());
    TEST_CHECK(profile.widgets->instances->at(1).flipX && profile.widgets->instances->at(1).flipY);
    TEST_CHECK(!profile.widgets->instances->at(1).enabled);
    TEST_CHECK(read(toml::table{}) == GreeterProfile{});
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
      invalid(input, "widgets.widget.clock1." + std::string(field));
    }
    for (const auto& [source, path] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"[layout.nope]", "layout.nope"},
             {"[layout.login]\nvisible=false", "layout.login.visible"},
             {"[layout.login]\noutput='DP-1'", "layout.login.output"},
             {"[layout.login]\nwidth=279", "layout.login.width"},
             {"[layout.power]\nscale=4.1", "layout.power.scale"},
             {"[layout.power]\nscale=true", "layout.power.scale"},
             {"[layout]\nlogin_mode='compact'", "layout.login_mode"},
             {"[widgets]\nwidget_order=['missing']", "widgets.widget_order"},
             {"[widgets.widget.foo]", "widgets.widget_order"},
             {"[widgets]\nwidget_order=[]\n[widgets.widget.login]", "widgets.widget.login"},
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
             {"metrics", "stat", "'gpu_temp'"},
             {"metrics", "stat", "'gpu_usage'"},
             {"metrics", "stat", "'gpu_vram'"},
             {"metrics", "stat", "'gpu_vram_used'"},
             {"metrics", "stat2", "'gpu_temp'"},
             {"metrics", "stat2", "'gpu_usage'"},
             {"metrics", "stat2", "'gpu_vram'"},
             {"metrics", "stat2", "'gpu_vram_used'"},
             {"metrics", "interface", "'eth0'"},
         }) {
      auto input = schema::writeGreeterConfig(profile);
      auto v = toml::parse("value=" + std::string(value));
      input["widgets"]["widget"][id]["settings"].as_table()->insert_or_assign(setting, *v.get("value"));
      invalid(input, "widgets.widget." + std::string(id) + ".settings." + std::string(setting));
    }
    const auto greeterStats = schema::greeterWidgetSettingSchema("sysmon");
    const auto desktopStats = desktop_settings::desktopWidgetSettingSchema("sysmon");
    for (const auto* key : {"stat", "stat2"}) {
      const auto greeterField = std::ranges::find_if(greeterStats, [&](const auto& field) { return field.key == key; });
      const auto desktopField = std::ranges::find_if(desktopStats, [&](const auto& field) { return field.key == key; });
      TEST_CHECK(greeterField != greeterStats.end() && desktopField != desktopStats.end());
      for (const auto* metric : {"gpu_temp", "gpu_usage", "gpu_vram", "gpu_vram_used"}) {
        TEST_CHECK(!std::ranges::contains(greeterField->enumValues, metric));
        TEST_CHECK(std::ranges::contains(desktopField->enumValues, metric));
      }
    }
    TEST_CHECK(schema::greeterWidgetSettingSchema("calendar").empty());
    TEST_CHECK(!desktop_settings::desktopWidgetSettingSchema("calendar").empty());
    auto calendar = schema::writeGreeterConfig(profile);
    calendar["widgets"]["widget"]["caption"].as_table()->insert_or_assign("type", "calendar");
    invalid(calendar, "widgets.widget.caption.type");
    auto input = schema::writeGreeterConfig(profile);
    input["widgets"]["widget_order"].as_array()->push_back("clock1");
    invalid(input, "widgets.widget_order");
    input = schema::writeGreeterConfig(profile);
    input["widgets"]["widget_order"].as_array()->erase(input["widgets"]["widget_order"].as_array()->begin());
    invalid(input, "widgets.widget_order");
    input = schema::writeGreeterConfig(profile);
    input["widgets"]["widget"]["clock2"].as_table()->erase("placement_width");
    invalid(input, "widgets.widget.clock2.placement_width");
    auto tooMany = profile;
    for (int index = 0; index < 65; ++index) {
      auto widget = profile.widgets->instances->front();
      widget.id = "widget-" + std::to_string(index);
      tooMany.widgets->instances->push_back(std::move(widget));
    }
    invalid(schema::writeGreeterConfig(tooMany), "widgets.widget_order");
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

  void checkPersistence(const std::filesystem::path& root) {
    const auto configPath = root / "config/noctalia/config.toml";
    const auto settingsPath = root / "state/noctalia/settings.toml";
    TEST_CHECK(::unsetenv("XDG_STATE_HOME") == 0);
    TEST_CHECK(::setenv("HOME", root.c_str(), 1) == 0);
    TEST_CHECK(*greeter::localProfilePath() == root / ".local/state/noctalia-greeter/greeter.toml");
    TEST_CHECK(::setenv("XDG_STATE_HOME", "relative", 1) == 0);
    TEST_CHECK(!greeter::localProfilePath());
    TEST_CHECK(::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1) == 0);
    const auto profilePath = *greeter::localProfilePath();
    TEST_CHECK(profilePath == root / "state/noctalia-greeter/greeter.toml");
    const auto adminPath = root / "installed-greeter/greeter.toml";
    const auto installedPath = root / "installed-greeter/sync.toml";
    writeFile(configPath, "[shell]\noffline_mode=true\n");
    writeFile(settingsPath, "config_version = " + std::to_string(noctalia::config::currentConfigVersion()) + "\n");
    writeFile(adminPath, "[layout.login]\nwidth=500\n");
    writeFile(installedPath, "[appearance]\nscheme='Synced'\n");
    ConfigService config;
    const auto desktop = config.config().desktopWidgets;
    const auto lockscreen = config.config().lockscreenWidgets;
    const auto configBefore = readFile(configPath), settingsBefore = readFile(settingsPath);
    TEST_CHECK(!std::filesystem::exists(profilePath));
    GreeterConfigDraft empty;
    TEST_CHECK(empty.profile() == GreeterProfile{} && empty.save());
    TEST_CHECK(!std::filesystem::exists(profilePath));
    const auto base = read(toml::parse(kProfile));
    TEST_CHECK(greeter::writeLocalProfile(profilePath, base));
    struct stat fileStat{};
    TEST_CHECK(::stat(profilePath.c_str(), &fileStat) == 0 && (fileStat.st_mode & 0777) == 0600);
    GreeterConfigDraft draft;
    TEST_CHECK(draft.profile() == base);
    const auto before = readFile(profilePath);
    draft.profile().widgets->instances->erase(draft.profile().widgets->instances->begin());
    draft.discard();
    TEST_CHECK(!draft.dirty() && readFile(profilePath) == before);
    draft.profile().widgets->instances->erase(draft.profile().widgets->instances->begin());
    TEST_CHECK(draft.save());
    TEST_CHECK(greeter::readLocalProfile(profilePath)->widgets->instances->size() == 5);
    const auto saved = readFile(profilePath);
    draft.profile().widgets->instances->front().type = "weather";
    TEST_CHECK(!draft.save() && !draft.error().empty() && readFile(profilePath) == saved);
    draft.discard();
    draft.profile().widgets = GreeterWidgetCollection{};
    TEST_CHECK(draft.save() && toml::parse_file(profilePath.string())["widgets"].as_table()->empty());
    draft.profile().widgets->instances.emplace();
    TEST_CHECK(draft.save() && greeter::readLocalProfile(profilePath)->widgets->instances->empty());
    draft.profile().widgets.reset();
    TEST_CHECK(draft.save() && !greeter::readLocalProfile(profilePath)->widgets);
    draft.profile().layout->components.at("login").width.reset();
    TEST_CHECK(draft.save() && !greeter::readLocalProfile(profilePath)->layout->components.at("login").width);

    GreeterConfigDraft stale;
    stale.profile().layout->loginMode = GreeterLoginMode::Full;
    auto external = *greeter::readLocalProfile(profilePath);
    external.layout->loginMode = GreeterLoginMode::Auto;
    TEST_CHECK(greeter::writeLocalProfile(profilePath, external));
    const auto externalText = readFile(profilePath);
    stale.refresh();
    TEST_CHECK(stale.backingChanged() && !stale.save() && readFile(profilePath) == externalText);
    stale.keepDraft();
    TEST_CHECK(!stale.backingChanged() && stale.dirty() && stale.save());
    TEST_CHECK(greeter::readLocalProfile(profilePath)->layout->loginMode == GreeterLoginMode::Full);

    FileWatcher watcher;
    bool changed = false;
    const auto watch = watcher.watch(profilePath, [&] { changed = true; }, FileWatcher::WatchTrigger::WriteCompleted);
    TEST_CHECK(watch != 0);
    external.layout->loginMode = GreeterLoginMode::Minimal;
    TEST_CHECK(greeter::writeLocalProfile(profilePath, external));
    pollfd descriptor{.fd = watcher.fd(), .events = POLLIN};
    TEST_CHECK(::poll(&descriptor, 1, 2000) > 0);
    watcher.dispatch();
    TEST_CHECK(changed);
    watcher.unwatch(watch);
    GreeterConfigDraft reload;
    reload.profile().layout->loginMode = GreeterLoginMode::Full;
    writeFile(profilePath, "[widgets]\nwidget_order='invalid'\n");
    const auto invalidBytes = readFile(profilePath);
    TEST_CHECK(!reload.save() && !reload.error().empty() && readFile(profilePath) == invalidBytes);
    reload.keepDraft();
    TEST_CHECK(reload.backingChanged());
    reload.discard();
    TEST_CHECK(reload.profile().layout->loginMode == GreeterLoginMode::Full);
    GreeterConfigDraft malformed;
    TEST_CHECK(!malformed.error().empty() && !malformed.save());
    writeFile(profilePath, "[widgets\n");
    TEST_CHECK(!greeter::readLocalProfile(profilePath));
    writeFile(profilePath, std::string(256U * 1024U + 1, ' '));
    TEST_CHECK(!greeter::readLocalProfile(profilePath));
    writeFile(profilePath, saved);
    reload.discard();
    TEST_CHECK(!reload.backingChanged() && !reload.dirty() && reload.error().empty());
    std::filesystem::remove(profilePath);
    reload.refresh();
    TEST_CHECK(reload.backingChanged());
    reload.discard();
    TEST_CHECK(reload.profile() == GreeterProfile{} && !reload.dirty());
    TEST_CHECK(greeter::writeLocalProfile(profilePath, base));
    GreeterConfigDraft failedWrite;
    failedWrite.profile().layout->loginMode = GreeterLoginMode::Auto;
    const auto preserved = readFile(profilePath);
    std::filesystem::create_directory(profilePath.string() + ".tmp");
    TEST_CHECK(!failedWrite.save() && !failedWrite.error().empty() && readFile(profilePath) == preserved);
    std::filesystem::remove(profilePath.string() + ".tmp");
    GreeterConfigDraft blocked(root / "blocked/greeter.toml");
    writeFile(root / "blocked", "not a directory");
    blocked.profile() = base;
    TEST_CHECK(!blocked.save() && !blocked.error().empty());
    config.forceReload();
    TEST_CHECK(config.config().desktopWidgets == desktop && config.config().lockscreenWidgets == lockscreen);
    TEST_CHECK(readFile(configPath) == configBefore && readFile(settingsPath) == settingsBefore);
    TEST_CHECK(!toml::parse_file(settingsPath.string())["shell"]["greeter"]);
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
  TEST_CHECK(::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1) == 0);
  checkSchema();
  checkPersistence(root);
}
