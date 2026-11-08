#include "config/config_types.h"
#include "core/input/key_chord.h"
#include "core/input/keybind_action.h"
#include "i18n/i18n.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <string>
#include <string_view>
#include <vector>

namespace settings::registry {

  namespace {

    std::vector<KeyChord> effectiveKeybindItems(const std::vector<KeyChord>& configured, KeybindAction action) {
      if (!configured.empty()) {
        return configured;
      }
      return defaultKeybindSet(action);
    }

  } // namespace

  void addKeybindSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.validate.label"),
        tr("settings.schema.keybinds.validate.description"), {"keybinds", "validate"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.validate, KeybindAction::Validate), .maxItems = 4
        },
        "keybind shortcut hotkey enter accept submit confirm"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.cancel.label"),
        tr("settings.schema.keybinds.cancel.description"), {"keybinds", "cancel"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.cancel, KeybindAction::Cancel), .maxItems = 4},
        "keybind shortcut hotkey escape close dismiss"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.left.label"),
        tr("settings.schema.keybinds.left.description"), {"keybinds", "left"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.left, KeybindAction::Left), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.right.label"),
        tr("settings.schema.keybinds.right.description"), {"keybinds", "right"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.right, KeybindAction::Right), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.up.label"),
        tr("settings.schema.keybinds.up.description"), {"keybinds", "up"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.up, KeybindAction::Up), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.down.label"),
        tr("settings.schema.keybinds.down.description"), {"keybinds", "down"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.down, KeybindAction::Down), .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.tab-previous.label"),
        tr("settings.schema.keybinds.tab-previous.description"), {"keybinds", "tab_previous"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.tabPrevious, KeybindAction::TabPrevious), .maxItems = 4
        },
        "keybind shortcut hotkey shift tab focus pane"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.tab-next.label"),
        tr("settings.schema.keybinds.tab-next.description"), {"keybinds", "tab_next"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.tabNext, KeybindAction::TabNext), .maxItems = 4},
        "keybind shortcut hotkey tab focus pane"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.category-previous.label"),
        tr("settings.schema.keybinds.category-previous.description"), {"keybinds", "category_previous"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.categoryPrevious, KeybindAction::CategoryPrevious),
            .maxItems = 4
        },
        "keybind shortcut hotkey cycle tab category previous"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.category-next.label"),
        tr("settings.schema.keybinds.category-next.description"), {"keybinds", "category_next"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.categoryNext, KeybindAction::CategoryNext), .maxItems = 4
        },
        "keybind shortcut hotkey cycle tab category next"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.copy.label"),
        tr("settings.schema.keybinds.copy.description"), {"keybinds", "copy"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.copy, KeybindAction::Copy), .maxItems = 4},
        "keybind shortcut hotkey copy clipboard"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.save.label"),
        tr("settings.schema.keybinds.save.description"), {"keybinds", "save"},
        KeybindListSetting{.items = effectiveKeybindItems(cfg.keybinds.save, KeybindAction::Save), .maxItems = 4},
        "keybind shortcut hotkey save file"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.select-all.label"),
        tr("settings.schema.keybinds.select-all.description"), {"keybinds", "select_all"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.selectAll, KeybindAction::SelectAll), .maxItems = 4
        },
        "keybind shortcut hotkey select all screenshot region desktop"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Keybinds, "keybinds", tr("settings.schema.keybinds.delete.label"),
        tr("settings.schema.keybinds.delete.description"), {"keybinds", "delete"},
        KeybindListSetting{
            .items = effectiveKeybindItems(cfg.keybinds.deleteEntry, KeybindAction::Delete), .maxItems = 4
        },
        "keybind shortcut hotkey delete remove clear"
    ));
  }

} // namespace settings::registry
