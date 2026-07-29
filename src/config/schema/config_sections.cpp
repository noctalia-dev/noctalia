#include "config/schema/config_sections.h"

#include "config/schema/config_schema.h"
#include "config/schema/engine.h"

#include <algorithm>
#include <array>
#include <utility>

namespace noctalia::config::schema {
  namespace {

    template <typename T> SectionSpec makeSection(std::string_view name, T Config::* member, const Schema<T>& schema) {
      return SectionSpec{
          .name = name,
          .read =
              [name, member, &schema](const toml::table& tbl, Config& out, Diagnostics& diag) {
                T candidate = out.*member;
                readInto(tbl, candidate, schema, name, diag);
                out.*member = std::move(candidate);
              },
          .write = [member, &schema](const Config& in) { return writeTable(in.*member, schema); },
          .collectUnknown = [name, &schema](
                                const toml::table& tbl, std::vector<std::string>& unknown
                            ) { collectUnknownKeys(tbl, schema, name, unknown); },
          .checkAgainstDefaults =
              [name, &schema](const toml::table& tbl, Diagnostics& diag) {
                T defaults{};
                readInto(tbl, defaults, schema, name, diag);
              },
          .sectionEqual = [member](const Config& a, const Config& b) { return a.*member == b.*member; },
          .allowUnknownPaths = {},
      };
    }

    const std::vector<SectionSpec>& sectionTable() {
      static const std::vector<SectionSpec> table = [] {
        std::vector<SectionSpec> t;

        t.push_back(makeSection("storage", &Config::storage, storageSchema()));
        t.push_back(makeSection("shell", &Config::shell, shellSchema()));
        t.push_back(makeSection("accessibility", &Config::accessibility, accessibilitySchema()));

        // [wallpaper] also carries app-managed state (the selected wallpapers), which
        // ConfigService reads separately — those keys are not settings.
        SectionSpec wallpaper = makeSection("wallpaper", &Config::wallpaper, wallpaperSchema());
        wallpaper.allowUnknownPaths = {
            "wallpaper.default", "wallpaper.last", "wallpaper.monitors", "wallpaper.favorite"
        };
        t.push_back(std::move(wallpaper));

        t.push_back(makeSection("theme", &Config::theme, themeSchema()));
        t.push_back(makeSection("backdrop", &Config::backdrop, backdropSchema()));
        t.push_back(makeSection("lockscreen", &Config::lockscreen, lockscreenSchema()));
        t.push_back(makeSection("notification", &Config::notification, notificationSchema()));
        t.push_back(makeSection("osd", &Config::osd, osdSchema()));
        t.push_back(makeSection("system", &Config::system, systemSchema()));
        t.push_back(makeSection("weather", &Config::weather, weatherSchema()));
        t.push_back(makeSection("calendar", &Config::calendar, calendarSchema()));
        t.push_back(makeSection("audio", &Config::audio, audioSchema()));
        t.push_back(makeSection("brightness", &Config::brightness, brightnessSchema()));
        t.push_back(makeSection("battery", &Config::battery, batterySchema()));
        t.push_back(makeSection("nightlight", &Config::nightlight, nightlightSchema()));
        t.push_back(makeSection("location", &Config::location, locationSchema()));
        t.push_back(makeSection("idle", &Config::idle, idleSchema()));
        t.push_back(makeSection("keybinds", &Config::keybinds, keybindsSchema()));
        t.push_back(makeSection("dock", &Config::dock, dockSchema()));
        t.push_back(makeSection("hot_corners", &Config::hotCorners, hotCornersSchema()));
        t.push_back(makeSection("control_center", &Config::controlCenter, controlCenterSchema()));
        t.push_back(makeSection("plugins", &Config::plugins, pluginsSchema()));
        t.push_back(makeSection("hooks", &Config::hooks, hooksSchema()));

        return t;
      }();
      return table;
    }

  } // namespace

  std::span<const SectionSpec> sections() { return sectionTable(); }

  const SectionSpec* findSection(std::string_view name) {
    const std::vector<SectionSpec>& table = sectionTable();
    const auto it = std::ranges::find(table, name, &SectionSpec::name);
    return it == table.end() ? nullptr : &*it;
  }

  std::span<const std::string_view> customRootKeys() {
    static constexpr std::array<std::string_view, 8> kKeys = {
        "bar",
        "widget",
        "desktop_widgets",
        "lockscreen_widgets",
        "plugin_settings",
        "include",
        "config_version",
        // Template palette files (e.g. user-templates.toml) use [config.custom_colors].
        "config",
    };
    return kKeys;
  }

  bool isKnownRootKey(std::string_view name) {
    return findSection(name) != nullptr || std::ranges::find(customRootKeys(), name) != customRootKeys().end();
  }

} // namespace noctalia::config::schema
