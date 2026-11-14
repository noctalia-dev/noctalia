// Locks the atomic set-and-clear contract of ConfigService::mutateOverrides. Switching a calendar
// account to another provider writes the new keys and retires the ones the new provider does not own.
// Doing that as a clear followed by a set publishes an intermediate account that is neither shape and
// fails schema validation, so the mutation must reach the config exactly once.

#include "config/config_migrations.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "shell/tray/tray_settings.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    TEST_CHECK(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  const CalendarConfig::Account* findAccount(const Config& config, std::string_view id) {
    const auto it = std::ranges::find(config.calendar.accounts, id, &CalendarConfig::Account::id);
    return it == config.calendar.accounts.end() ? nullptr : &*it;
  }

  const DockMonitorOverride* findDockMonitorOverride(const Config& config, std::string_view tableName) {
    const auto it = std::ranges::find(config.dock.monitorOverrides, tableName, &DockMonitorOverride::tableName);
    return it == config.dock.monitorOverrides.end() ? nullptr : &*it;
  }

  void checkCircularAvatarMigration(const std::filesystem::path& root) {
    const auto configHome = root / "config";
    const auto stateHome = root / "state";
    const auto configPath = configHome / "noctalia" / "config.toml";
    const auto includePath = configHome / "shared" / "avatar.toml";
    const auto settingsPath = stateHome / "noctalia" / "settings.toml";
    constexpr std::string_view configText =
        "[include]\nfiles = [\"../shared/avatar.toml\"]\nautoload = false\n[shell]\ncircular_avatar = true\n";
    constexpr std::string_view includeText = "[control_center]\ncircular_avatar = true\n";
    writeFile(configPath, configText);
    writeFile(includePath, includeText);
    writeFile(
        settingsPath, "config_version = 19\n[control_center]\ncircular_avatar = false\nshow_avatar_border = false\n"
    );
    TEST_CHECK(::setenv("NOCTALIA_CONFIG_HOME", configHome.c_str(), 1) == 0);
    TEST_CHECK(::setenv("XDG_STATE_HOME", stateHome.c_str(), 1) == 0);
    {
      ConfigService config;
      TEST_CHECK(!config.config().shell.circularAvatar);
      TEST_CHECK(!config.hasOverride({"control_center", "circular_avatar"}));
      TEST_CHECK(config.hasOverride({"shell", "circular_avatar"}));
      TEST_CHECK(!config.config().controlCenter.showAvatarBorder);
    }
    const std::string migratedText = readFile(settingsPath);
    const toml::table migrated = toml::parse(migratedText);
    TEST_CHECK(migrated["config_version"].value<std::int64_t>() == noctalia::config::currentConfigVersion());
    TEST_CHECK(migrated["shell"]["circular_avatar"].value<bool>() == false);
    TEST_CHECK(!migrated["control_center"]["circular_avatar"]);
    TEST_CHECK(migrated["control_center"]["show_avatar_border"].value<bool>() == false);
    TEST_CHECK(readFile(configPath) == configText);
    TEST_CHECK(readFile(includePath) == includeText);
    {
      ConfigService config;
      config.forceReload();
      TEST_CHECK(!config.config().shell.circularAvatar);
    }
    TEST_CHECK(readFile(settingsPath) == migratedText);
    TEST_CHECK(readFile(configPath) == configText);
    TEST_CHECK(readFile(includePath) == includeText);
  }

} // namespace

int main() {
  static const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-override-mutation-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  // TEST_CHECK exits through std::exit, which runs this, so a failed check does not leave the
  // fixture tree behind.
  std::atexit([] {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  });
  writeFile(root / "config" / "noctalia" / "config.toml", "[dock]\nenabled = true\nmonitors = [\"legacy\"]\n");

  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  const std::vector<std::string> typePath{"calendar", "account", "feed", "type"};
  const std::vector<std::string> serverUrlPath{"calendar", "account", "feed", "server_url"};
  const std::vector<std::string> vdirPath{"calendar", "account", "feed", "path"};

  {
    ConfigService config;

    const DockMonitorOverride* legacyDockOverride = findDockMonitorOverride(config.config(), "legacy");
    // legacy dock monitor allow-list is loaded as an override
    TEST_CHECK(legacyDockOverride != nullptr);
    // legacy dock monitor override remains owned by the hand-written config
    TEST_CHECK(!config.isOverrideOnlyDockMonitorOverride("legacy"));

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> create;
    create.emplace_back(std::vector<std::string>{"calendar", "enabled"}, true);
    create.emplace_back(typePath, std::string("ics"));
    create.emplace_back(serverUrlPath, std::string("https://example.com/calendar.ics"));
    TEST_CHECK(config.setOverrides(create));
    TEST_CHECK(config.hasOverride(serverUrlPath));

    int reloads = 0;
    config.addReloadCallback([&reloads] { ++reloads; }, "mutation-test");

    // Switch the account to a local vdir directory. `server_url` is a hard error on a vdir account and
    // `path` is required, so both edits belong to the same commit.
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> switchToVdir;
    switchToVdir.emplace_back(typePath, std::string("vdir"));
    switchToVdir.emplace_back(vdirPath, std::string("/tmp/noctalia-vdir-mutation-test"));
    TEST_CHECK(config.mutateOverrides(switchToVdir, {serverUrlPath}, nullptr));

    // provider switch reaches the config exactly once
    TEST_CHECK(reloads == 1);
    TEST_CHECK(!config.hasOverride(serverUrlPath));
    TEST_CHECK(config.hasOverride(vdirPath));
    TEST_CHECK(config.lastMutationError().empty());

    const CalendarConfig::Account* account = findAccount(config.config(), "feed");
    TEST_CHECK(account != nullptr);
    TEST_CHECK(account->type == "vdir");
    TEST_CHECK(account->path == "/tmp/noctalia-vdir-mutation-test");
    TEST_CHECK(account->serverUrl.empty());

    // A mutation that changes nothing must not commit, so consumers are not woken for a no-op.
    // clearing an absent key succeeds
    TEST_CHECK(config.mutateOverrides({}, {serverUrlPath}, nullptr));
    TEST_CHECK(reloads == 1);

    TEST_CHECK(config.createDockMonitorOverride("eDP-1"));
    // created dock monitor override is GUI-owned
    TEST_CHECK(config.isOverrideOnlyDockMonitorOverride("eDP-1"));
    const DockMonitorOverride* dockOverride = findDockMonitorOverride(config.config(), "eDP-1");
    TEST_CHECK(dockOverride != nullptr);
    // created dock monitor override defaults match to its table name
    TEST_CHECK(dockOverride->match == "eDP-1");

    const std::vector<std::string> dockIconSizePath{"dock", "monitor", "eDP-1", "icon_size"};
    TEST_CHECK(config.setOverride(dockIconSizePath, std::int64_t{42}));
    TEST_CHECK(config.renameDockMonitorOverride("eDP-1", "laptop"));
    // renamed dock monitor override removes the old table
    TEST_CHECK(!config.hasOverride(dockIconSizePath));
    TEST_CHECK(config.hasOverride({"dock", "monitor", "laptop", "icon_size"}));
    dockOverride = findDockMonitorOverride(config.config(), "laptop");
    TEST_CHECK(dockOverride != nullptr && dockOverride->match == "laptop");

    TEST_CHECK(config.deleteDockMonitorOverride("laptop"));
    TEST_CHECK(findDockMonitorOverride(config.config(), "laptop") == nullptr);
  }

  const std::filesystem::path overlayRoot = root / "overlay-case";
  writeFile(overlayRoot / "config" / "noctalia" / "config.toml", "[dock]\nenabled = true\n");
  writeFile(
      overlayRoot / "state" / "noctalia" / "settings.toml", "config_version = 14\n[dock]\nmonitors = [\"DP-1\"]\n"
  );
  ::setenv("NOCTALIA_CONFIG_HOME", (overlayRoot / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (overlayRoot / "state").c_str(), 1);
  {
    ConfigService config;
    const DockMonitorOverride* migrated = findDockMonitorOverride(config.config(), "DP-1");
    // sidecar monitor allow-list disabled the inherited global dock
    TEST_CHECK(!config.config().dock.enabled);
    // sidecar monitor allow-list became an enabled override
    TEST_CHECK(migrated != nullptr && migrated->enabled == true);
    TEST_CHECK(!config.hasOverride({"dock", "monitors"}));
    TEST_CHECK(config.hasOverride({"dock", "monitor", "DP-1", "enabled"}));
  }

  checkCircularAvatarMigration(root / "avatar-migration-case");

  // A bar-inherited widget setting resolves an unset value from the bar, so an explicit value equal to the
  // widget schema default is a real override and must survive the no-op override pruning.
  const std::filesystem::path inheritRoot = root / "bar-inherit-case";
  writeFile(
      inheritRoot / "config" / "noctalia" / "config.toml",
      "[bar.default]\nshow_tooltip = false\n\n[widget.clock]\ntype = \"clock\"\n\n[widget.tray]\ndrawer = "
      "true\ndetached_panel = true\n"
  );
  ::setenv("NOCTALIA_CONFIG_HOME", (inheritRoot / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (inheritRoot / "state").c_str(), 1);
  {
    ConfigService config;
    const std::vector<std::string> tooltipPath{"widget", "clock", "show_tooltip"};
    TEST_CHECK(config.setOverride(tooltipPath, true));
    // explicit widget show_tooltip = true survived pruning
    TEST_CHECK(config.hasOverride(tooltipPath));
    TEST_CHECK(config.clearOverride(tooltipPath));
    // clearing restored inheritance from the bar
    TEST_CHECK(!config.hasOverride(tooltipPath));

    const std::vector<std::string> trayOpacityPath{"widget", "tray", "drawer_background_opacity"};
    TEST_CHECK(!tray::resolvedTrayOptions(config).drawerBackgroundOpacity.has_value());
    TEST_CHECK(config.setOverride(trayOpacityPath, 0.5));
    TEST_CHECK(config.hasOverride(trayOpacityPath));
    TEST_CHECK(tray::resolvedTrayOptions(config).drawerBackgroundOpacity.value_or(0.0F) == 0.5F);
    TEST_CHECK(config.setOverride(trayOpacityPath, 1.0));
    TEST_CHECK(config.hasOverride(trayOpacityPath));
    TEST_CHECK(tray::resolvedTrayOptions(config).drawerBackgroundOpacity.value_or(0.0F) == 1.0F);
  }

  // shell.panel_anchor_bar names a bar: renaming that bar in Settings must carry the key along, or every
  // panel open without a source bar (IPC, shortcuts) fails on the stale name. Deleting it clears the key.
  const std::filesystem::path anchorRoot = root / "anchor-bar-case";
  writeFile(anchorRoot / "config" / "noctalia" / "config.toml", "");
  ::setenv("NOCTALIA_CONFIG_HOME", (anchorRoot / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (anchorRoot / "state").c_str(), 1);
  {
    ConfigService config;
    TEST_CHECK(config.createBarOverride("second"));
    TEST_CHECK(config.setOverride({"shell", "panel_anchor_bar"}, std::string("default")));
    TEST_CHECK(config.renameBarOverride("default", "topbar"));
    TEST_CHECK(config.config().shell.panelAnchorBar == "topbar");
    TEST_CHECK(config.renameBarOverride("second", "other"));
    // renaming another bar left panel_anchor_bar alone
    TEST_CHECK(config.config().shell.panelAnchorBar == "topbar");
    TEST_CHECK(config.deleteBarOverride("topbar"));
    TEST_CHECK(config.config().shell.panelAnchorBar.empty());
  }

  // A times-only location without coordinates is migrated to custom_schedule = true on the fly. An
  // explicit true override is what opts the user out of that migration, so it must survive pruning
  // even though the no-override config resolves to the same value. Otherwise the Settings toggle
  // drops the key again and the legacy-config warning will be back.
  const std::filesystem::path customScheduleRoot = root / "custom-schedule-case";
  writeFile(
      customScheduleRoot / "config" / "noctalia" / "config.toml",
      "[location]\nsunset = \"20:30\"\nsunrise = \"07:30\"\n"
  );
  ::setenv("NOCTALIA_CONFIG_HOME", (customScheduleRoot / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (customScheduleRoot / "state").c_str(), 1);
  {
    ConfigService config;
    const std::vector<std::string> customSchedulePath{"location", "custom_schedule"};

    TEST_CHECK(config.setOverride(customSchedulePath, true));
    TEST_CHECK(config.hasOverride(customSchedulePath));
    TEST_CHECK(config.config().location.customSchedule);

    TEST_CHECK(config.setOverride(customSchedulePath, false));
    TEST_CHECK(config.hasOverride(customSchedulePath));
    TEST_CHECK(!config.config().location.customSchedule);

    TEST_CHECK(config.setOverride(customSchedulePath, true));
    // toggling custom_schedule back on kept the override in settings.toml
    TEST_CHECK(config.hasOverride(customSchedulePath));
    TEST_CHECK(config.config().location.customSchedule);
  }

  return 0;
}
