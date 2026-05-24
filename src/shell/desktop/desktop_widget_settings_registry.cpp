#include "shell/desktop/desktop_widget_settings_registry.h"

#include "util/string_utils.h"

namespace desktop_settings {
  namespace {

    using settings::WidgetSettingSelectOption;
    using settings::WidgetSettingSpec;
    using settings::WidgetSettingValueType;
    using settings::WidgetSettingVisibility;

    const std::vector<DesktopWidgetTypeSpec> kDesktopWidgetTypeSpecs = {
        {.type = "clock", .labelKey = "desktop-widgets.editor.types.clock"},
        {.type = "audio_visualizer", .labelKey = "desktop-widgets.editor.types.audio-visualizer"},
        {.type = "sticker", .labelKey = "desktop-widgets.editor.types.sticker"},
        {.type = "weather", .labelKey = "desktop-widgets.editor.types.weather"},
        {.type = "media_player", .labelKey = "desktop-widgets.editor.types.media-player"},
        {.type = "sysmon", .labelKey = "desktop-widgets.editor.types.system-monitor"},
    };

    WidgetSettingSpec baseSpec(std::string_view key, WidgetSettingValueType type, WidgetSettingValue defaultValue) {
      WidgetSettingSpec spec;
      spec.key = std::string(key);
      spec.labelKey = "desktop-widgets.editor.settings." + StringUtils::snakeToKebab(key);
      spec.valueType = type;
      spec.defaultValue = std::move(defaultValue);
      return spec;
    }

    WidgetSettingSpec boolSpec(std::string_view key, bool defaultValue) {
      return baseSpec(key, WidgetSettingValueType::Bool, defaultValue);
    }

    WidgetSettingSpec
    doubleSpec(std::string_view key, double defaultValue, double minValue, double maxValue, double step = 1.0) {
      auto spec = baseSpec(key, WidgetSettingValueType::Double, defaultValue);
      spec.minValue = minValue;
      spec.maxValue = maxValue;
      spec.step = step;
      return spec;
    }

    WidgetSettingSpec stringSpec(std::string_view key, std::string defaultValue = {}) {
      return baseSpec(key, WidgetSettingValueType::String, std::move(defaultValue));
    }

    WidgetSettingSpec colorSpec(std::string_view key, std::string defaultValue = {}) {
      return baseSpec(key, WidgetSettingValueType::ColorSpec, std::move(defaultValue));
    }

    WidgetSettingSpec
    selectSpec(std::string_view key, std::string defaultValue, std::vector<WidgetSettingSelectOption> options) {
      auto spec = baseSpec(key, WidgetSettingValueType::Select, std::move(defaultValue));
      spec.options = std::move(options);
      return spec;
    }

    WidgetSettingSpec
    segmentedSpec(std::string_view key, std::string defaultValue, std::vector<WidgetSettingSelectOption> options) {
      auto spec = selectSpec(key, std::move(defaultValue), std::move(options));
      spec.segmented = true;
      return spec;
    }

  } // namespace

  const std::vector<DesktopWidgetTypeSpec>& desktopWidgetTypeSpecs() { return kDesktopWidgetTypeSpecs; }

  std::vector<WidgetSettingSpec> commonDesktopWidgetSettingSpecs() {
    const WidgetSettingVisibility backgroundOn{"background", {"true"}};

    auto bgColor = colorSpec("background_color", "surface");
    bgColor.visibleWhen = backgroundOn;

    auto bgRadius = doubleSpec("background_radius", 12.0, 0.0, 32.0, 1.0);
    bgRadius.visibleWhen = backgroundOn;

    auto bgPadding = doubleSpec("background_padding", 10.0, 0.0, 32.0, 1.0);
    bgPadding.visibleWhen = backgroundOn;

    auto bgOpacity = doubleSpec("background_opacity", 0.8, 0.0, 1.0, 0.01);
    bgOpacity.visibleWhen = backgroundOn;

    return {
        boolSpec("background", true), std::move(bgColor),   std::move(bgOpacity),
        std::move(bgRadius),          std::move(bgPadding),
    };
  }

  std::vector<WidgetSettingSpec> desktopWidgetSettingSpecs(std::string_view type) {
    const std::vector<WidgetSettingSelectOption> sysmonStats = {
        {"cpu_usage", "desktop-widgets.editor.settings.stat-cpu-usage"},
        {"cpu_temp", "desktop-widgets.editor.settings.stat-cpu-temp"},
        {"gpu_temp", "desktop-widgets.editor.settings.stat-gpu-temp"},
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

    std::vector<WidgetSettingSpec> specs;
    auto add = [&](WidgetSettingSpec spec) { specs.push_back(std::move(spec)); };

    if (type == "clock") {
      add(stringSpec("format", "{:%H:%M}"));
      add(colorSpec("color", "on_surface"));
      add(boolSpec("shadow", true));
    } else if (type == "audio_visualizer") {
      add(doubleSpec("aspect_ratio", 2.5, 0.5, 6.0, 0.1));
      add(doubleSpec("bands", 32.0, 4.0, 128.0, 4.0));
      add(boolSpec("mirrored", true));
      add(boolSpec("centered", true));
      add(boolSpec("show_when_idle", true));
      add(colorSpec("low_color", "primary"));
      add(colorSpec("high_color", "primary"));
    } else if (type == "sticker") {
      add(stringSpec("image_path"));
      add(doubleSpec("opacity", 1.0, 0.0, 1.0, 0.01));
    } else if (type == "weather") {
      add(colorSpec("color", "on_surface"));
      add(boolSpec("shadow", true));
    } else if (type == "media_player") {
      add(segmentedSpec(
          "layout", "horizontal",
          {{"horizontal", "desktop-widgets.editor.settings.horizontal"},
           {"vertical", "desktop-widgets.editor.settings.vertical"}}
      ));
      add(colorSpec("color", "on_surface"));
      add(boolSpec("shadow", true));
    } else if (type == "sysmon") {
      add(selectSpec("stat", "cpu_usage", sysmonStats));
      add(selectSpec("stat2", "", sysmonStatsWithNone));
      add(colorSpec("color", "primary"));
      add(colorSpec("color2", "secondary"));
      add(boolSpec("show_label", true));
      add(boolSpec("shadow", true));
    }

    return specs;
  }

} // namespace desktop_settings
