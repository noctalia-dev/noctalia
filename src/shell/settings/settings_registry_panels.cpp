#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  namespace {

    std::vector<SelectOption> controlCenterShortcutOptions(const Config& cfg) {
      auto catalog = ShortcutRegistry::catalog();
      std::vector<SelectOption> opts;
      opts.reserve(catalog.size());
      for (auto& shortcut : catalog) {
        if (!ShortcutRegistry::isAvailable(shortcut.type, cfg)) {
          continue;
        }
        std::string label = shortcut.literalLabel ? std::move(shortcut.labelKey) : i18n::tr(shortcut.labelKey);
        opts.push_back(SelectOption{std::move(shortcut.type), std::move(label)});
      }
      return opts;
    }

  } // namespace

  void addPanelSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    {
      SelectSetting anchorBarSelect;
      for (const auto& name : barNames(cfg)) {
        anchorBarSelect.options.push_back(SelectOption{name, name});
      }
      anchorBarSelect.selectedValue = cfg.shell.panelAnchorBar;
      anchorBarSelect.allowEmptySelection = true;
      entries.push_back(makeEntry(
          SettingsSection::Panels, "general", tr("settings.schema.panels.panel-anchor-bar.label"),
          tr("settings.schema.panels.panel-anchor-bar.description"), {"shell", "panel_anchor_bar"},
          std::move(anchorBarSelect), "anchor attach bar panel wallpaper launcher"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Panels, "general", tr("settings.schema.panels.floating-layer.label"),
        tr("settings.schema.panels.floating-layer.description"), {"shell", "panel", "floating_layer"},
        asSegmented(plainSelect(
            {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
            cfg.shell.panel.floatingLayer
        )),
        "floating detached panel layer shell z-order input method popup fullscreen"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.panels.transparency-mode.label"),
        tr("settings.schema.panels.transparency-mode.description"), {"shell", "panel", "transparency_mode"},
        asSegmented(enumSelect(kPanelTransparencyModes, cfg.shell.panel.transparencyMode)),
        "glass alpha transparency translucent opacity cards blur"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.panels.borders.label"),
        tr("settings.schema.panels.borders.description"), {"shell", "panel", "borders"},
        ToggleSetting{cfg.shell.panel.borders}, "outline border shell edge"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.panels.border-color.label"),
        tr("settings.schema.panels.border-color.description"), {"shell", "panel", "border_color"},
        colorSpecPicker(cfg.shell.panel.borderColor), "outline border color theme accent"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.panels.border-width.label"),
        tr("settings.schema.panels.border-width.description"), {"shell", "panel", "border_width"},
        sliderFor(cfg.shell.panel.borderWidth, noctalia::config::schema::kPanelBorderWidthRange, false),
        "outline border width thickness"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.shared.shadow.label"),
        tr("settings.schema.panels.shadow.description"), {"shell", "panel", "shadow"},
        ToggleSetting{cfg.shell.panel.shadow}, "shadow depth"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.panels.list-item-background.label"),
        tr("settings.schema.panels.list-item-background.description"), {"shell", "panel", "list_item_background"},
        ToggleSetting{cfg.shell.panel.listItemBackground}, "list item background fill row card launcher clipboard"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "effects", tr("settings.schema.panels.floating-offset.label"),
        tr("settings.schema.panels.floating-offset.description"), {"shell", "panel", "floating_offset"},
        StepperSetting{
            .value = static_cast<int>(cfg.shell.panel.floatingOffset),
            .minValue = 0,
            .maxValue = 100,
            .step = 1,
            .valueSuffix = "px",
        },
        "floating detached panel gap offset distance bar"
    ));
  }

  void addClipboardAndWallpaperPanelSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Panels, "clipboard", tr("settings.schema.panels.placement-clipboard.label"),
        tr("settings.schema.panels.placement-clipboard.description"), {"shell", "panel", "clipboard_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.clipboardPlacement)),
        "attached floating bar panel position"
    ));
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "clipboard", "clipboard", "settings.schema.panels.position-clipboard.label",
        "settings.schema.panels.position-clipboard.description", cfg.shell.panel.clipboardPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Panels, "clipboard", "clipboard", "settings.schema.panels.open-near-click-clipboard.label",
        "settings.schema.panels.open-near-click-clipboard.description", cfg.shell.panel.openNearClickClipboard,
        &ShellConfig::PanelConfig::clipboardPlacement, &ShellConfig::PanelConfig::clipboardPosition
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "polkit", tr("settings.schema.panels.placement-polkit.label"),
        tr("settings.schema.panels.placement-polkit.description"), {"shell", "panel", "polkit_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.polkitPlacement)),
        "attached floating bar panel polkit auth password"
    ));
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "polkit", "polkit", "settings.schema.panels.position-polkit.label",
        "settings.schema.panels.position-polkit.description", cfg.shell.panel.polkitPosition
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "wallpaper", tr("settings.schema.panels.wallpaper-show-names.label"),
        tr("settings.schema.panels.wallpaper-show-names.description"), {"shell", "panel", "wallpaper_show_names"},
        ToggleSetting{cfg.shell.panel.wallpaperShowNames}, "picker grid caption file name label thumbnail"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "wallpaper", tr("settings.schema.panels.placement-wallpaper.label"),
        tr("settings.schema.panels.placement-wallpaper.description"), {"shell", "panel", "wallpaper_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.wallpaperPlacement)),
        "attached floating bar panel position"
    ));
    entries.push_back(panelPositionEntry(
        SettingsSection::Panels, "wallpaper", "wallpaper", "settings.schema.panels.position-wallpaper.label",
        "settings.schema.panels.position-wallpaper.description", cfg.shell.panel.wallpaperPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::Panels, "wallpaper", "wallpaper", "settings.schema.panels.open-near-click-wallpaper.label",
        "settings.schema.panels.open-near-click-wallpaper.description", cfg.shell.panel.openNearClickWallpaper,
        &ShellConfig::PanelConfig::wallpaperPlacement, &ShellConfig::PanelConfig::wallpaperPosition
    ));
  }

  void addControlCenterSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "layout", tr("settings.schema.panels.placement-control-center.label"),
        tr("settings.schema.panels.placement-control-center.description"),
        {"shell", "panel", "control_center_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.controlCenterPlacement)),
        "attached floating bar panel position"
    ));
    entries.push_back(panelPositionEntry(
        SettingsSection::ControlCenter, "layout", "control_center",
        "settings.schema.panels.position-control-center.label",
        "settings.schema.panels.position-control-center.description", cfg.shell.panel.controlCenterPosition
    ));
    entries.push_back(panelOpenNearClickEntry(
        SettingsSection::ControlCenter, "layout", "control_center",
        "settings.schema.panels.open-near-click-control-center.label",
        "settings.schema.panels.open-near-click-control-center.description", cfg.shell.panel.openNearClickControlCenter,
        &ShellConfig::PanelConfig::controlCenterPlacement, &ShellConfig::PanelConfig::controlCenterPosition
    ));
    {
      SliderSetting width =
          sliderFor(cfg.controlCenter.width, noctalia::config::schema::kControlCenterWidthRange, true);
      width.valueSuffix = "px";
      entries.push_back(makeEntry(
          SettingsSection::ControlCenter, "layout", tr("settings.schema.panels.control-center-width.label"),
          tr("settings.schema.panels.control-center-width.description"), {"control_center", "width"}, std::move(width),
          "size dimension wide narrow"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "navigation", tr("settings.schema.panels.control-center-sidebar.label"),
        tr("settings.schema.panels.control-center-sidebar.description"), {"control_center", "sidebar"},
        asSegmented(enumSelect(kControlCenterSidebarModes, cfg.controlCenter.sidebarMode)),
        "full compact none sidebar icons narrow hidden"
    ));
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "navigation", tr("settings.schema.panels.control-center-sidebar-section.label"),
        tr("settings.schema.panels.control-center-sidebar-section.description"), {"control_center", "sidebar_section"},
        asSegmented(enumSelect(kControlCenterSidebarModes, cfg.controlCenter.sidebarSectionMode)),
        "full compact none sidebar icons narrow hidden tab direct widget shortcut"
    ));
    {
      MultiSelectSetting tabs;
      const auto catalog = ControlCenterPanel::hideableTabCatalog();
      tabs.options.reserve(catalog.size());
      tabs.selectedValues.reserve(catalog.size());
      for (const auto& tab : catalog) {
        tabs.options.push_back(SelectOption{std::string(tab.key), tr(tab.titleKey)});
        if (!std::ranges::contains(cfg.controlCenter.hiddenTabs, tab.key)) {
          tabs.selectedValues.emplace_back(tab.key);
        }
      }
      tabs.persistUnselected = true;
      entries.push_back(makeEntry(
          SettingsSection::ControlCenter, "navigation", tr("settings.schema.panels.control-center-tabs.label"),
          tr("settings.schema.panels.control-center-tabs.description"), {"control_center", "hidden_tabs"},
          std::move(tabs), "tabs sections visible hide show display brightness media audio network power"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "home", tr("settings.schema.panels.home-shortcuts.label"),
        tr("settings.schema.panels.home-shortcuts.description"), {"control_center", "shortcuts"},
        ShortcutListSetting{
            .items = cfg.controlCenter.shortcuts, .suggestedOptions = controlCenterShortcutOptions(cfg), .maxItems = 6
        },
        "quick settings shortcuts toggles wifi bluetooth caffeine night light dnd power media weather clipboard"
    ));
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "home", tr("settings.schema.panels.home-shortcuts-show-labels.label"),
        tr("settings.schema.panels.home-shortcuts-show-labels.description"), {"control_center", "show_shortcut_labels"},
        ToggleSetting{cfg.controlCenter.showShortcutLabels}, "shortcuts labels text hide show titles"
    ));
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "home", tr("settings.schema.panels.home-session-button.label"),
        tr("settings.schema.panels.home-session-button.description"), {"control_center", "show_session_button"},
        ToggleSetting{cfg.controlCenter.showSessionButton}, "session button show hide"
    ));
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "home", tr("settings.schema.panels.home-avatar-border.label"),
        tr("settings.schema.panels.home-avatar-border.description"), {"control_center", "show_avatar_border"},
        ToggleSetting{cfg.controlCenter.showAvatarBorder}, "avatar border ring accent hide show"
    ));
  }

} // namespace settings::registry
