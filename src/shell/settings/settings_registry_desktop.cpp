#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addDesktopSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Desktop, "widgets", tr("settings.schema.desktop.widgets.label"),
        tr("settings.schema.desktop.widgets.description"), {"desktop_widgets", "enabled"},
        ToggleSetting{cfg.desktopWidgets.enabled}, "desktop"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Desktop, "screen-corners", tr("settings.schema.desktop.screen-corners-enabled.label"),
        tr("settings.schema.desktop.screen-corners-enabled.description"), {"shell", "screen_corners", "enabled"},
        ToggleSetting{cfg.shell.screenCorners.enabled}, "screen corners rounded"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Desktop, "screen-corners", tr("settings.schema.desktop.screen-corners-size.label"),
        tr("settings.schema.desktop.screen-corners-size.description"), {"shell", "screen_corners", "size"},
        sliderFor(cfg.shell.screenCorners.size, noctalia::config::schema::kScreenCornersSizeRange, true),
        "screen corners radius"
    ));

    entries.push_back(makeEntry(
        SettingsSection::Desktop, "hot-corners", tr("settings.schema.desktop.hot-corners-enabled.label"),
        tr("settings.schema.desktop.hot-corners-enabled.description"), {"hot_corners", "enabled"},
        ToggleSetting{cfg.hotCorners.enabled}, "hot corners trigger mouse edge screen"
    ));
    {
      auto delay = sliderFor(
          static_cast<std::int64_t>(cfg.hotCorners.delayMs), noctalia::config::schema::kHotCornersDelayMsRange, true
      );
      delay.valueSuffix = "ms";
      SettingEntry e = makeEntry(
          SettingsSection::Desktop, "hot-corners", tr("settings.schema.desktop.hot-corners-delay-ms.label"),
          tr("settings.schema.desktop.hot-corners-delay-ms.description"), {"hot_corners", "delay_ms"}, std::move(delay),
          "hot corners delay hold ms timeout"
      );
      e.visibleWhen = [](const Config& conf) { return conf.hotCorners.enabled; };
      entries.push_back(std::move(e));
    }

    auto hotCornerActionSelect = [](const std::string& current) {
      return plainSelect(
          {{"none", "settings.options.hot-corners.none"},
           {"launcher", "settings.options.hot-corners.launcher"},
           {"control_center", "settings.options.hot-corners.control-center"},
           {"window_switcher", "settings.options.hot-corners.window-switcher"},
           {"command", "settings.options.hot-corners.command"}},
          current
      );
    };

    auto addCornerEntry = [&](const std::string& key, const std::string& labelKey, const std::string& currentAction,
                              const std::string& currentCommand) {
      SettingEntry e = makeEntry(
          SettingsSection::Desktop, "hot-corners", tr(labelKey + ".label"), tr(labelKey + ".description"),
          {"hot_corners", key, "action"}, hotCornerActionSelect(currentAction), "hot corners " + key
      );
      e.visibleWhen = [](const Config& conf) { return conf.hotCorners.enabled; };
      entries.push_back(std::move(e));

      SettingEntry c = makeEntry(
          SettingsSection::Desktop, "hot-corners", tr(labelKey + "-command.label"),
          tr(labelKey + "-command.description"), {"hot_corners", key, "command"},
          TextSetting{.value = currentCommand, .placeholder = "Run command..."}, "hot corners command execute " + key
      );
      c.visibleWhen = [key](const Config& conf) {
        if (!conf.hotCorners.enabled) {
          return false;
        }
        const HotCornersConfig::Corner* corner = nullptr;
        if (key == "top_left") {
          corner = &conf.hotCorners.topLeft;
        } else if (key == "top_right") {
          corner = &conf.hotCorners.topRight;
        } else if (key == "bottom_left") {
          corner = &conf.hotCorners.bottomLeft;
        } else if (key == "bottom_right") {
          corner = &conf.hotCorners.bottomRight;
        }
        return corner != nullptr && corner->action == "command";
      };
      entries.push_back(std::move(c));
    };

    addCornerEntry(
        "top_left", "settings.schema.desktop.hot-corners-top-left", cfg.hotCorners.topLeft.action,
        cfg.hotCorners.topLeft.command
    );
    addCornerEntry(
        "top_right", "settings.schema.desktop.hot-corners-top-right", cfg.hotCorners.topRight.action,
        cfg.hotCorners.topRight.command
    );
    addCornerEntry(
        "bottom_left", "settings.schema.desktop.hot-corners-bottom-left", cfg.hotCorners.bottomLeft.action,
        cfg.hotCorners.bottomLeft.command
    );
    addCornerEntry(
        "bottom_right", "settings.schema.desktop.hot-corners-bottom-right", cfg.hotCorners.bottomRight.action,
        cfg.hotCorners.bottomRight.command
    );
  }

} // namespace settings::registry
