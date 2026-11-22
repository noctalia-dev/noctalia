#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <string>
#include <string_view>
#include <vector>

namespace settings::registry {

  void addNiriSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    if (env.niriOverviewTypeToLaunchSupported || env.niriBackdropSupported) {
      if (env.niriOverviewTypeToLaunchSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Niri, "overview", tr("settings.schema.shell.niri-overview-type-to-launch.label"),
            tr("settings.schema.shell.niri-overview-type-to-launch.description"),
            {"shell", "niri_overview_type_to_launch_enabled"}, ToggleSetting{cfg.shell.niriOverviewTypeToLaunchEnabled},
            "niri overview type launch launcher search keyboard focus"
        ));
      }
      if (env.niriOverviewCloseOnLaunchSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Niri, "overview", tr("settings.schema.shell.niri-overview-close-on-launch.label"),
            tr("settings.schema.shell.niri-overview-close-on-launch.description"),
            {"shell", "niri_overview_close_on_launch_enabled"},
            ToggleSetting{cfg.shell.niriOverviewCloseOnLaunchEnabled}, "niri overview close app launch launcher"
        ));
      }
      if (env.niriBackdropSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Niri, "backdrop", tr("settings.schema.shared.enabled.label"),
            tr("settings.schema.backdrop.enabled.description"), {"backdrop", "enabled"},
            ToggleSetting{cfg.backdrop.enabled}, "wallpaper backdrop"
        ));
        entries.push_back(makeEntry(
            SettingsSection::Niri, "backdrop", tr("settings.schema.backdrop.blur-intensity.label"),
            tr("settings.schema.backdrop.blur-intensity.description"), {"backdrop", "blur_intensity"},
            sliderFor(cfg.backdrop.blurIntensity, noctalia::config::schema::kUnitRange, false), "wallpaper"
        ));
        entries.push_back(makeEntry(
            SettingsSection::Niri, "backdrop", tr("settings.schema.backdrop.tint-intensity.label"),
            tr("settings.schema.backdrop.tint-intensity.description"), {"backdrop", "tint_intensity"},
            sliderFor(cfg.backdrop.tintIntensity, noctalia::config::schema::kUnitRange, false), "wallpaper"
        ));
      }
    }
  }

  void addUmbrielSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    if (env.umbrielOverviewTypeToLaunchSupported) {
      entries.push_back(makeEntry(
          SettingsSection::Umbriel, "audio", tr("settings.schema.shell.umbriel-audio-provider.label"),
          tr("settings.schema.shell.umbriel-audio-provider.description"), {"shell", "umbriel_audio_provider_enabled"},
          ToggleSetting{cfg.shell.umbrielAudioProviderEnabled}, "umbriel audio spectrum shader effects provider"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Umbriel, "overview", tr("settings.schema.shell.umbriel-overview-type-to-launch.label"),
          tr("settings.schema.shell.umbriel-overview-type-to-launch.description"),
          {"shell", "umbriel_overview_type_to_launch_enabled"},
          ToggleSetting{cfg.shell.umbrielOverviewTypeToLaunchEnabled},
          "umbriel overview type launch launcher search keyboard focus"
      ));
    }
    if (env.umbrielOverviewCloseOnLaunchSupported) {
      entries.push_back(makeEntry(
          SettingsSection::Umbriel, "overview", tr("settings.schema.shell.umbriel-overview-close-on-launch.label"),
          tr("settings.schema.shell.umbriel-overview-close-on-launch.description"),
          {"shell", "umbriel_overview_close_on_launch_enabled"},
          ToggleSetting{cfg.shell.umbrielOverviewCloseOnLaunchEnabled}, "umbriel overview close app launch launcher"
      ));
    }
  }

} // namespace settings::registry
