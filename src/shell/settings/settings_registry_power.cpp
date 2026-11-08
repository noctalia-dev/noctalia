#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addPowerSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Power, "session-panel", tr("settings.schema.panels.placement-session.label"),
        tr("settings.schema.panels.placement-session.description"), {"shell", "panel", "session_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.sessionPlacement)),
        "attached floating bar panel power menu position"
    ));
    entries.push_back(panelPositionEntry(
        SettingsSection::Power, "session-panel", "session", "settings.schema.panels.position-session.label",
        "settings.schema.panels.position-session.description", cfg.shell.panel.sessionPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Power, "session-panel", "session", "settings.schema.panels.open-near-click-session.label",
        "settings.schema.panels.open-near-click-session.description", cfg.shell.panel.openNearClickSession,
        &ShellConfig::PanelConfig::sessionPlacement, &ShellConfig::PanelConfig::sessionPosition
    ));
    entries.push_back(makeEntry(
        SettingsSection::Power, "session-panel", tr("settings.schema.power.session-grid.label"),
        tr("settings.schema.power.session-grid.description"), {"shell", "session", "grid"},
        ToggleSetting{.checked = cfg.shell.session.grid}, "session panel grid layout rows columns"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Power, "session-panel", tr("settings.schema.power.session-grid-columns.label"),
          tr("settings.schema.power.session-grid-columns.description"), {"shell", "session", "grid_columns"},
          StepperSetting{
              .value = static_cast<int>(cfg.shell.session.gridColumns),
              .minValue = static_cast<int>(noctalia::config::schema::kSessionGridColumnsRange.min.value()),
              .maxValue = static_cast<int>(noctalia::config::schema::kSessionGridColumnsRange.max.value()),
              .step = static_cast<int>(noctalia::config::schema::kSessionGridColumnsRange.step.value()),
          },
          "session panel grid columns per row"
      );
      e.visibleWhen = [](const Config& c) { return c.shell.session.grid; };
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Power, "session-panel", tr("settings.schema.power.session-show-shortcuts.label"),
        tr("settings.schema.power.session-show-shortcuts.description"), {"shell", "session", "show_shortcuts"},
        ToggleSetting{.checked = cfg.shell.session.showShortcuts}, "session panel show shortcuts"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Power, "session-panel", tr("settings.schema.power.session-actions.label"),
        tr("settings.schema.power.session-actions.description"), {"shell", "session", "actions"},
        SessionPanelActionsSetting{.items = cfg.shell.session.actions},
        "session panel power menu logout reboot shutdown lock command actions order"
    ));
  }

  void addIdleSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Power, "idle", tr("settings.schema.idle.pre-action-fade.label"),
        tr("settings.schema.idle.pre-action-fade.description"), {"idle", "pre_action_fade_seconds"},
        StepperSetting{
            .value = static_cast<int>(std::lround(std::clamp(cfg.idle.preActionFadeSeconds, 0.0F, 30.0F))),
            .minValue = 0,
            .maxValue = 30,
            .step = 1,
            .valueSuffix = "s"
        },
        "idle fade dim seconds overlay"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Power, "idle", tr("settings.schema.idle.behaviors.label"),
        tr("settings.schema.idle.behaviors.description"), {"idle", "behavior"},
        IdleBehaviorsSetting{.items = cfg.idle.behaviors},
        "idle behavior timeout command resume screen lock dpms suspend lock_and_suspend caffeine"
    ));
  }

} // namespace settings::registry
