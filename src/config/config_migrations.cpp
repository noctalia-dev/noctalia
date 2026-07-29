#include "config/config_migrations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace noctalia::config {
  namespace {

    constexpr int kNegativeBarRadiusMigrationVersion = 1;
    constexpr int kCustomScheduleMigrationVersion = 2;
    constexpr int kWidgetActionsMigrationVersion = 3;
    constexpr int kWidgetGestureSettingsMigrationVersion = 4;
    constexpr int kRemainingWidgetGesturesMigrationVersion = 5;
    constexpr int kCustomButtonCommandsMigrationVersion = 6;
    constexpr int kDeadZoneActionsMigrationVersion = 7;
    constexpr int kLockscreenLoginBoxDeprecatedSettingsMigrationVersion = 8;
    constexpr std::int64_t kMaxBarRadius = 500;
    constexpr std::array<std::string_view, 5> kBarRadiusKeys = {
        "radius", "radius_top_left", "radius_top_right", "radius_bottom_left", "radius_bottom_right",
    };

    bool migrateNegativeRadii(toml::table& table) {
      bool changed = false;
      for (const std::string_view key : kBarRadiusKeys) {
        const auto radius = table[key].value<std::int64_t>();
        if (!radius.has_value() || *radius >= 0) {
          continue;
        }

        const std::int64_t magnitude = *radius <= -kMaxBarRadius ? kMaxBarRadius : -*radius;
        table.insert_or_assign(key, magnitude);
        changed = true;
      }

      if (changed) {
        table.insert_or_assign("concave_edge_corners", true);
      }
      return changed;
    }

    template <typename OnChanged> void migrateNegativeBarRadii(toml::table& root, OnChanged&& onChanged) {
      auto* bars = root["bar"].as_table();
      if (bars == nullptr) {
        return;
      }

      for (auto& [barName, barNode] : *bars) {
        auto* bar = barNode.as_table();
        if (bar == nullptr) {
          continue;
        }

        const std::string barPath = "bar." + std::string(barName.str());
        if (migrateNegativeRadii(*bar)) {
          onChanged(barPath);
        }

        auto* monitors = (*bar)["monitor"].as_table();
        if (monitors == nullptr) {
          continue;
        }
        for (auto& [monitorName, monitorNode] : *monitors) {
          auto* monitor = monitorNode.as_table();
          if (monitor != nullptr && migrateNegativeRadii(*monitor)) {
            onChanged(barPath + ".monitor." + std::string(monitorName.str()));
          }
        }
      }
    }

    void migrateNegativeBarRadiiSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateNegativeBarRadii(root, [&diag](const std::string& path) {
        diag.warn(path, "migrated negative corner radii to concave_edge_corners");
      });
    }

    // sunset/sunrise used to schedule day/night on their own whenever no coordinates were available.
    // They now require custom_schedule. Turn it on for configs that relied on the old behavior,
    // i.e. times are set and no coordinate source is.
    template <typename OnChanged> void migrateCustomSchedule(toml::table& root, OnChanged&& onChanged) {
      auto* location = root["location"].as_table();
      if (location == nullptr || location->contains("custom_schedule")) {
        return;
      }

      const auto sunset = (*location)["sunset"].value<std::string>();
      const auto sunrise = (*location)["sunrise"].value<std::string>();
      if (!sunset.has_value() || sunset->empty() || !sunrise.has_value() || sunrise->empty()) {
        return;
      }

      const bool autoLocate = (*location)["auto_locate"].value_or(false);
      const bool hasAddress = !(*location)["address"].value_or(std::string_view{}).empty();
      const bool hasCoordinates = (*location)["latitude"].is_number() && (*location)["longitude"].is_number();
      if (autoLocate || hasAddress || hasCoordinates) {
        return;
      }

      location->insert_or_assign("custom_schedule", true);
      onChanged("location");
    }

    void migrateCustomScheduleSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateCustomSchedule(root, [&diag](const std::string& path) {
        diag.warn(path, "sunset/sunrise now require custom_schedule; enabled it to keep the schedule");
      });
    }

    // `shell.middle_click_opens_widget_settings` became a gesture binding. Only the disabled case
    // needs carrying over: opening widget settings on middle click is now the built-in default.
    template <typename OnChanged> void migrateWidgetActions(toml::table& root, OnChanged&& onChanged) {
      auto* shell = root["shell"].as_table();
      if (shell == nullptr || !shell->contains("middle_click_opens_widget_settings")) {
        return;
      }

      const bool wasEnabled = (*shell)["middle_click_opens_widget_settings"].value_or(true);
      shell->erase("middle_click_opens_widget_settings");
      onChanged("shell");
      if (wasEnabled) {
        return;
      }

      // Unbind it on every configured bar, since the setting used to be global. A config with no
      // [bar] table at all uses the built-in default bar, so seed that one.
      auto* bars = root["bar"].as_table();
      if (bars == nullptr) {
        root.insert_or_assign("bar", toml::table{});
        bars = root["bar"].as_table();
      }
      if (bars->empty()) {
        bars->insert_or_assign("default", toml::table{});
      }

      for (auto& [barName, barNode] : *bars) {
        auto* bar = barNode.as_table();
        if (bar == nullptr) {
          continue;
        }
        auto* actions = (*bar)["actions"].as_table();
        if (actions == nullptr) {
          bar->insert_or_assign("actions", toml::table{});
          actions = (*bar)["actions"].as_table();
        }
        if (actions->contains("middle")) {
          continue;
        }
        actions->insert_or_assign("middle", "none");
        onChanged("bar." + std::string(barName.str()) + ".actions");
      }
    }

    // Writes a gesture binding, leaving an existing one alone: an explicit config always wins over
    // whatever a migrated setting would have implied.
    void bindAction(toml::table& widget, std::string_view gesture, const std::string& action) {
      auto* actions = widget["actions"].as_table();
      if (actions == nullptr) {
        widget.insert_or_assign("actions", toml::table{});
        actions = widget["actions"].as_table();
      }
      if (!actions->contains(gesture)) {
        actions->insert_or_assign(gesture, action);
      }
    }

    // `enable_scroll` and `cycle_command` were per-widget stand-ins for gestures these widgets now
    // declare as data. Both become ordinary bindings.
    template <typename OnChanged> void migrateWidgetGestureSettings(toml::table& root, OnChanged&& onChanged) {
      static constexpr std::array<std::string_view, 3> kScrollTypes{"workspaces", "taskbar", "power_profile"};

      auto* widgets = root["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }

      for (auto& [widgetName, widgetNode] : *widgets) {
        auto* widget = widgetNode.as_table();
        if (widget == nullptr) {
          continue;
        }
        // A widget entry without an explicit `type` is named after its type.
        const std::string type = (*widget)["type"].value_or(std::string(widgetName.str()));
        const std::string path = "widget." + std::string(widgetName.str());

        if (std::ranges::contains(kScrollTypes, type) && widget->contains("enable_scroll")) {
          const bool wasEnabled = (*widget)["enable_scroll"].value_or(true);
          widget->erase("enable_scroll");
          if (!wasEnabled) {
            bindAction(*widget, "scroll_up", "none");
            bindAction(*widget, "scroll_down", "none");
          }
          onChanged(path, "enable_scroll is now the scroll_up/scroll_down gesture bindings");
        }

        if (type == "keyboard_layout" && widget->contains("cycle_command")) {
          const auto command = (*widget)["cycle_command"].value_or(std::string{});
          widget->erase("cycle_command");
          if (!command.empty()) {
            bindAction(*widget, "left", "exec " + command);
          }
          onChanged(path, "cycle_command is now the left gesture binding");
        }
      }
    }

    // Second wave: the widgets migrated in stage 3. `scroll_step` becomes the step argument of the
    // scroll binding, and screenshot's `primary_click` picks the left binding's verb.
    template <typename OnChanged> void migrateRemainingWidgetGestures(toml::table& root, OnChanged&& onChanged) {
      // Both scroll verbs default to 5%, so only a non-default step has to survive as an argument.
      constexpr std::int64_t kDefaultScrollStep = 5;

      auto* widgets = root["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }

      for (auto& [widgetName, widgetNode] : *widgets) {
        auto* widget = widgetNode.as_table();
        if (widget == nullptr) {
          continue;
        }
        const std::string type = (*widget)["type"].value_or(std::string(widgetName.str()));
        const std::string path = "widget." + std::string(widgetName.str());
        const bool scrollType = type == "media" || type == "volume" || type == "brightness";

        if (scrollType && widget->contains("enable_scroll")) {
          const bool wasEnabled = (*widget)["enable_scroll"].value_or(true);
          widget->erase("enable_scroll");
          if (!wasEnabled) {
            bindAction(*widget, "scroll_up", "none");
            bindAction(*widget, "scroll_down", "none");
          }
          onChanged(path, "enable_scroll is now the scroll_up/scroll_down gesture bindings");
        }

        if ((type == "volume" || type == "brightness") && widget->contains("scroll_step")) {
          const auto step = (*widget)["scroll_step"].value_or(kDefaultScrollStep);
          widget->erase("scroll_step");
          if (step != kDefaultScrollStep) {
            const std::string suffix = " " + std::to_string(step) + "%";
            std::string upVerb = "brightness-up";
            std::string downVerb = "brightness-down";
            if (type == "volume") {
              const bool microphone = (*widget)["device"].value_or(std::string("output")) == "input";
              upVerb = microphone ? "mic-volume-up" : "volume-up";
              downVerb = microphone ? "mic-volume-down" : "volume-down";
            }
            bindAction(*widget, "scroll_up", upVerb + suffix);
            bindAction(*widget, "scroll_down", downVerb + suffix);
          }
          onChanged(path, "scroll_step is now the step argument of the scroll gesture bindings");
        }

        if (type == "screenshot" && widget->contains("primary_click")) {
          const auto primary = (*widget)["primary_click"].value_or(std::string("region"));
          widget->erase("primary_click");
          if (primary == "fullscreen") {
            bindAction(*widget, "left", "screenshot-fullscreen");
          }
          onChanged(path, "primary_click is now the left gesture binding");
        }
      }
    }

    // custom_button's per-gesture command keys are exactly what `exec` bindings express, and the
    // widget ran them through the same process::runAsync the `exec` verb uses.
    template <typename OnChanged> void migrateCustomButtonCommands(toml::table& root, OnChanged&& onChanged) {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kCommandKeys{{
          {"command", "left"},
          {"right_command", "right"},
          {"middle_command", "middle"},
          {"scroll_up_command", "scroll_up"},
          {"scroll_down_command", "scroll_down"},
      }};

      auto* widgets = root["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }

      for (auto& [widgetName, widgetNode] : *widgets) {
        auto* widget = widgetNode.as_table();
        if (widget == nullptr) {
          continue;
        }
        if ((*widget)["type"].value_or(std::string(widgetName.str())) != "custom_button") {
          continue;
        }
        const std::string path = "widget." + std::string(widgetName.str());

        // Disabled scroll used to win over the scroll commands, so unbind before they migrate.
        if (widget->contains("enable_scroll")) {
          const bool wasEnabled = (*widget)["enable_scroll"].value_or(true);
          widget->erase("enable_scroll");
          if (!wasEnabled) {
            bindAction(*widget, "scroll_up", "none");
            bindAction(*widget, "scroll_down", "none");
          }
          onChanged(path, "enable_scroll is now the scroll_up/scroll_down gesture bindings");
        }

        for (const auto& [key, gesture] : kCommandKeys) {
          if (!widget->contains(key)) {
            continue;
          }
          const auto command = (*widget)[key].value_or(std::string{});
          widget->erase(key);
          if (!command.empty()) {
            bindAction(*widget, gesture, "exec " + command);
          }
          onChanged(path, std::string(key) + " is now the " + std::string(gesture) + " gesture binding");
        }
      }
    }

    // The bar dead zone had the same five command keys custom_button did, running through the same
    // process::runAsync the `exec` verb uses.
    template <typename OnChanged> void migrateDeadZoneActions(toml::table& root, OnChanged&& onChanged) {
      static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kCommandKeys{{
          {"command", "left"},
          {"right_command", "right"},
          {"middle_command", "middle"},
          {"scroll_up_command", "scroll_up"},
          {"scroll_down_command", "scroll_down"},
      }};

      const auto migrateTable = [&onChanged](toml::table& owner, const std::string& path) {
        auto* deadZone = owner["dead_zone"].as_table();
        if (deadZone == nullptr) {
          return;
        }
        for (const auto& [key, gesture] : kCommandKeys) {
          if (!deadZone->contains(key)) {
            continue;
          }
          const auto command = (*deadZone)[key].value_or(std::string{});
          deadZone->erase(key);
          if (!command.empty()) {
            bindAction(*deadZone, gesture, "exec " + command);
          }
          onChanged(path + ".dead_zone", std::string(key) + " is now the " + std::string(gesture) + " binding");
        }
      };

      auto* bars = root["bar"].as_table();
      if (bars == nullptr) {
        return;
      }
      for (auto& [barName, barNode] : *bars) {
        auto* bar = barNode.as_table();
        if (bar == nullptr) {
          continue;
        }
        const std::string barPath = "bar." + std::string(barName.str());
        migrateTable(*bar, barPath);

        auto* monitors = (*bar)["monitor"].as_table();
        if (monitors == nullptr) {
          continue;
        }
        for (auto& [monitorName, monitorNode] : *monitors) {
          if (auto* monitor = monitorNode.as_table(); monitor != nullptr) {
            migrateTable(*monitor, barPath + ".monitor." + std::string(monitorName.str()));
          }
        }
      }
    }

    void migrateDeadZoneActionsSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateDeadZoneActions(root, [&diag](const std::string& path, std::string_view message) {
        diag.warn(path, std::string(message));
      });
    }

    template <typename OnChanged>
    void migrateLockscreenLoginBoxDeprecatedSettings(toml::table& root, OnChanged&& onChanged) {
      auto* section = root["lockscreen_widgets"].as_table();
      if (section == nullptr) {
        return;
      }
      auto* widgets = (*section)["widget"].as_table();
      if (widgets == nullptr) {
        return;
      }

      for (auto& [widgetId, widgetNode] : *widgets) {
        auto* widget = widgetNode.as_table();
        if (widget == nullptr) {
          continue;
        }
        const std::string type = (*widget)["type"].value_or(std::string{});
        const std::string id(widgetId.str());
        if (type != "login_box" && !id.starts_with("lockscreen-login-box@")) {
          continue;
        }
        auto* settings = (*widget)["settings"].as_table();
        if (settings == nullptr) {
          continue;
        }
        if (settings->erase("show_password_hint") > 0) {
          onChanged("lockscreen_widgets.widget." + id + ".settings");
        }
      }
    }

    void migrateLockscreenLoginBoxDeprecatedSettingsSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateLockscreenLoginBoxDeprecatedSettings(root, [&diag](const std::string& path) {
        diag.warn(path, "removed deprecated show_password_hint");
      });
    }

    void migrateCustomButtonCommandsSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateCustomButtonCommands(root, [&diag](const std::string& path, std::string_view message) {
        diag.warn(path, std::string(message));
      });
    }

    void migrateRemainingWidgetGesturesSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateRemainingWidgetGestures(root, [&diag](const std::string& path, std::string_view message) {
        diag.warn(path, std::string(message));
      });
    }

    void migrateWidgetGestureSettingsSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateWidgetGestureSettings(root, [&diag](const std::string& path, std::string_view message) {
        diag.warn(path, std::string(message));
      });
    }

    void migrateWidgetActionsSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateWidgetActions(root, [&diag](const std::string& path) {
        diag.warn(path, "middle_click_opens_widget_settings is now the `middle` widget gesture binding");
      });
    }

    std::uint64_t stableIssueHash(int migrationVersion, std::string_view path) {
      constexpr std::uint64_t kOffset = 14695981039346656037ULL;
      constexpr std::uint64_t kPrime = 1099511628211ULL;
      std::uint64_t hash = kOffset;
      const auto append = [&hash](std::string_view value) {
        for (const unsigned char byte : value) {
          hash ^= byte;
          hash *= kPrime;
        }
      };
      append(std::to_string(migrationVersion));
      append(":");
      append(path);
      return hash;
    }

    std::set<std::string> splitFingerprint(std::string_view fingerprint) {
      std::set<std::string> entries;
      std::size_t start = 0;
      while (start < fingerprint.size()) {
        const std::size_t end = fingerprint.find(',', start);
        const std::size_t length = end == std::string_view::npos ? fingerprint.size() - start : end - start;
        if (length > 0) {
          entries.emplace(fingerprint.substr(start, length));
        }
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
      return entries;
    }

  } // namespace

  const std::vector<ConfigMigration>& configMigrations() {
    static const std::vector<ConfigMigration> migrations = {
        {
            .toVersion = kNegativeBarRadiusMigrationVersion,
            .summary = "bar: migrate negative corner radii",
            .apply = migrateNegativeBarRadiiSidecar,
        },
        {
            .toVersion = kCustomScheduleMigrationVersion,
            .summary = "location: opt legacy sunset/sunrise schedules into custom_schedule",
            .apply = migrateCustomScheduleSidecar,
        },
        {
            .toVersion = kWidgetActionsMigrationVersion,
            .summary = "bar: move middle_click_opens_widget_settings to widget gesture actions",
            .apply = migrateWidgetActionsSidecar,
        },
        {
            .toVersion = kWidgetGestureSettingsMigrationVersion,
            .summary = "widget: move enable_scroll and cycle_command to gesture actions",
            .apply = migrateWidgetGestureSettingsSidecar,
        },
        {
            .toVersion = kRemainingWidgetGesturesMigrationVersion,
            .summary = "widget: move scroll_step and primary_click to gesture actions",
            .apply = migrateRemainingWidgetGesturesSidecar,
        },
        {
            .toVersion = kCustomButtonCommandsMigrationVersion,
            .summary = "widget: move custom_button commands to gesture actions",
            .apply = migrateCustomButtonCommandsSidecar,
        },
        {
            .toVersion = kDeadZoneActionsMigrationVersion,
            .summary = "bar: move dead zone commands to gesture actions",
            .apply = migrateDeadZoneActionsSidecar,
        },
        {
            .toVersion = kLockscreenLoginBoxDeprecatedSettingsMigrationVersion,
            .summary = "lockscreen: drop removed login box show_password_hint setting",
            .apply = migrateLockscreenLoginBoxDeprecatedSettingsSidecar,
        },
    };
    return migrations;
  }

  int currentConfigVersion() {
    const auto& migrations = configMigrations();
    return migrations.empty() ? 0 : migrations.back().toVersion;
  }

  std::optional<int> storedConfigVersion(const toml::table& root, schema::Diagnostics& diag) {
    const toml::node* node = root.get(kConfigVersionKey);
    if (node == nullptr) {
      return 0;
    }

    const auto value = node->value<std::int64_t>();
    if (!value.has_value()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }
    if (*value < 0 || *value > std::numeric_limits<int>::max()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }

    const int version = static_cast<int>(*value);
    if (version > currentConfigVersion()) {
      diag.fatal(
          std::string(kConfigVersionKey),
          std::format("version {} is newer than supported version {}", version, currentConfigVersion()),
          "config.version.unsupported"
      );
      return std::nullopt;
    }
    return version;
  }

  int applyPendingConfigMigrations(
      toml::table& root, int storedVersion, schema::Diagnostics& diag, std::span<const ConfigMigration> migrations
  ) {
    int appliedVersion = storedVersion;
    for (const ConfigMigration& migration : migrations) {
      if (migration.toVersion <= storedVersion) {
        continue;
      }
      migration.apply(root, diag);
      appliedVersion = migration.toVersion;
    }
    return appliedVersion;
  }

  void normalizeLegacyConfig(toml::table& root, LegacyConfigIssues& issues) {
    migrateNegativeBarRadii(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kNegativeBarRadiusMigrationVersion,
          .path = path,
          .message = "negative corner radii are deprecated; use positive radii and concave_edge_corners = true",
      });
    });
    migrateCustomSchedule(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kCustomScheduleMigrationVersion,
          .path = path,
          .message = "sunset/sunrise no longer schedule on their own; set custom_schedule = true",
      });
    });
    migrateWidgetActions(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kWidgetActionsMigrationVersion,
          .path = path,
          .message = "middle_click_opens_widget_settings is now the `middle` bar widget gesture binding",
      });
    });
    migrateWidgetGestureSettings(root, [&issues](const std::string& path, std::string_view message) {
      issues.push_back({
          .migrationVersion = kWidgetGestureSettingsMigrationVersion,
          .path = path,
          .message = std::string(message),
      });
    });
    migrateRemainingWidgetGestures(root, [&issues](const std::string& path, std::string_view message) {
      issues.push_back({
          .migrationVersion = kRemainingWidgetGesturesMigrationVersion,
          .path = path,
          .message = std::string(message),
      });
    });
    migrateCustomButtonCommands(root, [&issues](const std::string& path, std::string_view message) {
      issues.push_back({
          .migrationVersion = kCustomButtonCommandsMigrationVersion,
          .path = path,
          .message = std::string(message),
      });
    });
    migrateDeadZoneActions(root, [&issues](const std::string& path, std::string_view message) {
      issues.push_back({
          .migrationVersion = kDeadZoneActionsMigrationVersion,
          .path = path,
          .message = std::string(message),
      });
    });
    migrateLockscreenLoginBoxDeprecatedSettings(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kLockscreenLoginBoxDeprecatedSettingsMigrationVersion,
          .path = path,
          .message = "removed deprecated show_password_hint",
      });
    });
  }

  std::string legacyConfigIssueFingerprint(const LegacyConfigIssues& issues) {
    std::set<std::string> entries;
    for (const LegacyConfigIssue& issue : issues) {
      entries.insert(std::format("{:016x}", stableIssueHash(issue.migrationVersion, issue.path)));
    }

    std::string fingerprint;
    for (const std::string& entry : entries) {
      if (!fingerprint.empty()) {
        fingerprint.push_back(',');
      }
      fingerprint += entry;
    }
    return fingerprint;
  }

  bool legacyConfigFingerprintHasNewIssues(std::string_view currentFingerprint, std::string_view previousFingerprint) {
    const std::set<std::string> current = splitFingerprint(currentFingerprint);
    const std::set<std::string> previous = splitFingerprint(previousFingerprint);
    return std::ranges::any_of(current, [&previous](const std::string& entry) { return !previous.contains(entry); });
  }

  bool legacyConfigReminderIntervalElapsed(std::int64_t nowEpochSeconds, std::int64_t previousEpochSeconds) {
    return previousEpochSeconds > nowEpochSeconds
        || nowEpochSeconds - previousEpochSeconds >= kLegacyConfigReminderIntervalSeconds;
  }

} // namespace noctalia::config
