#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/directory_scanner.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addSecuritySettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Security, "network", tr("settings.schema.shell.offline-mode.label"),
        tr("settings.schema.shell.offline-mode.description"), {"shell", "offline_mode"},
        ToggleSetting{cfg.shell.offlineMode}, "network http fetch download"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "network", tr("settings.schema.shell.external-ip.label"),
        tr("settings.schema.shell.external-ip.description"), {"shell", "external_ip_enabled"},
        ToggleSetting{cfg.shell.externalIpEnabled}, "wan external ip public address network resolve"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "network", tr("settings.schema.shell.telemetry.label"),
        tr("settings.schema.shell.telemetry.description"), {"shell", "telemetry_enabled"},
        ToggleSetting{cfg.shell.telemetryEnabled}, "analytics ping privacy"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "authentication", tr("settings.schema.shell.polkit-agent.label"),
        tr("settings.schema.shell.polkit-agent.description"), {"shell", "polkit_agent"},
        ToggleSetting{cfg.shell.polkitAgent}, "auth password"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "authentication", tr("settings.schema.shared.colorize-icons.label"),
        tr("settings.schema.shared.colorize-icons.description"), {"shell", "polkit_colorize_icons"},
        ToggleSetting{cfg.shell.polkitColorizeIcons}, "authentication application icons colorize tint"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "authentication", tr("settings.schema.shell.password-style.label"),
        tr("settings.schema.shell.password-style.description"), {"shell", "password_style"},
        asSegmented(enumSelect(kPasswordMaskStyles, cfg.shell.passwordMaskStyle)), "polkit lock mask"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "privacy", tr("settings.schema.shell.privacy-mic-filter-regex.label"),
        tr("settings.schema.shell.privacy-mic-filter-regex.description"), {"shell", "privacy", "mic_filter_regex"},
        TextSetting{.value = cfg.shell.privacy.micFilterRegex, .placeholder = "", .browseFileExtensions = {}},
        "privacy microphone mic app process regex filter ignore"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "privacy", tr("settings.schema.shell.privacy-cam-filter-regex.label"),
        tr("settings.schema.shell.privacy-cam-filter-regex.description"), {"shell", "privacy", "cam_filter_regex"},
        TextSetting{.value = cfg.shell.privacy.camFilterRegex, .placeholder = "", .browseFileExtensions = {}},
        "privacy camera webcam app process regex filter ignore"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Security, "privacy", tr("settings.schema.shell.privacy-screen-filter-regex.label"),
        tr("settings.schema.shell.privacy-screen-filter-regex.description"),
        {"shell", "privacy", "screen_filter_regex"},
        TextSetting{.value = cfg.shell.privacy.screenFilterRegex, .placeholder = "", .browseFileExtensions = {}},
        "privacy screen share screenshare app process regex filter ignore"
    ));
  }

  void addLockScreenSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    const SettingVisibility lockscreenOn = [](const Config& c) { return c.lockscreen.enabled; };
    entries.push_back(makeEntry(
        SettingsSection::LockScreen, "general", tr("settings.schema.lockscreen.enabled.label"),
        tr("settings.schema.lockscreen.enabled.description"), {"lockscreen", "enabled"},
        ToggleSetting{cfg.lockscreen.enabled}, "lock screen session"
    ));
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "general", tr("settings.schema.lockscreen.lock-before-suspend.label"),
          tr("settings.schema.lockscreen.lock-before-suspend.description"), {"lockscreen", "lock_before_suspend"},
          ToggleSetting{cfg.lockscreen.lockBeforeSuspend}, "lock screen before suspend sleep lid close prepareforsleep"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "general", tr("settings.schema.lockscreen.dnd.label"),
          tr("settings.schema.lockscreen.dnd.description"), {"lockscreen", "dnd"}, ToggleSetting{cfg.lockscreen.dnd},
          "lock screen do not disturb dnd notifications"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "general", tr("settings.schema.lockscreen.grace-period.label"),
          tr("settings.schema.lockscreen.grace-period.description"), {"lockscreen", "grace_period_seconds"},
          StepperSetting{
              .value = std::clamp(cfg.lockscreen.gracePeriodSeconds, 0, 60),
              .minValue = 0,
              .maxValue = 60,
              .step = 1,
              .valueSuffix = "s"
          },
          "lock screen grace period passwordless unlock keypress mouse movement"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "general", tr("settings.schema.lockscreen.monitors.label"),
          tr("settings.schema.lockscreen.monitors.description"), {"lockscreen", "monitors"},
          ListSetting{.items = cfg.lockscreen.monitors, .suggestedOptions = env.availableOutputs},
          "lock screen monitor output connector"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "authentication", tr("settings.schema.lockscreen.fingerprint.label"),
          tr("settings.schema.lockscreen.fingerprint.description"), {"lockscreen", "fingerprint"},
          ToggleSetting{cfg.lockscreen.fingerprint}, "lock screen fingerprint fprintd biometric"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "authentication", tr("settings.schema.lockscreen.allow-empty-password.label"),
          tr("settings.schema.lockscreen.allow-empty-password.description"), {"lockscreen", "allow_empty_password"},
          ToggleSetting{cfg.lockscreen.allowEmptyPassword}, "lock screen empty password security key pam"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    if (env.screencopySupported) {
      auto e = makeEntry(
          SettingsSection::LockScreen, "background", tr("settings.schema.lockscreen.blurred-desktop.label"),
          tr("settings.schema.lockscreen.blurred-desktop.description"), {"lockscreen", "blurred_desktop"},
          ToggleSetting{cfg.lockscreen.blurredDesktop}, "lock screen desktop capture screencopy background"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      const SettingVisibility lockscreenWallpaperOn = [](const Config& c) {
        return c.lockscreen.enabled && !c.lockscreen.blurredDesktop;
      };
      auto e = makeEntry(
          SettingsSection::LockScreen, "background", tr("settings.schema.lockscreen.wallpaper.label"),
          tr("settings.schema.lockscreen.wallpaper.description"), {"lockscreen", "wallpaper"},
          TextSetting{
              .value = cfg.lockscreen.wallpaper,
              .placeholder = tr("settings.schema.lockscreen.wallpaper.placeholder"),
              .browseMode = TextSettingBrowseMode::OpenFile,
              .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true),
              .browseFallbackDirectory = wallpaper::resolveGlobalWallpaperDirectory(
                  cfg.wallpaper,
                  wallpaper::effectiveThemeMode(
                      shellThemeMode(cfg.theme), shellThemeMode(cfg.theme) == ThemeMode::Light
                  )
              ),
          },
          "lock screen background image custom"
      );
      e.visibleWhen = lockscreenWallpaperOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "background", tr("settings.schema.lockscreen.blur-intensity.label"),
          tr("settings.schema.lockscreen.blur-intensity.description"), {"lockscreen", "blur_intensity"},
          sliderFor(cfg.lockscreen.blurIntensity, noctalia::config::schema::kUnitRange, false), "lock screen blur"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "background", tr("settings.schema.lockscreen.tint-intensity.label"),
          tr("settings.schema.lockscreen.tint-intensity.description"), {"lockscreen", "tint_intensity"},
          sliderFor(cfg.lockscreen.tintIntensity, noctalia::config::schema::kUnitRange, false), "lock screen tint"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "background", tr("settings.schema.lockscreen.tint-color.label"),
          tr("settings.schema.lockscreen.tint-color.description"), {"lockscreen", "tint_color"},
          colorSpecPicker(cfg.lockscreen.tintColor), "lock screen tint color theme accent"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    if (env.screencopySupported) {
      MultiSelectSetting transitions;
      transitions.options.reserve(std::size(kLockscreenTransitions));
      for (const auto& opt : kLockscreenTransitions) {
        transitions.options.push_back(SelectOption{std::string(opt.key), tr(opt.labelKey)});
      }
      transitions.selectedValues.reserve(cfg.lockscreen.transitions.size());
      for (const auto& transition : cfg.lockscreen.transitions) {
        transitions.selectedValues.emplace_back(enumToKey(kLockscreenTransitions, transition));
      }
      auto transitionEffects = makeEntry(
          SettingsSection::LockScreen, "transition", tr("settings.schema.lockscreen.transitions.label"),
          tr("settings.schema.lockscreen.transitions.description"), {"lockscreen", "transition"},
          std::move(transitions), "lock screen effects animation pool"
      );
      transitionEffects.visibleWhen = lockscreenOn;
      entries.push_back(std::move(transitionEffects));

      auto transitionDuration = makeEntry(
          SettingsSection::LockScreen, "transition", tr("settings.schema.lockscreen.transition-duration.label"),
          tr("settings.schema.lockscreen.transition-duration.description"), {"lockscreen", "transition_duration"},
          sliderFor(
              cfg.lockscreen.transitionDurationMs, noctalia::config::schema::kLockscreenTransitionDurationRange, true
          ),
          "lock screen transition animation duration"
      );
      transitionDuration.visibleWhen = lockscreenOn;
      entries.push_back(std::move(transitionDuration));

      auto edgeSmoothness = makeEntry(
          SettingsSection::LockScreen, "transition", tr("settings.schema.lockscreen.edge-smoothness.label"),
          tr("settings.schema.lockscreen.edge-smoothness.description"), {"lockscreen", "edge_smoothness"},
          sliderFor(cfg.lockscreen.edgeSmoothness, noctalia::config::schema::kUnitRange, false),
          "lock screen transition feathering", true
      );
      edgeSmoothness.visibleWhen = lockscreenOn;
      entries.push_back(std::move(edgeSmoothness));
    }
    {
      auto e = makeEntry(
          SettingsSection::LockScreen, "widgets", tr("settings.schema.lockscreen.widgets.label"),
          tr("settings.schema.lockscreen.widgets.description"), {"lockscreen_widgets", "enabled"},
          ToggleSetting{cfg.lockscreenWidgets.enabled}, "lock screen widgets layout"
      );
      e.visibleWhen = lockscreenOn;
      entries.push_back(std::move(e));
    }
    if (env.greeterSyncAvailable) {
      entries.push_back(makeEntry(
          SettingsSection::LockScreen, "greeter", tr("settings.schema.shell.greeter-sync-privilege-command.label"),
          tr("settings.schema.shell.greeter-sync-privilege-command.description"),
          {"shell", "greeter_sync", "privilege_command"},
          TextSetting{
              .value = cfg.shell.greeterSync.privilegeCommand,
              .placeholder = "pkexec",
              .browseFileExtensions = {},
          },
          "greeter sync pkexec run0 ghostty terminal sudo polkit wrapper"
      ));
    }
  }

} // namespace settings::registry
