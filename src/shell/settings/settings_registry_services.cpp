#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "pipewire/sound_player.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addCalendarSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    const SettingVisibility calendarOn = [](const Config& c) { return c.calendar.enabled; };
    entries.push_back(makeEntry(
        SettingsSection::Calendar, "general", tr("settings.schema.services.calendar.label"),
        tr("settings.schema.services.calendar.description"), {"calendar", "enabled"},
        ToggleSetting{cfg.calendar.enabled}, "calendar events caldav google"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-event-date-format.label"),
          tr("settings.schema.services.calendar-event-date-format.description"), {"calendar", "event_date_format"},
          TextSetting{.value = cfg.calendar.eventDateFormat, .placeholder = "%A %e %B", .browseFileExtensions = {}},
          "calendar date format strftime chrono"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-event-time-format.label"),
          tr("settings.schema.services.calendar-event-time-format.description"), {"calendar", "event_time_format"},
          TextSetting{.value = cfg.calendar.eventTimeFormat, .placeholder = "%H:%M", .browseFileExtensions = {}},
          "calendar time format strftime chrono"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-dedupe-events.label"),
          tr("settings.schema.services.calendar-dedupe-events.description"), {"calendar", "dedupe_events"},
          ToggleSetting{cfg.calendar.dedupeEvents}, "calendar events duplicate merge"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-dedupe-ignore-patterns.label"),
          tr("settings.schema.services.calendar-dedupe-ignore-patterns.description"),
          {"calendar", "dedupe_ignore_patterns"}, ListSetting{.items = cfg.calendar.dedupeIgnorePatterns},
          "calendar events duplicate merge regex title"
      );
      e.visibleWhen = [](const Config& c) { return c.calendar.enabled && c.calendar.dedupeEvents; };
      entries.push_back(std::move(e));
    }
    // Week numbers are a grid decoration, so they stay available when event syncing is off.
    entries.push_back(makeEntry(
        SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-week-numbers.label"),
        tr("settings.schema.services.calendar-week-numbers.description"),
        {"control_center", "calendar", "show_week_numbers"},
        ToggleSetting{cfg.controlCenter.calendarTab.showWeekNumbers}, "calendar week numbers iso"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-show-events-location.label"),
        tr("settings.schema.services.calendar-show-events-location.description"),
        {"control_center", "calendar", "show_events_location"},
        ToggleSetting{cfg.controlCenter.calendarTab.showEventsLocation}, "calendar events location display"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-hide-past-events.label"),
        tr("settings.schema.services.calendar-hide-past-events.description"),
        {"control_center", "calendar", "hide_past_events"}, ToggleSetting{cfg.controlCenter.calendarTab.hidePastEvents},
        "calendar past events today finished hide"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "general", tr("settings.schema.services.calendar-past-events-threshold.label"),
          tr("settings.schema.services.calendar-past-events-threshold.description"),
          {"control_center", "calendar", "past_events_threshold_minutes"},
          sliderFor(
              cfg.controlCenter.calendarTab.pastEventsThresholdMinutes,
              noctalia::config::schema::kPastEventsThresholdMinutesRange, true
          ),
          "calendar past events threshold minutes hide old finished"
      );
      e.visibleWhen = [](const Config& c) { return c.controlCenter.calendarTab.hidePastEvents; };
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-reminders.label"),
          tr("settings.schema.services.calendar-reminders.description"), {"calendar", "reminders", "enabled"},
          ToggleSetting{cfg.calendar.reminders.enabled}, "calendar reminder notification alarm upcoming"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
    const SettingVisibility remindersOn = [](const Config& c) {
      return c.calendar.enabled && c.calendar.reminders.enabled;
    };
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders",
          tr("settings.schema.services.calendar-reminder-use-event.label"),
          tr("settings.schema.services.calendar-reminder-use-event.description"),
          {"calendar", "reminders", "use_event_reminders"}, ToggleSetting{cfg.calendar.reminders.useEventReminders},
          "calendar reminder valarm override event"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-reminder-lead.label"),
          tr("settings.schema.services.calendar-reminder-lead.description"),
          {"calendar", "reminders", "default_lead_minutes"},
          sliderFor(
              cfg.calendar.reminders.defaultLeadMinutes, noctalia::config::schema::kReminderLeadMinutesRange, true
          ),
          "calendar reminder lead minutes before default"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-reminders", tr("settings.schema.services.calendar-digest-time.label"),
          tr("settings.schema.services.calendar-digest-time.description"),
          {"calendar", "reminders", "all_day_digest_time"},
          TextSetting{
              .value = cfg.calendar.reminders.allDayDigestTime, .placeholder = "09:00", .browseFileExtensions = {}
          },
          "calendar all day digest time morning"
      );
      e.visibleWhen = remindersOn;
      entries.push_back(std::move(e));
    }
    // Sync cadence belongs with the accounts it drives; the account rows are injected right after it.
    {
      auto e = makeEntry(
          SettingsSection::Calendar, "calendar-accounts",
          tr("settings.schema.services.calendar-refresh-interval.label"),
          tr("settings.schema.services.calendar-refresh-interval.description"), {"calendar", "refresh_minutes"},
          sliderFor(cfg.calendar.refreshMinutes, noctalia::config::schema::kRefreshMinutesRange, true), "calendar sync"
      );
      e.visibleWhen = calendarOn;
      entries.push_back(std::move(e));
    }
  }

  void addServiceSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.audio-overdrive.label"),
        tr("settings.schema.services.audio-overdrive.description"), {"audio", "enable_overdrive"},
        ToggleSetting{cfg.audio.enableOverdrive}, "volume"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.shared.colorize-icons.label"),
        tr("settings.schema.shared.colorize-icons.description"), {"audio", "colorize_icons"},
        ToggleSetting{cfg.audio.colorizeIcons}, "audio mixer application icons colorize tint"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.shell-sounds.label"),
        tr("settings.schema.services.shell-sounds.description"), {"audio", "enable_sounds"},
        ToggleSetting{cfg.audio.enableSounds}, "sound"
    ));
    const std::size_t soundsGatedStart = entries.size();
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.volume-sounds.label"),
        tr("settings.schema.services.volume-sounds.description"), {"audio", "enable_volume_sounds"},
        ToggleSetting{cfg.audio.enableVolumeSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.notification-sounds.label"),
        tr("settings.schema.services.notification-sounds.description"), {"audio", "enable_notification_sounds"},
        ToggleSetting{cfg.audio.enableNotificationSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.power-sounds.label"),
        tr("settings.schema.services.power-sounds.description"), {"audio", "enable_power_sounds"},
        ToggleSetting{cfg.audio.enablePowerSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.screenshot-sounds.label"),
        tr("settings.schema.services.screenshot-sounds.description"), {"audio", "enable_screenshot_sounds"},
        ToggleSetting{cfg.audio.enableScreenshotSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.sound-volume.label"),
        tr("settings.schema.services.sound-volume.description"), {"audio", "sound_volume"},
        sliderFor(cfg.audio.soundVolume, noctalia::config::schema::kUnitRange, false), "sound"
    ));
    std::vector<SelectOption> soundThemeOptions;
    for (const auto& [value, label] : SoundPlayer::availableThemes()) {
      soundThemeOptions.push_back(SelectOption{value, label});
    }
    if (soundThemeOptions.empty()) {
      soundThemeOptions.push_back(SelectOption{"freedesktop", tr("settings.schema.services.sound-theme.default")});
    }
    entries.push_back(makeEntry(
        SettingsSection::Services, "audio", tr("settings.schema.services.sound-theme.label"),
        tr("settings.schema.services.sound-theme.description"), {"audio", "sound_theme"},
        SelectSetting{std::move(soundThemeOptions), cfg.audio.soundTheme}, "sound theme"
    ));
    for (std::size_t i = soundsGatedStart; i < entries.size(); ++i) {
      SettingVisibility prev = std::move(entries[i].visibleWhen);
      entries[i].visibleWhen = prev
          ? SettingVisibility{[prev = std::move(prev)](const Config& c) { return c.audio.enableSounds && prev(c); }}
          : SettingVisibility{[](const Config& c) { return c.audio.enableSounds; }};
    }
    {
      // A rate set in TOML that is not a preset gets its own option instead of showing as unknown.
      std::vector<int> frameRates{30, 60, 100, 120, 144, 165, 240};
      if (!std::ranges::contains(frameRates, cfg.audio.spectrumFrameRate)) {
        frameRates.insert(
            std::ranges::upper_bound(frameRates, cfg.audio.spectrumFrameRate), cfg.audio.spectrumFrameRate
        );
      }
      std::vector<SelectOption> frameRateOptions;
      for (const int fps : frameRates) {
        const std::string value = std::to_string(fps);
        frameRateOptions.push_back(
            SelectOption{value, tr("settings.schema.services.spectrum-frame-rate.option", "fps", value)}
        );
      }
      SelectSetting frameRateSelect{std::move(frameRateOptions), std::to_string(cfg.audio.spectrumFrameRate)};
      frameRateSelect.valueType = SelectValueType::Integer;
      entries.push_back(makeEntry(
          SettingsSection::Services, "audio", tr("settings.schema.services.spectrum-frame-rate.label"),
          tr("settings.schema.services.spectrum-frame-rate.description"), {"audio", "spectrum_frame_rate"},
          std::move(frameRateSelect), "visualizer spectrum fps"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Services, "brightness", tr("settings.schema.services.ddcutil.label"),
        env.ddcutilAvailable ? tr("settings.schema.services.ddcutil.description")
                             : tr("settings.schema.services.ddcutil.requires-ddcutil"),
        {"brightness", "enable_ddcutil"},
        ToggleSetting{.checked = cfg.brightness.enableDdcutil, .enabled = env.ddcutilAvailable}, "monitor ddcutil"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "brightness", tr("settings.schema.services.minimum-brightness.label"),
        tr("settings.schema.services.minimum-brightness.description"), {"brightness", "minimum_brightness"},
        sliderFor(cfg.brightness.minimumBrightness, noctalia::config::schema::kUnitRange, false), "floor clamp"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "brightness", tr("settings.schema.services.sync-monitor-brightness.label"),
        tr("settings.schema.services.sync-monitor-brightness.description"), {"brightness", "sync_all_monitors"},
        ToggleSetting{.checked = cfg.brightness.syncBrightnessOfAllMonitors}, "monitor brightness"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "bluetooth", tr("settings.schema.services.bluetooth-connect-when-enabled.label"),
        tr("settings.schema.services.bluetooth-connect-when-enabled.description"),
        {"bluetooth", "connect_when_enabled"}, ToggleSetting{cfg.bluetooth.connectWhenEnabled},
        "bluetooth reconnect connect trusted paired devices startup resume"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "bluetooth", tr("settings.schema.services.bluetooth-connection-notifications.label"),
        tr("settings.schema.services.bluetooth-connection-notifications.description"),
        {"bluetooth", "connection_notifications"}, ToggleSetting{cfg.bluetooth.connectionNotifications},
        "bluetooth device connect disconnect notification"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Services, "media", tr("settings.schema.services.mpris-blacklist.label"),
        tr("settings.schema.services.mpris-blacklist.description"), {"shell", "mpris", "blacklist"},
        ListSetting{.items = cfg.shell.mpris.blacklist}, "mpris media player dbus session blacklist"
    ));
  }

} // namespace settings::registry
