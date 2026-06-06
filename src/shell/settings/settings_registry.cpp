#include "shell/settings/settings_registry.h"

#include "config/schema/config_schema.h"
#include "config/schema/ranges.h"
#include "core/log.h"
#include "core/process.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/sysmon_threshold_profile.h"
#include "theme/builtin_palettes.h"
#include "theme/builtin_templates.h"
#include "ui/app_icon_colorization.h"
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

    // Builds a slider whose bounds come from the shared schema Range — the same
    // constant the parser clamps with — so the UI range and the config clamp are
    // one source. `integerValue` (write as int64) stays explicit: it is a UI/write
    // choice, not implied by the range's numeric type (e.g. transition_duration).
    template <typename V, typename T>
    SliderSetting sliderFor(V value, const noctalia::config::schema::Range<T>& range, bool integerValue) {
      return SliderSetting{
          static_cast<double>(value), static_cast<double>(range.min.value()), static_cast<double>(range.max.value()),
          static_cast<double>(range.step.value()), integerValue
      };
    }

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

    StepperSetting barPanelOverlapStepper(std::int32_t value) {
      return StepperSetting{
          .value = static_cast<int>(value),
          .minValue = -2,
          .maxValue = 3,
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
    if (section == "security")
      return "shield-lock";
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
    if (section == "system")
      return "activity-heartbeat";
    if (section == "services")
      return "stack-2";
    if (section == "location")
      return "map-pin";
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
        sliderFor(cfg.shell.uiScale, noctalia::config::schema::kScaleRange, false), "size"
    ));
    entries.push_back(makeEntry(
        "appearance", "interface", tr("settings.schema.appearance.corner-roundness.label"),
        tr("settings.schema.appearance.corner-roundness.description"), {"shell", "corner_radius_scale"},
        sliderFor(cfg.shell.cornerRadiusScale, noctalia::config::schema::kCornerRadiusScaleRange, false),
        "rounded corners radius"
    ));
    entries.push_back(makeEntry(
        "appearance", "interface", tr("settings.schema.appearance.app-icon-colorize.label"),
        tr("settings.schema.appearance.app-icon-colorize.description"), {"shell", "app_icon_colorize"},
        ToggleSetting{cfg.shell.appIconColorize}, "tint all application icons"
    ));
    {
      const SettingVisibility colorizeOn{{"shell", "app_icon_colorize"}, {"true"}};
      ShellConfig colorizeShell = cfg.shell;
      colorizeShell.appIconColorize = true;
      const ColorSpec pickerColor =
          cfg.shell.appIconColor.value_or(*effectiveShellAppIconColorizationTint(colorizeShell));
      auto e = makeEntry(
          "appearance", "interface", tr("settings.schema.appearance.app-icon-color.label"),
          tr("settings.schema.appearance.app-icon-color.description"), {"shell", "app_icon_color"},
          colorSpecPicker(pickerColor), "color role dock tray application icons"
      );
      e.visibleWhen = colorizeOn;
      entries.push_back(std::move(e));
    }
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
        sliderFor(cfg.shell.animation.speed, noctalia::config::schema::kAnimationSpeedRange, false), "motion"
    ));
    entries.push_back(makeEntry(
        "appearance", "effects", tr("settings.schema.shared.shadow-direction.label"),
        tr("settings.schema.appearance.global-shadow-direction.description"), {"shell", "shadow", "direction"},
        enumSelect(kShadowDirections, cfg.shell.shadow.direction), "shadow direction"
    ));
    entries.push_back(makeEntry(
        "appearance", "effects", tr("settings.schema.shared.shadow-alpha.label"),
        tr("settings.schema.appearance.global-shadow-alpha.description"), {"shell", "shadow", "alpha"},
        sliderFor(cfg.shell.shadow.alpha, noctalia::config::schema::kUnitRange, false), "shadow opacity", true
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
      auto monitorPath = [&](std::string key) {
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
        sliderFor(
            cfg.wallpaper.transitionDurationMs, noctalia::config::schema::kWallpaperTransitionDurationRange, true
        ),
        "fade animation"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "transition", tr("settings.schema.wallpaper.edge-smoothness.label"),
        tr("settings.schema.wallpaper.edge-smoothness.description"), {"wallpaper", "edge_smoothness"},
        sliderFor(cfg.wallpaper.edgeSmoothness, noctalia::config::schema::kUnitRange, false), "transition feathering",
        true
    ));
    entries.push_back(makeEntry(
        "wallpaper", "transition", tr("settings.schema.wallpaper.transition-on-startup.label"),
        tr("settings.schema.wallpaper.transition-on-startup.description"), {"wallpaper", "transition_on_startup"},
        ToggleSetting{cfg.wallpaper.transitionOnStartup}, "startup animation"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "automation", tr("settings.schema.wallpaper.automation.label"),
        tr("settings.schema.wallpaper.automation.description"), {"wallpaper", "automation", "enabled"},
        ToggleSetting{cfg.wallpaper.automation.enabled}, "rotate slideshow"
    ));
    entries.push_back(makeEntry(
        "wallpaper", "automation", tr("settings.schema.wallpaper.automation-interval.label"),
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
    const auto builtInTemplatesOn = SettingVisibility{{"theme", "templates", "enable_builtin_templates"}, {"true"}};
    const auto communityTemplatesOn = SettingVisibility{{"theme", "templates", "enable_community_templates"}, {"true"}};
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
        templateOptions.push_back(SelectOption{.value = t.id, .label = t.displayName, .description = t.category});
      }
      auto e = makeEntry(
          "templates", "built-in", tr("settings.schema.templates.builtin-ids.label"),
          tr("settings.schema.templates.builtin-ids.description"), {"theme", "templates", "builtin_ids"},
          TemplateGridSetting{
              .options = std::move(templateOptions),
              .selectedValues = cfg.theme.templates.builtinIds,
              .emptyText = tr("settings.schema.templates.builtin-ids.empty"),
          },
          "theme templates apps foot walker gtk"
      );
      e.visibleWhen = builtInTemplatesOn;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "templates", "community", tr("settings.schema.templates.enable-community-templates.label"),
        tr("settings.schema.templates.enable-community-templates.description"),
        {"theme", "templates", "enable_community_templates"},
        ToggleSetting{cfg.theme.templates.enableCommunityTemplates}, "theme templates community"
    ));
    {
      auto e = makeEntry(
          "templates", "community", tr("settings.schema.templates.community-ids.label"),
          tr("settings.schema.templates.community-ids.description"), {"theme", "templates", "community_ids"},
          TemplateGridSetting{
              .options = env.communityTemplates,
              .selectedValues = cfg.theme.templates.communityIds,
              .emptyText = tr("settings.schema.templates.community-ids.empty"),
          },
          "theme templates community apps discord fuzzel vscode walker"
      );
      e.visibleWhen = communityTemplatesOn;
      entries.push_back(std::move(e));
    }

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
        asSegmented(enumSelect(kDockLauncherPositions, cfg.dock.launcherPosition)), "launcher apps grid"
    ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.dock.launcher-icon.label"),
        tr("settings.schema.dock.launcher-icon.description"), {"dock", "launcher_icon"},
        TextSetting{.value = cfg.dock.launcherIcon, .placeholder = "grid-dots", .browseFileExtensions = {}},
        "launcher apps icon glyph"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.position.label"), tr("settings.schema.dock.position.description"),
        {"dock", "position"}, asSegmented(enumSelect(kDockEdges, cfg.dock.position)), "edge"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.dock.icon-size.label"), tr("settings.schema.dock.icon-size.description"),
        {"dock", "icon_size"}, sliderFor(cfg.dock.iconSize, noctalia::config::schema::kDockIconSizeRange, true), "apps"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.main-axis-padding.label"),
        tr("settings.schema.dock.main-axis-padding.description"), {"dock", "main_axis_padding"},
        sliderFor(cfg.dock.mainAxisPadding, noctalia::config::schema::kDockPaddingRange, true), "inset"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.cross-axis-padding.label"),
        tr("settings.schema.dock.cross-axis-padding.description"), {"dock", "cross_axis_padding"},
        sliderFor(cfg.dock.crossAxisPadding, noctalia::config::schema::kDockPaddingRange, true), "inset"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.dock.item-spacing.label"),
        tr("settings.schema.dock.item-spacing.description"), {"dock", "item_spacing"},
        sliderFor(cfg.dock.itemSpacing, noctalia::config::schema::kDockItemSpacingRange, true), "gap"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.ends-margin.label"),
        tr("settings.schema.dock.ends-margin.description"), {"dock", "margin_ends"},
        sliderFor(cfg.dock.marginEnds, noctalia::config::schema::kDockMarginEndsRange, true), "gap inset"
    ));
    entries.push_back(makeEntry(
        "dock", "layout", tr("settings.schema.shared.edge-margin.label"),
        tr("settings.schema.dock.edge-margin.description"), {"dock", "margin_edge"},
        sliderFor(cfg.dock.marginEdge, noctalia::config::schema::kDockMarginEdgeRange, true), "gap inset"
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-radius.label"),
        tr("settings.schema.dock.corner-radius.description"), {"dock", "radius"},
        sliderFor(cfg.dock.radius, noctalia::config::schema::kDockRadiusRange, true), "rounded"
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-top-left.label"),
        tr("settings.schema.dock.corner-top-left.description"), {"dock", "radius_top_left"},
        sliderFor(cfg.dock.radiusTopLeft, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-top-right.label"),
        tr("settings.schema.dock.corner-top-right.description"), {"dock", "radius_top_right"},
        sliderFor(cfg.dock.radiusTopRight, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-bottom-left.label"),
        tr("settings.schema.dock.corner-bottom-left.description"), {"dock", "radius_bottom_left"},
        sliderFor(cfg.dock.radiusBottomLeft, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "shape", tr("settings.schema.shared.corner-bottom-right.label"),
        tr("settings.schema.dock.corner-bottom-right.description"), {"dock", "radius_bottom_right"},
        sliderFor(cfg.dock.radiusBottomRight, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
    ));
    entries.push_back(makeEntry(
        "dock", "effects", tr("settings.schema.shared.background-opacity.label"),
        tr("settings.schema.dock.background-opacity.description"), {"dock", "background_opacity"},
        sliderFor(cfg.dock.backgroundOpacity, noctalia::config::schema::kUnitRange, false), "alpha"
    ));
    entries.push_back(makeEntry(
        "dock", "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.dock.shadow.description"),
        {"dock", "shadow"}, ToggleSetting{cfg.dock.shadow}, "shadow"
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.active-icon-scale.label"),
        tr("settings.schema.dock.active-icon-scale.description"), {"dock", "active_scale"},
        sliderFor(cfg.dock.activeScale, noctalia::config::schema::kDockActiveScaleRange, false), "focused", true
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.inactive-icon-scale.label"),
        tr("settings.schema.dock.inactive-icon-scale.description"), {"dock", "inactive_scale"},
        sliderFor(cfg.dock.inactiveScale, noctalia::config::schema::kDockInactiveScaleRange, false), "unfocused", true
    ));
    entries.push_back(makeEntry(
        "dock", "behavior", tr("settings.schema.dock.magnification.label"),
        tr("settings.schema.dock.magnification.description"), {"dock", "magnification"},
        ToggleSetting{cfg.dock.magnification}, "magnify zoom mac"
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.magnification-scale.label"),
        tr("settings.schema.dock.magnification-scale.description"), {"dock", "magnification_scale"},
        sliderFor(cfg.dock.magnificationScale, noctalia::config::schema::kDockMagnificationScaleRange, false),
        "magnify zoom"
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.active-icon-opacity.label"),
        tr("settings.schema.dock.active-icon-opacity.description"), {"dock", "active_opacity"},
        sliderFor(cfg.dock.activeOpacity, noctalia::config::schema::kUnitRange, false), "focused alpha", true
    ));
    entries.push_back(makeEntry(
        "dock", "focus-styling", tr("settings.schema.dock.inactive-icon-opacity.label"),
        tr("settings.schema.dock.inactive-icon-opacity.description"), {"dock", "inactive_opacity"},
        sliderFor(cfg.dock.inactiveOpacity, noctalia::config::schema::kUnitRange, false), "unfocused alpha", true
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
        "panels", "control-center", tr("settings.schema.panels.control-center-sidebar.label"),
        tr("settings.schema.panels.control-center-sidebar.description"), {"control_center", "sidebar"},
        asSegmented(enumSelect(kControlCenterSidebarModes, cfg.controlCenter.sidebarMode)),
        "full compact none sidebar icons narrow hidden"
    ));
    entries.push_back(makeEntry(
        "panels", "control-center", tr("settings.schema.panels.control-center-sidebar-section.label"),
        tr("settings.schema.panels.control-center-sidebar-section.description"), {"control_center", "sidebar_section"},
        asSegmented(enumSelect(kControlCenterSidebarModes, cfg.controlCenter.sidebarSectionMode)),
        "full compact none sidebar icons narrow hidden tab direct widget shortcut"
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
        "panels", "launcher", tr("settings.schema.panels.launcher-show-icons.label"),
        tr("settings.schema.panels.launcher-show-icons.description"), {"shell", "panel", "launcher_show_icons"},
        ToggleSetting{cfg.shell.panel.launcherShowIcons}, "launcher app icons hide"
    ));
    entries.push_back(makeEntry(
        "panels", "launcher", tr("settings.schema.panels.launcher-compact.label"),
        tr("settings.schema.panels.launcher-compact.description"), {"shell", "panel", "launcher_compact"},
        ToggleSetting{cfg.shell.panel.launcherCompact}, "launcher compact rows dense"
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
        sliderFor(cfg.shell.screenCorners.size, noctalia::config::schema::kScreenCornersSizeRange, true),
        "screen corners radius"
    ));

    // Shell
    entries.push_back(makeEntry(
        "shell", "general", tr("settings.schema.shell.avatar-path.label"),
        tr("settings.schema.shell.avatar-path.description"), {"shell", "avatar_path"},
        TextSetting{
            .value = cfg.shell.avatarPath,
            .placeholder = tr("settings.schema.shell.avatar-path.placeholder"),
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = {".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp", ".gif"}
        },
        "image picture"
    ));
    // Security
    entries.push_back(makeEntry(
        "security", "privacy-security", tr("settings.schema.shell.offline-mode.label"),
        tr("settings.schema.shell.offline-mode.description"), {"shell", "offline_mode"},
        ToggleSetting{cfg.shell.offlineMode}, "network http fetch download"
    ));
    entries.push_back(makeEntry(
        "security", "privacy-security", tr("settings.schema.shell.telemetry.label"),
        tr("settings.schema.shell.telemetry.description"), {"shell", "telemetry_enabled"},
        ToggleSetting{cfg.shell.telemetryEnabled}, "analytics ping privacy"
    ));
    entries.push_back(makeEntry(
        "security", "privacy-security", tr("settings.schema.shell.polkit-agent.label"),
        tr("settings.schema.shell.polkit-agent.description"), {"shell", "polkit_agent"},
        ToggleSetting{cfg.shell.polkitAgent}, "auth password"
    ));
    entries.push_back(makeEntry(
        "security", "privacy-security", tr("settings.schema.shell.password-style.label"),
        tr("settings.schema.shell.password-style.description"), {"shell", "password_style"},
        asSegmented(enumSelect(kPasswordMaskStyles, cfg.shell.passwordMaskStyle)), "polkit lock mask"
    ));
    if (env.screencopySupported) {
      entries.push_back(makeEntry(
          "security", "lock-screen", tr("settings.schema.lockscreen.blurred-desktop.label"),
          tr("settings.schema.lockscreen.blurred-desktop.description"), {"lockscreen", "blurred_desktop"},
          ToggleSetting{cfg.lockscreen.blurredDesktop}, "lock screen desktop capture screencopy background"
      ));
    }
    entries.push_back(makeEntry(
        "security", "lock-screen", tr("settings.schema.lockscreen.blur-intensity.label"),
        tr("settings.schema.lockscreen.blur-intensity.description"), {"lockscreen", "blur_intensity"},
        sliderFor(cfg.lockscreen.blurIntensity, noctalia::config::schema::kUnitRange, false), "lock screen blur"
    ));
    entries.push_back(makeEntry(
        "security", "lock-screen", tr("settings.schema.lockscreen.tint-intensity.label"),
        tr("settings.schema.lockscreen.tint-intensity.description"), {"lockscreen", "tint_intensity"},
        sliderFor(cfg.lockscreen.tintIntensity, noctalia::config::schema::kUnitRange, false), "lock screen tint"
    ));
    {
      const SettingVisibility lockscreenWallpaperOn{{"lockscreen", "blurred_desktop"}, {"false"}};
      auto e = makeEntry(
          "security", "lock-screen", tr("settings.schema.lockscreen.wallpaper.label"),
          tr("settings.schema.lockscreen.wallpaper.description"), {"lockscreen", "wallpaper"},
          TextSetting{
              .value = cfg.lockscreen.wallpaper,
              .placeholder = tr("settings.schema.lockscreen.wallpaper.placeholder"),
              .browseMode = TextSettingBrowseMode::OpenFile,
              .browseFileExtensions = {".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp", ".gif"},
              .browseFallbackDirectory = wallpaper::resolveGlobalWallpaperDirectory(cfg.wallpaper, cfg.theme.mode),
          },
          "lock screen background image custom"
      );
      e.visibleWhen = lockscreenWallpaperOn;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "security", "lock-screen", tr("settings.schema.lockscreen.widgets.label"),
        tr("settings.schema.lockscreen.widgets.description"), {"lockscreen_widgets", "enabled"},
        ToggleSetting{cfg.lockscreenWidgets.enabled}, "lock screen widgets layout"
    ));
    entries.push_back(makeEntry(
        "shell", "general", tr("settings.schema.shell.time-format.label"),
        tr("settings.schema.shell.time-format.description"), {"shell", "time_format"},
        TextSetting{.value = cfg.shell.timeFormat, .placeholder = "{:%H:%M}", .browseFileExtensions = {}},
        "clock time format strftime chrono"
    ));
    entries.push_back(makeEntry(
        "shell", "general", tr("settings.schema.shell.date-format.label"),
        tr("settings.schema.shell.date-format.description"), {"shell", "date_format"},
        TextSetting{.value = cfg.shell.dateFormat, .placeholder = "%A, %x", .browseFileExtensions = {}},
        "calendar date format strftime chrono"
    ));
    const SettingVisibility weatherOn{{"weather", "enabled"}, {"true"}};
    {
      auto e = makeEntry(
          "shell", "general", tr("settings.schema.shell.show-location.label"),
          tr("settings.schema.shell.show-location.description"), {"shell", "show_location"},
          ToggleSetting{cfg.shell.showLocation}, "weather"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "shell", "general", tr("settings.schema.shell.middle-click-opens-widget-settings.label"),
        tr("settings.schema.shell.middle-click-opens-widget-settings.description"),
        {"shell", "middle_click_opens_widget_settings"}, ToggleSetting{cfg.shell.middleClickOpensWidgetSettings},
        "bar widget settings middle click configure"
    ));
    if (process::systemdAvailable()) {
      entries.push_back(makeEntry(
          "shell", "general", tr("settings.schema.shell.launch-apps-as-systemd-services.label"),
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
          "shell", "clipboard", tr("settings.schema.shell.clipboard-confirm-clear-history.label"),
          tr("settings.schema.shell.clipboard-confirm-clear-history.description"),
          {"shell", "clipboard_confirm_clear_history"}, ToggleSetting{cfg.shell.clipboardConfirmClearHistory},
          "clipboard history clear confirm pinned"
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
        "shell", "screenshot", tr("settings.schema.shell.screenshot-save-to-file.label"),
        tr("settings.schema.shell.screenshot-save-to-file.description"), {"shell", "screenshot", "save_to_file"},
        ToggleSetting{cfg.shell.screenshot.saveToFile}, "screenshot capture save png file"
    ));
    {
      auto e = makeEntry(
          "shell", "screenshot", tr("settings.schema.shell.screenshot-directory.label"),
          tr("settings.schema.shell.screenshot-directory.description"), {"shell", "screenshot", "directory"},
          TextSetting{
              .value = cfg.shell.screenshot.directory,
              .placeholder = tr("settings.schema.shell.screenshot-directory.placeholder"),
              .browseMode = TextSettingBrowseMode::SelectFolder,
              .browseFileExtensions = {}
          },
          "screenshot capture directory folder save location"
      );
      e.visibleWhen = SettingVisibility{{"shell", "screenshot", "save_to_file"}, {"true"}};
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "shell", "screenshot", tr("settings.schema.shell.screenshot-filename-pattern.label"),
          tr("settings.schema.shell.screenshot-filename-pattern.description"),
          {"shell", "screenshot", "filename_pattern"},
          TextSetting{
              .value = cfg.shell.screenshot.filenamePattern,
              .placeholder = "screenshot_%Y%m%d_%H%M%S",
              .browseFileExtensions = {}
          },
          "screenshot capture filename pattern strftime"
      );
      e.visibleWhen = SettingVisibility{{"shell", "screenshot", "save_to_file"}, {"true"}};
      entries.push_back(std::move(e));
    }
    entries.push_back(makeEntry(
        "shell", "screenshot", tr("settings.schema.shell.screenshot-copy-to-clipboard.label"),
        tr("settings.schema.shell.screenshot-copy-to-clipboard.description"),
        {"shell", "screenshot", "copy_to_clipboard"}, ToggleSetting{cfg.shell.screenshot.copyToClipboard},
        "screenshot capture clipboard copy"
    ));
    entries.push_back(makeEntry(
        "shell", "screenshot", tr("settings.schema.shell.screenshot-freeze-screen.label"),
        tr("settings.schema.shell.screenshot-freeze-screen.description"), {"shell", "screenshot", "freeze_screen"},
        ToggleSetting{cfg.shell.screenshot.freezeScreen}, "screenshot capture freeze region region"
    ));
    entries.push_back(makeEntry(
        "shell", "screenshot", tr("settings.schema.shell.screenshot-pipe-to-command.label"),
        tr("settings.schema.shell.screenshot-pipe-to-command.description"), {"shell", "screenshot", "pipe_to_command"},
        ToggleSetting{cfg.shell.screenshot.pipeToCommand}, "screenshot capture pipe command stdin"
    ));
    {
      auto e = makeEntry(
          "shell", "screenshot", tr("settings.schema.shell.screenshot-pipe-command.label"),
          tr("settings.schema.shell.screenshot-pipe-command.description"), {"shell", "screenshot", "pipe_command"},
          TextSetting{
              .value = cfg.shell.screenshot.pipeCommand,
              .placeholder = tr("settings.schema.shell.screenshot-pipe-command.placeholder"),
              .width = 320.0f,
              .browseFileExtensions = {}
          },
          "screenshot capture pipe command stdin png"
      );
      e.visibleWhen = SettingVisibility{{"shell", "screenshot", "pipe_to_command"}, {"true"}};
      entries.push_back(std::move(e));
    }
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
        {"osd", "scale"}, sliderFor(cfg.osd.scale, noctalia::config::schema::kScaleRange, false),
        "hud overlay volume brightness size scale multiplier"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-offset-x.label"),
        tr("settings.schema.shell.osd-offset-x.description"), {"osd", "offset_x"},
        StepperSetting{.value = cfg.osd.offsetX, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"},
        "hud overlay horizontal margin"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-offset-y.label"),
        tr("settings.schema.shell.osd-offset-y.description"), {"osd", "offset_y"},
        StepperSetting{.value = cfg.osd.offsetY, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"},
        "hud overlay vertical margin"
    ));
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-background-opacity.label"),
        tr("settings.schema.shell.osd-background-opacity.description"), {"osd", "background_opacity"},
        sliderFor(cfg.osd.backgroundOpacity, noctalia::config::schema::kUnitRange, false), "hud overlay popup opacity"
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
    entries.push_back(makeEntry(
        "popups", "osd", tr("settings.schema.shell.osd-monitors.label"),
        tr("settings.schema.shell.osd-monitors.description"), {"osd", "monitors"},
        ListSetting{.items = cfg.osd.monitors, .suggestedOptions = env.availableOutputs},
        "monitor output display screen hud overlay"
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
            sliderFor(cfg.backdrop.blurIntensity, noctalia::config::schema::kUnitRange, false), "wallpaper"
        ));
        entries.push_back(makeEntry(
            "niri", "backdrop", tr("settings.schema.backdrop.tint-intensity.label"),
            tr("settings.schema.backdrop.tint-intensity.description"), {"backdrop", "tint_intensity"},
            sliderFor(cfg.backdrop.tintIntensity, noctalia::config::schema::kUnitRange, false), "wallpaper"
        ));
      }
    }

    // System
    const SettingVisibility monitorOn{{"system", "monitor", "enabled"}, {"true"}};
    entries.push_back(makeEntry(
        "system", "monitor", tr("settings.schema.services.system-monitor.label"),
        tr("settings.schema.services.system-monitor.description"), {"system", "monitor", "enabled"},
        ToggleSetting{cfg.system.monitor.enabled}, "system monitor cpu ram memory"
    ));
    {
      // The slider goes down to 0, which disables the metric (no polling, no dGPU wakeups).
      constexpr float kPollMin = SystemConfig::MonitorConfig::kDisabledPollSeconds;
      constexpr float kPollMax = SystemConfig::MonitorConfig::kMaxPollSeconds;
      constexpr float kPollStep = 1.0f;
      const auto& mon = cfg.system.monitor;
      auto addPoll = [&](std::string_view labelKey, std::string_view descKey, std::vector<std::string> path,
                         float value) {
        const float clampedValue = std::clamp(value, kPollMin, kPollMax);
        auto entry = makeEntry(
            "system", "monitor", tr(labelKey), tr(descKey), std::move(path),
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

      auto addThresholdPair = [&](std::string_view baseKey, std::string_view statLabelKey, double activityValue,
                                  double criticalValue, noctalia::sysmon::ThresholdProfile profile, bool integerValue,
                                  std::string valueSuffix) {
        const std::vector<std::string> activityPath = {
            "system", "monitor", std::string(baseKey) + "_activity_threshold"
        };
        const std::vector<std::string> criticalPath = {
            "system", "monitor", std::string(baseKey) + "_critical_threshold"
        };
        const std::string statLabel = tr(statLabelKey);

        auto activitySlider =
            SliderSetting{activityValue, profile.minValue, profile.maxValue, profile.step, integerValue};
        activitySlider.valueSuffix = valueSuffix;
        activitySlider.linkedCommit = [criticalValue, criticalPath](double committedValue) {
          if (committedValue <= criticalValue) {
            return std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{};
          }
          return std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
              {criticalPath, ConfigOverrideValue{committedValue}}
          };
        };
        auto activity = makeEntry(
            "system", "monitor",
            tr("settings.schema.services.system-monitor.activity-threshold.label", "stat", statLabel),
            tr("settings.schema.services.system-monitor.activity-threshold.description"), activityPath,
            std::move(activitySlider), "system monitor threshold", true
        );
        activity.visibleWhen = monitorOn;
        entries.push_back(std::move(activity));

        auto criticalSlider =
            SliderSetting{criticalValue, profile.minValue, profile.maxValue, profile.step, integerValue};
        criticalSlider.valueSuffix = std::move(valueSuffix);
        criticalSlider.linkedCommit = [activityValue, activityPath](double committedValue) {
          if (committedValue >= activityValue) {
            return std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{};
          }
          return std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
              {activityPath, ConfigOverrideValue{committedValue}}
          };
        };
        auto critical = makeEntry(
            "system", "monitor",
            tr("settings.schema.services.system-monitor.critical-threshold.label", "stat", statLabel),
            tr("settings.schema.services.system-monitor.critical-threshold.description"), criticalPath,
            std::move(criticalSlider), "system monitor threshold", true
        );
        critical.visibleWhen = monitorOn;
        entries.push_back(std::move(critical));
      };

      using noctalia::sysmon::Stat;
      addThresholdPair(
          "cpu_usage", "settings.schema.services.system-monitor.stats.cpu-usage", mon.cpuUsageActivityThreshold,
          mon.cpuUsageCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuUsage), true, "%"
      );
      addThresholdPair(
          "cpu_temp", "settings.schema.services.system-monitor.stats.cpu-temp", mon.cpuTempActivityThreshold,
          mon.cpuTempCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::CpuTemp), true, "°C"
      );
      addThresholdPair(
          "gpu_temp", "settings.schema.services.system-monitor.stats.gpu-temp", mon.gpuTempActivityThreshold,
          mon.gpuTempCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuTemp), true, "°C"
      );
      addThresholdPair(
          "gpu_usage", "settings.schema.services.system-monitor.stats.gpu-usage", mon.gpuUsageActivityThreshold,
          mon.gpuUsageCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuUsage), true, "%"
      );
      addThresholdPair(
          "gpu_vram", "settings.schema.services.system-monitor.stats.gpu-vram", mon.gpuVramActivityThreshold,
          mon.gpuVramCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::GpuVram), true, "%"
      );
      addThresholdPair(
          "ram_pct", "settings.schema.services.system-monitor.stats.ram-usage", mon.ramPctActivityThreshold,
          mon.ramPctCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::RamPct), true, "%"
      );
      addThresholdPair(
          "swap_pct", "settings.schema.services.system-monitor.stats.swap-usage", mon.swapPctActivityThreshold,
          mon.swapPctCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::SwapPct), true, "%"
      );
      addThresholdPair(
          "disk_pct", "settings.schema.services.system-monitor.stats.disk-usage", mon.diskPctActivityThreshold,
          mon.diskPctCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::DiskPct), true, "%"
      );
      addThresholdPair(
          "net_rx", "settings.schema.services.system-monitor.stats.network-rx", mon.netRxActivityThreshold,
          mon.netRxCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::NetRx), false, "MB/s"
      );
      addThresholdPair(
          "net_tx", "settings.schema.services.system-monitor.stats.network-tx", mon.netTxActivityThreshold,
          mon.netTxCriticalThreshold, noctalia::sysmon::thresholdProfile(Stat::NetTx), false, "MB/s"
      );
    }
    entries.push_back(makeEntry(
        "system", "screen-time", tr("settings.schema.shell.screen-time-enabled.label"),
        tr("settings.schema.shell.screen-time-enabled.description"), {"shell", "screen_time_enabled"},
        ToggleSetting{cfg.shell.screenTimeEnabled}, "screen time usage tracking control center"
    ));
    if (env.batteryAvailable) {
      if (env.systemBatteryAvailable) {
        entries.push_back(makeEntry(
            "system", "battery", tr("settings.schema.system.battery-warning-threshold.label"),
            tr("settings.schema.system.battery-warning-threshold.description"), {"battery", "warning_threshold"},
            sliderFor(cfg.battery.warningThreshold, noctalia::config::schema::kBatteryWarningThresholdRange, true),
            "battery low warning threshold notification"
        ));
      }
      for (const auto& device : env.batteryDeviceOptions) {
        int value = 0;
        if (const auto it = env.batteryWarningThresholds.find(device.value); it != env.batteryWarningThresholds.end()) {
          value = it->second;
        }
        entries.push_back(makeEntry(
            "system", "battery",
            tr("settings.schema.system.battery-device-warning-threshold.label", "device", device.label),
            tr("settings.schema.system.battery-device-warning-threshold.description"),
            {"battery", "device", device.value, "warning_threshold"},
            SliderSetting{std::clamp(value, 0, 100), 0.0f, 100.0f, 1.0f, true},
            std::string("battery device low warning threshold notification ") + device.label + " " + device.value
        ));
      }
    }

    // Location — single source of "where am I"; shared by weather, night light, and theme auto mode.
    entries.push_back(makeEntry(
        "location", "location", tr("settings.schema.services.location-auto-locate.label"),
        tr("settings.schema.services.location-auto-locate.description"), {"location", "auto_locate"},
        ToggleSetting{cfg.location.autoLocate}, "location ip geolocate gps coordinate"
    ));
    const SettingVisibility autoLocateOff{{"location", "auto_locate"}, {"false"}};
    {
      auto e = makeEntry(
          "location", "location", tr("settings.schema.services.location-address.label"),
          tr("settings.schema.services.location-address.description"), {"location", "address"},
          TextSetting{
              .value = cfg.location.address,
              .placeholder = tr("settings.schema.services.location-address.placeholder"),
              .browseFileExtensions = {}
          },
          "location address city geocode"
      );
      e.visibleWhen = autoLocateOff;
      entries.push_back(std::move(e));
    }
    // Manual schedule fallback: shown only when no network location is configured (auto-locate off
    // and no address). The address gate is build-time; the auto-locate gate is evaluated live.
    const SettingVisibility manualLocationHidden{{"location", "auto_locate"}, {}};
    const SettingVisibility& manualLocationControlsVisible =
        cfg.location.address.empty() ? autoLocateOff : manualLocationHidden;
    {
      auto e = makeEntry(
          "location", "location", tr("settings.schema.services.sunset.label"),
          tr("settings.schema.services.sunset.description"), {"location", "sunset"},
          TextSetting{.value = cfg.location.sunset, .placeholder = "20:30", .browseFileExtensions = {}},
          "time schedule sunset"
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "location", "location", tr("settings.schema.services.sunrise.label"),
          tr("settings.schema.services.sunrise.description"), {"location", "sunrise"},
          TextSetting{.value = cfg.location.sunrise, .placeholder = "07:30", .browseFileExtensions = {}},
          "time schedule sunrise"
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "location", "location", tr("settings.schema.services.latitude.label"),
          tr("settings.schema.services.latitude.description"), {"location", "latitude"},
          OptionalNumberSetting{cfg.location.latitude, -90.0, 90.0, "52.5200"}, "coordinate location sunrise sunset",
          true
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "location", "location", tr("settings.schema.services.longitude.label"),
          tr("settings.schema.services.longitude.description"), {"location", "longitude"},
          OptionalNumberSetting{cfg.location.longitude, -180.0, 180.0, "13.4050"}, "coordinate location sunrise sunset",
          true
      );
      e.visibleWhen = manualLocationControlsVisible;
      entries.push_back(std::move(e));
    }

    // Weather — consumes the resolved location.
    entries.push_back(makeEntry(
        "location", "weather", tr("settings.schema.services.weather.label"),
        tr("settings.schema.services.weather.description"), {"weather", "enabled"}, ToggleSetting{cfg.weather.enabled},
        "forecast"
    ));
    {
      auto e = makeEntry(
          "location", "weather", tr("settings.schema.services.weather-unit.label"),
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
          "location", "weather", tr("settings.schema.services.weather-effects.label"),
          tr("settings.schema.services.weather-effects.description"), {"weather", "effects"},
          ToggleSetting{cfg.weather.effects}, "forecast visuals"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }
    {
      auto e = makeEntry(
          "location", "weather", tr("settings.schema.services.weather-refresh-interval.label"),
          tr("settings.schema.services.weather-refresh-interval.description"), {"weather", "refresh_minutes"},
          sliderFor(cfg.weather.refreshMinutes, noctalia::config::schema::kRefreshMinutesRange, true), "forecast"
      );
      e.visibleWhen = weatherOn;
      entries.push_back(std::move(e));
    }

    if (!env.gammaControlAvailable) {
      entries.push_back(makeEntry(
          "location", "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.requires-gamma-control"), {"nightlight", "enabled"},
          ToggleSetting{.checked = cfg.nightlight.enabled, .enabled = false}, "nightlight"
      ));
    } else {
      entries.push_back(makeEntry(
          "location", "night-light", tr("settings.schema.services.night-light.label"),
          tr("settings.schema.services.night-light.description"), {"nightlight", "enabled"},
          ToggleSetting{cfg.nightlight.enabled}, "nightlight"
      ));
      const SettingVisibility nightLightOn{{"nightlight", "enabled"}, {"true"}};
      {
        auto e = makeEntry(
            "location", "night-light", tr("settings.schema.services.force-night-light.label"),
            tr("settings.schema.services.force-night-light.description"), {"nightlight", "force"},
            ToggleSetting{cfg.nightlight.force}, "nightlight"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
      // Both sliders span the same range; the day > night invariant is enforced at commit time
      // via SliderSetting::linkedCommit, which pushes the other temperature when needed.
      const double tempMin = static_cast<double>(NightLightConfig::kTemperatureMin);
      const double tempMax = static_cast<double>(NightLightConfig::kTemperatureMax);
      const double tempStep = static_cast<double>(NightLightConfig::kTemperatureGap);

      SliderSetting daySlider{static_cast<double>(cfg.nightlight.dayTemperature), tempMin, tempMax, tempStep, true};
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
            "location", "night-light", tr("settings.schema.services.day-temperature.label"),
            tr("settings.schema.services.day-temperature.description"), {"nightlight", "temperature_day"},
            std::move(daySlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }

      SliderSetting nightSlider{static_cast<double>(cfg.nightlight.nightTemperature), tempMin, tempMax, tempStep, true};
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
            "location", "night-light", tr("settings.schema.services.night-temperature.label"),
            tr("settings.schema.services.night-temperature.description"), {"nightlight", "temperature_night"},
            std::move(nightSlider), "nightlight kelvin"
        );
        e.visibleWhen = nightLightOn;
        entries.push_back(std::move(e));
      }
    }

    const SettingVisibility calendarOn{{"calendar", "enabled"}, {"true"}};
    entries.push_back(makeEntry(
        "services", "calendar", tr("settings.schema.services.calendar.label"),
        tr("settings.schema.services.calendar.description"), {"calendar", "enabled"},
        ToggleSetting{cfg.calendar.enabled}, "calendar events caldav google"
    ));
    {
      auto e = makeEntry(
          "services", "calendar", tr("settings.schema.services.calendar-refresh-interval.label"),
          tr("settings.schema.services.calendar-refresh-interval.description"), {"calendar", "refresh_minutes"},
          sliderFor(cfg.calendar.refreshMinutes, noctalia::config::schema::kRefreshMinutesRange, true), "calendar sync"
      );
      e.visibleWhen = calendarOn;
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
        sliderFor(cfg.audio.soundVolume, noctalia::config::schema::kUnitRange, false), "sound"
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
        "services", "brightness", tr("settings.schema.services.ddcutil.label"),
        env.ddcutilAvailable ? tr("settings.schema.services.ddcutil.description")
                             : tr("settings.schema.services.ddcutil.requires-ddcutil"),
        {"brightness", "enable_ddcutil"},
        ToggleSetting{.checked = cfg.brightness.enableDdcutil, .enabled = env.ddcutilAvailable}, "monitor ddcutil"
    ));
    entries.push_back(makeEntry(
        "services", "media", tr("settings.schema.services.mpris-blacklist.label"),
        tr("settings.schema.services.mpris-blacklist.description"), {"shell", "mpris", "blacklist"},
        ListSetting{.items = cfg.shell.mpris.blacklist}, "mpris media player dbus session blacklist"
    ));

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
        "idle behavior timeout command resume screen lock dpms suspend lock_and_suspend caffeine"
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
      case HookKind::BatteryCharging:
      case HookKind::BatteryDischarging:
      case HookKind::BatteryPercentageChanged:
      case HookKind::PowerProfileChanged:
        return "power";
      case HookKind::Count:
        break;
      }
      return "general";
    };

    auto hookTags = [](HookKind kind) -> std::string {
      std::string tags = "hook command script exec event trigger";
      if (kind == HookKind::BatteryCharging
          || kind == HookKind::BatteryDischarging
          || kind == HookKind::BatteryPercentageChanged) {
        tags += " battery power";
      }
      if (kind == HookKind::PowerProfileChanged) {
        tags += " power profile performance balanced saver";
      }
      if (kind == HookKind::WallpaperChanged || kind == HookKind::ColorsChanged || kind == HookKind::ThemeModeChanged) {
        tags += " wallpaper colors theme mode light dark auto";
      }
      if (kind == HookKind::WifiEnabled
          || kind == HookKind::WifiDisabled
          || kind == HookKind::BluetoothEnabled
          || kind == HookKind::BluetoothDisabled) {
        tags += " network wifi bluetooth";
      }
      if (kind == HookKind::SessionLocked
          || kind == HookKind::SessionUnlocked
          || kind == HookKind::LoggingOut
          || kind == HookKind::Rebooting
          || kind == HookKind::ShuttingDown
          || kind == HookKind::Started) {
        tags += " session startup";
      }
      return tags;
    };

    for (const auto& kind : kHookKinds) {
      const auto index = static_cast<std::size_t>(kind.value);
      const std::string key(kind.key);
      const std::string baseKey = "settings.schema.hooks.events." + i18n::keySegment(key);
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

    // Notifications
    entries.push_back(makeEntry(
        "notifications", "general", tr("settings.schema.notifications.daemon.label"),
        tr("settings.schema.notifications.daemon.description"), {"notification", "enable_daemon"},
        ToggleSetting{cfg.notification.enableDaemon}, "dbus"
    ));
    entries.push_back(makeEntry(
        "notifications", "general", tr("settings.schema.notifications.show-app-name.label"),
        tr("settings.schema.notifications.show-app-name.description"), {"notification", "show_app_name"},
        ToggleSetting{cfg.notification.showAppName}, "application identity header"
    ));
    entries.push_back(makeEntry(
        "notifications", "general", tr("settings.schema.notifications.show-actions.label"),
        tr("settings.schema.notifications.show-actions.description"), {"notification", "show_actions"},
        ToggleSetting{cfg.notification.showActions}, "action buttons"
    ));
    entries.push_back(makeEntry(
        "notifications", "toasts", tr("settings.schema.notifications.layer.label"),
        tr("settings.schema.notifications.layer.description"), {"notification", "layer"},
        asSegmented(plainSelect(
            {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
            cfg.notification.layer
        )),
        "toast layer shell z-order"
    ));
    entries.push_back(makeEntry(
        "notifications", "toasts", tr("settings.schema.notifications.position.label"),
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
        "notifications", "toasts", tr("settings.schema.notifications.scale.label"),
        tr("settings.schema.notifications.scale.description"), {"notification", "scale"},
        sliderFor(cfg.notification.scale, noctalia::config::schema::kScaleRange, false), "toast size scale"
    ));
    entries.push_back(makeEntry(
        "notifications", "toasts", tr("settings.schema.notifications.offset-x.label"),
        tr("settings.schema.notifications.offset-x.description"), {"notification", "offset_x"},
        StepperSetting{
            .value = cfg.notification.offsetX, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"
        },
        "offset margin horizontal"
    ));
    entries.push_back(makeEntry(
        "notifications", "toasts", tr("settings.schema.notifications.offset-y.label"),
        tr("settings.schema.notifications.offset-y.description"), {"notification", "offset_y"},
        StepperSetting{
            .value = cfg.notification.offsetY, .minValue = 0, .maxValue = 200, .step = 1, .valueSuffix = "px"
        },
        "offset margin vertical"
    ));
    entries.push_back(makeEntry(
        "notifications", "toasts", tr("settings.schema.notifications.toast-opacity.label"),
        tr("settings.schema.notifications.toast-opacity.description"), {"notification", "background_opacity"},
        sliderFor(cfg.notification.backgroundOpacity, noctalia::config::schema::kUnitRange, false), "popup"
    ));
    entries.push_back(makeEntry(
        "notifications", "general", tr("settings.schema.notifications.collapse-on-dismiss.label"),
        tr("settings.schema.notifications.collapse-on-dismiss.description"), {"notification", "collapse_on_dismiss"},
        ToggleSetting{cfg.notification.collapseOnDismiss}, "reorder stack slide"
    ));
    entries.push_back(makeEntry(
        "notifications", "toasts", tr("settings.schema.notifications.monitors.label"),
        tr("settings.schema.notifications.monitors.description"), {"notification", "monitors"},
        ListSetting{.items = cfg.notification.monitors, .suggestedOptions = env.availableOutputs},
        "monitor output display screen"
    ));
    {
      MultiSelectSetting allowedUrgencies;
      allowedUrgencies.options = {
          {"low", tr("settings.options.notification-urgency.low")},
          {"normal", tr("settings.options.notification-urgency.normal")},
          {"critical", tr("settings.options.notification-urgency.critical")},
      };
      if (cfg.notification.allowedUrgencies.empty()) {
        allowedUrgencies.selectedValues = {"low", "normal", "critical"};
      } else {
        allowedUrgencies.selectedValues = cfg.notification.allowedUrgencies;
      }
      allowedUrgencies.requireAtLeastOne = true;
      entries.push_back(makeEntry(
          "notifications", "filtering", tr("settings.schema.notifications.allowed-urgencies.label"),
          tr("settings.schema.notifications.allowed-urgencies.description"), {"notification", "allowed_urgencies"},
          std::move(allowedUrgencies), "urgency low normal critical filter"
      ));
    }
    entries.push_back(makeEntry(
        "notifications", "filtering", tr("settings.schema.notifications.blacklist.label"),
        tr("settings.schema.notifications.blacklist.description"), {"notification", "blacklist"},
        ListSetting{.items = cfg.notification.blacklist},
        "blacklist block suppress filter app name desktop entry category substring"
    ));
    entries.push_back(makeEntry(
        "notifications", "filtering", tr("settings.schema.notifications.blacklist-allow-critical.label"),
        tr("settings.schema.notifications.blacklist-allow-critical.description"),
        {"notification", "blacklist_allow_critical"}, ToggleSetting{cfg.notification.blacklistAllowCritical},
        "critical urgency bypass"
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
          section, "general", tr("settings.schema.bar.layer.label"), tr("settings.schema.bar.layer.description"),
          path("layer"),
          asSegmented(plainSelect(
              {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}}, bar.layer
          )),
          "layer shell z-order"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.thickness.label"), tr("settings.schema.bar.thickness.description"),
          path("thickness"), SliderSetting{bar.thickness, 10.0f, 120.0f, 1.0f, true}, "height width"
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
          SliderSetting{bar.padding, 0.0f, 80.0f, 1.0f, true}, "inset"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-radius.label"),
          tr("settings.schema.bar.corner-radius.description"), path("radius"),
          SliderSetting{bar.radius, 0.0f, 80.0f, 1.0f, true}, "rounded"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-left.label"),
          tr("settings.schema.bar.corner-top-left.description"), path("radius_top_left"),
          SliderSetting{bar.radiusTopLeft, 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-right.label"),
          tr("settings.schema.bar.corner-top-right.description"), path("radius_top_right"),
          SliderSetting{bar.radiusTopRight, 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
          tr("settings.schema.bar.corner-bottom-left.description"), path("radius_bottom_left"),
          SliderSetting{bar.radiusBottomLeft, 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
          tr("settings.schema.bar.corner-bottom-right.description"), path("radius_bottom_right"),
          SliderSetting{bar.radiusBottomRight, 0.0f, 80.0f, 1.0f, true}, "rounded corner", true
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
          section, "effects", tr("settings.schema.shared.background-opacity.label"),
          tr("settings.schema.bar.background-opacity.description"), path("background_opacity"),
          SliderSetting{bar.backgroundOpacity, 0.0f, 1.0f, 0.01f, false}, "alpha"
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
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.panel-overlap.label"),
          tr("settings.schema.bar.panel-overlap.description"), path("panel_overlap"),
          barPanelOverlapStepper(bar.panelOverlap), "seam gap overlap attached panel fractional scale", true
      ));
      {
        std::vector<SelectOption> fontWeightOptions;
        const auto widgetOptions =
            buildLabelFontWeightSelectOptions(cfg.shell.fontFamily, FontWeightSelectKind::BarDefault, bar.fontWeight);
        fontWeightOptions.reserve(widgetOptions.size());
        for (const auto& option : widgetOptions) {
          fontWeightOptions.push_back(SelectOption{option.value, tr(option.labelKey)});
        }
        SelectSetting fontWeightSelect{std::move(fontWeightOptions), std::to_string(bar.fontWeight)};
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
          SliderSetting{bar.widgetSpacing, 0.0f, 32.0f, 1.0f, true}, "gap"
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
        auto monitorPath = [&](std::string key) {
          std::vector<std::string> p = root;
          p.push_back(std::move(key));
          return p;
        };

        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.enabled.label"),
            tr("settings.schema.bar.enabled.description"), monitorPath("enabled"),
            ToggleSetting{ovr.enabled.value_or(bar.enabled)}, "visible"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.position.label"),
            tr("settings.schema.bar.position.description"), monitorPath("position"),
            positionSelect(ovr.position.value_or(bar.position)), "edge"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.auto-hide.label"),
            tr("settings.schema.bar.auto-hide.description"), monitorPath("auto_hide"),
            ToggleSetting{ovr.autoHide.value_or(bar.autoHide)}, "autohide"
        ));
        {
          auto e = makeEntry(
              section, "general", tr("settings.schema.shared.reserve-space.label"),
              tr("settings.schema.bar.reserve-space.description"), monitorPath("reserve_space"),
              ToggleSetting{ovr.reserveSpace.value_or(bar.reserveSpace)}, "exclusive zone"
          );
          e.visibleWhen = SettingVisibility{monitorPath("auto_hide"), {"false"}};
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.bar.layer.label"), tr("settings.schema.bar.layer.description"),
            monitorPath("layer"),
            asSegmented(plainSelect(
                {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
                ovr.layer.value_or(bar.layer)
            )),
            "layer shell z-order"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.thickness.label"),
            tr("settings.schema.bar.thickness.description"), monitorPath("thickness"),
            SliderSetting{ovr.thickness.value_or(bar.thickness), 10.0f, 120.0f, 1.0f, true}, "height width"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-scale.label"),
            tr("settings.schema.bar.content-scale.description"), monitorPath("scale"),
            SliderSetting{ovr.scale.value_or(bar.scale), 0.5f, 4.0f, 0.05f, false}, "zoom size"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.ends-margin.label"),
            tr("settings.schema.bar.ends-margin.description"), monitorPath("margin_ends"),
            barMarginStepper(ovr.marginEnds.value_or(bar.marginEnds)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.edge-margin.label"),
            tr("settings.schema.bar.edge-margin.description"), monitorPath("margin_edge"),
            barMarginStepper(ovr.marginEdge.value_or(bar.marginEdge)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-padding.label"),
            tr("settings.schema.bar.content-padding.description"), monitorPath("padding"),
            SliderSetting{ovr.padding.value_or(bar.padding), 0.0f, 80.0f, 1.0f, true}, "inset"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-radius.label"),
            tr("settings.schema.bar.corner-radius.description"), monitorPath("radius"),
            SliderSetting{ovr.radius.value_or(bar.radius), 0.0f, 80.0f, 1.0f, true}, "rounded"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-left.label"),
            tr("settings.schema.bar.corner-top-left.description"), monitorPath("radius_top_left"),
            SliderSetting{ovr.radiusTopLeft.value_or(bar.radiusTopLeft), 0.0f, 80.0f, 1.0f, true}, "rounded corner",
            true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-right.label"),
            tr("settings.schema.bar.corner-top-right.description"), monitorPath("radius_top_right"),
            SliderSetting{ovr.radiusTopRight.value_or(bar.radiusTopRight), 0.0f, 80.0f, 1.0f, true}, "rounded corner",
            true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
            tr("settings.schema.bar.corner-bottom-left.description"), monitorPath("radius_bottom_left"),
            SliderSetting{ovr.radiusBottomLeft.value_or(bar.radiusBottomLeft), 0.0f, 80.0f, 1.0f, true},
            "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
            tr("settings.schema.bar.corner-bottom-right.description"), monitorPath("radius_bottom_right"),
            SliderSetting{ovr.radiusBottomRight.value_or(bar.radiusBottomRight), 0.0f, 80.0f, 1.0f, true},
            "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border.label"), tr("settings.schema.bar.border.description"),
            monitorPath("border"), colorSpecPicker(ovr.border, true, tr("common.states.inherit")), "outline color", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border-width.label"),
            tr("settings.schema.bar.border-width.description"), monitorPath("border_width"),
            SliderSetting{ovr.borderWidth.value_or(bar.borderWidth), 0.0f, 20.0f, 0.5f, false}, "outline stroke", true
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.background-opacity.label"),
            tr("settings.schema.bar.background-opacity.description"), monitorPath("background_opacity"),
            SliderSetting{ovr.backgroundOpacity.value_or(bar.backgroundOpacity), 0.0f, 1.0f, 0.01f, false}, "alpha"
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.bar.shadow.description"),
            monitorPath("shadow"), ToggleSetting{ovr.shadow.value_or(bar.shadow)}, "shadow"
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.contact-shadow.label"),
            tr("settings.schema.bar.contact-shadow.description"), monitorPath("contact_shadow"),
            ToggleSetting{ovr.contactShadow.value_or(bar.contactShadow)}, "shadow contact panel attached"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.panel-overlap.label"),
            tr("settings.schema.bar.panel-overlap.description"), monitorPath("panel_overlap"),
            barPanelOverlapStepper(ovr.panelOverlap.value_or(bar.panelOverlap)),
            "seam gap overlap attached panel fractional scale", true
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-spacing.label"),
            tr("settings.schema.bar.widget-spacing.description"), monitorPath("widget_spacing"),
            SliderSetting{ovr.widgetSpacing.value_or(bar.widgetSpacing), 0.0f, 32.0f, 1.0f, true}, "gap"
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-color.label"),
            tr("settings.schema.bar.widget-color.description"), monitorPath("color"),
            colorSpecPicker(ovr.widgetColor, true, tr("common.states.inherit")), "color foreground", true
        ));
        entries.push_back(makeEntry(
            section, "capsules", tr("settings.schema.bar.widget-capsules.label"),
            tr("settings.schema.bar.widget-capsules.description"), monitorPath("capsule"),
            ToggleSetting{ovr.widgetCapsuleDefault.value_or(bar.widgetCapsuleDefault)}, "pill"
        ));
        const SettingVisibility monitorCapsuleOn{monitorPath("capsule"), {"true"}};
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-radius.label"),
              tr("settings.schema.bar.capsule-radius.description"), monitorPath("capsule_radius"),
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
              section, "capsules", tr("settings.schema.bar.capsule-fill.label"),
              tr("settings.schema.bar.capsule-fill.description"), monitorPath("capsule_fill"),
              colorSpecPicker(ovr.widgetCapsuleFill, true, tr("common.states.inherit")), "color pill", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-foreground.label"),
              tr("settings.schema.bar.capsule-foreground.description"), monitorPath("capsule_foreground"),
              colorSpecPicker(ovr.widgetCapsuleForeground, true, tr("common.states.inherit")), "color foreground pill",
              true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-border.label"),
              tr("settings.schema.bar.capsule-border.description"), monitorPath("capsule_border"),
              colorSpecPicker(
                  ovr.widgetCapsuleBorderSpecified ? ovr.widgetCapsuleBorder : std::optional<ColorSpec>{}, true,
                  tr("common.states.inherit")
              ),
              "color pill outline", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-padding.label"),
              tr("settings.schema.bar.capsule-padding.description"), monitorPath("capsule_padding"),
              SliderSetting{ovr.widgetCapsulePadding.value_or(bar.widgetCapsulePadding), 0.0f, 48.0f, 1.0f, false},
              "pill inset", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-opacity.label"),
              tr("settings.schema.bar.capsule-opacity.description"), monitorPath("capsule_opacity"),
              SliderSetting{ovr.widgetCapsuleOpacity.value_or(bar.widgetCapsuleOpacity), 0.0f, 1.0f, 0.01f, false},
              "pill alpha", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.start-widgets.label"),
            tr("settings.schema.bar.start-widgets.description"), monitorPath("start"),
            ListSetting{.items = ovr.startWidgets.value_or(bar.startWidgets)}, "left"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.center-widgets.label"),
            tr("settings.schema.bar.center-widgets.description"), monitorPath("center"),
            ListSetting{.items = ovr.centerWidgets.value_or(bar.centerWidgets)}, "middle"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.end-widgets.label"),
            tr("settings.schema.bar.end-widgets.description"), monitorPath("end"),
            ListSetting{.items = ovr.endWidgets.value_or(bar.endWidgets)}, "right"
        ));
      }
    }

    // Integrity guard: every override path (and visibility-condition path) must
    // resolve to a real schema key, else the entry silently reads/writes a dead
    // override. Build-determined, so checked once per process; warn-only.
    {
      static bool verified = false;
      if (!verified) {
        verified = true;
        const Logger log("settings");
        const auto verify = [&](const std::vector<std::string>& path, std::string_view what) {
          if (!path.empty() && !noctalia::config::schema::isKnownConfigPath(path)) {
            std::string dotted;
            for (const auto& seg : path) {
              dotted += (dotted.empty() ? "" : ".") + seg;
            }
            log.warn("settings registry {} path does not resolve to a schema key: {}", what, dotted);
          }
        };
        for (const auto& entry : entries) {
          if (!std::holds_alternative<ButtonSetting>(entry.control)) {
            verify(entry.path, "override");
          }
          if (entry.visibleWhen.has_value()) {
            for (const auto& condition : entry.visibleWhen->all) {
              verify(condition.path, "visibleWhen");
            }
          }
        }
      }
    }

    return entries;
  }

} // namespace settings
