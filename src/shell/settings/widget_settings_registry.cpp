#include "shell/settings/widget_settings_registry.h"

#include "i18n/i18n.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>
#include <unordered_set>
#include <utility>

namespace settings {
  namespace {

    using i18n::tr;

    const std::vector<WidgetTypeSpec> kWidgetTypeSpecs = {
        {.type = "active_window", .labelKey = "settings.widgets.types.active-window", .glyph = "app-window"},
        {.type = "audio_visualizer", .labelKey = "settings.widgets.types.audio-visualizer", .glyph = "wave-sine"},
        {.type = "battery", .labelKey = "settings.widgets.types.battery", .glyph = "battery-4"},
        {.type = "bluetooth", .labelKey = "settings.widgets.types.bluetooth", .glyph = "bluetooth"},
        {.type = "brightness", .labelKey = "settings.widgets.types.brightness", .glyph = "brightness-high"},
        {.type = "clock", .labelKey = "settings.widgets.types.clock", .glyph = "clock"},
        {.type = "control-center", .labelKey = "settings.widgets.types.control-center", .glyph = "noctalia"},
        {.type = "clipboard", .labelKey = "settings.widgets.types.clipboard", .glyph = "clipboard"},
        {.type = "custom_button", .labelKey = "settings.widgets.types.custom-button", .glyph = "circuit-pushbutton"},
        {.type = "caffeine", .labelKey = "settings.widgets.types.caffeine", .glyph = "caffeine-off"},
        {.type = "keyboard_layout", .labelKey = "settings.widgets.types.keyboard-layout", .glyph = "keyboard"},
        {.type = "launcher", .labelKey = "settings.widgets.types.launcher", .glyph = "search"},
        {.type = "lock_keys", .labelKey = "settings.widgets.types.lock-keys", .glyph = "lock"},
        {.type = "media", .labelKey = "settings.widgets.types.media", .glyph = "disc-filled"},
        {.type = "network", .labelKey = "settings.widgets.types.network", .glyph = "wifi-off"},
        {.type = "nightlight", .labelKey = "settings.widgets.types.nightlight", .glyph = "nightlight-off"},
        {.type = "notifications", .labelKey = "settings.widgets.types.notifications", .glyph = "bell"},
        {.type = "power_profile", .labelKey = "settings.widgets.types.power-profile", .glyph = "balanced"},
        {.type = "scripted", .labelKey = "settings.widgets.types.scripted", .glyph = "script"},
        {.type = "session", .labelKey = "settings.widgets.types.session", .glyph = "shutdown"},
        {.type = "settings", .labelKey = "settings.widgets.types.settings", .glyph = "settings"},
        {.type = "spacer", .labelKey = "settings.widgets.types.spacer", .glyph = "arrows-horizontal"},
        {.type = "sysmon", .labelKey = "settings.widgets.types.sysmon", .glyph = "cpu-usage"},
        {.type = "taskbar", .labelKey = "settings.widgets.types.taskbar", .glyph = "apps"},
        {.type = "test", .labelKey = "settings.widgets.types.test", .glyph = "flask", .visibleInPicker = false},
        {.type = "theme_mode", .labelKey = "settings.widgets.types.theme-mode", .glyph = "theme-mode"},
        {.type = "tray", .labelKey = "settings.widgets.types.tray", .glyph = "apps"},
        {.type = "volume", .labelKey = "settings.widgets.types.volume", .glyph = "volume-high"},
        {.type = "wallpaper", .labelKey = "settings.widgets.types.wallpaper", .glyph = "wallpaper-selector"},
        {.type = "weather", .labelKey = "settings.widgets.types.weather", .glyph = "weather-cloud"},
        {.type = "workspaces", .labelKey = "settings.widgets.types.workspaces", .glyph = "layout-grid"},
    };

    const WidgetTypeSpec* findWidgetTypeSpec(std::string_view type) {
      for (const auto& spec : kWidgetTypeSpecs) {
        if (spec.type == type) {
          return &spec;
        }
      }
      return nullptr;
    }

    std::string defaultWidgetGlyph(std::string_view type) {
      const auto* spec = findWidgetTypeSpec(type);
      if (spec != nullptr && !spec->glyph.empty()) {
        return std::string(spec->glyph);
      }
      return "apps";
    }

    std::string nonEmptyGlyph(std::string value, std::string_view fallback) {
      return value.empty() ? std::string(fallback) : std::move(value);
    }

    std::string widgetGlyph(std::string_view type, const WidgetConfig* config = nullptr) {
      if (config == nullptr) {
        return defaultWidgetGlyph(type);
      }
      if (type == "scripted") {
        if (const std::string script = config->getString("script", ""); !script.empty()) {
          if (auto manifest = scripting::manifestForScriptConfig(script);
              manifest.has_value() && !manifest->icon.empty()) {
            return manifest->icon;
          }
        }
        return defaultWidgetGlyph(type);
      }
      if (type == "clipboard") {
        return nonEmptyGlyph(config->getString("glyph", "clipboard"), "clipboard");
      }
      if (type == "control-center") {
        return nonEmptyGlyph(config->getString("glyph", "noctalia"), "search");
      }
      if (type == "custom_button") {
        return nonEmptyGlyph(config->getString("glyph", "heart"), "heart");
      }
      if (type == "launcher") {
        return nonEmptyGlyph(config->getString("glyph", "search"), "search");
      }
      if (type == "session") {
        return nonEmptyGlyph(config->getString("glyph", "shutdown"), "shutdown");
      }
      if (type == "settings") {
        return nonEmptyGlyph(config->getString("glyph", "settings"), "search");
      }
      if (type == "wallpaper") {
        return nonEmptyGlyph(config->getString("glyph", "wallpaper-selector"), "wallpaper-selector");
      }
      if (type == "sysmon") {
        const std::string stat = config->getString("stat", "cpu_usage");
        if (stat == "cpu_temp") {
          return "cpu-temperature";
        }
        if (stat == "gpu_temp") {
          return "temperature";
        }
        if (stat == "gpu_vram" || stat == "ram_used" || stat == "ram_pct") {
          return "memory";
        }
        if (stat == "swap_pct" || stat == "disk_pct") {
          return "storage";
        }
        if (stat == "net_rx") {
          return "download";
        }
        if (stat == "net_tx") {
          return "upload";
        }
      }
      if (type == "volume" && config->getString("device", "output") == "input") {
        return "microphone";
      }
      return defaultWidgetGlyph(type);
    }

    WidgetSettingSpec
    baseSpec(std::string_view key, WidgetSettingValueType type, WidgetSettingValue defaultValue, bool advanced) {
      WidgetSettingSpec spec;
      spec.key = std::string(key);
      spec.labelKey = std::string("settings.widgets.settings.") + std::string(key) + ".label";
      spec.descriptionKey = std::string("settings.widgets.settings.") + std::string(key) + ".description";
      spec.valueType = type;
      spec.defaultValue = std::move(defaultValue);
      spec.advanced = advanced;
      return spec;
    }

    WidgetSettingSpec boolSpec(std::string_view key, bool defaultValue, bool advanced = false) {
      return baseSpec(key, WidgetSettingValueType::Bool, defaultValue, advanced);
    }

    WidgetSettingSpec intSpec(
        std::string_view key, std::int64_t defaultValue, double minValue, double maxValue, double step = 1.0,
        bool advanced = false
    ) {
      auto spec = baseSpec(key, WidgetSettingValueType::Int, defaultValue, advanced);
      spec.minValue = minValue;
      spec.maxValue = maxValue;
      spec.step = step;
      return spec;
    }

    WidgetSettingSpec doubleSpec(
        std::string_view key, double defaultValue, double minValue, double maxValue, double step = 1.0,
        bool advanced = false
    ) {
      auto spec = baseSpec(key, WidgetSettingValueType::Double, defaultValue, advanced);
      spec.minValue = minValue;
      spec.maxValue = maxValue;
      spec.step = step;
      return spec;
    }

    WidgetSettingSpec
    optionalDoubleSpec(std::string_view key, double minValue, double maxValue, bool advanced = false) {
      auto spec = baseSpec(key, WidgetSettingValueType::OptionalDouble, 0.0, advanced);
      spec.minValue = minValue;
      spec.maxValue = maxValue;
      return spec;
    }

    WidgetSettingSpec stringSpec(std::string_view key, std::string defaultValue = {}, bool advanced = false) {
      return baseSpec(key, WidgetSettingValueType::String, std::move(defaultValue), advanced);
    }

    WidgetSettingSpec colorSpec(std::string_view key, std::string defaultValue = {}, bool advanced = false) {
      return baseSpec(key, WidgetSettingValueType::ColorSpec, std::move(defaultValue), advanced);
    }

    WidgetSettingSpec
    stringListSpec(std::string_view key, std::vector<std::string> defaultValue = {}, bool advanced = false) {
      return baseSpec(key, WidgetSettingValueType::StringList, std::move(defaultValue), advanced);
    }

    WidgetSettingSpec selectSpec(
        std::string_view key, std::string defaultValue, std::vector<WidgetSettingSelectOption> options,
        bool advanced = false
    ) {
      auto spec = baseSpec(key, WidgetSettingValueType::Select, std::move(defaultValue), advanced);
      spec.options = std::move(options);
      return spec;
    }

    WidgetSettingSpec segmentedSpec(
        std::string_view key, std::string defaultValue, std::vector<WidgetSettingSelectOption> options,
        bool advanced = false
    ) {
      auto spec = selectSpec(key, std::move(defaultValue), std::move(options), advanced);
      spec.segmented = true;
      return spec;
    }

    std::string widgetInstanceDisplayLabel(std::string_view name) {
      if (name == "cpu") {
        return tr("settings.widgets.instances.cpu");
      }
      if (name == "temp") {
        return tr("settings.widgets.instances.temp");
      }
      if (name == "ram") {
        return tr("settings.widgets.instances.ram");
      }
      if (name == "date") {
        return tr("settings.widgets.instances.date");
      }
      if (name == "output_volume") {
        return tr("settings.widgets.instances.output-volume");
      }
      if (name == "input_volume") {
        return tr("settings.widgets.instances.input-volume");
      }
      if (name == "network_tx") {
        return tr("settings.widgets.instances.network-tx");
      }
      if (name == "network_rx") {
        return tr("settings.widgets.instances.network-rx");
      }
      return std::string(name);
    }

    void addPickerEntry(
        std::vector<WidgetPickerEntry>& entries, std::unordered_set<std::string>& seen, std::string value,
        std::string label, std::string description, std::string icon, WidgetReferenceKind kind
    ) {
      if (!seen.insert(value).second) {
        return;
      }
      entries.push_back(
          WidgetPickerEntry{
              .value = std::move(value),
              .label = std::move(label),
              .description = std::move(description),
              .icon = std::move(icon),
              .kind = kind,
          }
      );
    }

    void collectLaneUnknowns(
        const std::vector<std::string>& widgets, std::vector<WidgetPickerEntry>& entries,
        std::unordered_set<std::string>& seen, const Config& cfg
    ) {
      for (const auto& name : widgets) {
        if (isBuiltInWidgetType(name) || cfg.widgets.contains(name)) {
          continue;
        }
        addPickerEntry(entries, seen, name, name, name, "warning", WidgetReferenceKind::Unknown);
      }
    }

  } // namespace

  const std::vector<WidgetTypeSpec>& widgetTypeSpecs() { return kWidgetTypeSpecs; }

  bool isBuiltInWidgetType(std::string_view type) { return findWidgetTypeSpec(type) != nullptr; }

  bool widgetTypeRequiresNamedConfig(std::string_view type) {
    return type == "custom_button" || type == "scripted" || type == "spacer";
  }

  std::string widgetTypeForReference(const Config& cfg, std::string_view name) {
    if (const auto it = cfg.widgets.find(std::string(name)); it != cfg.widgets.end() && !it->second.type.empty()) {
      return it->second.type;
    }
    if (isBuiltInWidgetType(name)) {
      return std::string(name);
    }
    return {};
  }

  std::string titleFromWidgetKey(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    bool upperNext = true;
    for (const char c : key) {
      if (c == '_' || c == '-') {
        out.push_back(' ');
        upperNext = true;
      } else if (upperNext) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        upperNext = false;
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  WidgetReferenceInfo widgetReferenceInfo(const Config& cfg, std::string_view name) {
    if (const auto* spec = findWidgetTypeSpec(name)) {
      if (const auto it = cfg.widgets.find(std::string(name));
          it != cfg.widgets.end() && !it->second.type.empty() && it->second.type != name) {
        return WidgetReferenceInfo{
            .title = std::string(name),
            .detail = it->second.type,
            .badge = tr("settings.entities.widget.kinds.named"),
            .kind = WidgetReferenceKind::Named,
        };
      }
      return WidgetReferenceInfo{
          .title = tr(spec->labelKey),
          .detail = std::string(name),
          .badge = tr("settings.entities.widget.kinds.built-in"),
          .kind = WidgetReferenceKind::BuiltIn,
      };
    }

    if (const auto it = cfg.widgets.find(std::string(name)); it != cfg.widgets.end()) {
      std::string title = widgetInstanceDisplayLabel(name);
      if (it->second.type == "scripted") {
        if (const std::string script = it->second.getString("script", ""); !script.empty()) {
          if (auto manifest = scripting::manifestForScriptConfig(script);
              manifest.has_value() && !manifest->label.empty()) {
            title = manifest->label;
          }
        }
      }
      return WidgetReferenceInfo{
          .title = std::move(title),
          .detail = it->second.type.empty() ? tr("settings.entities.widget.detail.custom") : it->second.type,
          .badge = tr("settings.entities.widget.kinds.named"),
          .kind = WidgetReferenceKind::Named,
      };
    }

    return WidgetReferenceInfo{
        .title = std::string(name),
        .detail = std::string(name),
        .badge = tr("settings.entities.widget.kinds.unknown"),
        .kind = WidgetReferenceKind::Unknown,
    };
  }

  std::vector<WidgetPickerEntry> widgetPickerEntries(const Config& cfg) {
    std::vector<WidgetPickerEntry> entries;
    std::unordered_set<std::string> seen;

    for (const auto& spec : kWidgetTypeSpecs) {
      if (!spec.visibleInPicker) {
        continue;
      }
      const auto configIt = cfg.widgets.find(std::string(spec.type));
      const WidgetConfig* widgetConfig = configIt != cfg.widgets.end() ? &configIt->second : nullptr;
      addPickerEntry(
          entries, seen, std::string(spec.type), tr(spec.labelKey), std::string(spec.type),
          widgetGlyph(
              widgetConfig != nullptr && !widgetConfig->type.empty() ? widgetConfig->type : spec.type, widgetConfig
          ),
          WidgetReferenceKind::BuiltIn
      );
    }

    for (const auto& [name, widget] : cfg.widgets) {
      if (isBuiltInWidgetType(name)) {
        continue;
      }
      std::string label = widgetInstanceDisplayLabel(name);
      if (widget.type == "scripted") {
        if (const std::string script = widget.getString("script", ""); !script.empty()) {
          if (auto manifest = scripting::manifestForScriptConfig(script);
              manifest.has_value() && !manifest->label.empty()) {
            label = manifest->label;
          }
        }
      }
      addPickerEntry(
          entries, seen, name, label, widget.type.empty() ? tr("settings.entities.widget.detail.custom") : widget.type,
          widgetGlyph(widget.type, &widget), WidgetReferenceKind::Named
      );
    }

    // Bundled scripted widgets that declare a Lua manifest appear as one-click presets.
    for (auto& script : scripting::discoverBundledScriptedWidgets()) {
      if (!seen.insert(script.id).second) {
        continue;
      }
      entries.push_back(
          WidgetPickerEntry{
              .value = script.id,
              .label = script.manifest.label.empty() ? script.id : script.manifest.label,
              .description = script.manifest.description,
              .icon = script.manifest.icon.empty() ? "script" : script.manifest.icon,
              .script = script.assetScript,
              .kind = WidgetReferenceKind::Preset,
          }
      );
    }

    for (const auto& bar : cfg.bars) {
      collectLaneUnknowns(bar.startWidgets, entries, seen, cfg);
      collectLaneUnknowns(bar.centerWidgets, entries, seen, cfg);
      collectLaneUnknowns(bar.endWidgets, entries, seen, cfg);
      for (const auto& ovr : bar.monitorOverrides) {
        if (ovr.startWidgets.has_value()) {
          collectLaneUnknowns(*ovr.startWidgets, entries, seen, cfg);
        }
        if (ovr.centerWidgets.has_value()) {
          collectLaneUnknowns(*ovr.centerWidgets, entries, seen, cfg);
        }
        if (ovr.endWidgets.has_value()) {
          collectLaneUnknowns(*ovr.endWidgets, entries, seen, cfg);
        }
      }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
      if (a.label == b.label) {
        return a.value < b.value;
      }
      return a.label < b.label;
    });
    return entries;
  }

  std::vector<WidgetSettingSpec> commonWidgetSettingSpecs() {
    const WidgetSettingVisibility capsuleOn{"capsule", {"true"}};

    auto anchor = boolSpec("anchor", false, true);
    auto widgetColor = colorSpec("color", {}, true);

    auto capsuleToggle = boolSpec("capsule", false);
    auto capsuleGroup = stringSpec("capsule_group");
    capsuleGroup.visibleWhen = capsuleOn;

    auto capsuleFill = colorSpec("capsule_fill", "", true);
    capsuleFill.visibleWhen = capsuleOn;

    auto capsuleBorder = colorSpec("capsule_border", {}, true);
    capsuleBorder.visibleWhen = capsuleOn;

    auto capsuleForeground = colorSpec("capsule_foreground", {}, true);
    capsuleForeground.visibleWhen = capsuleOn;

    auto capsulePadding = doubleSpec("capsule_padding", static_cast<double>(Style::barCapsulePadding), 0.0, 48.0, 1.0);
    capsulePadding.visibleWhen = capsuleOn;
    auto capsuleRadius = optionalDoubleSpec("capsule_radius", 0.0, 80.0);
    capsuleRadius.visibleWhen = capsuleOn;
    auto capsuleOpacity = doubleSpec("capsule_opacity", 1.0, 0.0, 1.0, 0.01);
    capsuleOpacity.visibleWhen = capsuleOn;
    return {
        std::move(anchor),         std::move(widgetColor),    std::move(capsuleToggle), std::move(capsuleRadius),
        std::move(capsuleGroup),   std::move(capsuleFill),    std::move(capsuleBorder), std::move(capsuleForeground),
        std::move(capsulePadding), std::move(capsuleOpacity),
    };
  }

  std::vector<WidgetSettingSpec> widgetSettingSpecs(std::string_view type) {
    std::vector<WidgetSettingSpec> specs = commonWidgetSettingSpecs();

    auto add = [&](WidgetSettingSpec spec) { specs.push_back(std::move(spec)); };
    const std::vector<WidgetSettingSelectOption> shortFull = {
        {"short", "settings.widgets.options.short"},
        {"full", "settings.widgets.options.full"},
    };
    const std::vector<WidgetSettingSelectOption> sysmonStats = {
        {"cpu_usage", "settings.widgets.options.cpu-usage"},   {"cpu_temp", "settings.widgets.options.cpu-temp"},
        {"gpu_temp", "settings.widgets.options.gpu-temp"},     {"gpu_vram", "settings.widgets.options.gpu-vram"},
        {"ram_used", "settings.widgets.options.ram-used"},     {"ram_pct", "settings.widgets.options.ram-percent"},
        {"swap_pct", "settings.widgets.options.swap-percent"}, {"disk_pct", "settings.widgets.options.disk-percent"},
        {"net_rx", "settings.widgets.options.net-rx"},         {"net_tx", "settings.widgets.options.net-tx"},
    };
    const std::vector<WidgetSettingSelectOption> sysmonDisplay = {
        {"gauge", "settings.widgets.options.gauge"},
        {"graph", "settings.widgets.options.graph"},
        {"text", "settings.widgets.options.text"},
    };
    const std::vector<WidgetSettingSelectOption> workspaceDisplay = {
        {"id", "settings.widgets.options.id"},
        {"name", "settings.widgets.options.name"},
        {"none", "settings.widgets.options.none"},
    };
    const std::vector<WidgetSettingSelectOption> workspaceLabelPlacement = {
        {"corner", "settings.widgets.options.workspace-label-corner"},
        {"centered", "settings.widgets.options.workspace-label-centered"},
        {"inside", "settings.widgets.options.workspace-label-inside"},
    };
    const std::vector<WidgetSettingSelectOption> mediaTitleScroll = {
        {"none", "settings.widgets.options.none"},
        {"always", "settings.widgets.options.always"},
        {"on_hover", "settings.widgets.options.on-hover"},
    };
    const std::vector<WidgetSettingSelectOption> activeWindowDisplay = {
        {"icon_and_text", "settings.widgets.options.icon-and-text"},
        {"icon_only", "settings.widgets.options.icon-only"},
        {"text_only", "settings.widgets.options.text-only"},
    };
    const std::vector<WidgetSettingSelectOption> scriptedScopes = {
        {"instance", "settings.widgets.options.instance"},
        {"shared", "settings.widgets.options.shared"},
    };
    const std::vector<WidgetSettingSelectOption> volumeDeviceOptions = {
        {"output", "settings.widgets.options.output"},
        {"input", "settings.widgets.options.input"},
    };
    if (type == "active_window") {
      add(doubleSpec("min_length", 80.0, 0.0, 800.0, 1.0));
      add(doubleSpec("max_length", 260.0, 40.0, 800.0, 1.0));
      add(doubleSpec("icon_size", static_cast<double>(Style::fontSizeBody), 8.0, 64.0, 1.0));
      add(selectSpec("title_scroll", "none", mediaTitleScroll));
      {
        auto display = selectSpec("display", "icon_and_text", activeWindowDisplay);
        display.descriptionKey = "settings.widgets.settings.display.active-window-description";
        add(std::move(display));
      }
    } else if (type == "audio_visualizer") {
      add(doubleSpec("width", 56.0, 8.0, 400.0, 1.0));
      add(intSpec("bands", 16, 2.0, 128.0, 1.0));
      add(boolSpec("mirrored", true));
      add(boolSpec("centered", true));
      add(boolSpec("show_when_idle", false));
      {
        auto low = colorSpec("low_color", "primary");
        add(std::move(low));
      }
      {
        auto high = colorSpec("high_color", "primary");
        add(std::move(high));
      }
    } else if (type == "battery") {
      add(selectSpec(
          "display_mode", "icon",
          {{"icon", "settings.widgets.options.icon"}, {"graphic", "settings.widgets.options.graphic"}}
      ));
      add(boolSpec("show_label", true));
      add(boolSpec("hide_when_plugged", false));
      add(boolSpec("hide_when_full", false));
      add(selectSpec("device", "auto", {{"auto", "common.states.auto"}}));
      add(intSpec("warning_threshold", 20, 0.0, 100.0, 1.0));
      {
        auto warn = colorSpec("warning_color", "error");
        add(std::move(warn));
      }
    } else if (type == "bluetooth") {
      add(boolSpec("show_label", false));
      add(boolSpec("hide_when_no_connected_device", false));
    } else if (type == "brightness") {
      add(boolSpec("show_label", true));
    } else if (type == "clock") {
      add(stringSpec("format", "{:%H:%M}"));
      add(stringSpec("vertical_format"));
    } else if (type == "clipboard") {
      add(stringSpec("glyph", "clipboard"));
    } else if (type == "keyboard_layout") {
      add(stringSpec("cycle_command"));
      add(boolSpec("hide_label", false));
      {
        auto display = segmentedSpec("display", "short", shortFull);
        display.visibleWhen = WidgetSettingVisibility{"hide_label", {"false"}};
        add(std::move(display));
      }
    } else if (type == "launcher") {
      add(stringSpec("glyph", "search"));
      add(stringSpec("custom_image", ""));
    } else if (type == "control-center") {
      add(stringSpec("glyph", "noctalia"));
      add(stringSpec("custom_image", ""));
    } else if (type == "custom_button") {
      add(stringSpec("glyph", "heart"));
      add(stringSpec("label"));
      add(stringSpec("tooltip"));
      add(stringSpec("image_path", ""));
      add(boolSpec("image_auto_reload", false));
      add(stringSpec("command"));
      add(stringSpec("right_command"));
      add(stringSpec("middle_command"));
      add(stringSpec("scroll_up_command"));
      add(stringSpec("scroll_down_command"));
    } else if (type == "lock_keys") {
      add(boolSpec("show_caps_lock", true));
      add(boolSpec("show_num_lock", true));
      add(boolSpec("show_scroll_lock", false));
      add(boolSpec("hide_when_off", false));
      add(segmentedSpec("display", "short", shortFull));
    } else if (type == "media") {
      add(doubleSpec("min_length", 80.0, 0.0, 800.0, 1.0));
      add(doubleSpec("max_length", 220.0, 40.0, 800.0, 1.0));
      add(doubleSpec("art_size", 16.0, 8.0, 96.0, 1.0));
      add(selectSpec("title_scroll", "none", mediaTitleScroll));
      add(boolSpec("hide_when_no_media", false));
    } else if (type == "network") {
      add(boolSpec("show_label", true));
    } else if (type == "notifications") {
      add(boolSpec("hide_when_no_unread", false));
    } else if (type == "scripted") {
      add(stringSpec("script"));
      add(selectSpec("scope", "instance", scriptedScopes));
      add(boolSpec("hot_reload", false, true));
    } else if (type == "session") {
      add(stringSpec("glyph", "shutdown"));
    } else if (type == "settings") {
      add(stringSpec("glyph", "settings"));
    } else if (type == "spacer") {
      add(doubleSpec("length", 20.0, 0.0, 400.0, 1.0));
    } else if (type == "sysmon") {
      add(selectSpec("stat", "cpu_usage", sysmonStats));
      {
        auto path = stringSpec("path", "/");
        path.visibleWhen = WidgetSettingVisibility{"stat", {"disk_pct"}};
        add(std::move(path));
      }
      add(segmentedSpec("display", "gauge", sysmonDisplay));
      add(boolSpec("show_label", true));
      {
        auto minW = doubleSpec("label_min_width", 0.0, 0.0, 200.0, 1.0);
        minW.visibleWhen = WidgetSettingVisibility{"show_label", {"true"}};
        add(std::move(minW));
      }
    } else if (type == "taskbar") {
      add(boolSpec("group_by_workspace", false));
      add(boolSpec("show_all_outputs", false));
      add(boolSpec("only_active_workspace", false));
      {
        auto showWsLabel = boolSpec("show_workspace_label", true);
        showWsLabel.visibleWhen =
            WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
        add(std::move(showWsLabel));
      }
      {
        auto labelPlacement = selectSpec("workspace_label_placement", "corner", workspaceLabelPlacement);
        labelPlacement.visibleWhen =
            WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
        add(std::move(labelPlacement));
      }
      {
        auto hideEmpty = boolSpec("hide_empty_workspaces", false);
        hideEmpty.visibleWhen =
            WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
        add(std::move(hideEmpty));
      }
      {
        auto groupCapsule = boolSpec("workspace_group_capsule", true);
        groupCapsule.descriptionKey = "settings.widgets.settings.workspace_group_capsule.description";
        groupCapsule.visibleWhen =
            WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
        add(std::move(groupCapsule));
      }
      const WidgetSettingVisibility groupedWorkspaceSettings{
          WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}
      };
      {
        auto focusedColor = colorSpec("focused_color", "primary");
        focusedColor.visibleWhen = groupedWorkspaceSettings;
        add(std::move(focusedColor));
      }
      {
        auto occupiedColor = colorSpec("occupied_color", "secondary");
        occupiedColor.visibleWhen = groupedWorkspaceSettings;
        add(std::move(occupiedColor));
      }
      {
        auto emptyColor = colorSpec("empty_color", "secondary");
        emptyColor.visibleWhen = groupedWorkspaceSettings;
        add(std::move(emptyColor));
      }
      for (auto& spec : specs) {
        if (spec.key == "capsule_radius") {
          spec.descriptionKey = "settings.widgets.settings.capsule_radius.taskbar-description";
          spec.visibleWhen = WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
          break;
        }
      }
    } else if (type == "tray") {
      add(stringListSpec("hidden"));
      add(stringListSpec("pinned"));
      add(boolSpec("match_adjacent_spacing", false));
      add(boolSpec("drawer", false));
      {
        auto cols = intSpec("drawer_columns", 3, 1.0, 5.0, 1.0);
        cols.visibleWhen = WidgetSettingVisibility{"drawer", {"true"}};
        add(std::move(cols));
      }
    } else if (type == "volume") {
      add(segmentedSpec("device", "output", volumeDeviceOptions));
      add(boolSpec("show_label", true));
    } else if (type == "wallpaper") {
      add(stringSpec("glyph", "wallpaper-selector"));
    } else if (type == "weather") {
      add(doubleSpec("max_length", 160.0, 40.0, 800.0, 1.0));
      add(boolSpec("show_condition", true));
    } else if (type == "workspaces") {
      const WidgetSettingVisibility pillStyleOnly{{"minimal", {"false"}}};
      for (auto& spec : specs) {
        if (spec.key == "capsule_radius") {
          spec.descriptionKey = "settings.widgets.settings.capsule_radius.workspaces-description";
          spec.visibleWhen = pillStyleOnly;
          break;
        }
      }
      {
        auto minimal = boolSpec("minimal", false);
        minimal.descriptionKey = "settings.widgets.settings.minimal.workspaces-description";
        add(std::move(minimal));
      }
      add(segmentedSpec("display", "id", workspaceDisplay));
      {
        auto hideWhenEmpty = boolSpec("hide_when_empty", false);
        hideWhenEmpty.descriptionKey = "settings.widgets.settings.hide_when_empty.workspaces-description";
        add(std::move(hideWhenEmpty));
      }
      {
        auto maxLabelChars = intSpec("max_label_chars", 1, 1.0, 20.0, 1.0);
        maxLabelChars.descriptionKey = "settings.widgets.settings.max_label_chars.workspaces-description";
        add(std::move(maxLabelChars));
      }
      {
        auto pillScale = doubleSpec("pill_scale", 1.0, 0.1, 1.0, 0.05);
        pillScale.descriptionKey = "settings.widgets.settings.pill_scale.workspaces-description";
        pillScale.visibleWhen = pillStyleOnly;
        add(std::move(pillScale));
      }
      {
        auto focusedColor = colorSpec("focused_color", "primary");
        add(std::move(focusedColor));
      }
      {
        auto occupiedColor = colorSpec("occupied_color", "secondary");
        add(std::move(occupiedColor));
      }
      {
        auto emptyColor = colorSpec("empty_color", "secondary");
        add(std::move(emptyColor));
      }
    }

    return specs;
  }

  std::vector<WidgetSettingSpec> manifestSettingSpecs(const scripting::ScriptWidgetManifest& manifest) {
    std::vector<WidgetSettingSpec> specs;
    specs.reserve(manifest.settings.size());
    for (const auto& field : manifest.settings) {
      WidgetSettingSpec spec;
      spec.key = field.key;
      spec.literalLabel = field.label.empty() ? field.key : field.label;
      spec.literalDescription = field.description;
      spec.advanced = field.advanced;
      spec.minValue = field.minValue;
      spec.maxValue = field.maxValue;
      spec.step = field.step;

      switch (field.type) {
      case scripting::ManifestFieldType::Bool:
        spec.valueType = WidgetSettingValueType::Bool;
        spec.defaultValue = field.boolDefault;
        break;
      case scripting::ManifestFieldType::Int:
        spec.valueType = WidgetSettingValueType::Int;
        spec.defaultValue = static_cast<std::int64_t>(field.numberDefault);
        break;
      case scripting::ManifestFieldType::Double:
        spec.valueType = WidgetSettingValueType::Double;
        spec.defaultValue = field.numberDefault;
        break;
      case scripting::ManifestFieldType::Select:
        spec.valueType = WidgetSettingValueType::Select;
        spec.defaultValue = field.stringDefault;
        spec.literalLabels = true;
        for (const auto& opt : field.options) {
          spec.options.push_back(WidgetSettingSelectOption{.value = opt.value, .labelKey = opt.label});
        }
        break;
      case scripting::ManifestFieldType::Color:
        spec.valueType = WidgetSettingValueType::ColorSpec;
        spec.defaultValue = field.stringDefault;
        break;
      case scripting::ManifestFieldType::String:
      default:
        spec.valueType = WidgetSettingValueType::String;
        spec.defaultValue = field.stringDefault;
        break;
      }

      if (field.visibleWhen.has_value()) {
        spec.visibleWhen = WidgetSettingVisibility{field.visibleWhen->key, field.visibleWhen->values};
      }
      specs.push_back(std::move(spec));
    }
    return specs;
  }

  std::vector<WidgetSettingSpec> widgetSettingSpecs(std::string_view type, const WidgetConfig* config) {
    if (type == "scripted" && config != nullptr) {
      const std::string script = config->getString("script", "");
      if (!script.empty()) {
        if (auto manifest = scripting::manifestForScriptConfig(script); manifest.has_value()) {
          std::vector<WidgetSettingSpec> specs = commonWidgetSettingSpecs();
          auto fromManifest = manifestSettingSpecs(*manifest);
          specs.insert(
              specs.end(), std::make_move_iterator(fromManifest.begin()), std::make_move_iterator(fromManifest.end())
          );
          // Power users keep the raw scripted knobs, tucked under "advanced".
          const std::vector<WidgetSettingSelectOption> scriptedScopes = {
              {.value = "instance", .labelKey = "settings.widgets.options.instance"},
              {.value = "shared", .labelKey = "settings.widgets.options.shared"},
          };
          specs.push_back(selectSpec("scope", "instance", scriptedScopes, true));
          specs.push_back(boolSpec("hot_reload", false, true));
          return specs;
        }
      }
    }
    return widgetSettingSpecs(type);
  }

  namespace {

    bool widgetSettingValuesEqual(const WidgetSettingValue& a, const WidgetSettingValue& b) {
      const auto numericValue = [](const WidgetSettingValue& value) -> std::optional<double> {
        if (const auto* i = std::get_if<std::int64_t>(&value)) {
          return static_cast<double>(*i);
        }
        if (const auto* d = std::get_if<double>(&value)) {
          return *d;
        }
        return std::nullopt;
      };

      const auto aNum = numericValue(a);
      const auto bNum = numericValue(b);
      if (aNum.has_value() || bNum.has_value()) {
        return aNum.has_value() && bNum.has_value() && std::abs(*aNum - *bNum) <= 1.0e-5;
      }
      if (a.index() != b.index()) {
        return false;
      }
      return std::visit(
          [&](const auto& lhs) {
            using T = std::decay_t<decltype(lhs)>;
            const auto* rhs = std::get_if<T>(&b);
            return rhs != nullptr && lhs == *rhs;
          },
          a
      );
    }

  } // namespace

  std::optional<WidgetSettingSpec> findWidgetSettingSpec(std::string_view widgetType, std::string_view settingKey) {
    const std::string key(settingKey);
    for (const auto& spec : widgetSettingSpecs(widgetType)) {
      if (spec.key == key) {
        return spec;
      }
    }
    return std::nullopt;
  }

  bool configOverrideValueMatchesWidgetSetting(
      const ConfigOverrideValue& overrideValue, const WidgetSettingValue& settingValue
  ) {
    const auto matchesBool = [&](bool value) {
      if (const auto* settingBool = std::get_if<bool>(&settingValue)) {
        return value == *settingBool;
      }
      return false;
    };
    const auto matchesInt = [&](std::int64_t value) {
      if (const auto* settingInt = std::get_if<std::int64_t>(&settingValue)) {
        return value == *settingInt;
      }
      if (const auto* settingDouble = std::get_if<double>(&settingValue)) {
        return std::abs(static_cast<double>(value) - *settingDouble) <= 1.0e-5;
      }
      return false;
    };
    const auto matchesDouble = [&](double value) {
      if (const auto* settingDouble = std::get_if<double>(&settingValue)) {
        return std::abs(value - *settingDouble) <= 1.0e-5;
      }
      if (const auto* settingInt = std::get_if<std::int64_t>(&settingValue)) {
        return std::abs(value - static_cast<double>(*settingInt)) <= 1.0e-5;
      }
      return false;
    };
    const auto matchesString = [&](const std::string& value) {
      if (const auto* settingString = std::get_if<std::string>(&settingValue)) {
        return value == *settingString;
      }
      return false;
    };
    const auto matchesStringList = [&](const std::vector<std::string>& value) {
      if (const auto* settingList = std::get_if<std::vector<std::string>>(&settingValue)) {
        return value == *settingList;
      }
      return false;
    };

    return std::visit(
        [&](const auto& value) -> bool {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, bool>) {
            return matchesBool(value);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return matchesInt(value);
          } else if constexpr (std::is_same_v<T, double>) {
            return matchesDouble(value);
          } else if constexpr (std::is_same_v<T, std::string>) {
            return matchesString(value);
          } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            return matchesStringList(value);
          }
          return false;
        },
        overrideValue
    );
  }

  bool widgetOverrideValueMatchesRegistryDefault(
      std::string_view widgetType, std::string_view settingKey, const ConfigOverrideValue& overrideValue
  ) {
    const auto spec = findWidgetSettingSpec(widgetType, settingKey);
    if (!spec.has_value()) {
      return false;
    }
    // OptionalDouble unset means inherit/auto, 0 is a valid explicit radius and must persist.
    if (spec->valueType == WidgetSettingValueType::OptionalDouble) {
      return false;
    }
    return configOverrideValueMatchesWidgetSetting(overrideValue, spec->defaultValue);
  }

  bool widgetSettingOverrideIsEffective(
      std::string_view widgetName, std::string_view settingKey, const Config& withOverride,
      const Config& withoutOverride
  ) {
    const auto valueInConfig = [](const Config& cfg, std::string_view name,
                                  std::string_view key) -> std::optional<WidgetSettingValue> {
      const auto widgetIt = cfg.widgets.find(std::string(name));
      if (widgetIt == cfg.widgets.end()) {
        return std::nullopt;
      }
      const auto settingIt = widgetIt->second.settings.find(std::string(key));
      if (settingIt == widgetIt->second.settings.end()) {
        return std::nullopt;
      }
      return settingIt->second;
    };

    std::string widgetType(widgetName);
    if (const auto withIt = withOverride.widgets.find(std::string(widgetName)); withIt != withOverride.widgets.end()) {
      widgetType = withIt->second.type;
    } else if (
        const auto withoutIt = withoutOverride.widgets.find(std::string(widgetName));
        withoutIt != withoutOverride.widgets.end()
    ) {
      widgetType = withoutIt->second.type;
    }

    const auto spec = findWidgetSettingSpec(widgetType, settingKey);
    const auto withValue = valueInConfig(withOverride, widgetName, settingKey);
    const auto withoutValue = valueInConfig(withoutOverride, widgetName, settingKey);
    if (!withValue.has_value() && !withoutValue.has_value()) {
      return false;
    }
    if (!withValue.has_value() || !withoutValue.has_value()) {
      return true;
    }
    if (spec.has_value() && spec->valueType == WidgetSettingValueType::OptionalDouble) {
      return !widgetSettingValuesEqual(*withValue, *withoutValue);
    }
    if (!spec.has_value()) {
      return !widgetSettingValuesEqual(*withValue, *withoutValue);
    }

    const WidgetSettingValue defaultValue = spec->defaultValue;
    const auto resolvedValue = [&](const Config& cfg) -> WidgetSettingValue {
      if (const auto value = valueInConfig(cfg, widgetName, settingKey); value.has_value()) {
        return *value;
      }
      return defaultValue;
    };

    return !widgetSettingValuesEqual(resolvedValue(withOverride), resolvedValue(withoutOverride));
  }

} // namespace settings
