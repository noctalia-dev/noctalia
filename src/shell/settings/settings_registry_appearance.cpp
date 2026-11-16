#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/palette_swatch_preview.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"
#include "shell/settings/supported_languages.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "theme/builtin_palettes.h"
#include "theme/builtin_templates.h"
#include "ui/palette.h"
#include "util/file_utils.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  namespace {

    SelectSetting builtinPaletteSelect(std::string_view selected, ThemeMode mode) {
      std::vector<SelectOption> opts;
      opts.reserve(noctalia::theme::builtinPalettes().size());
      for (const auto& palette : noctalia::theme::builtinPalettes()) {
        opts.push_back(
            SelectOption{
                .value = std::string(palette.name),
                .label = std::string(palette.name),
                .description = {},
                .preview = builtinPaletteSwatchPreview(palette, mode),
            }
        );
      }
      return SelectSetting{
          .options = std::move(opts), .selectedValue = std::string(selected), .preferredWidth = 240.0F
      };
    }

    SelectSetting wallpaperSchemeSelect(std::string_view selected) {
      return plainSelect(
          {{"m3-content", "theme.scheme.m3-content"},
           {"m3-tonal-spot", "theme.scheme.m3-tonal-spot"},
           {"m3-fruit-salad", "theme.scheme.m3-fruit-salad"},
           {"m3-rainbow", "theme.scheme.m3-rainbow"},
           {"m3-monochrome", "theme.scheme.m3-monochrome"},
           {"vibrant", "theme.scheme.vibrant"},
           {"faithful", "theme.scheme.faithful"},
           {"soft", "theme.scheme.soft"},
           {"dysfunctional", "theme.scheme.dysfunctional"},
           {"muted", "theme.scheme.muted"}},
          selected
      );
    }

    SelectSetting languageSelect(std::string_view selected) {
      std::vector<SelectOption> opts;
      opts.reserve(kSupportedLanguages.size() + 1);
      opts.push_back(SelectOption{"", i18n::tr("common.states.auto")});
      for (const auto& language : kSupportedLanguages) {
        opts.push_back(SelectOption{std::string(language.code), std::string(language.displayName)});
      }
      return SelectSetting{std::move(opts), std::string(selected)};
    }

  } // namespace

  void addAppearanceSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", tr("settings.schema.appearance.theme-mode.label"),
        tr("settings.schema.appearance.theme-mode.description"), {"theme", "mode"},
        asSegmented(enumSelect(kThemeModes, cfg.theme.mode)), "dark light auto colors"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", tr("settings.schema.appearance.shell-theme-mode.label"),
        tr("settings.schema.appearance.shell-theme-mode.description"), {"theme", "shell_mode"},
        asSegmented(enumSelect(kShellThemeModes, cfg.theme.shellMode)),
        "dark light auto follow shell apps templates colors"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", tr("settings.schema.appearance.palette-source.label"),
        tr("settings.schema.appearance.palette-source.description"), {"theme", "source"},
        asSegmented(enumSelect(kPaletteSources, cfg.theme.source)), "palette colors"
    ));
    if (cfg.theme.source == PaletteSource::Builtin) {
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.builtin-palette.label"),
          tr("settings.schema.appearance.builtin-palette.description"), {"theme", "builtin"},
          builtinPaletteSelect(cfg.theme.builtinPalette, shellThemeMode(cfg.theme)), "builtin palette colors"
      ));
    } else if (cfg.theme.source == PaletteSource::Wallpaper) {
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.wallpaper-generation-scheme.label"),
          tr("settings.schema.appearance.wallpaper-generation-scheme.description"), {"theme", "wallpaper_scheme"},
          wallpaperSchemeSelect(cfg.theme.wallpaperScheme), "wallpaper palette generator scheme material you m3 colors"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.terminal-colors.label"),
          tr("settings.schema.appearance.terminal-colors.description"), {"theme", "terminal_colors"},
          asSegmented(enumSelect(kTerminalColorsOptions, cfg.theme.terminalColors)),
          "terminal ansi colors red green semantic wallpaper"
      ));
    } else if (cfg.theme.source == PaletteSource::Community) {
      SettingControl communityPaletteControl =
          TextSetting{.value = cfg.theme.communityPalette, .placeholder = "Oxocarbon", .browseFileExtensions = {}};
      if (!env.communityPalettes.empty()) {
        communityPaletteControl = SearchPickerSetting{
            .options = env.communityPalettes,
            .selectedValue = cfg.theme.communityPalette,
            .placeholder = tr("settings.schema.appearance.community-palette.search-placeholder"),
            .emptyText = tr("ui.controls.search-picker.empty"),
            .preferredHeight = 240.0F,
            .onSelect = {},
            .keepOpenOnSelect = true,
        };
      }
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.community-palette.label"),
          tr("settings.schema.appearance.community-palette.description"), {"theme", "community_palette"},
          std::move(communityPaletteControl), "community palette colors"
      ));
    } else if (cfg.theme.source == PaletteSource::Custom) {
      SettingControl customPaletteControl =
          TextSetting{.value = cfg.theme.customPalette, .placeholder = "", .browseFileExtensions = {}};
      if (!env.customPalettes.empty()) {
        customPaletteControl = SearchPickerSetting{
            .options = env.customPalettes,
            .selectedValue = cfg.theme.customPalette,
            .placeholder = tr("settings.schema.appearance.custom-palette.search-placeholder"),
            .emptyText = tr("ui.controls.search-picker.empty"),
            .preferredHeight = 240.0F,
            .onSelect = {},
            .keepOpenOnSelect = true,
        };
      }
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", tr("settings.schema.appearance.custom-palette.label"),
          tr("settings.schema.appearance.custom-palette.description"), {"theme", "custom_palette"},
          std::move(customPaletteControl), "custom palette colors"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", tr("settings.schema.appearance.palette-inspector.label"),
        tr("settings.schema.appearance.palette-inspector.description"), {},
        ButtonSetting{
            .label = tr("settings.schema.appearance.palette-inspector.button"),
            .action = {},
            .glyph = "color-swatch",
        },
        "palette inspector template tokens colors swatches hex terminal ansi"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", tr("settings.schema.appearance.pure-black-dark.label"),
        tr("settings.schema.appearance.pure-black-dark.description"), {"theme", "pure_black_dark"},
        ToggleSetting{cfg.theme.pureBlackDark}, "oled amoled true black background contrast"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", tr("settings.schema.appearance.sync-color-scheme.label"),
        tr("settings.schema.appearance.sync-color-scheme.description"), {"theme", "sync_color_scheme"},
        ToggleSetting{cfg.theme.syncColorScheme}, "gnome gtk portal dark light prefer color scheme apps"
    ));
    {
      SettingControl fontFamilyControl =
          TextSetting{.value = cfg.shell.fontFamily, .placeholder = "sans-serif", .browseFileExtensions = {}};
      if (!env.fontFamilies.empty()) {
        fontFamilyControl = SearchPickerSetting{
            .options = env.fontFamilies,
            .selectedValue = cfg.shell.fontFamily,
            .placeholder = "sans-serif",
            .emptyText = tr("ui.controls.search-picker.empty"),
            .preferredHeight = 280.0F,
        };
      }
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "interface", tr("settings.schema.appearance.font-family.label"),
          tr("settings.schema.appearance.font-family.description"), {"shell", "font_family"},
          std::move(fontFamilyControl), "typeface"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "interface", tr("settings.schema.appearance.language.label"),
        tr("settings.schema.appearance.language.description"), {"shell", "lang"}, languageSelect(cfg.shell.lang),
        "locale translation", true
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "interface", tr("settings.schema.appearance.corner-roundness.label"),
        tr("settings.schema.appearance.corner-roundness.description"), {"shell", "corner_radius_scale"},
        sliderFor(cfg.shell.cornerRadiusScale, noctalia::config::schema::kCornerRadiusScaleRange, false),
        "rounded corners radius"
    ));
    {
      const ColorSpec pickerColor = *effectiveShellAppIconColorizationTint(cfg.shell, true);
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "interface", tr("settings.schema.appearance.app-icon-color.label"),
          tr("settings.schema.appearance.app-icon-color.description"), {"shell", "app_icon_color"},
          colorSpecPicker(pickerColor), "color role dock tray application icons"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "interface", tr("settings.schema.appearance.show-close-buttons.label"),
        tr("settings.schema.appearance.show-close-buttons.description"), {"shell", "show_close_buttons"},
        ToggleSetting{cfg.shell.showCloseButtons},
        "close button cross dismiss hide panel notification toast popup dialog sheet settings window"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "accessibility", tr("settings.schema.appearance.ui-scale.label"),
        tr("settings.schema.appearance.ui-scale.description"), {"accessibility", "ui_scale"},
        sliderFor(cfg.accessibility.uiScale, noctalia::config::schema::kScaleRange, false), "size scale text panels"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "accessibility", tr("settings.schema.accessibility.high-contrast.label"),
        tr("settings.schema.accessibility.high-contrast.description"), {"accessibility", "high_contrast"},
        ToggleSetting{cfg.accessibility.highContrast}, "accessibility high contrast visually impaired"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "motion", tr("settings.schema.appearance.animations.label"),
        tr("settings.schema.appearance.animations.description"), {"shell", "animation", "enabled"},
        ToggleSetting{cfg.shell.animation.enabled}, "motion"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "motion", tr("settings.schema.appearance.animation-speed.label"),
        tr("settings.schema.appearance.animation-speed.description"), {"shell", "animation", "speed"},
        sliderFor(cfg.shell.animation.speed, noctalia::config::schema::kAnimationSpeedRange, false), "motion"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "borders", tr("settings.schema.appearance.button-borders.label"),
        tr("settings.schema.appearance.button-borders.description"), {"shell", "button_borders"},
        ToggleSetting{cfg.shell.buttonBorders}, "button outline border flat minimal"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "borders", tr("settings.schema.appearance.input-borders.label"),
        tr("settings.schema.appearance.input-borders.description"), {"shell", "input_borders"},
        ToggleSetting{cfg.shell.inputBorders}, "input text box field outline border flat minimal"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "borders", tr("settings.schema.appearance.popup-borders.label"),
        tr("settings.schema.appearance.popup-borders.description"), {"shell", "popup_borders"},
        ToggleSetting{cfg.shell.popupBorders}, "popup menu dropdown outline border flat minimal"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "borders", tr("settings.schema.appearance.card-borders.label"),
        tr("settings.schema.appearance.card-borders.description"), {"shell", "card_borders"},
        ToggleSetting{cfg.shell.cardBorders}, "card section outline border flat minimal"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "effects", tr("settings.schema.shared.shadow-direction.label"),
        tr("settings.schema.appearance.global-shadow-direction.description"), {"shell", "shadow", "direction"},
        enumSelect(kShadowDirections, cfg.shell.shadow.direction), "shadow direction"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "effects", tr("settings.schema.shared.shadow-alpha.label"),
        tr("settings.schema.appearance.global-shadow-alpha.description"), {"shell", "shadow", "alpha"},
        sliderFor(cfg.shell.shadow.alpha, noctalia::config::schema::kUnitRange, false), "shadow opacity", true
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "effects", tr("settings.schema.appearance.popup-shadows.label"),
        tr("settings.schema.appearance.popup-shadows.description"), {"shell", "popup_shadows"},
        ToggleSetting{cfg.shell.popupShadows}, "popup menu dropdown drop shadow depth"
    ));
  }

  void addWallpaperSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "general", tr("settings.schema.shared.enabled.label"),
        tr("settings.schema.wallpaper.enabled.description"), {"wallpaper", "enabled"},
        ToggleSetting{cfg.wallpaper.enabled}, "background image"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "general", tr("settings.schema.wallpaper.fill-mode.label"),
        tr("settings.schema.wallpaper.fill-mode.description"), {"wallpaper", "fill_mode"},
        asSegmented(enumSelect(kWallpaperFillModes, cfg.wallpaper.fillMode)), "scale aspect"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "general", tr("settings.schema.wallpaper.fill-color.label"),
        tr("settings.schema.wallpaper.fill-color.description"), {"wallpaper", "fill_color"},
        colorSpecPicker(cfg.wallpaper.fillColor, true), "background solid color"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.directory.label"),
        tr("settings.schema.wallpaper.directory.description"), {"wallpaper", "directory"},
        TextSetting{
            .value = cfg.wallpaper.directory,
            .placeholder = FileUtils::defaultPicturesDirectory().string(),
            .browseMode = TextSettingBrowseMode::SelectFolder,
            .browseFileExtensions = {}
        },
        "folder path"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.directory-light.label"),
        tr("settings.schema.wallpaper.directory-light.description"), {"wallpaper", "directory_light"},
        TextSetting{
            .value = cfg.wallpaper.directoryLight,
            .placeholder = tr("settings.schema.wallpaper.directory-light.placeholder"),
            .browseMode = TextSettingBrowseMode::SelectFolder,
            .browseFileExtensions = {}
        },
        "folder path light theme", true
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.directory-dark.label"),
        tr("settings.schema.wallpaper.directory-dark.description"), {"wallpaper", "directory_dark"},
        TextSetting{
            .value = cfg.wallpaper.directoryDark,
            .placeholder = tr("settings.schema.wallpaper.directory-dark.placeholder"),
            .browseMode = TextSettingBrowseMode::SelectFolder,
            .browseFileExtensions = {}
        },
        "folder path dark theme", true
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.per-monitor-directories.label"),
        tr("settings.schema.wallpaper.per-monitor-directories.description"), {"wallpaper", "per_monitor_directories"},
        ToggleSetting{cfg.wallpaper.perMonitorDirectories}, "per display folder"
    ));
    for (const auto& outputOpt : env.availableOutputs) {
      const std::string& connector = outputOpt.value;
      if (connector.empty()) {
        continue;
      }
      const std::vector<std::string> root = {"wallpaper", "monitor", connector};
      auto monitorPath = [&](std::string key) {
        std::vector<std::string> p = root;
        p.push_back(std::move(key));
        return p;
      };
      constexpr SettingsSection section = SettingsSection::Wallpaper;
      const WallpaperMonitorOverride* ovr = nullptr;
      for (const auto& candidate : cfg.wallpaper.monitorOverrides) {
        if (candidate.match == connector) {
          ovr = &candidate;
          break;
        }
      }
      auto perMonOn = SettingVisibility{[](const Config& c) { return c.wallpaper.perMonitorDirectories; }};
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.fill-color.label"), monitorPath("fill_color"),
            colorSpecPicker(
                ovr != nullptr ? ovr->fillColor : std::optional<ColorSpec>{}, true, tr("common.states.inherit")
            ),
            "monitor background solid color", true
        );
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.monitor-directory.label"),
            monitorPath("directory"),
            TextSetting{
                .value = ovr != nullptr && ovr->directory.has_value() ? *ovr->directory : "",
                .placeholder = FileUtils::defaultPicturesDirectory().string(),
                .browseMode = TextSettingBrowseMode::SelectFolder,
                .browseFileExtensions = {}
            },
            "monitor folder"
        );
        e.visibleWhen = perMonOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.monitor-directory-light.label"),
            monitorPath("directory_light"),
            TextSetting{
                .value = ovr != nullptr && ovr->directoryLight.has_value() ? *ovr->directoryLight : "",
                .placeholder = tr("settings.schema.wallpaper.monitor-directory-light.placeholder"),
                .browseMode = TextSettingBrowseMode::SelectFolder,
                .browseFileExtensions = {}
            },
            "monitor light folder", true
        );
        e.visibleWhen = perMonOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.monitor-directory-dark.label"),
            monitorPath("directory_dark"),
            TextSetting{
                .value = ovr != nullptr && ovr->directoryDark.has_value() ? *ovr->directoryDark : "",
                .placeholder = tr("settings.schema.wallpaper.monitor-directory-dark.placeholder"),
                .browseMode = TextSettingBrowseMode::SelectFolder,
                .browseFileExtensions = {}
            },
            "monitor dark folder", true
        );
        e.visibleWhen = perMonOn;
        entries.push_back(std::move(e));
      }
    }
    {
      MultiSelectSetting transitions;
      transitions.options.reserve(std::size(kWallpaperTransitions));
      for (const auto& opt : kWallpaperTransitions) {
        transitions.options.push_back(SelectOption{std::string(opt.key), tr(opt.labelKey)});
      }
      transitions.selectedValues.reserve(cfg.wallpaper.transitions.size());
      for (const auto& t : cfg.wallpaper.transitions) {
        transitions.selectedValues.emplace_back(enumToKey(kWallpaperTransitions, t));
      }
      entries.push_back(makeEntry(
          SettingsSection::Wallpaper, "transition", tr("settings.schema.wallpaper.transitions.label"),
          tr("settings.schema.wallpaper.transitions.description"), {"wallpaper", "transition"}, std::move(transitions),
          "effects animation pool"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "transition", tr("settings.schema.wallpaper.transition-duration.label"),
        tr("settings.schema.wallpaper.transition-duration.description"), {"wallpaper", "transition_duration"},
        sliderFor(
            cfg.wallpaper.transitionDurationMs, noctalia::config::schema::kWallpaperTransitionDurationRange, true
        ),
        "fade animation"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "transition", tr("settings.schema.wallpaper.edge-smoothness.label"),
        tr("settings.schema.wallpaper.edge-smoothness.description"), {"wallpaper", "edge_smoothness"},
        sliderFor(cfg.wallpaper.edgeSmoothness, noctalia::config::schema::kUnitRange, false), "transition feathering",
        true
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "transition", tr("settings.schema.wallpaper.transition-on-startup.label"),
        tr("settings.schema.wallpaper.transition-on-startup.description"), {"wallpaper", "transition_on_startup"},
        ToggleSetting{cfg.wallpaper.transitionOnStartup}, "startup animation"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "transition", tr("settings.schema.wallpaper.sync-transitions.label"),
        tr("settings.schema.wallpaper.sync-transitions.description"), {"wallpaper", "sync_transitions"},
        ToggleSetting{cfg.wallpaper.syncTransitions}, "all monitors same effect"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "automation", tr("settings.schema.wallpaper.automation.label"),
        tr("settings.schema.wallpaper.automation.description"), {"wallpaper", "automation", "enabled"},
        ToggleSetting{cfg.wallpaper.automation.enabled}, "rotate slideshow"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "automation", tr("settings.schema.wallpaper.automation-interval.label"),
        tr("settings.schema.wallpaper.automation-interval.description"),
        {"wallpaper", "automation", "interval_seconds"},
        StepperSetting{
            .value = cfg.wallpaper.automation.intervalSeconds,
            .minValue = static_cast<int>(noctalia::config::schema::kWallpaperAutomationIntervalRange.min.value()),
            .maxValue = static_cast<int>(noctalia::config::schema::kWallpaperAutomationIntervalRange.max.value()),
            .step = static_cast<int>(noctalia::config::schema::kWallpaperAutomationIntervalRange.step.value()),
            .valueSuffix = "s",
        },
        "rotate slideshow"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "automation", tr("settings.schema.wallpaper.automation-order.label"),
        tr("settings.schema.wallpaper.automation-order.description"), {"wallpaper", "automation", "order"},
        asSegmented(enumSelect(kWallpaperAutomationOrders, cfg.wallpaper.automation.order)), "rotate slideshow"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "automation", tr("settings.schema.wallpaper.automation-recursive.label"),
        tr("settings.schema.wallpaper.automation-recursive.description"), {"wallpaper", "automation", "recursive"},
        ToggleSetting{cfg.wallpaper.automation.recursive}, "subdirectories"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "automation", tr("settings.schema.wallpaper.automation-pause-on-fullscreen.label"),
        tr("settings.schema.wallpaper.automation-pause-on-fullscreen.description"),
        {"wallpaper", "automation", "pause_on_fullscreen"}, ToggleSetting{cfg.wallpaper.automation.pauseOnFullscreen},
        "rotate slideshow game video"
    ));
  }

  void addTemplateSettings(std::vector<SettingEntry>& entries, const Config& cfg) {
    using i18n::tr;
    const auto builtInTemplatesOn =
        SettingVisibility{[](const Config& c) { return c.theme.templates.enableBuiltinTemplates; }};
    const auto communityTemplatesOn =
        SettingVisibility{[](const Config& c) { return c.theme.templates.enableCommunityTemplates; }};
    entries.push_back(makeEntry(
        SettingsSection::Templates, "built-in", tr("settings.schema.templates.enable-builtins.label"),
        tr("settings.schema.templates.enable-builtins.description"), {"theme", "templates", "enable_builtin_templates"},
        ToggleSetting{cfg.theme.templates.enableBuiltinTemplates}, "theme templates"
    ));
    {
      auto e = makeEntry(
          SettingsSection::Templates, "built-in", tr("settings.schema.templates.builtin-ids.label"),
          tr("settings.schema.templates.builtin-ids.description"), {"theme", "templates", "builtin_ids"},
          TemplateGridSetting{
              .options = noctalia::theme::availableTemplates(),
              .selectedValues = cfg.theme.templates.builtinIds,
              .emptyText = tr("settings.schema.templates.builtin-ids.empty"),
          },
          "theme templates apps foot walker gtk"
      );
      e.visibleWhen = builtInTemplatesOn;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        SettingsSection::Templates, "community", tr("settings.schema.templates.enable-community-templates.label"),
        tr("settings.schema.templates.enable-community-templates.description"),
        {"theme", "templates", "enable_community_templates"},
        ToggleSetting{cfg.theme.templates.enableCommunityTemplates}, "theme templates community"
    ));
    {
      const std::size_t enabledCount = cfg.theme.templates.communityIds.size();
      auto e = makeEntry(
          SettingsSection::Templates, "community", tr("settings.schema.templates.community-ids.label"),
          enabledCount == 0 ? tr("settings.schema.templates.community-ids.description")
                            : tr("settings.templates.store.enabled-count", "count", std::to_string(enabledCount)),
          {},
          ButtonSetting{
              .label = tr("settings.templates.store.browse"),
              .glyph = "apps",
          },
          "theme templates community apps discord fuzzel vscode walker store browse"
      );
      e.visibleWhen = communityTemplatesOn;
      entries.push_back(std::move(e));
    }
  }

} // namespace settings::registry
