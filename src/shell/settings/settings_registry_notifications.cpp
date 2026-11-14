#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addOsdSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-enabled.label"),
        tr("settings.schema.shell.osd-enabled.description"), {"osd", "enabled"}, ToggleSetting{cfg.osd.enabled},
        "hud overlay master enable disable all"
    ));
    const std::size_t osdGatedStart = entries.size();
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-orientation.label"),
        tr("settings.schema.shell.osd-orientation.description"), {"osd", "orientation"},
        asSegmented(plainSelect(
            {{"horizontal", "settings.options.orientation.horizontal"},
             {"vertical", "settings.options.orientation.vertical"}},
            cfg.osd.orientation
        )),
        "hud overlay volume brightness vertical"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-position.label"),
        tr("settings.schema.shell.osd-position.description"), {"osd", "position"},
        plainSelect(
            {{"top_right", "settings.options.screen-position.top-right"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"},
             {"center_right", "settings.options.screen-position.center-right"},
             {"center_left", "settings.options.screen-position.center-left"},
             {"center", "settings.options.screen-position.center"}},
            cfg.osd.position
        ),
        "hud overlay volume brightness horizontal text"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-position-vertical.label"),
        tr("settings.schema.shell.osd-position-vertical.description"), {"osd", "position_vertical"},
        plainSelect(
            {{"top_right", "settings.options.screen-position.top-right"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"},
             {"center_right", "settings.options.screen-position.center-right"},
             {"center_left", "settings.options.screen-position.center-left"},
             {"center", "settings.options.screen-position.center"}},
            cfg.osd.positionVertical
        ),
        "hud overlay volume brightness vertical slider"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-hide-delay.label"),
        tr("settings.schema.shell.osd-hide-delay.description"), {"osd", "hide_delay_ms"},
        StepperSetting{
            .value = static_cast<int>(cfg.osd.hideDelayMs),
            .minValue = static_cast<int>(noctalia::config::schema::kOsdHideDelayMsRange.min.value()),
            .maxValue = static_cast<int>(noctalia::config::schema::kOsdHideDelayMsRange.max.value()),
            .step = static_cast<int>(noctalia::config::schema::kOsdHideDelayMsRange.step.value()),
            .valueSuffix = "ms",
        },
        "hud overlay popup timeout duration visible"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-scale.label"),
        tr("settings.schema.shell.osd-scale.description"), {"osd", "scale"},
        sliderFor(cfg.osd.scale, noctalia::config::schema::kScaleRange, false),
        "hud overlay volume brightness size scale multiplier"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-offset-x.label"),
        tr("settings.schema.shell.osd-offset-x.description"), {"osd", "offset_x"},
        StepperSetting{.value = cfg.osd.offsetX, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"},
        "hud overlay horizontal margin"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-offset-y.label"),
        tr("settings.schema.shell.osd-offset-y.description"), {"osd", "offset_y"},
        StepperSetting{.value = cfg.osd.offsetY, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"},
        "hud overlay vertical margin"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-background-opacity.label"),
        tr("settings.schema.shell.osd-background-opacity.description"), {"osd", "background_opacity"},
        sliderFor(cfg.osd.backgroundOpacity, noctalia::config::schema::kUnitRange, false),
        "hud overlay popup alpha transparency translucent opacity"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-border.label"),
        tr("settings.schema.shell.osd-border.description"), {"osd", "border"}, ToggleSetting{cfg.osd.border},
        "outline border"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-border-color.label"),
        tr("settings.schema.shell.osd-border-color.description"), {"osd", "border_color"},
        colorSpecPicker(cfg.osd.borderColor), "outline border color theme accent"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-border-width.label"),
        tr("settings.schema.shell.osd-border-width.description"), {"osd", "border_width"},
        sliderFor(cfg.osd.borderWidth, noctalia::config::schema::kOsdBorderWidthRange, false),
        "outline border width thickness"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-follow-focused-output.label"),
        tr("settings.schema.shell.osd-follow-focused-output.description"), {"osd", "follow_focused_output"},
        ToggleSetting{cfg.osd.followFocusedOutput}, "monitor output display focused active hud overlay"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Osd, "osd", tr("settings.schema.shell.osd-monitors.label"),
          tr("settings.schema.shell.osd-monitors.description"), {"osd", "monitors"},
          ListSetting{.items = cfg.osd.monitors, .suggestedOptions = env.availableOutputs},
          "monitor output display screen hud overlay"
      );
      e.visibleWhen = [](const Config& c) { return !c.osd.followFocusedOutput; };
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-volume.label"),
        tr("settings.schema.shell.osd-kinds-volume.description"), {"osd", "kinds", "volume"},
        ToggleSetting{cfg.osd.kinds.volume}, "hud overlay audio output input microphone"
    ));
    {
      const SettingVisibility volumeOn = [](const Config& c) { return c.osd.kinds.volume; };
      SettingEntry outputEntry = makeEntry(
          SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-volume-output.label"),
          tr("settings.schema.shell.osd-kinds-volume-output.description"), {"osd", "kinds", "volume_output"},
          ToggleSetting{cfg.osd.kinds.volumeOutput}, "hud overlay audio speaker sink output"
      );
      outputEntry.advanced = true;
      outputEntry.visibleWhen = volumeOn;
      entries.push_back(std::move(outputEntry));
      SettingEntry inputEntry = makeEntry(
          SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-volume-input.label"),
          tr("settings.schema.shell.osd-kinds-volume-input.description"), {"osd", "kinds", "volume_input"},
          ToggleSetting{cfg.osd.kinds.volumeInput}, "hud overlay audio microphone source input"
      );
      inputEntry.advanced = true;
      inputEntry.visibleWhen = volumeOn;
      entries.push_back(std::move(inputEntry));
    }
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-brightness.label"),
        tr("settings.schema.shell.osd-kinds-brightness.description"), {"osd", "kinds", "brightness"},
        ToggleSetting{cfg.osd.kinds.brightness}, "hud overlay display backlight"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-wifi.label"),
        tr("settings.schema.shell.osd-kinds-wifi.description"), {"osd", "kinds", "wifi"},
        ToggleSetting{cfg.osd.kinds.wifi}, "hud overlay wireless network"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-bluetooth.label"),
        tr("settings.schema.shell.osd-kinds-bluetooth.description"), {"osd", "kinds", "bluetooth"},
        ToggleSetting{cfg.osd.kinds.bluetooth}, "hud overlay bt"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-power-profile.label"),
        tr("settings.schema.shell.osd-kinds-power-profile.description"), {"osd", "kinds", "power_profile"},
        ToggleSetting{cfg.osd.kinds.powerProfile}, "hud overlay balanced performance power saver"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-caffeine.label"),
        tr("settings.schema.shell.osd-kinds-caffeine.description"), {"osd", "kinds", "caffeine"},
        ToggleSetting{cfg.osd.kinds.caffeine}, "hud overlay idle inhibitor"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-nightlight.label"),
        tr("settings.schema.shell.osd-kinds-nightlight.description"), {"osd", "kinds", "nightlight"},
        ToggleSetting{cfg.osd.kinds.nightlight}, "hud overlay night light gamma"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-dnd.label"),
        tr("settings.schema.shell.osd-kinds-dnd.description"), {"osd", "kinds", "dnd"},
        ToggleSetting{cfg.osd.kinds.dnd}, "hud overlay do not disturb notifications"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-lock-keys.label"),
        tr("settings.schema.shell.osd-kinds-lock-keys.description"), {"osd", "kinds", "lock_keys"},
        ToggleSetting{cfg.osd.kinds.lockKeys}, "hud overlay caps num scroll keyboard"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-keyboard-layout.label"),
        tr("settings.schema.shell.osd-kinds-keyboard-layout.description"), {"osd", "kinds", "keyboard_layout"},
        ToggleSetting{cfg.osd.kinds.keyboardLayout}, "hud overlay xkb input language layout switch"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-media.label"),
        tr("settings.schema.shell.osd-kinds-media.description"), {"osd", "kinds", "media"},
        ToggleSetting{cfg.osd.kinds.media}, "hud overlay mpris audio music"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-media-volume.label"),
        tr("settings.schema.shell.osd-kinds-media-volume.description"), {"osd", "kinds", "media_volume"},
        ToggleSetting{cfg.osd.kinds.mediaVolume}, "hud overlay mpris media player volume level"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-privacy.label"),
        tr("settings.schema.shell.osd-kinds-privacy.description"), {"osd", "kinds", "privacy"},
        ToggleSetting{cfg.osd.kinds.privacy}, "hud overlay microphone camera screen share recording"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-keyboard-backlight.label"),
        tr("settings.schema.shell.osd-kinds-keyboard-backlight.description"), {"osd", "kinds", "keyboard_backlight"},
        ToggleSetting{cfg.osd.kinds.keyboardBacklight}, "hud overlay keyboard backlight kbd"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Osd, "kinds", tr("settings.schema.shell.osd-kinds-custom.label"),
        tr("settings.schema.shell.osd-kinds-custom.description"), {"osd", "kinds", "custom"},
        ToggleSetting{cfg.osd.kinds.custom}, "hud overlay plugin custom script"
    ));
    // Gate every OSD entry after the master toggle on osd.enabled, preserving any per-entry visibility.
    for (std::size_t i = osdGatedStart; i < entries.size(); ++i) {
      SettingVisibility prev = std::move(entries[i].visibleWhen);
      entries[i].visibleWhen = prev
          ? SettingVisibility{[prev = std::move(prev)](const Config& c) { return c.osd.enabled && prev(c); }}
          : SettingVisibility{[](const Config& c) { return c.osd.enabled; }};
    }
  }

  void addNotificationSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "general", tr("settings.schema.notifications.daemon.label"),
        tr("settings.schema.notifications.daemon.description"), {"notification", "enable_daemon"},
        ToggleSetting{cfg.notification.enableDaemon}, "dbus"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "general", tr("settings.schema.shared.colorize-icons.label"),
        tr("settings.schema.shared.colorize-icons.description"), {"notification", "colorize_icons"},
        ToggleSetting{cfg.notification.colorizeIcons}, "notification application icons colorize tint"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "general", tr("settings.schema.notifications.show-app-name.label"),
        tr("settings.schema.notifications.show-app-name.description"), {"notification", "show_app_name"},
        ToggleSetting{cfg.notification.showAppName}, "application identity header"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "general", tr("settings.schema.notifications.show-actions.label"),
        tr("settings.schema.notifications.show-actions.description"), {"notification", "show_actions"},
        ToggleSetting{cfg.notification.showActions}, "action buttons"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "general", tr("settings.schema.notifications.show-progress-bar.label"),
        tr("settings.schema.notifications.show-progress-bar.description"), {"notification", "show_progress_bar"},
        ToggleSetting{cfg.notification.showProgressBar},
        "notification toast timeout progress bar countdown indicator critical accent"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "general", tr("settings.schema.notifications.collapse-on-dismiss.label"),
        tr("settings.schema.notifications.collapse-on-dismiss.description"), {"notification", "collapse_on_dismiss"},
        ToggleSetting{cfg.notification.collapseOnDismiss}, "reorder stack slide"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.layer.label"),
        tr("settings.schema.notifications.layer.description"), {"notification", "layer"},
        asSegmented(plainSelect(
            {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
            cfg.notification.layer
        )),
        "toast layer shell z-order"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.position.label"),
        tr("settings.schema.notifications.position.description"), {"notification", "position"},
        plainSelect(
            {{"top_right", "settings.options.screen-position.top-right"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"}},
            cfg.notification.position
        ),
        "toast popup placement anchor"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.newest-on-edge.label"),
        tr("settings.schema.notifications.newest-on-edge.description"), {"notification", "newest_on_edge"},
        ToggleSetting{cfg.notification.newestOnEdge}, "toast stack order newest oldest push direction"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.scale.label"),
        tr("settings.schema.notifications.scale.description"), {"notification", "scale"},
        sliderFor(cfg.notification.scale, noctalia::config::schema::kScaleRange, false), "toast size scale"
    ));
    {
      SliderSetting width = sliderFor(cfg.notification.width, noctalia::config::schema::kNotificationWidthRange, true);
      width.valueSuffix = "px";
      entries.push_back(makeEntry(
          SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.width.label"),
          tr("settings.schema.notifications.width.description"), {"notification", "width"}, std::move(width),
          "toast size dimension wide narrow"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.offset-x.label"),
        tr("settings.schema.notifications.offset-x.description"), {"notification", "offset_x"},
        StepperSetting{
            .value = cfg.notification.offsetX, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"
        },
        "offset margin horizontal"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.offset-y.label"),
        tr("settings.schema.notifications.offset-y.description"), {"notification", "offset_y"},
        StepperSetting{
            .value = cfg.notification.offsetY, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"
        },
        "offset margin vertical"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.max-visible.label"),
        tr("settings.schema.notifications.max-visible.description"), {"notification", "max_visible"},
        StepperSetting{
            .value = cfg.notification.maxVisible, .minValue = 0, .maxValue = 20, .step = 1, .valueSuffix = ""
        },
        "maximum on-screen toasts count limit"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.toast-opacity.label"),
        tr("settings.schema.notifications.toast-opacity.description"), {"notification", "background_opacity"},
        sliderFor(cfg.notification.backgroundOpacity, noctalia::config::schema::kUnitRange, false),
        "popup alpha transparency translucent opacity"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.border.label"),
        tr("settings.schema.notifications.border.description"), {"notification", "border"},
        ToggleSetting{cfg.notification.border}, "outline border"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.border-color.label"),
        tr("settings.schema.notifications.border-color.description"), {"notification", "border_color"},
        colorSpecPicker(cfg.notification.borderColor), "outline border color theme accent"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.border-width.label"),
        tr("settings.schema.notifications.border-width.description"), {"notification", "border_width"},
        sliderFor(cfg.notification.borderWidth, noctalia::config::schema::kNotificationToastBorderWidthRange, false),
        "outline border width thickness"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.follow-focused-output.label"),
        tr("settings.schema.notifications.follow-focused-output.description"),
        {"notification", "follow_focused_output"}, ToggleSetting{cfg.notification.followFocusedOutput},
        "monitor output display focused active"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Notifications, "toasts", tr("settings.schema.notifications.monitors.label"),
          tr("settings.schema.notifications.monitors.description"), {"notification", "monitors"},
          ListSetting{.items = cfg.notification.monitors, .suggestedOptions = env.availableOutputs},
          "monitor output display screen"
      );
      e.visibleWhen = [](const Config& c) { return !c.notification.followFocusedOutput; };
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "history", tr("settings.schema.notifications.group-by-app.label"),
        tr("settings.schema.notifications.group-by-app.description"), {"notification", "group_by_app"},
        ToggleSetting{cfg.notification.groupByApp}, "notification history group-by-app"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "history", tr("settings.schema.notifications.keep-dismissed-in-history.label"),
        tr("settings.schema.notifications.keep-dismissed-in-history.description"),
        {"notification", "keep_dismissed_in_history"}, ToggleSetting{cfg.notification.keepDismissedInHistory},
        "history dismissed toast remove keep"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "history", tr("settings.schema.notifications.history-retention-hours.label"),
        tr("settings.schema.notifications.history-retention-hours.description"),
        {"notification", "history_retention_hours"},
        StepperSetting{
            .value = cfg.notification.historyRetentionHours,
            .minValue = 0,
            .maxValue = 8760,
            .step = 1,
            .valueSuffix = "h",
        },
        "history retention hours clear"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "filtering", tr("settings.schema.notifications.filters.label"),
        tr("settings.schema.notifications.filters.description"), {"notification", "filter"},
        NotificationFiltersSetting{.items = cfg.notification.filters},
        "filter blacklist suppress toast history sound dnd bypass do not disturb app name desktop entry category "
        "urgency"
    ));
  }

} // namespace settings::registry
