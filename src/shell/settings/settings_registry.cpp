#include "shell/settings/settings_registry.h"

#include "core/process.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "theme/builtin_palettes.h"
#include "theme/builtin_templates.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace settings {
  namespace {

    constexpr int kBarMarginMax = 4096;

    SelectSetting asSegmented(SelectSetting setting) {
      setting.segmented = true;
      return setting;
    }

    StepperSetting barMarginStepper(std::int32_t value) {
      return StepperSetting{
          .value = static_cast<int>(value),
          .minValue = 0,
          .maxValue = kBarMarginMax,
          .step = 1,
          .valueSuffix = "px",
      };
    }

    std::optional<int> radiusStepperValue(const std::optional<double>& value) {
      if (!value.has_value()) {
        return std::nullopt;
      }
      return std::clamp(static_cast<int>(std::lround(*value)), 0, 80);
    }

    int radiusStepperFallback(const std::optional<double>& value) { return radiusStepperValue(value).value_or(8); }

    template <typename T, std::size_t N> SelectSetting enumSelect(const EnumOption<T> (&options)[N], T selected) {
      std::vector<SelectOption> opts;
      opts.reserve(N);
      std::string selectedValue;
      for (const auto& option : options) {
        std::string key(option.key);
        if (option.value == selected) {
          selectedValue = key;
        }
        opts.push_back(SelectOption{std::move(key), i18n::tr(option.labelKey)});
      }
      if (selectedValue.empty() && N > 0) {
        selectedValue = std::string(options[0].key);
      }
      return SelectSetting{std::move(opts), std::move(selectedValue)};
    }

    SelectSetting
    plainSelect(std::initializer_list<std::pair<std::string_view, std::string_view>> items, std::string_view selected) {
      std::vector<SelectOption> opts;
      opts.reserve(items.size());
      for (const auto& [value, labelKey] : items) {
        opts.push_back(SelectOption{std::string(value), i18n::tr(labelKey)});
      }
      return SelectSetting{std::move(opts), std::string(selected)};
    }

    ColorSwatchPreview palettePreviewFromPalette(const ::Palette& palette) {
      return ColorSwatchPreview{
          .surface = fixedColorSpec(palette.surface),
          .swatches = {
              fixedColorSpec(palette.primary),
              fixedColorSpec(palette.secondary),
              fixedColorSpec(palette.tertiary),
              fixedColorSpec(palette.error),
          },
      };
    }

    ColorSwatchPreview builtinPalettePreview(const noctalia::theme::BuiltinPalette& palette, ThemeMode mode) {
      return palettePreviewFromPalette(mode == ThemeMode::Light ? palette.light.palette : palette.dark.palette);
    }

    SelectSetting builtinPaletteSelect(std::string_view selected, ThemeMode mode) {
      std::vector<SelectOption> opts;
      opts.reserve(noctalia::theme::builtinPalettes().size());
      for (const auto& palette : noctalia::theme::builtinPalettes()) {
        opts.push_back(
            SelectOption{
                .value = std::string(palette.name),
                .label = std::string(palette.name),
                .description = {},
                .preview = builtinPalettePreview(palette, mode),
            }
        );
      }
      return SelectSetting{
          .options = std::move(opts), .selectedValue = std::string(selected), .preferredWidth = 240.0f
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
           {"dysfunctional", "theme.scheme.dysfunctional"},
           {"muted", "theme.scheme.muted"}},
          selected
      );
    }

    std::vector<SelectOption> controlCenterShortcutOptions() {
      std::vector<SelectOption> opts;
      opts.reserve(ShortcutRegistry::catalog().size());
      for (const auto& shortcut : ShortcutRegistry::catalog()) {
        opts.push_back(SelectOption{std::string(shortcut.type), i18n::tr(shortcut.labelKey)});
      }
      return opts;
    }

    SelectSetting languageSelect(std::string_view selected) {
      std::vector<SelectOption> opts;
      opts.reserve(i18n::kSupportedLanguages.size() + 1);
      opts.push_back(SelectOption{"", i18n::tr("common.states.auto")});
      for (const auto& language : i18n::kSupportedLanguages) {
        opts.push_back(SelectOption{std::string(language.code), std::string(language.displayName)});
      }
      return SelectSetting{std::move(opts), std::string(selected)};
    }

    ColorSpecPickerSetting
    colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel = {}) {
      return ColorSpecPickerSetting{
          .roles = {},
          .selectedValue = optionalColorSpecConfigValue(selected),
          .allowNone = allowNone,
          .allowCustomColor = true,
          .noneLabel = std::move(noneLabel),
      };
    }

    ColorSpecPickerSetting
    colorSpecPicker(const ColorSpec& selected, bool allowNone = false, std::string noneLabel = {}) {
      return colorSpecPicker(std::optional<ColorSpec>{selected}, allowNone, std::move(noneLabel));
    }

    std::string pathText(const std::vector<std::string>& path) {
      std::string out;
      for (const auto& part : path) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += part;
      }
      return out;
    }

    SettingEntry makeEntry(
        std::string section, std::string group, std::string title, std::string subtitle, std::vector<std::string> path,
        SettingControl control, std::string tags = {}, bool advanced = false
    ) {
      std::string searchText = section + " " + group + " " + title + " " + subtitle + " " + pathText(path) + " " + tags;
      if (advanced) {
        searchText += " advanced";
      }
      return SettingEntry{
          .section = std::move(section),
          .group = std::move(group),
          .title = std::move(title),
          .subtitle = std::move(subtitle),
          .path = std::move(path),
          .control = std::move(control),
          .advanced = advanced,
          .searchText = StringUtils::toLower(searchText),
          .visibleWhen = std::nullopt,
      };
    }

  } // namespace

  const BarConfig* findBar(const Config& cfg, std::string_view name) {
    for (const auto& bar : cfg.bars) {
      if (bar.name == name) {
        return &bar;
      }
    }
    return nullptr;
  }

  const BarMonitorOverride* findMonitorOverride(const BarConfig& bar, std::string_view match) {
    for (const auto& ovr : bar.monitorOverrides) {
      if (ovr.match == match) {
        return &ovr;
      }
    }
    return nullptr;
  }

  std::vector<std::string> barNames(const Config& cfg) {
    std::vector<std::string> names;
    names.reserve(cfg.bars.size());
    for (const auto& bar : cfg.bars) {
      names.push_back(bar.name);
    }
    return names;
  }

  std::string normalizedSettingQuery(std::string_view query) { return StringUtils::toLower(query); }

  bool matchesNormalizedSettingQuery(const SettingEntry& entry, std::string_view normalizedQuery) {
    if (normalizedQuery.empty()) {
      return true;
    }
    return entry.searchText.find(normalizedQuery) != std::string::npos;
  }

  bool matchesSettingQuery(const SettingEntry& entry, std::string_view query) {
    return matchesNormalizedSettingQuery(entry, normalizedSettingQuery(query));
  }

  bool isBarMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 5 && path[0] == "bar" && path[2] == "monitor";
  }

  bool settingEntryMatchesBarNavigation(
      const SettingEntry& entry, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  ) {
    if (entry.section != "bar" || entry.path.size() < 2 || entry.path[0] != "bar") {
      return true;
    }
    if (selectedBarName.empty() || entry.path[1] != selectedBarName) {
      return false;
    }
    const bool monitorEntry = isBarMonitorOverrideSettingPath(entry.path);
    if (selectedMonitorOverride.empty()) {
      return !monitorEntry;
    }
    return monitorEntry && entry.path[3] == selectedMonitorOverride;
  }

  std::string barSettingContentSectionKey(const SettingEntry& entry) {
    if (entry.section != "bar" || entry.path.size() < 2) {
      return entry.section;
    }
    std::string key = "bar:" + entry.path[1];
    if (isBarMonitorOverrideSettingPath(entry.path)) {
      key += ":monitor:" + entry.path[3];
    }
    return key;
  }

  std::string_view sectionGlyph(std::string_view section) {
    if (section == "appearance")
      return "adjustments-horizontal";
    if (section == "templates")
      return "color-swatch";
    if (section == "shell")
      return "app-window";
    if (section == "dock")
      return "layout-bottombar-inactive";
    if (section == "panels")
      return "layout-bottombar";
    if (section == "idle")
      return "coffee";
    if (section == "niri")
      return "niri";
    if (section == "wallpaper")
      return "paint";
    if (section == "desktop")
      return "layout-board";
    if (section == "services")
      return "stack-2";
    if (section == "hooks")
      return "link";
    if (section == "popups")
      return "message-circle";
    if (section == "notifications")
      return "bell";
    if (section == "bar")
      return "crop-3-2";
    return "settings";
  }

  std::vector<SettingEntry> buildSettingsRegistry(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride,
      const RegistryEnvironment& env
  ) {
    (void)selectedBar;
    (void)selectedMonitorOverride;
    using i18n::tr;
    const auto positionSelect = [](std::string_view selected) {
      return asSegmented(plainSelect(
          {{"top", "settings.options.edge.top"},
           {"bottom", "settings.options.edge.bottom"},
           {"left", "settings.options.edge.left"},
           {"right", "settings.options.edge.right"}},
          selected
      ));
    };
    const auto launcherPositionSelect = [](std::string_view selected) {
      return asSegmented(plainSelect(
          {{"none", "settings.options.dock-launcher-position.none"},
           {"start", "settings.options.dock-launcher-position.start"},
           {"end", "settings.options.dock-launcher-position.end"}},
          selected
      ));
    };
    std::vector<SettingEntry> entries;

    // Appearance
    entries.push_back(makeEntry(
        "appearance", "theme", tr("settings.schema.appearance.theme-mode.label"),
        tr("settings.schema.appearance.theme-mode.description"), {"theme", "mode"},
        asSegmented(enumSelect(kThemeModes, cfg.theme.mode)), "dark light auto colors"
    ));
    entries.push_back(makeEntry(
        "appearance", "theme", tr("settings.schema.appearance.palette-source.label"),
        tr("settings.schema.appearance.palette-source.description"), {"theme", "source"},
        asSegmented(enumSelect(kPaletteSources, cfg.theme.source)), "palette colors"
    ));
    if (cfg.theme.source == PaletteSource::Builtin) {
      entries.push_back(makeEntry(
          "appearance", "theme", tr("settings.schema.appearance.builtin-palette.label"),
          tr("settings.schema.appearance.builtin-palette.description"), {"theme", "builtin"},
          builtinPaletteSelect(cfg.theme.builtinPalette, cfg.theme.mode), "builtin palette colors"
      ));
    } else if (cfg.theme.source == PaletteSource::Wallpaper) {
      entries.push_back(makeEntry(
          "appearance", "theme", tr("settings.schema.appearance.wallpaper-generation-scheme.label"),
          tr("settings.schema.appearance.wallpaper-generation-scheme.description"), {"theme", "wallpaper_scheme"},
          wallpaperSchemeSelect(cfg.theme.wallpaperScheme), "wallpaper palette generator scheme material you m3 colors"
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
            .preferredHeight = 240.0f,
        };
      }
      entries.push_back(makeEntry(
          "appearance", "theme", tr("settings.schema.appearance.community-palette.label"),
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
            .preferredHeight = 240.0f,
        };
      }
      entries.push_back(makeEntry(
          "appearance", "theme", tr("settings.schema.appearance.custom-palette.label"),
          tr("settings.schema.appearance.custom-palette.description"), {"theme", "custom_palette"},
          std::move(customPaletteControl), "custom palette colors"
      ));
    }
    entries.push_back(makeEntry(
        "appearance", "interface", tr("settings.schema.appearance.ui-scale.label"),
        tr("settings.schema.appearance.ui-scale.description"), {"shell", "ui_scale"},
        SliderSetting{cfg.shell.uiScale, 0.5f, 2.5f, 0.05f, false}, "size"
    ));
    entries.push_back(makeEntry(
        "appearance", "interface", tr("settings.schema.appearance.corner-roundness.label"),
        tr("settings.schema.appearance.corner-roundness.description"), {"shell", "corner_radius_scale"},
        SliderSetting{cfg.shell.cornerRadiusScale, 0.0f, 2.0f, 0.05f, false}, "rounded corners radius"
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
            .preferredHeight = 280.0f,
        };
      }
      entries.push_back(makeEntry(
          "appearance", "interface", tr("settings.schema.appearance.font-family.label"),
          tr("settings.schema.appearance.font-family.description"), {"shell", "font_family"},
          std::move(fontFamilyControl), "typeface"
      ));
    }
    entries.push_back(makeEntry(
        "appearance", "interface", tr("settings.schema.appearance.language.label"),
        tr("settings.schema.appearance.language.description"), {"shell", "lang"}, languageSelect(cfg.shell.lang),
        "locale translation", true
    ));
    entries.push_back(makeEntry(
        "appearance", "motion", tr("settings.schema.appearance.animations.label"),
        tr("settings.schema.appearance.animations.description"), {"shell", "animation", "enabled"},
        ToggleSetting{cfg.shell.animation.enabled}, "motion"
    ));
    entries.push_back(makeEntry(
        "appearance", "motion", tr("settings.schema.appearance.animation-speed.label"),
        tr("settings.schema.appearance.animation-speed.description"), {"shell", "animation", "speed"},
        SliderSetting{cfg.shell.animation.speed, 0.1f, 4.0f, 0.05f, false}, "motion"
    ));
    entries.push_back(makeEntry(
        "appearance", "effects", tr("settings.schema.shared.shadow-blur.label"),
        tr("settings.schema.appearance.global-shadow-blur.description"), {"shell", "shadow", "blur"},
        SliderSetting{static_cast<float>(cfg.shell.shadow.blur), 0.0f, 100.0f, 1.0f, true}, "shadow depth"
    ));
    entries.push_back(makeEntry(
        "appearance", "effects", tr("settings.schema.shared.shadow-x.label"),
        tr("settings.schema.appearance.global-shadow-x.description"), {"shell", "shadow", "offset_x"},
        SliderSetting{static_cast<float>(cfg.shell.shadow.offsetX), -40.0f, 40.0f, 1.0f, true}, "shadow offset", true
    ));
    entries.push_back(makeEntry(
        "appearance", "effects", tr("settings.schema.shared.shadow-y.label"),
        tr("settings.schema.appearance.global-shadow-y.description"), {"shell", "shadow", "offset_y"},
        SliderSetting{static_cast<float>(cfg.shell.shadow.offsetY), -40.0f, 40.0f, 1.0f, true}, "shadow offset", true
    ));
    entries.push_back(makeEntry(
        "appearance", "effects", tr("settings.schema.shared.shadow-alpha.label"),
        tr("settings.schema.appearance.global-shadow-alpha.description"), {"shell", "shadow", "alpha"},
        SliderSetting{cfg.shell.shadow.alpha, 0.0f, 1.0f, 0.01f, false}, "shadow opacity", true
    ));

    // Wallpaper
    entries.push_back(makeEntry(
        "wallpaper", "general", tr("settings.schema.shared.enabled.label"),
        tr("settings.schema.wallpaper.enabled.description"), {"wallpaper", "enabled"},
        ToggleSetting{cfg.wallpaper.enabled}, "background image"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "general", tr("settings.schema.wallpaper.fill-mode.label"),
        tr("settings.schema.wallpaper.fill-mode.description"), {"wallpaper", "fill_mode"},
        asSegmented(enumSelect(kWallpaperFillModes, cfg.wallpaper.fillMode)), "scale aspect"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "general", tr("settings.schema.wallpaper.fill-color.label"),
        tr("settings.schema.wallpaper.fill-color.description"), {"wallpaper", "fill_color"},
        colorSpecPicker(cfg.wallpaper.fillColor, true), "background solid color"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "directories", tr("settings.schema.wallpaper.directory.label"),
        tr("settings.schema.wallpaper.directory.description"), {"wallpaper", "directory"},
        TextSetting{
            .value = cfg.wallpaper.directory,
            .placeholder = "~/Pictures/Wallpapers",
            .browseMode = TextSettingBrowseMode::SelectFolder,
            .browseFileExtensions = {}
        },
        "folder path"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "directories", tr("settings.schema.wallpaper.directory-light.label"),
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
        "wallpaper", "directories", tr("settings.schema.wallpaper.directory-dark.label"),
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
        "wallpaper", "directories", tr("settings.schema.wallpaper.per-monitor-directories.label"),
        tr("settings.schema.wallpaper.per-monitor-directories.description"), {"wallpaper", "per_monitor_directories"},
        ToggleSetting{cfg.wallpaper.perMonitorDirectories}, "per display folder"
    ));
    for (const auto& outputOpt : env.availableOutputs) {
      const std::string& connector = outputOpt.value;
      if (connector.empty()) {
        continue;
      }
      const std::vector<std::string> root = {"wallpaper", "monitor", connector};
      auto mpath = [&](std::string key) {
        std::vector<std::string> p = root;
        p.push_back(std::move(key));
        return p;
      };
      const std::string section = "wallpaper";
      const WallpaperMonitorOverride* ovr = nullptr;
      for (const auto& candidate : cfg.wallpaper.monitorOverrides) {
        if (candidate.match == connector) {
          ovr = &candidate;
          break;
        }
      }
      auto perMonOn = SettingVisibility{{"wallpaper", "per_monitor_directories"}, {"true"}};
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.fill-color.label"), mpath("fill_color"),
            colorSpecPicker(
                ovr != nullptr ? ovr->fillColor : std::optional<ColorSpec>{}, true, tr("common.states.inherit")
            ),
            "monitor background solid color", true
        );
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "monitors", connector, tr("settings.schema.wallpaper.monitor-directory.label"), mpath("directory"),
            TextSetting{
                .value = ovr != nullptr && ovr->directory.has_value() ? *ovr->directory : "",
                .placeholder = "~/Pictures/Wallpapers",
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
            mpath("directory_light"),
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
            mpath("directory_dark"),
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
      transitions.requireAtLeastOne = true;
      entries.push_back(makeEntry(
          "wallpaper", "transition", tr("settings.schema.wallpaper.transitions.label"),
          tr("settings.schema.wallpaper.transitions.description"), {"wallpaper", "transition"}, std::move(transitions),
          "effects animation pool"
      ));
    }
    entries.push_back(makeEntry(
        "wallpaper", "transition", tr("settings.schema.wallpaper.transition-duration.label"),
        tr("settings.schema.wallpaper.transition-duration.description"), {"wallpaper", "transition_duration"},
        SliderSetting{cfg.wallpaper.transitionDurationMs, 100.0f, 30000.0f, 100.0f, true}, "fade animation"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "transition", tr("settings.schema.wallpaper.edge-smoothness.label"),
        tr("settings.schema.wallpaper.edge-smoothness.description"), {"wallpaper", "edge_smoothness"},
        SliderSetting{cfg.wallpaper.edgeSmoothness, 0.0f, 1.0f, 0.01f, false}, "transition feathering", true
    ));
    entries.push_back(makeEntry(
        "wallpaper", "automation", tr("settings.schema.wallpaper.automation.label"),
        tr("settings.schema.wallpaper.automation.description"), {"wallpaper", "automation", "enabled"},
        ToggleSetting{cfg.wallpaper.automation.enabled}, "rotate slideshow"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "automation", tr("settings.schema.wallpaper.automation-interval.label"),
        tr("settings.schema.wallpaper.automation-interval.description"),
        {"wallpaper", "automation", "interval_minutes"},
        SliderSetting{static_cast<float>(cfg.wallpaper.automation.intervalMinutes), 0.0f, 1440.0f, 1.0f, true},
        "rotate slideshow"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "automation", tr("settings.schema.wallpaper.automation-order.label"),
        tr("settings.schema.wallpaper.automation-order.description"), {"wallpaper", "automation", "order"},
        asSegmented(enumSelect(kWallpaperAutomationOrders, cfg.wallpaper.automation.order)), "rotate slideshow"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "automation", tr("settings.schema.wallpaper.automation-recursive.label"),
        tr("settings.schema.wallpaper.automation-recursive.description"), {"wallpaper", "automation", "recursive"},
        ToggleSetting{cfg.wallpaper.automation.recursive}, "subdirectories", true
    ));

    // Templates
    entries.push_back(makeEntry(
        "templates", "built-in", tr("settings.schema.templates.enable-builtins.label"),
        tr("settings.schema.templates.enable-builtins.description"), {"theme", "templates", "enable_builtin_templates"},
        ToggleSetting{cfg.theme.templates.enableBuiltinTemplates}, "theme templates"
    ));
    {
      const auto availableTemplates = noctalia::theme::availableTemplates();
      std::vector<SelectOption> templateOptions;
      templateOptions.reserve(availableTemplates.size());
      for (const auto& t : availableTemplates) {
        templateOptions.push_back(SelectOption{t.id, t.displayName});
      }
      entries.push_back(makeEntry(
          "templates", "built-in", tr("settings.schema.templates.builtin-ids.label"),
          tr("settings.schema.templates.builtin-ids.description"), {"theme", "templates", "builtin_ids"},
          ListSetting{.items = cfg.theme.templates.builtinIds, .suggestedOptions = std::move(templateOptions)},
          "theme templates apps foot walker gtk"
      ));
    }
    entries.push_back(makeEntry(
        "templates", "community", tr("settings.schema.templates.enable-community-templates.label"),
        tr("settings.schema.templates.enable-community-templates.description"),
        {"theme", "templates", "enable_community_templates"},
        ToggleSetting{cfg.theme.templates.enableCommunityTemplates}, "theme templates community"
    ));
    entries.push_back(makeEntry(
        "templates", "community", tr("settings.schema.templates.community-ids.label"),
        tr("settings.schema.templates.community-ids.description"), {"theme", "templates", "community_ids"},
        ListSetting{.items = cfg.theme.templates.communityIds, .suggestedOptions = env.communityTemplates},
        "theme templates community apps discord fuzzel vscode walker"
    ));

    // Dock
    entries.push_back(makeEntry(
        "dock", "general", tr("settings.schema.shared.enabled.label"), tr("settings.schema.dock.enabled.description"),
        {"dock", "enabled"}, ToggleSetting{cfg.dock.enabled}, "launcher apps"
    ));
    entries.push_back(makeEntry(
        "dock", "general", tr("settings.schema.dock.active-monitor-only.label"),
        tr("settings.schema.dock.active-monitor-only.description"), {"dock", "active_monitor_only"},
        ToggleSetting{cfg.dock.activeMonitorOnly}, "monitor"
    ));
    entries.push_back(makeEntry(
        "dock", "general", tr("settings.schema.dock.monitors.label"), tr("settings.schema.dock.monitors.description"),
        {"dock", "monitors"}, ListSetting{.items = cfg.dock.monitors, .suggestedOptions = env.availableOutputs},
        "monitor output display screen"
    ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.shared.auto-hide.label"),
        tr("settings.schema.dock.auto-hide.description"), {"dock", "auto_hide"}, ToggleSetting{cfg.dock.autoHide},
        "autohide"
    ));
    if (cfg.dock.autoHide)
      entries.push_back(makeEntry(
          "dock", "behavior", tr("settings.schema.shared.reserve-space.label"),
          tr("settings.schema.dock.reserve-space.description"), {"dock", "reserve_space"},
          ToggleSetting{cfg.dock.reserveSpace}, "exclusive zone"
      ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.dock.show-running.label"),
        tr("settings.schema.dock.show-running.description"), {"dock", "show_running"},
        ToggleSetting{cfg.dock.showRunning}, "windows"
    ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.dock.show-dots.label"),
        tr("settings.schema.dock.show-dots.description"), {"dock", "show_dots"}, ToggleSetting{cfg.dock.showDots},
        "running dots"
    ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.dock.show-instance-count.label"),
        tr("settings.schema.dock.show-instance-count.description"), {"dock", "show_instance_count"},
        ToggleSetting{cfg.dock.showInstanceCount}, "badge windows"
    ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.dock.launcher-position.label"),
        tr("settings.schema.dock.launcher-position.description"), {"dock", "launcher_position"},
        launcherPositionSelect(cfg.dock.launcherPosition), "launcher apps grid"
    ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.dock.launcher-icon.label"),
        tr("settings.schema.dock.launcher-icon.description"), {"dock", "launcher_icon"},
        TextSetting{.value = cfg.dock.launcherIcon, .placeholder = "grid-dots", .browseFileExtensions = {}},
        "launcher apps icon glyph"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.position.label"), tr("settings.schema.dock.position.description"),
        {"dock", "position"}, positionSelect(cfg.dock.position), "edge"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.dock.icon-size.label"), tr("settings.schema.dock.icon-size.description"),
        {"dock", "icon_size"}, SliderSetting{static_cast<float>(cfg.dock.iconSize), 16.0f, 128.0f, 1.0f, true}, "apps"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.padding.label"), tr("settings.schema.dock.padding.description"),
        {"dock", "padding"}, SliderSetting{static_cast<float>(cfg.dock.padding), 0.0f, 100.0f, 1.0f, true}, "inset"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.dock.item-spacing.label"),
        tr("settings.schema.dock.item-spacing.description"), {"dock", "item_spacing"},
        SliderSetting{static_cast<float>(cfg.dock.itemSpacing), 0.0f, 100.0f, 1.0f, true}, "gap"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.ends-margin.label"),
        tr("settings.schema.dock.ends-margin.description"), {"dock", "margin_ends"},
        SliderSetting{static_cast<float>(cfg.dock.marginEnds), 0.0f, 500.0f, 1.0f, true}, "gap inset"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.edge-margin.label"),
        tr("settings.schema.dock.edge-margin.description"), {"dock", "margin_edge"},
        SliderSetting{static_cast<float>(cfg.dock.marginEdge), 0.0f, 100.0f, 1.0f, true}, "gap inset"
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-radius.label"),
        tr("settings.schema.dock.corner-radius.description"), {"dock", "radius"},
        SliderSetting{static_cast<float>(cfg.dock.radius), 0.0f, 80.0f, 1.0f, true}, "rounded"
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-top-left.label"),
        tr("settings.schema.dock.corner-top-left.description"), {"dock", "radius_top_left"},
        SliderSetting{static_cast<float>(cfg.dock.radiusTopLeft), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-top-right.label"),
        tr("settings.schema.dock.corner-top-right.description"), {"dock", "radius_top_right"},
        SliderSetting{static_cast<float>(cfg.dock.radiusTopRight), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-bottom-left.label"),
        tr("settings.schema.dock.corner-bottom-left.description"), {"dock", "radius_bottom_left"},
        SliderSetting{static_cast<float>(cfg.dock.radiusBottomLeft), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-bottom-right.label"),
        tr("settings.schema.dock.corner-bottom-right.description"), {"dock", "radius_bottom_right"},
        SliderSetting{static_cast<float>(cfg.dock.radiusBottomRight), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.background-opacity.label"),
        tr("settings.schema.dock.background-opacity.description"), {"dock", "background_opacity"},
        SliderSetting{cfg.dock.backgroundOpacity, 0.0f, 1.0f, 0.01f, false}, "alpha"
    ));
    entries.push_back(makeEntry(
        "dock", "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.dock.shadow.description"),
        {"dock", "shadow"}, ToggleSetting{cfg.dock.shadow}, "shadow"
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.active-icon-scale.label"),
        tr("settings.schema.dock.active-icon-scale.description"), {"dock", "active_scale"},
        SliderSetting{cfg.dock.activeScale, 0.1f, 1.75f, 0.05f, false}, "focused", true
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.inactive-icon-scale.label"),
        tr("settings.schema.dock.inactive-icon-scale.description"), {"dock", "inactive_scale"},
        SliderSetting{cfg.dock.inactiveScale, 0.1f, 1.0f, 0.05f, false}, "unfocused", true
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.active-icon-opacity.label"),
        tr("settings.schema.dock.active-icon-opacity.description"), {"dock", "active_opacity"},
        SliderSetting{cfg.dock.activeOpacity, 0.0f, 1.0f, 0.01f, false}, "focused alpha", true
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.inactive-icon-opacity.label"),
        tr("settings.schema.dock.inactive-icon-opacity.description"), {"dock", "inactive_opacity"},
        SliderSetting{cfg.dock.inactiveOpacity, 0.0f, 1.0f, 0.01f, false}, "unfocused alpha", true
    ));
    entries.push_back(makeEntry(
        "dock", "pinned-apps", tr("settings.schema.dock.pinned-apps.label"),
        tr("settings.schema.dock.pinned-apps.description"), {"dock", "pinned"}, ListSetting{.items = cfg.dock.pinned},
        "favorites"
    ));

    // Panels
    entries.push_back(makeEntry(
        "panels", "effects", tr("settings.schema.panels.transparency-mode.label"),
        tr("settings.schema.panels.transparency-mode.description"), {"shell", "panel", "transparency_mode"},
        asSegmented(enumSelect(kPanelTransparencyModes, cfg.shell.panel.transparencyMode)),
        "glass opacity alpha translucent cards blur"
    ));
    entries.push_back(makeEntry(
        "panels", "effects", tr("settings.schema.panels.borders.label"),
        tr("settings.schema.panels.borders.description"), {"shell", "panel", "borders"},
        ToggleSetting{cfg.shell.panel.borders}, "outline border card"
    ));
    entries.push_back(makeEntry(
        "panels", "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.panels.shadow.description"),
        {"shell", "panel", "shadow"}, ToggleSetting{cfg.shell.panel.shadow}, "shadow depth"
    ));
    entries.push_back(makeEntry(
        "panels", "control-center", tr("settings.schema.panels.placement-control-center.label"),
        tr("settings.schema.panels.placement-control-center.description"),
        {"shell", "panel", "control_center_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.controlCenterPlacement)),
        "attached floating centered bar panel position"
    ));
    {
      auto e = makeEntry(
          "panels", "control-center", tr("settings.schema.panels.open-near-click-control-center.label"),
          tr("settings.schema.panels.open-near-click-control-center.description"),
          {"shell", "panel", "open_near_click_control_center"},
          ToggleSetting{cfg.shell.panel.openNearClickControlCenter}, "open near click position anchor"
      );
      e.visibleWhen = SettingVisibility{{"shell", "panel", "control_center_placement"}, {"attached", "floating"}};
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "panels", "control-center", tr("settings.schema.panels.compact-control-center.label"),
        tr("settings.schema.panels.compact-control-center.description"), {"control_center", "compact"},
        ToggleSetting{cfg.controlCenter.compact}, "compact sidebar icons narrow"
    ));
    entries.push_back(makeEntry(
        "panels", "control-center", tr("settings.schema.panels.home-shortcuts.label"),
        tr("settings.schema.panels.home-shortcuts.description"), {"control_center", "shortcuts"},
        ShortcutListSetting{
            .items = cfg.controlCenter.shortcuts, .suggestedOptions = controlCenterShortcutOptions(), .maxItems = 6
        },
        "quick settings shortcuts toggles wifi bluetooth caffeine night light dnd power media weather clipboard"
    ));
    entries.push_back(makeEntry(
        "panels", "launcher", tr("settings.schema.panels.placement-launcher.label"),
        tr("settings.schema.panels.placement-launcher.description"), {"shell", "panel", "launcher_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.launcherPlacement)),
        "attached floating centered bar panel position"
    ));
    {
      auto e = makeEntry(
          "panels", "launcher", tr("settings.schema.panels.open-near-click-launcher.label"),
          tr("settings.schema.panels.open-near-click-launcher.description"),
          {"shell", "panel", "open_near_click_launcher"}, ToggleSetting{cfg.shell.panel.openNearClickLauncher},
          "open near click position anchor"
      );
      e.visibleWhen = SettingVisibility{{"shell", "panel", "launcher_placement"}, {"attached", "floating"}};
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "panels", "launcher", tr("settings.schema.panels.launcher-categories.label"),
        tr("settings.schema.panels.launcher-categories.description"), {"shell", "panel", "launcher_categories"},
        ToggleSetting{cfg.shell.panel.launcherCategories}, "launcher categories filter"
    ));
    entries.push_back(makeEntry(
        "panels", "clipboard", tr("settings.schema.panels.placement-clipboard.label"),
        tr("settings.schema.panels.placement-clipboard.description"), {"shell", "panel", "clipboard_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.clipboardPlacement)),
        "attached floating centered bar panel position"
    ));
    {
      auto e = makeEntry(
          "panels", "clipboard", tr("settings.schema.panels.open-near-click-clipboard.label"),
          tr("settings.schema.panels.open-near-click-clipboard.description"),
          {"shell", "panel", "open_near_click_clipboard"}, ToggleSetting{cfg.shell.panel.openNearClickClipboard},
          "open near click position anchor"
      );
      e.visibleWhen = SettingVisibility{{"shell", "panel", "clipboard_placement"}, {"attached", "floating"}};
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "panels", "wallpaper", tr("settings.schema.panels.placement-wallpaper.label"),
        tr("settings.schema.panels.placement-wallpaper.description"), {"shell", "panel", "wallpaper_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.wallpaperPlacement)),
        "attached floating centered bar panel position"
    ));
    {
      auto e = makeEntry(
          "panels", "wallpaper", tr("settings.schema.panels.open-near-click-wallpaper.label"),
          tr("settings.schema.panels.open-near-click-wallpaper.description"),
          {"shell", "panel", "open_near_click_wallpaper"}, ToggleSetting{cfg.shell.panel.openNearClickWallpaper},
          "open near click position anchor"
      );
      e.visibleWhen = SettingVisibility{{"shell", "panel", "wallpaper_placement"}, {"attached", "floating"}};
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "panels", "session-panel", tr("settings.schema.panels.placement-session.label"),
        tr("settings.schema.panels.placement-session.description"), {"shell", "panel", "session_placement"},
        asSegmented(enumSelect(kPanelPlacements, cfg.shell.panel.sessionPlacement)),
        "attached floating centered bar panel power menu position"
    ));
    {
      auto e = makeEntry(
          "panels", "session-panel", tr("settings.schema.panels.open-near-click-session.label"),
          tr("settings.schema.panels.open-near-click-session.description"),
          {"shell", "panel", "open_near_click_session"}, ToggleSetting{cfg.shell.panel.openNearClickSession},
          "open near click position anchor"
      );
      e.visibleWhen = SettingVisibility{{"shell", "panel", "session_placement"}, {"attached", "floating"}};
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "panels", "session-panel", tr("settings.schema.panels.session-actions.label"),
        tr("settings.schema.panels.session-actions.description"), {"shell", "session", "actions"},
        SessionPanelActionsSetting{.items = cfg.shell.session.actions},
        "session panel power menu logout reboot shutdown lock command actions order"
    ));

    // Desktop
    entries.push_back(makeEntry(
        "desktop", "widgets", tr("settings.schema.desktop.widgets.label"),
        tr("settings.schema.desktop.widgets.description"), {"desktop_widgets", "enabled"},
        ToggleSetting{cfg.desktopWidgets.enabled}, "desktop"
    ));
    entries.push_back(makeEntry(
        "desktop", "screen-corners", tr("settings.schema.desktop.screen-corners-enabled.label"),
        tr("settings.schema.desktop.screen-corners-enabled.description"), {"shell", "screen_corners", "enabled"},
        ToggleSetting{cfg.shell.screenCorners.enabled}, "screen corners rounded"
    ));
    entries.push_back(makeEntry(
        "desktop", "screen-corners", tr("settings.schema.desktop.screen-corners-size.label"),
        tr("settings.schema.desktop.screen-corners-size.description"), {"shell", "screen_corners", "size"},
        SliderSetting{static_cast<float>(cfg.shell.screenCorners.size), 1.0f, 100.0f, 1.0f, true},
        "screen corners radius"
    ));

    // Shell
    entries.push_back(makeEntry(
        "shell", "profile", tr("settings.schema.shell.avatar-path.label"),
        tr("settings.schema.shell.avatar-path.description"), {"shell", "avatar_path"},
        TextSetting{
            .value = cfg.shell.avatarPath,
            .placeholder = tr("settings.schema.shell.avatar-path.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = {".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp", ".gif"}
        },
        "image picture"
    ));
    entries.push_back(makeEntry(
        "shell", "network", tr("settings.schema.shell.offline-mode.label"),
        tr("settings.schema.shell.offline-mode.description"), {"shell", "offline_mode"},
        ToggleSetting{cfg.shell.offlineMode}, "network http fetch download"
    ));
    entries.push_back(makeEntry(
        "shell", "network", tr("settings.schema.shell.telemetry.label"),
        tr("settings.schema.shell.telemetry.description"), {"shell", "telemetry_enabled"},
        ToggleSetting{cfg.shell.telemetryEnabled}, "analytics ping privacy"
    ));
    entries.push_back(makeEntry(
        "shell", "security", tr("settings.schema.shell.polkit-agent.label"),
        tr("settings.schema.shell.polkit-agent.description"), {"shell", "polkit_agent"},
        ToggleSetting{cfg.shell.polkitAgent}, "auth password"
    ));
    entries.push_back(makeEntry(
        "shell", "security", tr("settings.schema.shell.password-style.label"),
        tr("settings.schema.shell.password-style.description"), {"shell", "password_style"},
        asSegmented(enumSelect(kPasswordMaskStyles, cfg.shell.passwordMaskStyle)), "polkit lock mask"
    ));
    entries.push_back(makeEntry(
        "shell", "behavior", tr("settings.schema.shell.middle-click-opens-widget-settings.label"),
        tr("settings.schema.shell.middle-click-opens-widget-settings.description"),
        {"shell", "middle_click_opens_widget_settings"}, ToggleSetting{cfg.shell.middleClickOpensWidgetSettings},
        "bar widget settings middle click configure"
    ));
    entries.push_back(makeEntry(
        "shell", "formats", tr("settings.schema.shell.time-format.label"),
        tr("settings.schema.shell.time-format.description"), {"shell", "time_format"},
        TextSetting{.value = cfg.shell.timeFormat, .placeholder = "{:%H:%M}", .browseFileExtensions = {}},
        "clock time format strftime chrono"
    ));
    entries.push_back(makeEntry(
        "shell", "formats", tr("settings.schema.shell.date-format.label"),
        tr("settings.schema.shell.date-format.description"), {"shell", "date_format"},
        TextSetting{.value = cfg.shell.dateFormat, .placeholder = "%A, %x", .browseFileExtensions = {}},
        "calendar date format strftime chrono"
    ));
    const SettingVisibility weatherOn{{"weather", "enabled"}, {"true"}};
    {
      auto e = makeEntry(
          "shell", "location", tr("settings.schema.shell.show-location.label"),
          tr("settings.schema.shell.show-location.description"), {"shell", "show_location"},
          ToggleSetting{cfg.shell.showLocation}, "weather"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    if (process::systemdAvailable()) {
      entries.push_back(makeEntry(
          "shell", "launcher", tr("settings.schema.shell.launch-apps-as-systemd-services.label"),
          tr("settings.schema.shell.launch-apps-as-systemd-services.description"),
          {"shell", "launch_apps_as_systemd_services"}, ToggleSetting{cfg.shell.launchAppsAsSystemdServices}
      ));
    }
    const SettingVisibility clipboardOn{{"shell", "clipboard_enabled"}, {"true"}};
    entries.push_back(makeEntry(
        "shell", "clipboard", tr("settings.schema.shell.clipboard-enabled.label"),
        tr("settings.schema.shell.clipboard-enabled.description"), {"shell", "clipboard_enabled"},
        ToggleSetting{cfg.shell.clipboardEnabled}, "clipboard history paste copy"
    ));
    {
      auto e = makeEntry(
          "shell", "clipboard", tr("settings.schema.shell.clipboard-history-max-entries.label"),
          tr("settings.schema.shell.clipboard-history-max-entries.description"),
          {"shell", "clipboard_history_max_entries"},
          StepperSetting{
              .value = std::clamp(cfg.shell.clipboardHistoryMaxEntries, 10, 200),
              .minValue = 10,
              .maxValue = 200,
              .step = 5
          },
          "clipboard history limit entries"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "shell", "clipboard", tr("settings.schema.shell.clipboard-auto-paste.label"),
          tr("settings.schema.shell.clipboard-auto-paste.description"), {"shell", "clipboard_auto_paste"},
          enumSelect(kClipboardAutoPasteModes, cfg.shell.clipboardAutoPaste), "clipboard paste"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "shell", "clipboard", tr("settings.schema.shell.clipboard-image-action.label"),
          tr("settings.schema.shell.clipboard-image-action.description"), {"shell", "clipboard_image_action_command"},
          TextSetting{
              .value = cfg.shell.clipboardImageActionCommand,
              .placeholder = tr("settings.schema.shell.clipboard-image-action.placeholder"),
              .width = 320.0f,
              .browseFileExtensions = {}
          },
          "clipboard image action annotation editor external gimp satty gradia"
      );
      e.visibleWhen = clipboardOn;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "shell", "screen-time", tr("settings.schema.shell.screen-time-enabled.label"),
        tr("settings.schema.shell.screen-time-enabled.description"), {"shell", "screen_time_enabled"},
        ToggleSetting{cfg.shell.screenTimeEnabled}, "screen time usage tracking control center"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-position.label"),
        tr("settings.schema.shell.osd-position.description"), {"osd", "position"},
        plainSelect(
            {{"top_right", "settings.options.screen-position.top-right"},
             {"top_left", "settings.options.screen-position.top-left"},
             {"top_center", "settings.options.screen-position.top-center"},
             {"bottom_right", "settings.options.screen-position.bottom-right"},
             {"bottom_left", "settings.options.screen-position.bottom-left"},
             {"bottom_center", "settings.options.screen-position.bottom-center"},
             {"center_right", "settings.options.screen-position.center-right"},
             {"center_left", "settings.options.screen-position.center-left"}},
            cfg.osd.position
        ),
        "hud overlay volume brightness"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-orientation.label"),
        tr("settings.schema.shell.osd-orientation.description"), {"osd", "orientation"},
        asSegmented(plainSelect(
            {{"horizontal", "settings.options.orientation.horizontal"},
             {"vertical", "settings.options.orientation.vertical"}},
            cfg.osd.orientation
        )),
        "hud overlay volume brightness vertical"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-scale.label"), tr("settings.schema.shell.osd-scale.description"),
        {"osd", "scale"}, SliderSetting{cfg.osd.scale, 0.5f, 2.5f, 0.05f, false},
        "hud overlay volume brightness size scale multiplier"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-lock-keys.label"),
        tr("settings.schema.shell.osd-lock-keys.description"), {"osd", "lock_keys"}, ToggleSetting{cfg.osd.lockKeys},
        "hud overlay caps num scroll keyboard"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-keyboard-layout.label"),
        tr("settings.schema.shell.osd-keyboard-layout.description"), {"osd", "keyboard_layout"},
        ToggleSetting{cfg.osd.keyboardLayout}, "hud overlay xkb input language layout switch"
    ));

    // Keybinds (lives under Shell)
    entries.push_back(makeEntry(
        "shell", "keybinds", tr("settings.schema.keybinds.validate.label"),
        tr("settings.schema.keybinds.validate.description"), {"keybinds", "validate"},
        KeybindListSetting{.items = cfg.keybinds.validate, .maxItems = 4},
        "keybind shortcut hotkey enter accept submit confirm"
    ));
    entries.push_back(makeEntry(
        "shell", "keybinds", tr("settings.schema.keybinds.cancel.label"),
        tr("settings.schema.keybinds.cancel.description"), {"keybinds", "cancel"},
        KeybindListSetting{.items = cfg.keybinds.cancel, .maxItems = 4}, "keybind shortcut hotkey escape close dismiss"
    ));
    entries.push_back(makeEntry(
        "shell", "keybinds", tr("settings.schema.keybinds.left.label"), tr("settings.schema.keybinds.left.description"),
        {"keybinds", "left"}, KeybindListSetting{.items = cfg.keybinds.left, .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        "shell", "keybinds", tr("settings.schema.keybinds.right.label"),
        tr("settings.schema.keybinds.right.description"), {"keybinds", "right"},
        KeybindListSetting{.items = cfg.keybinds.right, .maxItems = 4}, "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        "shell", "keybinds", tr("settings.schema.keybinds.up.label"), tr("settings.schema.keybinds.up.description"),
        {"keybinds", "up"}, KeybindListSetting{.items = cfg.keybinds.up, .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));
    entries.push_back(makeEntry(
        "shell", "keybinds", tr("settings.schema.keybinds.down.label"), tr("settings.schema.keybinds.down.description"),
        {"keybinds", "down"}, KeybindListSetting{.items = cfg.keybinds.down, .maxItems = 4},
        "keybind shortcut hotkey arrow move"
    ));

    // Niri-specific integrations
    if (env.niriOverviewTypeToLaunchSupported || env.niriBackdropSupported) {
      if (env.niriOverviewTypeToLaunchSupported) {
        entries.push_back(makeEntry(
            "niri", "overview", tr("settings.schema.shell.niri-overview-type-to-launch.label"),
            tr("settings.schema.shell.niri-overview-type-to-launch.description"),
            {"shell", "niri_overview_type_to_launch_enabled"}, ToggleSetting{cfg.shell.niriOverviewTypeToLaunchEnabled},
            "niri overview type launch launcher search keyboard focus"
        ));
      }
      if (env.niriBackdropSupported) {
        entries.push_back(makeEntry(
            "niri", "backdrop", tr("settings.schema.shared.enabled.label"),
            tr("settings.schema.backdrop.enabled.description"), {"backdrop", "enabled"},
            ToggleSetting{cfg.backdrop.enabled}, "wallpaper backdrop"
        ));
        entries.push_back(makeEntry(
            "niri", "backdrop", tr("settings.schema.backdrop.blur-intensity.label"),
            tr("settings.schema.backdrop.blur-intensity.description"), {"backdrop", "blur_intensity"},
            SliderSetting{cfg.backdrop.blurIntensity, 0.0f, 1.0f, 0.01f, false}, "wallpaper"
        ));
        entries.push_back(makeEntry(
            "niri", "backdrop", tr("settings.schema.backdrop.tint-intensity.label"),
            tr("settings.schema.backdrop.tint-intensity.description"), {"backdrop", "tint_intensity"},
            SliderSetting{cfg.backdrop.tintIntensity, 0.0f, 1.0f, 0.01f, false}, "wallpaper"
        ));
      }
    }

    // Services
    const SettingVisibility monitorOn{{"system", "monitor", "enabled"}, {"true"}};
    entries.push_back(makeEntry(
        "services", "system", tr("settings.schema.services.system-monitor.label"),
        tr("settings.schema.services.system-monitor.description"), {"system", "monitor", "enabled"},
        ToggleSetting{cfg.system.monitor.enabled}, "system monitor cpu ram memory"
    ));
    {
      constexpr float kPollMin = SystemConfig::MonitorConfig::kMinPollSeconds;
      constexpr float kPollMax = SystemConfig::MonitorConfig::kMaxPollSeconds;
      constexpr float kPollStep = 1.0f;
      const auto& mon = cfg.system.monitor;
      auto addPoll = [&](std::string_view labelKey, std::string_view descKey, std::vector<std::string> path,
                         float value) {
        const float clampedValue = std::clamp(value, kPollMin, kPollMax);
        auto entry = makeEntry(
            "services", "system", tr(labelKey), tr(descKey), std::move(path),
            SliderSetting{clampedValue, kPollMin, kPollMax, kPollStep, true}, "system monitor", true
        );
        entry.visibleWhen = monitorOn;
        entries.push_back(std::move(entry));
      };
      addPoll(
          "settings.schema.services.system-monitor.cpu-poll.label",
          "settings.schema.services.system-monitor.cpu-poll.description", {"system", "monitor", "cpu_poll_seconds"},
          mon.cpuPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.gpu-poll.label",
          "settings.schema.services.system-monitor.gpu-poll.description", {"system", "monitor", "gpu_poll_seconds"},
          mon.gpuPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.memory-poll.label",
          "settings.schema.services.system-monitor.memory-poll.description",
          {"system", "monitor", "memory_poll_seconds"}, mon.memoryPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.network-poll.label",
          "settings.schema.services.system-monitor.network-poll.description",
          {"system", "monitor", "network_poll_seconds"}, mon.networkPollSeconds
      );
      addPoll(
          "settings.schema.services.system-monitor.disk-poll.label",
          "settings.schema.services.system-monitor.disk-poll.description", {"system", "monitor", "disk_poll_seconds"},
          mon.diskPollSeconds
      );
    }
    entries.push_back(makeEntry(
        "services", "weather", tr("settings.schema.services.weather.label"),
        tr("settings.schema.services.weather.description"), {"weather", "enabled"}, ToggleSetting{cfg.weather.enabled},
        "forecast"
    ));
    {
      auto e = makeEntry(
          "services", "weather", tr("settings.schema.services.weather-location.label"),
          tr("settings.schema.services.weather-location.description"), {"weather", "auto_locate"},
          ToggleSetting{cfg.weather.autoLocate}, "forecast gps"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "services", "weather", tr("settings.schema.services.weather-unit.label"),
          tr("settings.schema.services.weather-unit.description"), {"weather", "unit"},
          asSegmented(plainSelect(
              {{"metric", "settings.options.weather.unit.metric"},
               {"imperial", "settings.options.weather.unit.imperial"}},
              cfg.weather.unit
          )),
          "temperature"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "services", "weather", tr("settings.schema.services.weather-address.label"),
          tr("settings.schema.services.weather-address.description"), {"weather", "address"},
          TextSetting{
              .value = cfg.weather.address,
              .placeholder = tr("settings.schema.services.weather-address.placeholder"),
              .browseFileExtensions = {}
          },
          "location"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "services", "weather", tr("settings.schema.services.weather-effects.label"),
          tr("settings.schema.services.weather-effects.description"), {"weather", "effects"},
          ToggleSetting{cfg.weather.effects}, "forecast visuals"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "services", "weather", tr("settings.schema.services.weather-refresh-interval.label"),
          tr("settings.schema.services.weather-refresh-interval.description"), {"weather", "refresh_minutes"},
          SliderSetting{static_cast<float>(cfg.weather.refreshMinutes), 5.0f, 240.0f, 5.0f, true}, "forecast"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "services", "audio", tr("settings.schema.services.audio-overdrive.label"),
        tr("settings.schema.services.audio-overdrive.description"), {"audio", "enable_overdrive"},
        ToggleSetting{cfg.audio.enableOverdrive}, "volume"
    ));
    entries.push_back(makeEntry(
        "services", "audio", tr("settings.schema.services.shell-sounds.label"),
        tr("settings.schema.services.shell-sounds.description"), {"audio", "enable_sounds"},
        ToggleSetting{cfg.audio.enableSounds}, "sound"
    ));
    entries.push_back(makeEntry(
        "services", "audio", tr("settings.schema.services.sound-volume.label"),
        tr("settings.schema.services.sound-volume.description"), {"audio", "sound_volume"},
        SliderSetting{cfg.audio.soundVolume, 0.0f, 1.0f, 0.01f, false}, "sound"
    ));
    entries.push_back(makeEntry(
        "services", "audio", tr("settings.schema.services.volume-change-sound.label"),
        tr("settings.schema.services.volume-change-sound.description"), {"audio", "volume_change_sound"},
        TextSetting{
            .value = cfg.audio.volumeChangeSound,
            .placeholder = tr("settings.schema.services.volume-change-sound.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = {".wav"}
        },
        "sound path file", true
    ));
    entries.push_back(makeEntry(
        "services", "audio", tr("settings.schema.services.notification-sound.label"),
        tr("settings.schema.services.notification-sound.description"), {"audio", "notification_sound"},
        TextSetting{
            .value = cfg.audio.notificationSound,
            .placeholder = tr("settings.schema.services.notification-sound.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = {".wav"}
        },
        "sound path file", true
    ));
    entries.push_back(makeEntry(
        "services", "media", tr("settings.schema.services.mpris-blacklist.label"),
        tr("settings.schema.services.mpris-blacklist.description"), {"shell", "mpris", "blacklist"},
        ListSetting{.items = cfg.shell.mpris.blacklist}, "mpris media player dbus session blacklist"
    ));
    entries.push_back(makeEntry(
        "services", "brightness", tr("settings.schema.services.ddcutil.label"),
        env.ddcutilAvailable ? tr("settings.schema.services.ddcutil.description")
                             : tr("settings.schema.services.ddcutil.requires-ddcutil"),
        {"brightness", "enable_ddcutil"},
        ToggleSetting{.checked = cfg.brightness.enableDdcutil, .enabled = env.ddcutilAvailable}, "monitor ddcutil"
    ));
    if (!env.gammaControlAvailable) {
      entries.push_back(makeEntry(
          "services", "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.requires-gamma-control"), {"nightlight", "enabled"},
          ToggleSetting{.checked = cfg.nightlight.enabled, .enabled = false}, "nightlight"
      ));
    } else {
      entries.push_back(makeEntry(
          "services", "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.description"), {"nightlight", "enabled"},
          ToggleSetting{cfg.nightlight.enabled}, "nightlight"
      ));
      const SettingVisibility nightLightOn{{"nightlight", "enabled"}, {"true"}};
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.force-night-light.label"),
            tr("settings.schema.services.force-night-light.description"), {"nightlight", "force"},
            ToggleSetting{cfg.nightlight.force}, "nightlight"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
      const bool weatherLocationConfigured =
          cfg.weather.enabled && (cfg.weather.autoLocate || !cfg.weather.address.empty());
      const std::string weatherLocationSubtitle =
          weatherLocationConfigured ? tr("settings.schema.services.use-weather-location.description")
                                    : tr("settings.schema.services.use-weather-location.requires-weather-location");
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.use-weather-location.label"),
            weatherLocationSubtitle, {"nightlight", "use_weather_location"},
            ToggleSetting{cfg.nightlight.useWeatherLocation}, "location"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
      const SettingVisibility nightLightOnWeatherLocationOff{std::vector<SettingVisibilityCondition>{
          {{"nightlight", "enabled"}, {"true"}},
          {{"nightlight", "use_weather_location"}, {"false"}},
      }};
      const SettingVisibility& manualNightLightControlsVisible =
          weatherLocationConfigured ? nightLightOnWeatherLocationOff : nightLightOn;
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.night-light-start-time.label"),
            tr("settings.schema.services.night-light-start-time.description"), {"nightlight", "start_time"},
            TextSetting{.value = cfg.nightlight.startTime, .placeholder = "20:30", .browseFileExtensions = {}},
            "time schedule sunset"
        );
        e.visibleWhen = manualNightLightControlsVisible;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.night-light-stop-time.label"),
            tr("settings.schema.services.night-light-stop-time.description"), {"nightlight", "stop_time"},
            TextSetting{.value = cfg.nightlight.stopTime, .placeholder = "07:30", .browseFileExtensions = {}},
            "time schedule sunrise"
        );
        e.visibleWhen = manualNightLightControlsVisible;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.latitude.label"),
            tr("settings.schema.services.latitude.description"), {"nightlight", "latitude"},
            OptionalNumberSetting{cfg.nightlight.latitude, -90.0, 90.0, "52.5200"},
            "coordinate location sunrise sunset", true
        );
        e.visibleWhen = manualNightLightControlsVisible;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.longitude.label"),
            tr("settings.schema.services.longitude.description"), {"nightlight", "longitude"},
            OptionalNumberSetting{cfg.nightlight.longitude, -180.0, 180.0, "13.4050"},
            "coordinate location sunrise sunset", true
        );
        e.visibleWhen = manualNightLightControlsVisible;
        entries.push_back(std::move(e));
      }
      // Both sliders span the same range; the day > night invariant is enforced at commit time
      // via SliderSetting::linkedCommit, which pushes the other temperature when needed.
      const float tempMin = static_cast<float>(NightLightConfig::kTemperatureMin);
      const float tempMax = static_cast<float>(NightLightConfig::kTemperatureMax);
      const float tempStep = static_cast<float>(NightLightConfig::kTemperatureGap);

      SliderSetting daySlider{static_cast<float>(cfg.nightlight.dayTemperature), tempMin, tempMax, tempStep, true};
      daySlider.linkedCommit = [curNight = cfg.nightlight.nightTemperature](double v) {
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
        const auto newDay = std::clamp(
            static_cast<std::int32_t>(std::lround(v)), NightLightConfig::kTemperatureMin,
            NightLightConfig::kTemperatureMax
        );
        if (newDay - NightLightConfig::kTemperatureGap < curNight) {
          std::int32_t pushedNight =
              std::max(NightLightConfig::kTemperatureMin, newDay - NightLightConfig::kTemperatureGap);
          if (pushedNight + NightLightConfig::kTemperatureGap > newDay) {
            // Day was below kTemperatureMin + kTemperatureGap; bump day up too. The slider value
            // refresh comes through the rebuilt registry on the next config reload.
            const std::int32_t bumpedDay =
                std::min(NightLightConfig::kTemperatureMax, pushedNight + NightLightConfig::kTemperatureGap);
            overrides.emplace_back(
                std::vector<std::string>{"nightlight", "temperature_day"}, static_cast<std::int64_t>(bumpedDay)
            );
          }
          overrides.emplace_back(
              std::vector<std::string>{"nightlight", "temperature_night"}, static_cast<std::int64_t>(pushedNight)
          );
        }
        return overrides;
      };
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.day-temperature.label"),
            tr("settings.schema.services.day-temperature.description"), {"nightlight", "temperature_day"},
            std::move(daySlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }

      SliderSetting nightSlider{static_cast<float>(cfg.nightlight.nightTemperature), tempMin, tempMax, tempStep, true};
      nightSlider.linkedCommit = [curDay = cfg.nightlight.dayTemperature](double v) {
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
        const auto newNight = std::clamp(
            static_cast<std::int32_t>(std::lround(v)), NightLightConfig::kTemperatureMin,
            NightLightConfig::kTemperatureMax
        );
        if (curDay - NightLightConfig::kTemperatureGap < newNight) {
          std::int32_t pushedDay =
              std::min(NightLightConfig::kTemperatureMax, newNight + NightLightConfig::kTemperatureGap);
          if (pushedDay - NightLightConfig::kTemperatureGap < newNight) {
            const std::int32_t bumpedNight =
                std::max(NightLightConfig::kTemperatureMin, pushedDay - NightLightConfig::kTemperatureGap);
            overrides.emplace_back(
                std::vector<std::string>{"nightlight", "temperature_night"}, static_cast<std::int64_t>(bumpedNight)
            );
          }
          overrides.emplace_back(
              std::vector<std::string>{"nightlight", "temperature_day"}, static_cast<std::int64_t>(pushedDay)
          );
        }
        return overrides;
      };
      {
        auto e = makeEntry(
            "services", "night-light", tr("settings.schema.services.night-temperature.label"),
            tr("settings.schema.services.night-temperature.description"), {"nightlight", "temperature_night"},
            std::move(nightSlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
    }

    // Idle
    entries.push_back(makeEntry(
        "idle", "general", tr("settings.schema.idle.pre-action-fade.label"),
        tr("settings.schema.idle.pre-action-fade.description"), {"idle", "pre_action_fade_seconds"},
        StepperSetting{
            .value = static_cast<int>(std::lround(std::clamp(cfg.idle.preActionFadeSeconds, 0.0f, 30.0f))),
            .minValue = 0,
            .maxValue = 30,
            .step = 1,
            .valueSuffix = "s"
        },
        "idle fade dim seconds overlay"
    ));
    entries.push_back(makeEntry(
        "idle", "behavior", tr("settings.schema.idle.behaviors.label"),
        tr("settings.schema.idle.behaviors.description"), {"idle", "behavior"},
        IdleBehaviorsSetting{.items = cfg.idle.behaviors},
        "idle behavior timeout command resume screen lock dpms suspend lock_before_suspend caffeine"
    ));

    // Hooks
    auto hookGroup = [](HookKind kind) -> std::string {
      switch (kind) {
      case HookKind::Started:
      case HookKind::SessionLocked:
      case HookKind::SessionUnlocked:
      case HookKind::LoggingOut:
      case HookKind::Rebooting:
      case HookKind::ShuttingDown:
        return "lifecycle";
      case HookKind::WallpaperChanged:
      case HookKind::ColorsChanged:
      case HookKind::ThemeModeChanged:
        return "theme";
      case HookKind::WifiEnabled:
      case HookKind::WifiDisabled:
      case HookKind::BluetoothEnabled:
      case HookKind::BluetoothDisabled:
        return "network";
      case HookKind::BatteryStateChanged:
      case HookKind::BatteryUnderThreshold:
      case HookKind::PowerProfileChanged:
        return "power";
      case HookKind::Count:
        break;
      }
      return "general";
    };

    auto hookTags = [](HookKind kind) -> std::string {
      std::string tags = "hook command script exec event trigger";
      if (kind == HookKind::BatteryUnderThreshold || kind == HookKind::BatteryStateChanged) {
        tags += " battery power";
      }
      if (kind == HookKind::PowerProfileChanged) {
        tags += " power profile performance balanced saver";
      }
      if (kind == HookKind::WallpaperChanged || kind == HookKind::ColorsChanged || kind == HookKind::ThemeModeChanged) {
        tags += " wallpaper colors theme mode light dark auto";
      }
      if (kind == HookKind::WifiEnabled || kind == HookKind::WifiDisabled || kind == HookKind::BluetoothEnabled ||
          kind == HookKind::BluetoothDisabled) {
        tags += " network wifi bluetooth";
      }
      if (kind == HookKind::SessionLocked || kind == HookKind::SessionUnlocked || kind == HookKind::LoggingOut ||
          kind == HookKind::Rebooting || kind == HookKind::ShuttingDown || kind == HookKind::Started) {
        tags += " session startup";
      }
      return tags;
    };

    for (const auto& kind : kHookKinds) {
      const auto index = static_cast<std::size_t>(kind.value);
      const std::string key(kind.key);
      const std::string baseKey = "settings.schema.hooks.events." + key;
      const std::string hookCmd = cfg.hooks.commands[index].empty() ? "" : cfg.hooks.commands[index][0];
      entries.push_back(makeEntry(
          "hooks", hookGroup(kind.value), tr(baseKey + ".label"), tr(baseKey + ".description"), {"hooks", key},
          TextSetting{
              .value = hookCmd,
              .placeholder = tr("settings.schema.hooks.command-placeholder"),
              .width = 320.0f,
              .browseFileExtensions = {}
          },
          hookTags(kind.value)
      ));
    }

    entries.push_back(makeEntry(
        "hooks", "power", tr("settings.schema.hooks.battery-low-threshold.label"),
        tr("settings.schema.hooks.battery-low-threshold.description"), {"hooks", "battery_low_percent_threshold"},
        SliderSetting{static_cast<float>(cfg.hooks.batteryLowPercentThreshold), 0.0f, 100.0f, 1.0f, true},
        "battery threshold hook trigger"
    ));

    // Popups
    entries.push_back(makeEntry(
        "popups", "notifications", tr("settings.schema.notifications.daemon.label"),
        tr("settings.schema.notifications.daemon.description"), {"notification", "enable_daemon"},
        ToggleSetting{cfg.notification.enableDaemon}, "dbus"
    ));
    entries.push_back(makeEntry(
        "popups", "notifications", tr("settings.schema.notifications.layer.label"),
        tr("settings.schema.notifications.layer.description"), {"notification", "layer"},
        asSegmented(plainSelect(
            {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
            cfg.notification.layer
        )),
        "toast layer shell z-order"
    ));
    entries.push_back(makeEntry(
        "popups", "notifications", tr("settings.schema.notifications.position.label"),
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
        "popups", "notifications", tr("settings.schema.notifications.scale.label"),
        tr("settings.schema.notifications.scale.description"), {"notification", "scale"},
        SliderSetting{cfg.notification.scale, 0.5f, 2.5f, 0.05f, false}, "toast size scale"
    ));
    entries.push_back(makeEntry(
        "popups", "notifications", tr("settings.schema.notifications.offset-x.label"),
        tr("settings.schema.notifications.offset-x.description"), {"notification", "offset_x"},
        StepperSetting{
            .value = cfg.notification.offsetX, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"
        },
        "offset margin horizontal"
    ));
    entries.push_back(makeEntry(
        "popups", "notifications", tr("settings.schema.notifications.offset-y.label"),
        tr("settings.schema.notifications.offset-y.description"), {"notification", "offset_y"},
        StepperSetting{
            .value = cfg.notification.offsetY, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"
        },
        "offset margin vertical"
    ));
    entries.push_back(makeEntry(
        "popups", "notifications", tr("settings.schema.notifications.toast-opacity.label"),
        tr("settings.schema.notifications.toast-opacity.description"), {"notification", "background_opacity"},
        SliderSetting{cfg.notification.backgroundOpacity, 0.0f, 1.0f, 0.01f, false}, "popup"
    ));
    entries.push_back(makeEntry(
        "popups", "notifications", tr("settings.schema.notifications.monitors.label"),
        tr("settings.schema.notifications.monitors.description"), {"notification", "monitors"},
        ListSetting{.items = cfg.notification.monitors, .suggestedOptions = env.availableOutputs},
        "monitor output display screen"
    ));

    // Bar — register every configured bar so global search can surface settings from all of them.
    for (const auto& bar : cfg.bars) {
      const std::string section = "bar";
      const std::vector<std::string> root = {"bar", bar.name};
      auto path = [&](std::string key) {
        std::vector<std::string> p = root;
        p.push_back(std::move(key));
        return p;
      };
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.enabled.label"), tr("settings.schema.bar.enabled.description"),
          path("enabled"), ToggleSetting{bar.enabled}, "visible"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.position.label"),
          tr("settings.schema.bar.position.description"), path("position"), positionSelect(bar.position), "edge"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.auto-hide.label"),
          tr("settings.schema.bar.auto-hide.description"), path("auto_hide"), ToggleSetting{bar.autoHide}, "autohide"
      ));
      {
        auto e = makeEntry(
            section, "general", tr("settings.schema.shared.reserve-space.label"),
            tr("settings.schema.bar.reserve-space.description"), path("reserve_space"), ToggleSetting{bar.reserveSpace},
            "exclusive zone"
        );
        e.visibleWhen = SettingVisibility{path("auto_hide"), {"false"}};
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.thickness.label"), tr("settings.schema.bar.thickness.description"),
          path("thickness"), SliderSetting{static_cast<float>(bar.thickness), 10.0f, 120.0f, 1.0f, true}, "height width"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.content-scale.label"),
          tr("settings.schema.bar.content-scale.description"), path("scale"),
          SliderSetting{bar.scale, 0.5f, 4.0f, 0.05f, false}, "zoom size"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.ends-margin.label"),
          tr("settings.schema.bar.ends-margin.description"), path("margin_ends"), barMarginStepper(bar.marginEnds),
          "gap inset"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.edge-margin.label"),
          tr("settings.schema.bar.edge-margin.description"), path("margin_edge"), barMarginStepper(bar.marginEdge),
          "gap inset"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.content-padding.label"),
          tr("settings.schema.bar.content-padding.description"), path("padding"),
          SliderSetting{static_cast<float>(bar.padding), 0.0f, 80.0f, 1.0f, true}, "inset"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-radius.label"),
          tr("settings.schema.bar.corner-radius.description"), path("radius"),
          SliderSetting{static_cast<float>(bar.radius), 0.0f, 80.0f, 1.0f, true}, "rounded"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-left.label"),
          tr("settings.schema.bar.corner-top-left.description"), path("radius_top_left"),
          SliderSetting{static_cast<float>(bar.radiusTopLeft), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-right.label"),
          tr("settings.schema.bar.corner-top-right.description"), path("radius_top_right"),
          SliderSetting{static_cast<float>(bar.radiusTopRight), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
          tr("settings.schema.bar.corner-bottom-left.description"), path("radius_bottom_left"),
          SliderSetting{static_cast<float>(bar.radiusBottomLeft), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
          tr("settings.schema.bar.corner-bottom-right.description"), path("radius_bottom_right"),
          SliderSetting{static_cast<float>(bar.radiusBottomRight), 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.background-opacity.label"),
          tr("settings.schema.bar.background-opacity.description"), path("background_opacity"),
          SliderSetting{bar.backgroundOpacity, 0.0f, 1.0f, 0.01f, false}, "alpha"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.bar.border.label"), tr("settings.schema.bar.border.description"),
          path("border"), colorSpecPicker(bar.border), "outline color", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.bar.border-width.label"),
          tr("settings.schema.bar.border-width.description"), path("border_width"),
          SliderSetting{bar.borderWidth, 0.0f, 20.0f, 0.5f, false}, "outline stroke", true
      ));
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.bar.shadow.description"),
          path("shadow"), ToggleSetting{bar.shadow}, "shadow"
      ));
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.contact-shadow.label"),
          tr("settings.schema.bar.contact-shadow.description"), path("contact_shadow"),
          ToggleSetting{bar.contactShadow}, "shadow contact panel attached"
      ));
      {
        auto fontWeightSelect = plainSelect(
            {{"100", "settings.options.bar.font-weight.thin"},
             {"200", "settings.options.bar.font-weight.ultra-light"},
             {"300", "settings.options.bar.font-weight.light"},
             {"350", "settings.options.bar.font-weight.semi-light"},
             {"380", "settings.options.bar.font-weight.book"},
             {"400", "settings.options.bar.font-weight.regular"},
             {"500", "settings.options.bar.font-weight.medium"},
             {"600", "settings.options.bar.font-weight.semi-bold"},
             {"700", "settings.options.bar.font-weight.bold"},
             {"800", "settings.options.bar.font-weight.ultra-bold"},
             {"900", "settings.options.bar.font-weight.heavy"},
             {"1000", "settings.options.bar.font-weight.ultra-heavy"}},
            std::to_string(bar.fontWeight)
        );
        fontWeightSelect.integerValue = true;
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.font-weight.label"),
            tr("settings.schema.bar.font-weight.description"), path("font_weight"), std::move(fontWeightSelect),
            "font text weight"
        ));
      }
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.widget-spacing.label"),
          tr("settings.schema.bar.widget-spacing.description"), path("widget_spacing"),
          SliderSetting{static_cast<float>(bar.widgetSpacing), 0.0f, 32.0f, 1.0f, true}, "gap"
      ));
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.widget-color.label"),
          tr("settings.schema.bar.widget-color.description"), path("color"), colorSpecPicker(bar.widgetColor, true),
          "color foreground", true
      ));
      entries.push_back(makeEntry(
          section, "capsules", tr("settings.schema.bar.widget-capsules.label"),
          tr("settings.schema.bar.widget-capsules.description"), path("capsule"),
          ToggleSetting{bar.widgetCapsuleDefault}, "pill"
      ));
      const SettingVisibility capsuleOn{path("capsule"), {"true"}};
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-radius.label"),
            tr("settings.schema.bar.capsule-radius.description"), path("capsule_radius"),
            OptionalStepperSetting{
                .value = radiusStepperValue(bar.widgetCapsuleRadius),
                .minValue = 0,
                .maxValue = 80,
                .step = 1,
                .fallbackValue = radiusStepperFallback(bar.widgetCapsuleRadius),
                .unsetLabel = tr("common.states.auto"),
                .customLabel = tr("common.states.custom")
            },
            "pill rounded radius", true
        );
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-groups.label"),
            tr("settings.schema.bar.capsule-groups.description"), path("capsule_groups"),
            ListSetting{.items = bar.widgetCapsuleGroups}, "grouped capsules"
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-fill.label"),
            tr("settings.schema.bar.capsule-fill.description"), path("capsule_fill"),
            colorSpecPicker(bar.widgetCapsuleFill), "color pill", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-foreground.label"),
            tr("settings.schema.bar.capsule-foreground.description"), path("capsule_foreground"),
            colorSpecPicker(bar.widgetCapsuleForeground, true), "color foreground pill", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-border.label"),
            tr("settings.schema.bar.capsule-border.description"), path("capsule_border"),
            colorSpecPicker(bar.widgetCapsuleBorder, true), "color pill outline", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-padding.label"),
            tr("settings.schema.bar.capsule-padding.description"), path("capsule_padding"),
            SliderSetting{bar.widgetCapsulePadding, 0.0f, 48.0f, 1.0f, false}, "pill inset", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-opacity.label"),
            tr("settings.schema.bar.capsule-opacity.description"), path("capsule_opacity"),
            SliderSetting{bar.widgetCapsuleOpacity, 0.0f, 1.0f, 0.01f, false}, "pill alpha", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.start-widgets.label"),
          tr("settings.schema.bar.start-widgets.description"), path("start"), ListSetting{.items = bar.startWidgets},
          "left"
      ));
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.center-widgets.label"),
          tr("settings.schema.bar.center-widgets.description"), path("center"), ListSetting{.items = bar.centerWidgets},
          "middle"
      ));
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.end-widgets.label"),
          tr("settings.schema.bar.end-widgets.description"), path("end"), ListSetting{.items = bar.endWidgets}, "right"
      ));
    }

    // Bar monitor overrides (all bars).
    for (const auto& bar : cfg.bars) {
      for (const auto& ovr : bar.monitorOverrides) {
        const std::string section = "bar";
        const std::vector<std::string> root = {"bar", bar.name, "monitor", ovr.match};
        auto mpath = [&](std::string key) {
          std::vector<std::string> p = root;
          p.push_back(std::move(key));
          return p;
        };

        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.enabled.label"),
            tr("settings.schema.bar.enabled.description"), mpath("enabled"),
            ToggleSetting{ovr.enabled.value_or(bar.enabled)}, "visible"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.auto-hide.label"),
            tr("settings.schema.bar.auto-hide.description"), mpath("auto_hide"),
            ToggleSetting{ovr.autoHide.value_or(bar.autoHide)}, "autohide"
        ));
        {
          auto e = makeEntry(
              section, "general", tr("settings.schema.shared.reserve-space.label"),
              tr("settings.schema.bar.reserve-space.description"), mpath("reserve_space"),
              ToggleSetting{ovr.reserveSpace.value_or(bar.reserveSpace)}, "exclusive zone"
          );
          e.visibleWhen = SettingVisibility{mpath("auto_hide"), {"false"}};
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.thickness.label"),
            tr("settings.schema.bar.thickness.description"), mpath("thickness"),
            SliderSetting{static_cast<float>(ovr.thickness.value_or(bar.thickness)), 10.0f, 120.0f, 1.0f, true},
            "height width"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-scale.label"),
            tr("settings.schema.bar.content-scale.description"), mpath("scale"),
            SliderSetting{ovr.scale.value_or(bar.scale), 0.5f, 4.0f, 0.05f, false}, "zoom size"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.ends-margin.label"),
            tr("settings.schema.bar.ends-margin.description"), mpath("margin_ends"),
            barMarginStepper(ovr.marginEnds.value_or(bar.marginEnds)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.edge-margin.label"),
            tr("settings.schema.bar.edge-margin.description"), mpath("margin_edge"),
            barMarginStepper(ovr.marginEdge.value_or(bar.marginEdge)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-padding.label"),
            tr("settings.schema.bar.content-padding.description"), mpath("padding"),
            SliderSetting{static_cast<float>(ovr.padding.value_or(bar.padding)), 0.0f, 80.0f, 1.0f, true}, "inset"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-radius.label"),
            tr("settings.schema.bar.corner-radius.description"), mpath("radius"),
            SliderSetting{static_cast<float>(ovr.radius.value_or(bar.radius)), 0.0f, 80.0f, 1.0f, true}, "rounded"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-left.label"),
            tr("settings.schema.bar.corner-top-left.description"), mpath("radius_top_left"),
            SliderSetting{static_cast<float>(ovr.radiusTopLeft.value_or(bar.radiusTopLeft)), 0.0f, 80.0f, 1.0f, true},
            "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-right.label"),
            tr("settings.schema.bar.corner-top-right.description"), mpath("radius_top_right"),
            SliderSetting{static_cast<float>(ovr.radiusTopRight.value_or(bar.radiusTopRight)), 0.0f, 80.0f, 1.0f, true},
            "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
            tr("settings.schema.bar.corner-bottom-left.description"), mpath("radius_bottom_left"),
            SliderSetting{
                static_cast<float>(ovr.radiusBottomLeft.value_or(bar.radiusBottomLeft)), 0.0f, 80.0f, 1.0f, true
            },
            "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
            tr("settings.schema.bar.corner-bottom-right.description"), mpath("radius_bottom_right"),
            SliderSetting{
                static_cast<float>(ovr.radiusBottomRight.value_or(bar.radiusBottomRight)), 0.0f, 80.0f, 1.0f, true
            },
            "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.background-opacity.label"),
            tr("settings.schema.bar.background-opacity.description"), mpath("background_opacity"),
            SliderSetting{ovr.backgroundOpacity.value_or(bar.backgroundOpacity), 0.0f, 1.0f, 0.01f, false}, "alpha"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border.label"), tr("settings.schema.bar.border.description"),
            mpath("border"), colorSpecPicker(ovr.border, true, tr("common.states.inherit")), "outline color", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border-width.label"),
            tr("settings.schema.bar.border-width.description"), mpath("border_width"),
            SliderSetting{ovr.borderWidth.value_or(bar.borderWidth), 0.0f, 20.0f, 0.5f, false}, "outline stroke", true
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.bar.shadow.description"),
            mpath("shadow"), ToggleSetting{ovr.shadow.value_or(bar.shadow)}, "shadow"
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.contact-shadow.label"),
            tr("settings.schema.bar.contact-shadow.description"), mpath("contact_shadow"),
            ToggleSetting{ovr.contactShadow.value_or(bar.contactShadow)}, "shadow contact panel attached"
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-spacing.label"),
            tr("settings.schema.bar.widget-spacing.description"), mpath("widget_spacing"),
            SliderSetting{static_cast<float>(ovr.widgetSpacing.value_or(bar.widgetSpacing)), 0.0f, 32.0f, 1.0f, true},
            "gap"
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-color.label"),
            tr("settings.schema.bar.widget-color.description"), mpath("color"),
            colorSpecPicker(ovr.widgetColor, true, tr("common.states.inherit")), "color foreground", true
        ));
        entries.push_back(makeEntry(
            section, "capsules", tr("settings.schema.bar.widget-capsules.label"),
            tr("settings.schema.bar.widget-capsules.description"), mpath("capsule"),
            ToggleSetting{ovr.widgetCapsuleDefault.value_or(bar.widgetCapsuleDefault)}, "pill"
        ));
        const SettingVisibility mCapsuleOn{mpath("capsule"), {"true"}};
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-radius.label"),
              tr("settings.schema.bar.capsule-radius.description"), mpath("capsule_radius"),
              OptionalStepperSetting{
                  .value = radiusStepperValue(ovr.widgetCapsuleRadius),
                  .minValue = 0,
                  .maxValue = 80,
                  .step = 1,
                  .fallbackValue = radiusStepperFallback(bar.widgetCapsuleRadius),
                  .unsetLabel = tr("common.states.inherit"),
                  .customLabel = tr("common.states.custom")
              },
              "pill rounded radius", true
          );
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-groups.label"),
              tr("settings.schema.bar.capsule-groups.description"), mpath("capsule_groups"),
              ListSetting{.items = ovr.widgetCapsuleGroups.value_or(bar.widgetCapsuleGroups)}, "grouped capsules"
          );
          e.visibleWhen = mCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-fill.label"),
              tr("settings.schema.bar.capsule-fill.description"), mpath("capsule_fill"),
              colorSpecPicker(ovr.widgetCapsuleFill, true, tr("common.states.inherit")), "color pill", true
          );
          e.visibleWhen = mCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-foreground.label"),
              tr("settings.schema.bar.capsule-foreground.description"), mpath("capsule_foreground"),
              colorSpecPicker(ovr.widgetCapsuleForeground, true, tr("common.states.inherit")), "color foreground pill",
              true
          );
          e.visibleWhen = mCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-border.label"),
              tr("settings.schema.bar.capsule-border.description"), mpath("capsule_border"),
              colorSpecPicker(
                  ovr.widgetCapsuleBorderSpecified ? ovr.widgetCapsuleBorder : std::optional<ColorSpec>{}, true,
                  tr("common.states.inherit")
              ),
              "color pill outline", true
          );
          e.visibleWhen = mCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-padding.label"),
              tr("settings.schema.bar.capsule-padding.description"), mpath("capsule_padding"),
              SliderSetting{
                  static_cast<float>(ovr.widgetCapsulePadding.value_or(bar.widgetCapsulePadding)), 0.0f, 48.0f, 1.0f,
                  false
              },
              "pill inset", true
          );
          e.visibleWhen = mCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-opacity.label"),
              tr("settings.schema.bar.capsule-opacity.description"), mpath("capsule_opacity"),
              SliderSetting{
                  static_cast<float>(ovr.widgetCapsuleOpacity.value_or(bar.widgetCapsuleOpacity)), 0.0f, 1.0f, 0.01f,
                  false
              },
              "pill alpha", true
          );
          e.visibleWhen = mCapsuleOn;
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.start-widgets.label"),
            tr("settings.schema.bar.start-widgets.description"), mpath("start"),
            ListSetting{.items = ovr.startWidgets.value_or(bar.startWidgets)}, "left"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.center-widgets.label"),
            tr("settings.schema.bar.center-widgets.description"), mpath("center"),
            ListSetting{.items = ovr.centerWidgets.value_or(bar.centerWidgets)}, "middle"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.end-widgets.label"),
            tr("settings.schema.bar.end-widgets.description"), mpath("end"),
            ListSetting{.items = ovr.endWidgets.value_or(bar.endWidgets)}, "right"
        ));
      }
    }

    return entries;
  }

} // namespace settings
