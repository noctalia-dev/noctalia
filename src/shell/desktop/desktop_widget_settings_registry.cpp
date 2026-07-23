#include "shell/desktop/desktop_widget_settings_registry.h"

#include "i18n/i18n.h"
#include "scripting/plugin_i18n.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/font_family_catalog.h"
#include "shell/settings/widget_settings_registry.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdint>

namespace desktop_settings {
  namespace {

    using settings::WidgetControlKind;
    using settings::WidgetSettingSelectOption;
    using settings::WidgetSettingSpec;
    using settings::WidgetSettingVisibility;
    using settings::WidgetSettingVisibilityCondition;

    const std::vector<DesktopWidgetTypeSpec> kDesktopWidgetTypeSpecs = {
        {.type = "audio_visualizer", .labelKey = "desktop-widgets.editor.types.audio-visualizer"},
        {.type = "button", .labelKey = "desktop-widgets.editor.types.button"},
        {.type = "clock", .labelKey = "desktop-widgets.editor.types.clock"},
        {.type = "fancy_audio_visualizer", .labelKey = "desktop-widgets.editor.types.fancy-audio-visualizer"},
        {.type = "label", .labelKey = "desktop-widgets.editor.types.label"},
        {.type = "media_player", .labelKey = "desktop-widgets.editor.types.media-player"},
        {.type = "sticker", .labelKey = "desktop-widgets.editor.types.sticker"},
        {.type = "sysmon", .labelKey = "desktop-widgets.editor.types.system-monitor"},
        {.type = "volume", .labelKey = "desktop-widgets.editor.types.volume"},
        {.type = "weather", .labelKey = "desktop-widgets.editor.types.weather"},
    };

    WidgetSettingSpec baseSpec(std::string_view key, WidgetControlKind control, WidgetSettingValue defaultValue) {
      WidgetSettingSpec spec;
      spec.schema.key = std::string(key);
      spec.schema.type = settings::schemaTypeForControl(control);
      spec.schema.defaultValue = std::move(defaultValue);
      spec.control = control;
      spec.labelKey = "desktop-widgets.editor.settings." + StringUtils::snakeToKebab(key);
      return spec;
    }

    WidgetSettingSpec boolSpec(std::string_view key, bool defaultValue) {
      return baseSpec(key, WidgetControlKind::Bool, defaultValue);
    }

    WidgetSettingSpec
    intSpec(std::string_view key, std::int64_t defaultValue, double minValue, double maxValue, double step = 1.0) {
      auto spec = baseSpec(key, WidgetControlKind::Int, defaultValue);
      spec.schema.minValue = minValue;
      spec.schema.maxValue = maxValue;
      spec.schema.step = step;
      return spec;
    }

    WidgetSettingSpec stepperIntSpec(
        std::string_view key, std::int64_t defaultValue, double minValue, double maxValue, double step = 1.0
    ) {
      auto spec = intSpec(key, defaultValue, minValue, maxValue, step);
      spec.stepper = true;
      return spec;
    }

    WidgetSettingSpec
    doubleSpec(std::string_view key, double defaultValue, double minValue, double maxValue, double step = 1.0) {
      auto spec = baseSpec(key, WidgetControlKind::Double, defaultValue);
      spec.schema.minValue = minValue;
      spec.schema.maxValue = maxValue;
      spec.schema.step = step;
      return spec;
    }

    WidgetSettingSpec stringSpec(std::string_view key, std::string defaultValue = {}) {
      return baseSpec(key, WidgetControlKind::String, std::move(defaultValue));
    }

    WidgetSettingSpec glyphSpec(std::string_view key, std::string defaultValue = {}) {
      return baseSpec(key, WidgetControlKind::Glyph, std::move(defaultValue));
    }

    WidgetSettingSpec colorSpec(std::string_view key, std::string defaultValue = {}) {
      return baseSpec(key, WidgetControlKind::ColorSpec, std::move(defaultValue));
    }

    WidgetSettingSpec
    selectSpec(std::string_view key, std::string defaultValue, std::vector<WidgetSettingSelectOption> options) {
      auto spec = baseSpec(key, WidgetControlKind::Select, std::move(defaultValue));
      for (const auto& option : options) {
        spec.schema.enumValues.push_back(option.value);
      }
      spec.options = std::move(options);
      return spec;
    }

    WidgetSettingSpec
    segmentedSpec(std::string_view key, std::string defaultValue, std::vector<WidgetSettingSelectOption> options) {
      auto spec = selectSpec(key, std::move(defaultValue), std::move(options));
      spec.segmented = true;
      return spec;
    }

    // Font picker rendered as a dropdown of installed families, but validated as a free string:
    // a font configured on another machine but absent here must still load (no silent reset).
    // Empty value = inherit the shell font.
    WidgetSettingSpec fontFamilySpec() {
      auto spec = baseSpec("font_family", WidgetControlKind::Select, std::string{});
      spec.schema.type = noctalia::config::schema::WidgetSettingType::String;
      spec.options = settings::buildFontFamilySelectOptions();
      spec.literalLabels = true;
      return spec;
    }

    // Resolve "author/plugin:entry" to its [[desktop_widget]] entry, or nullopt.
    std::optional<scripting::ResolvedPluginEntry>
    resolvePluginDesktopWidget(std::string_view type, scripting::PluginRegistry* pluginRegistry = nullptr) {
      if (!type.contains('/')) {
        return std::nullopt;
      }
      auto& registry = pluginRegistry != nullptr ? *pluginRegistry : scripting::PluginRegistry::instance();
      registry.ensureScanned();
      auto entry = registry.resolve(type);
      if (entry.has_value() && entry->entry->kind == scripting::PluginEntryKind::DesktopWidget) {
        return entry;
      }
      return std::nullopt;
    }

  } // namespace

  const std::vector<DesktopWidgetTypeSpec>& desktopWidgetTypeSpecs() { return kDesktopWidgetTypeSpecs; }

  std::vector<DesktopWidgetTypeOption> desktopWidgetTypeOptions() {
    std::vector<DesktopWidgetTypeOption> options;
    options.reserve(kDesktopWidgetTypeSpecs.size());
    for (const auto& spec : kDesktopWidgetTypeSpecs) {
      options.push_back(DesktopWidgetTypeOption{.value = std::string(spec.type), .label = i18n::tr(spec.labelKey)});
    }

    scripting::PluginRegistry::instance().ensureScanned();
    for (const auto& entry :
         scripting::PluginRegistry::instance().entriesOfKind(scripting::PluginEntryKind::DesktopWidget)) {
      const std::string entryId = entry.fullId();
      std::string label = entry.manifest->name.empty() ? entryId : entry.manifest->name;
      options.push_back(DesktopWidgetTypeOption{.value = entryId, .label = std::move(label)});
    }

    std::ranges::sort(options, {}, &DesktopWidgetTypeOption::label);
    return options;
  }

  std::string desktopWidgetTypeLabel(std::string_view type) {
    for (const auto& spec : kDesktopWidgetTypeSpecs) {
      if (spec.type == type) {
        return i18n::tr(spec.labelKey);
      }
    }
    if (type == "login_box") {
      return i18n::tr("desktop-widgets.editor.types.login-box");
    }
    if (auto entry = resolvePluginDesktopWidget(type); entry.has_value()) {
      if (!entry->manifest->name.empty()) {
        return entry->manifest->name;
      }
    }
    return std::string(type);
  }

  std::vector<WidgetSettingSpec> commonDesktopWidgetSettingSpecs(std::string_view type) {
    if (type == "login_box") {
      auto bgColor = colorSpec("background_color", "surface_variant");
      auto bgRadius = intSpec("background_radius", 12.0, 0.0, 32.0, 1.0);
      auto bgOpacity = doubleSpec("background_opacity", 0.88, 0.0, 1.0, 0.01);
      return {
          std::move(bgColor),
          std::move(bgOpacity),
          std::move(bgRadius),
      };
    }

    if (type == "button") {
      return {};
    }

    const WidgetSettingVisibility backgroundOn{"background", {"true"}};
    const bool backgroundDefault = type != "fancy_audio_visualizer";

    auto bgColor = colorSpec("background_color", "surface");
    bgColor.visibleWhen = backgroundOn;

    auto bgRadius = intSpec("background_radius", 12.0, 0.0, 32.0, 1.0);
    bgRadius.visibleWhen = backgroundOn;

    auto bgPadding = intSpec("background_padding", 10.0, 0.0, 32.0, 1.0);
    bgPadding.visibleWhen = backgroundOn;

    auto bgOpacity = doubleSpec("background_opacity", 0.8, 0.0, 1.0, 0.01);
    bgOpacity.visibleWhen = backgroundOn;

    return {
        boolSpec("background", backgroundDefault),
        std::move(bgColor),
        std::move(bgOpacity),
        std::move(bgRadius),
        std::move(bgPadding),
    };
  }

  std::vector<WidgetSettingSpec> desktopWidgetSettingSpecs(std::string_view type) {
    if (auto pluginEntry = resolvePluginDesktopWidget(type)) {
      scripting::PluginTranslationCatalog translations;
      translations.load(pluginEntry->sourcePath.parent_path());
      return settings::manifestSettingSpecs(pluginEntry->entry->settings, &translations);
    }

    const std::vector<WidgetSettingSelectOption> sysmonStats = {
        {"cpu_usage", "desktop-widgets.editor.settings.stat-cpu-usage"},
        {"cpu_temp", "desktop-widgets.editor.settings.stat-cpu-temp"},
        {"gpu_temp", "desktop-widgets.editor.settings.stat-gpu-temp"},
        {"gpu_usage", "desktop-widgets.editor.settings.stat-gpu-usage"},
        {"gpu_vram", "desktop-widgets.editor.settings.stat-gpu-vram"},
        {"ram_pct", "desktop-widgets.editor.settings.stat-ram-pct"},
        {"swap_pct", "desktop-widgets.editor.settings.stat-swap-pct"},
        {"net_rx", "desktop-widgets.editor.settings.stat-net-rx"},
        {"net_tx", "desktop-widgets.editor.settings.stat-net-tx"},
    };

    std::vector<WidgetSettingSelectOption> sysmonStatsWithNone = {
        {"", "desktop-widgets.editor.settings.stat-none"},
    };
    sysmonStatsWithNone.insert(sysmonStatsWithNone.end(), sysmonStats.begin(), sysmonStats.end());

    const std::vector<WidgetSettingSelectOption> networkSpeedUnits = {
        {"auto", "desktop-widgets.editor.settings.network-speed-unit-auto"},
        {"kb", "desktop-widgets.editor.settings.network-speed-unit-kilobytes"},
        {"mb", "desktop-widgets.editor.settings.network-speed-unit-megabytes"},
    };

    std::vector<WidgetSettingSpec> specs;
    auto add = [&](WidgetSettingSpec spec) { specs.push_back(std::move(spec)); };

    if (type == "clock") {
      const WidgetSettingVisibility digitalOnly{{"clock_style", {"digital"}}};
      const WidgetSettingVisibility analogOnly{{"clock_style", {"analog"}}};
      add(segmentedSpec(
          "clock_style", "digital",
          {{"digital", "desktop-widgets.editor.settings.clock-style-digital"},
           {"analog", "desktop-widgets.editor.settings.clock-style-analog"}}
      ));
      auto format = stringSpec("format", "{:%H:%M}");
      format.visibleWhen = digitalOnly;
      add(std::move(format));
      auto centerText = boolSpec("center_text", false);
      centerText.visibleWhen = digitalOnly;
      add(std::move(centerText));
      auto timezone = stringSpec("timezone", "");
      timezone.labelKey = "settings.widgets.settings.timezone.label";
      timezone.descriptionKey = "settings.widgets.settings.timezone.description";
      add(std::move(timezone));
      add(colorSpec("color", "on_surface"));
      add(fontFamilySpec());
      // Shadow is a text shadow on the digital label; analog mode has no shadow.
      auto shadow = boolSpec("shadow", true);
      shadow.visibleWhen = digitalOnly;
      add(std::move(shadow));
      auto circle = boolSpec("circle", true);
      circle.visibleWhen = analogOnly;
      add(std::move(circle));
    } else if (type == "audio_visualizer") {
      add(intSpec("bands", 32, 4.0, 128.0, 4.0));
      add(boolSpec("mirrored", true));
      add(boolSpec("centered", true));
      add(boolSpec("show_when_idle", true));
      add(colorSpec("color_1", "primary"));
      add(colorSpec("color_2", "primary"));
    } else if (type == "fancy_audio_visualizer") {
      const WidgetSettingVisibility barsVisible{"visualization_mode", {"bars", "bars_rings", "all"}};
      const WidgetSettingVisibility waveVisible{"visualization_mode", {"wave", "wave_rings", "all"}};
      const WidgetSettingVisibility ringsVisible{"visualization_mode", {"rings", "bars_rings", "wave_rings", "all"}};

      add(selectSpec(
          "visualization_mode", "bars_rings",
          {{"bars", "desktop-widgets.editor.settings.visualization-mode-bars"},
           {"wave", "desktop-widgets.editor.settings.visualization-mode-wave"},
           {"rings", "desktop-widgets.editor.settings.visualization-mode-rings"},
           {"bars_rings", "desktop-widgets.editor.settings.visualization-mode-bars-rings"},
           {"wave_rings", "desktop-widgets.editor.settings.visualization-mode-wave-rings"},
           {"all", "desktop-widgets.editor.settings.visualization-mode-all"}}
      ));
      add(doubleSpec("sensitivity", 1.5, 0.5, 3.0, 0.1));
      add(doubleSpec("rotation_speed", 0.5, 0.0, 2.0, 0.1));
      auto barWidth = doubleSpec("bar_width", 0.6, 0.2, 1.0, 0.1);
      barWidth.visibleWhen = barsVisible;
      add(std::move(barWidth));
      auto waveThickness = doubleSpec("wave_thickness", 1.0, 0.3, 2.0, 0.1);
      waveThickness.visibleWhen = waveVisible;
      add(std::move(waveThickness));
      auto ringOpacity = doubleSpec("ring_opacity", 0.8, 0.0, 1.0, 0.1);
      ringOpacity.visibleWhen = ringsVisible;
      add(std::move(ringOpacity));
      add(doubleSpec("inner_diameter", 0.7, 0.0, 1.0, 0.05));
      add(doubleSpec("bloom_intensity", 0.5, 0.0, 1.0, 0.05));
      add(boolSpec("fade_when_idle", true));
      add(colorSpec("primary_color", "primary"));
      add(colorSpec("secondary_color", "secondary"));
    } else if (type == "sticker") {
      add(stringSpec("image_path"));
      add(doubleSpec("opacity", 1.0, 0.0, 1.0, 0.01));
    } else if (type == "weather") {
      add(colorSpec("color", "on_surface"));
      add(fontFamilySpec());
      add(boolSpec("shadow", true));
      add(boolSpec("show_forecast", false));
      auto forecastDays = stepperIntSpec("forecast_days", 3, 1.0, 6.0, 1.0);
      forecastDays.visibleWhen = WidgetSettingVisibility{"show_forecast", {"true"}};
      add(std::move(forecastDays));
    } else if (type == "media_player") {
      add(segmentedSpec(
          "layout", "horizontal",
          {{"horizontal", "desktop-widgets.editor.settings.horizontal"},
           {"vertical", "desktop-widgets.editor.settings.vertical"}}
      ));
      add(colorSpec("color", "on_surface"));
      add(fontFamilySpec());
      add(boolSpec("shadow", true));
      add(boolSpec("hide_when_no_media", false));
    } else if (type == "label") {
      add(stringSpec("title", "Title"));
      add(stringSpec("description"));
      add(colorSpec("color", "on_surface"));
      add(doubleSpec("opacity", 1.0, 0.0, 1.0, 0.01));
      add(fontFamilySpec());
      add(boolSpec("shadow", true));
    } else if (type == "button") {
      add(boolSpec("background", true));
      add(glyphSpec("glyph", "heart"));
      add(stringSpec("label"));
      add(stringSpec("command"));
      add(selectSpec(
          "variant", "default",
          {{"default", "desktop-widgets.editor.settings.variant-default"},
           {"primary", "desktop-widgets.editor.settings.variant-primary"},
           {"secondary", "desktop-widgets.editor.settings.variant-secondary"},
           {"outline", "desktop-widgets.editor.settings.variant-outline"},
           {"ghost", "desktop-widgets.editor.settings.variant-ghost"},
           {"destructive", "desktop-widgets.editor.settings.variant-destructive"}}
      ));
      add(colorSpec("color"));
      add(colorSpec("hover_background", "hover"));
      add(fontFamilySpec());
    } else if (type == "sysmon") {
      const std::vector<WidgetSettingSelectOption> sysmonDisplay = {
          {"graph", "desktop-widgets.editor.settings.display-graph"},
          {"gauge", "desktop-widgets.editor.settings.display-gauge"},
      };
      const WidgetSettingVisibility graphOnly{"display", {"graph"}};
      const WidgetSettingVisibility gaugeOnly{"display", {"gauge"}};

      add(selectSpec("stat", "cpu_usage", sysmonStats));
      {
        auto stat2 = selectSpec("stat2", "", sysmonStatsWithNone);
        stat2.visibleWhen = graphOnly;
        add(std::move(stat2));
      }
      {
        auto interface = stringSpec("interface");
        interface.visibleWhen =
            WidgetSettingVisibility{{{"stat", {"net_rx", "net_tx"}}, {"stat2", {"net_rx", "net_tx"}}}};
        add(std::move(interface));
      }
      {
        auto unit = selectSpec("network_speed_unit", "auto", networkSpeedUnits);
        unit.visibleWhen = WidgetSettingVisibility{{{"stat", {"net_rx", "net_tx"}}, {"stat2", {"net_rx", "net_tx"}}}};
        add(std::move(unit));
      }
      {
        auto compact = boolSpec("network_speed_compact", false);
        compact.visibleWhen =
            WidgetSettingVisibility{{{"stat", {"net_rx", "net_tx"}}, {"stat2", {"net_rx", "net_tx"}}}};
        add(std::move(compact));
      }
      add(segmentedSpec("display", "graph", sysmonDisplay));
      {
        auto gaugeLayout = segmentedSpec(
            "gauge_layout", "horizontal",
            {
                {"horizontal", "desktop-widgets.editor.settings.horizontal"},
                {"vertical", "desktop-widgets.editor.settings.vertical"},
            }
        );
        gaugeLayout.visibleWhen = gaugeOnly;
        add(std::move(gaugeLayout));
      }
      add(colorSpec("color", "primary"));
      {
        auto color2 = colorSpec("color2", "secondary");
        color2.visibleWhen = graphOnly;
        add(std::move(color2));
      }
      {
        auto highlight = colorSpec("highlight_color", "error");
        highlight.visibleWhen = gaugeOnly;
        add(std::move(highlight));
      }
      add(fontFamilySpec());
      add(boolSpec("show_label", true));
      {
        auto minW = intSpec("label_min_width", 0, 0.0, 200.0, 1.0);
        WidgetSettingVisibility showLabelGauge;
        showLabelGauge.all = {
            WidgetSettingVisibilityCondition{"display", {"gauge"}},
            WidgetSettingVisibilityCondition{"show_label", {"true"}},
        };
        minW.visibleWhen = showLabelGauge;
        add(std::move(minW));
      }
      add(boolSpec("shadow", true));
    } else if (type == "volume") {
      add(segmentedSpec(
          "device", "output",
          {{"output", "settings.widgets.options.output"}, {"input", "settings.widgets.options.input"}}
      ));
      add(glyphSpec("glyph", ""));
      add(colorSpec("fill_color", "primary"));
      add(colorSpec("track_color", "on_surface_variant"));
      add(boolSpec("show_device", true));
      add(stepperIntSpec("scroll_step", 5, 1.0, 25.0, 1.0));
      add(fontFamilySpec());
      add(boolSpec("shadow", true));
    } else if (type == "login_box") {
      add(boolSpec("show_login_button", true));
      add(boolSpec("show_password_hint", true));
      add(boolSpec("show_caps_lock", true));
      add(boolSpec("show_keyboard_layout", true));
      add(doubleSpec("input_opacity", 1.0, 0.0, 1.0, 0.01));
      add(intSpec("input_radius", 6.0, 0.0, 32.0, 1.0));
      add(boolSpec("center_password_text", false));
    }

    return specs;
  }

  noctalia::config::schema::WidgetSettingSchema
  desktopWidgetSettingSchema(std::string_view type, scripting::PluginRegistry* pluginRegistry) {
    noctalia::config::schema::WidgetSettingSchema out;
    if (auto pluginEntry = resolvePluginDesktopWidget(type, pluginRegistry)) {
      for (const auto& spec : settings::manifestSettingSpecs(pluginEntry->entry->settings)) {
        out.push_back(spec.schema);
      }
    } else {
      for (const auto& spec : desktopWidgetSettingSpecs(type)) {
        out.push_back(spec.schema);
      }
    }
    for (const auto& spec : commonDesktopWidgetSettingSpecs(type)) {
      out.push_back(spec.schema);
    }
    return out;
  }

  void applyDesktopWidgetDefaultSettings(
      std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view type,
      DesktopWidgetSettingsScope scope
  ) {
    const std::vector<WidgetSettingSpec> specs = scope == DesktopWidgetSettingsScope::Widget
        ? desktopWidgetSettingSpecs(type)
        : commonDesktopWidgetSettingSpecs(type);
    for (const auto& spec : specs) {
      settings.insert_or_assign(spec.schema.key, spec.schema.defaultValue);
    }
  }

  void applyAllDesktopWidgetDefaultSettings(
      std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view type
  ) {
    applyDesktopWidgetDefaultSettings(settings, type, DesktopWidgetSettingsScope::Widget);
    applyDesktopWidgetDefaultSettings(settings, type, DesktopWidgetSettingsScope::Background);
  }

} // namespace desktop_settings
