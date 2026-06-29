#include "shell/settings/widget_settings_registry.h"

#include "i18n/i18n.h"
#include "scripting/plugin_i18n.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/font_family_catalog.h"
#include "shell/settings/font_weight_catalog.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>
#include <unordered_set>
#include <utility>

namespace settings {
  namespace schema = noctalia::config::schema;

  // File/Folder/Glyph carry a String value; Select an Enum; ColorSpec a Color; the rest map 1:1.
  schema::WidgetSettingType schemaTypeForControl(WidgetControlKind control) {
    switch (control) {
    case WidgetControlKind::Bool:
      return schema::WidgetSettingType::Bool;
    case WidgetControlKind::Int:
      return schema::WidgetSettingType::Int;
    case WidgetControlKind::Double:
      return schema::WidgetSettingType::Double;
    case WidgetControlKind::OptionalDouble:
      return schema::WidgetSettingType::OptionalDouble;
    case WidgetControlKind::String:
    case WidgetControlKind::File:
    case WidgetControlKind::Folder:
    case WidgetControlKind::Glyph:
      return schema::WidgetSettingType::String;
    case WidgetControlKind::StringList:
      return schema::WidgetSettingType::StringList;
    case WidgetControlKind::StringMap:
      return schema::WidgetSettingType::StringMap;
    case WidgetControlKind::Select:
      return schema::WidgetSettingType::Enum;
    case WidgetControlKind::ColorSpec:
      return schema::WidgetSettingType::Color;
    }
    return schema::WidgetSettingType::String;
  }

  namespace {

    using i18n::tr;

    std::optional<scripting::ResolvedPluginEntry> resolvePluginWidget(std::string_view type);

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
        {.type = "privacy", .labelKey = "settings.widgets.types.privacy", .glyph = "shield-lock"},
        {.type = "screenshot", .labelKey = "settings.widgets.types.screenshot", .glyph = "screenshot"},
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
      if (auto pw = resolvePluginWidget(type)) {
        return pw->manifest->icon.empty() ? std::string("apps") : pw->manifest->icon;
      }
      if (config == nullptr) {
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
      if (type == "keyboard_layout") {
        return nonEmptyGlyph(config->getString("glyph", "keyboard"), "keyboard");
      }
      if (type == "sysmon") {
        if (const std::string custom = config->getString("glyph", ""); !custom.empty()) {
          return custom;
        }
        const std::string stat = config->getString("stat", "cpu_usage");
        if (stat == "cpu_temp") {
          return "cpu-temperature";
        }
        if (stat == "gpu_temp") {
          return "temperature";
        }
        if (stat == "gpu_usage") {
          return "gpu-usage";
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

    // Resolve a widget `type` to a plugin [[widget]] entry, or nullopt for built-ins.
    std::optional<scripting::ResolvedPluginEntry> resolvePluginWidget(std::string_view type) {
      scripting::PluginRegistry::instance().ensureScanned();
      auto entry = scripting::PluginRegistry::instance().resolve(type);
      if (entry.has_value() && entry->entry->kind == scripting::PluginEntryKind::Widget) {
        return entry;
      }
      return std::nullopt;
    }

    std::string appendVersion(std::string text, std::string_view version) {
      if (version.empty()) {
        return text;
      }
      const std::string versionText = "version " + std::string(version);
      if (text.empty()) {
        return versionText;
      }
      text += " (";
      text += versionText;
      text += ")";
      return text;
    }

    WidgetSettingSpec
    baseSpec(std::string_view key, WidgetControlKind control, WidgetSettingValue defaultValue, bool advanced) {
      WidgetSettingSpec spec;
      spec.schema.key = std::string(key);
      spec.schema.type = schemaTypeForControl(control);
      spec.schema.defaultValue = std::move(defaultValue);
      spec.control = control;
      spec.labelKey = std::string("settings.widgets.settings.") + i18n::keySegment(key) + ".label";
      spec.descriptionKey = std::string("settings.widgets.settings.") + i18n::keySegment(key) + ".description";
      spec.advanced = advanced;
      return spec;
    }

    WidgetSettingSpec withGroup(WidgetSettingSpec spec, WidgetSettingGroup group) {
      spec.group = group;
      return spec;
    }

    WidgetSettingSpec boolSpec(std::string_view key, bool defaultValue, bool advanced = false) {
      return baseSpec(key, WidgetControlKind::Bool, defaultValue, advanced);
    }

    WidgetSettingSpec intSpec(
        std::string_view key, std::int64_t defaultValue, double minValue, double maxValue, double step = 1.0,
        bool advanced = false
    ) {
      auto spec = baseSpec(key, WidgetControlKind::Int, defaultValue, advanced);
      spec.schema.minValue = minValue;
      spec.schema.maxValue = maxValue;
      spec.schema.step = step;
      return spec;
    }

    WidgetSettingSpec stepperIntSpec(
        std::string_view key, std::int64_t defaultValue, double minValue, double maxValue, double step = 1.0,
        std::string valueSuffix = {}, bool advanced = false
    ) {
      auto spec = intSpec(key, defaultValue, minValue, maxValue, step, advanced);
      spec.stepper = true;
      spec.valueSuffix = std::move(valueSuffix);
      return spec;
    }

    WidgetSettingSpec doubleSpec(
        std::string_view key, double defaultValue, double minValue, double maxValue, double step = 1.0,
        bool advanced = false
    ) {
      auto spec = baseSpec(key, WidgetControlKind::Double, defaultValue, advanced);
      spec.schema.minValue = minValue;
      spec.schema.maxValue = maxValue;
      spec.schema.step = step;
      return spec;
    }

    WidgetSettingSpec
    optionalDoubleSpec(std::string_view key, double minValue, double maxValue, bool advanced = false) {
      auto spec = baseSpec(key, WidgetControlKind::OptionalDouble, 0.0, advanced);
      spec.schema.minValue = minValue;
      spec.schema.maxValue = maxValue;
      return spec;
    }

    WidgetSettingSpec stringSpec(std::string_view key, std::string defaultValue = {}, bool advanced = false) {
      return baseSpec(key, WidgetControlKind::String, std::move(defaultValue), advanced);
    }

    WidgetSettingSpec glyphSpec(std::string_view key, std::string defaultValue = {}, bool advanced = false) {
      return baseSpec(key, WidgetControlKind::Glyph, std::move(defaultValue), advanced);
    }

    WidgetSettingSpec colorSpec(std::string_view key, std::string defaultValue = {}, bool advanced = false) {
      return baseSpec(key, WidgetControlKind::ColorSpec, std::move(defaultValue), advanced);
    }

    WidgetSettingSpec
    stringListSpec(std::string_view key, std::vector<std::string> defaultValue = {}, bool advanced = false) {
      return baseSpec(key, WidgetControlKind::StringList, std::move(defaultValue), advanced);
    }

    WidgetSettingSpec stringMapSpec(std::string_view key, bool advanced = false) {
      return baseSpec(key, WidgetControlKind::StringMap, std::string{}, advanced);
    }

    WidgetSettingSpec selectSpec(
        std::string_view key, std::string defaultValue, std::vector<WidgetSettingSelectOption> options,
        bool advanced = false
    ) {
      auto spec = baseSpec(key, WidgetControlKind::Select, std::move(defaultValue), advanced);
      for (const auto& option : options) {
        spec.schema.enumValues.push_back(option.value);
      }
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

  bool isPluginWidgetType(std::string_view type) { return resolvePluginWidget(type).has_value(); }

  bool widgetTypeRequiresNamedConfig(std::string_view type) {
    return type == "custom_button" || type == "spacer" || resolvePluginWidget(type).has_value();
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

  WidgetReferenceInfo widgetReferenceInfo(const Config& cfg, std::string_view name, bool includeManifestVersion) {
    if (const auto* spec = findWidgetTypeSpec(name)) {
      if (const auto it = cfg.widgets.find(std::string(name));
          it != cfg.widgets.end() && !it->second.type.empty() && it->second.type != name) {
        return WidgetReferenceInfo{
            .title = std::string(name),
            .detail = it->second.type,
            .kind = WidgetReferenceKind::Named,
        };
      }
      return WidgetReferenceInfo{
          .title = tr(spec->labelKey),
          .detail = std::string(name),
          .kind = WidgetReferenceKind::BuiltIn,
      };
    }

    if (const auto it = cfg.widgets.find(std::string(name)); it != cfg.widgets.end()) {
      std::string title = widgetInstanceDisplayLabel(name);
      std::string detail = it->second.type.empty() ? tr("settings.entities.widget.detail.custom") : it->second.type;
      if (auto pw = resolvePluginWidget(it->second.type)) {
        if (!pw->manifest->name.empty()) {
          title = pw->manifest->name;
        }
        if (includeManifestVersion) {
          detail = appendVersion(std::move(detail), pw->manifest->version);
        }
      }
      return WidgetReferenceInfo{
          .title = std::move(title),
          .detail = std::move(detail),
          .kind = WidgetReferenceKind::Named,
      };
    }

    return WidgetReferenceInfo{
        .title = widgetInstanceDisplayLabel(name),
        .detail = std::string(name),
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
      // Only surface named instances of built-in multi-instance types (custom_button, spacer).
      // Plugin-typed instances are already represented by their registry [[widget]] entry below,
      // and stale/invalid types (e.g. "scripted") are surfaced loudly by config_validate.
      if (!widget.type.empty() && !isBuiltInWidgetType(widget.type)) {
        continue;
      }
      std::string label = widgetInstanceDisplayLabel(name);
      std::string description = widget.type.empty() ? tr("settings.entities.widget.detail.custom") : widget.type;
      addPickerEntry(
          entries, seen, name, label, std::move(description), widgetGlyph(widget.type, &widget),
          WidgetReferenceKind::Named
      );
    }

    // Plugin [[widget]] entries appear as one-click adds (value = entry id).
    scripting::PluginRegistry::instance().ensureScanned();
    for (const auto& entry : scripting::PluginRegistry::instance().entriesOfKind(scripting::PluginEntryKind::Widget)) {
      const std::string entryId = entry.fullId();
      if (!seen.insert(entryId).second) {
        continue;
      }
      std::string label = entry.manifest->name.empty() ? entryId : entry.manifest->name;
      std::string description = appendVersion(entry.manifest->description, entry.manifest->version);
      entries.push_back(
          WidgetPickerEntry{
              .value = entryId,
              .label = std::move(label),
              .description = std::move(description),
              .icon = entry.manifest->icon.empty() ? "apps" : entry.manifest->icon,
              .kind = WidgetReferenceKind::Plugin,
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

    std::ranges::sort(entries, [](const auto& a, const auto& b) {
      if (a.label == b.label) {
        return a.value < b.value;
      }
      return a.label < b.label;
    });
    return entries;
  }

  std::vector<WidgetSettingSpec> commonWidgetSettingSpecs(std::string_view shellFontFamily) {
    const WidgetSettingVisibility capsuleOn{"capsule", {"true"}};

    auto anchor = withGroup(boolSpec("anchor", false, true), WidgetSettingGroup::Presentation);
    auto scale = withGroup(doubleSpec("scale", 1.0, 0.2, 2.5, 0.05), WidgetSettingGroup::Presentation);
    auto widgetColor = withGroup(colorSpec("color", {}, true), WidgetSettingGroup::Presentation);
    auto widgetIconColor = withGroup(colorSpec("icon_color", {}, true), WidgetSettingGroup::Presentation);
    auto fontWeightOptions =
        buildLabelFontWeightSelectOptions(shellFontFamily, FontWeightSelectKind::WidgetInheritDefault);
    auto fontWeight =
        withGroup(selectSpec("font_weight", "", std::move(fontWeightOptions), true), WidgetSettingGroup::Presentation);
    fontWeight.integerValue = true;

    // Font picker rendered as a filterable search picker but validated as a free string: a font configured
    // elsewhere but absent here must still load. Empty value = inherit the bar/shell font.
    auto fontFamily = baseSpec("font_family", WidgetControlKind::Select, std::string{}, true);
    fontFamily.schema.type = schema::WidgetSettingType::String;
    fontFamily.options = buildFontFamilySelectOptions();
    fontFamily.literalLabels = true;
    fontFamily = withGroup(std::move(fontFamily), WidgetSettingGroup::Presentation);

    auto capsuleToggle = withGroup(boolSpec("capsule", false), WidgetSettingGroup::Presentation);
    auto capsuleFill = withGroup(colorSpec("capsule_fill", "", true), WidgetSettingGroup::Presentation);
    capsuleFill.visibleWhen = capsuleOn;

    auto capsuleBorder = withGroup(colorSpec("capsule_border", {}, true), WidgetSettingGroup::Presentation);
    capsuleBorder.visibleWhen = capsuleOn;

    auto capsuleForeground = withGroup(colorSpec("capsule_foreground", {}, true), WidgetSettingGroup::Presentation);
    capsuleForeground.visibleWhen = capsuleOn;

    auto capsulePadding = withGroup(
        intSpec("capsule_padding", static_cast<double>(Style::barCapsulePadding), 0.0, 48.0, 1.0),
        WidgetSettingGroup::Presentation
    );
    capsulePadding.visibleWhen = capsuleOn;
    auto capsuleRadius = withGroup(optionalDoubleSpec("capsule_radius", 0.0, 80.0), WidgetSettingGroup::Presentation);
    capsuleRadius.visibleWhen = capsuleOn;
    auto capsuleOpacity =
        withGroup(doubleSpec("capsule_opacity", 1.0, 0.0, 1.0, 0.01), WidgetSettingGroup::Presentation);
    capsuleOpacity.visibleWhen = capsuleOn;

    return {
        std::move(anchor),         std::move(scale),         std::move(widgetColor),       std::move(widgetIconColor),
        std::move(fontFamily),     std::move(fontWeight),    std::move(capsuleToggle),     std::move(capsuleRadius),
        std::move(capsuleFill),    std::move(capsuleBorder), std::move(capsuleForeground), std::move(capsulePadding),
        std::move(capsuleOpacity),
    };
  }

  std::vector<WidgetSettingSpec>
  widgetSettingSpecs(std::string_view type, std::string_view shellFontFamily, bool supportsTaskbarWorkspaceGrouping) {
    std::vector<WidgetSettingSpec> specs;
    auto commonSpecs = commonWidgetSettingSpecs(shellFontFamily);

    auto add = [&](WidgetSettingSpec spec) { specs.push_back(std::move(spec)); };
    const std::vector<WidgetSettingSelectOption> shortFull = {
        {"short", "settings.widgets.options.short"},
        {"full", "settings.widgets.options.full"},
    };
    const std::vector<WidgetSettingSelectOption> sysmonStats = {
        {"cpu_usage", "settings.widgets.options.cpu-usage"},   {"cpu_temp", "settings.widgets.options.cpu-temp"},
        {"gpu_temp", "settings.widgets.options.gpu-temp"},     {"gpu_usage", "settings.widgets.options.gpu-usage"},
        {"gpu_vram", "settings.widgets.options.gpu-vram"},     {"ram_used", "settings.widgets.options.ram-used"},
        {"ram_pct", "settings.widgets.options.ram-percent"},   {"swap_pct", "settings.widgets.options.swap-percent"},
        {"disk_pct", "settings.widgets.options.disk-percent"}, {"net_rx", "settings.widgets.options.net-rx"},
        {"net_tx", "settings.widgets.options.net-tx"},
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
    const std::vector<WidgetSettingSelectOption> volumeDeviceOptions = {
        {"output", "settings.widgets.options.output"},
        {"input", "settings.widgets.options.input"},
    };
    if (type == "active_window") {
      add(intSpec("min_length", 80, 0.0, 800.0, 1.0));
      add(intSpec("max_length", 260, 40.0, 800.0, 1.0));
      add(intSpec("icon_size", static_cast<double>(Style::fontSizeBody), 8.0, 64.0, 1.0));
      add(selectSpec("title_scroll", "none", mediaTitleScroll));
      {
        auto display = selectSpec("display", "icon_and_text", activeWindowDisplay);
        display.descriptionKey = "settings.widgets.settings.display.active-window-description";
        add(std::move(display));
      }
      add(boolSpec("show_empty_label", false));
    } else if (type == "audio_visualizer") {
      add(intSpec("width", 56.0, 8.0, 400.0, 1.0));
      add(intSpec("bands", 16, 2.0, 128.0, 1.0));
      add(boolSpec("mirrored", true));
      add(boolSpec("centered", true));
      add(boolSpec("show_when_idle", false));
      {
        auto color1 = colorSpec("color_1", "primary");
        add(std::move(color1));
      }
      {
        auto color2 = colorSpec("color_2", "primary");
        add(std::move(color2));
      }
    } else if (type == "battery") {
      add(selectSpec(
          "display_mode", "glyph",
          {{"glyph", "settings.widgets.options.glyph"}, {"graphic", "settings.widgets.options.graphic"}}
      ));
      add(boolSpec("show_label", true));
      add(boolSpec("hide_when_plugged", false));
      add(boolSpec("hide_when_full", false));
      add(selectSpec("device", "auto", {{"auto", "common.states.auto"}}));
      {
        auto warn = colorSpec("warning_color", "error");
        add(std::move(warn));
      }
    } else if (type == "bluetooth") {
      add(boolSpec("show_label", false));
      add(boolSpec("hide_when_no_connected_device", false));
    } else if (type == "brightness") {
      add(stepperIntSpec("scroll_step", 5, 1.0, 25.0, 1.0, "%"));
      add(boolSpec("show_label", true));
    } else if (type == "clock") {
      add(stringSpec("format", "{:%H:%M}"));
      add(stringSpec("vertical_format"));
      add(stringSpec("tooltip_format"));
    } else if (type == "clipboard") {
      add(glyphSpec("glyph", "clipboard"));
    } else if (type == "screenshot") {
      add(glyphSpec("glyph", "screenshot"));
      add(segmentedSpec(
          "primary_click", "region",
          {
              {"region", "settings.widgets.options.screenshot-primary-region"},
              {"fullscreen", "settings.widgets.options.screenshot-primary-fullscreen"},
          }
      ));
    } else if (type == "keyboard_layout") {
      add(stringSpec("cycle_command"));
      add(boolSpec("hide_when_single_layout", false));
      add(boolSpec("show_icon", true));
      {
        auto glyph = glyphSpec("glyph", "keyboard");
        glyph.visibleWhen = WidgetSettingVisibility{"show_icon", {"true"}};
        add(std::move(glyph));
      }
      add(boolSpec("show_label", true));
      {
        auto display = segmentedSpec("display", "short", shortFull);
        display.visibleWhen = WidgetSettingVisibility{"show_label", {"true"}};
        add(std::move(display));
      }
      {
        auto labels = stringMapSpec("custom_labels");
        labels.visibleWhen = WidgetSettingVisibility{"show_label", {"true"}};
        add(std::move(labels));
      }
    } else if (type == "launcher") {
      add(glyphSpec("glyph", "search"));
      add(stringSpec("custom_image", ""));
      add(boolSpec("custom_image_colorize", false));
    } else if (type == "control-center") {
      add(glyphSpec("glyph", "noctalia"));
      add(stringSpec("custom_image", ""));
      add(boolSpec("custom_image_colorize", false));
    } else if (type == "custom_button") {
      add(glyphSpec("glyph", "heart"));
      add(stringSpec("label"));
      add(stringSpec("tooltip"));
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
      const WidgetSettingVisibility notAlbumArtOnly{"album_art_only", {"false"}};
      const WidgetSettingVisibility notHideAlbumArt{"hide_album_art", {"false"}};
      {
        auto albumArtOnly = boolSpec("album_art_only", false);
        albumArtOnly.horizontalBarOnly = true;
        albumArtOnly.visibleWhen = notHideAlbumArt;
        add(std::move(albumArtOnly));
      }
      {
        auto hideAlbumArt = boolSpec("hide_album_art", false);
        hideAlbumArt.horizontalBarOnly = true;
        hideAlbumArt.visibleWhen = notAlbumArtOnly;
        add(std::move(hideAlbumArt));
      }
      {
        auto minLength = intSpec("min_length", 80, 0.0, 800.0, 1.0);
        minLength.visibleWhen = notAlbumArtOnly;
        add(std::move(minLength));
      }
      {
        auto maxLength = intSpec("max_length", 220, 40.0, 800.0, 1.0);
        maxLength.visibleWhen = notAlbumArtOnly;
        add(std::move(maxLength));
      }
      add(intSpec("art_size", 16.0, 8.0, 96.0, 1.0));
      {
        auto titleScroll = selectSpec("title_scroll", "none", mediaTitleScroll);
        titleScroll.visibleWhen = notAlbumArtOnly;
        add(std::move(titleScroll));
      }
      add(boolSpec("hide_when_no_media", false));
    } else if (type == "network") {
      add(boolSpec("show_label", true));
    } else if (type == "notifications") {
      add(boolSpec("hide_when_no_unread", false));
    } else if (type == "privacy") {
      add(boolSpec("hide_inactive", false));
      add(intSpec("icon_spacing", 4, 0.0, 48.0, 1.0));
      add(colorSpec("active_color", "primary"));
      add(colorSpec("inactive_color", "outline"));
    } else if (type == "session") {
      add(glyphSpec("glyph", "shutdown"));
    } else if (type == "settings") {
      add(glyphSpec("glyph", "settings"));
    } else if (type == "spacer") {
      add(intSpec("length", 20, 0.0, 400.0, 1.0));
    } else if (type == "sysmon") {
      add(selectSpec("stat", "cpu_usage", sysmonStats));
      {
        auto glyph = glyphSpec("glyph", "");
        glyph.descriptionKey = "settings.widgets.settings.glyph.sysmon-description";
        add(std::move(glyph));
      }
      {
        auto path = stringSpec("path", "/");
        path.visibleWhen = WidgetSettingVisibility{"stat", {"disk_pct"}};
        add(std::move(path));
      }
      {
        auto interface = stringSpec("interface");
        interface.visibleWhen = WidgetSettingVisibility{"stat", {"net_rx", "net_tx"}};
        add(std::move(interface));
      }
      add(segmentedSpec("display", "gauge", sysmonDisplay));
      add(colorSpec("highlight_color", "error"));
      add(boolSpec("show_label", true));
      {
        auto minW = intSpec("label_min_width", 0, 0.0, 200.0, 1.0);
        minW.visibleWhen = WidgetSettingVisibility{"show_label", {"true"}};
        add(std::move(minW));
      }
    } else if (type == "taskbar") {
      add(boolSpec("show_all_outputs", false));
      if (supportsTaskbarWorkspaceGrouping) {
        add(boolSpec("group_by_workspace", false));
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
          groupCapsule.descriptionKey = "settings.widgets.settings.workspace-group-capsule.description";
          groupCapsule.visibleWhen =
              WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
          add(std::move(groupCapsule));
        }
        {
          auto focusedOutputOnly = boolSpec("focused_output_only", false);
          focusedOutputOnly.descriptionKey = "settings.widgets.settings.focused-output-only.taskbar-description";
          focusedOutputOnly.visibleWhen =
              WidgetSettingVisibility{WidgetSettingVisibilityCondition{"show_workspace_label", {"true"}}};
          add(std::move(focusedOutputOnly));
        }
        {
          auto singleIconPerApp = boolSpec("group_single_icon_per_app", false);
          singleIconPerApp.visibleWhen =
              WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
          add(std::move(singleIconPerApp));
        }
      }
      add(boolSpec("show_active_indicator", true));
      add(doubleSpec("active_opacity", 1.0, 0.1, 1.0, 0.01));
      add(doubleSpec("inactive_opacity", 1.0, 0.1, 1.0, 0.01));
      if (supportsTaskbarWorkspaceGrouping) {
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
        for (auto& spec : commonSpecs) {
          if (spec.schema.key == "capsule_radius") {
            spec.descriptionKey = "settings.widgets.settings.capsule-radius.taskbar-description";
            spec.visibleWhen =
                WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"true"}}};
            break;
          }
        }
        {
          auto showWindowTitle = boolSpec("show_window_title", false);
          showWindowTitle.visibleWhen =
              WidgetSettingVisibility{WidgetSettingVisibilityCondition{"group_by_workspace", {"false"}}};
          add(std::move(showWindowTitle));
        }
        const WidgetSettingVisibility windowTitleSettings = [] {
          WidgetSettingVisibility v;
          v.all = {{"group_by_workspace", {"false"}}, {"show_window_title", {"true"}}};
          return v;
        }();
        {
          auto windowTitleMaxWidth = intSpec("window_title_max_width", 100.0, 10.0, 200.0, 1.0);
          windowTitleMaxWidth.visibleWhen = windowTitleSettings;
          add(std::move(windowTitleMaxWidth));
        }
        {
          auto taskbarMaxWidth = intSpec("taskbar_max_width", 8192.0, 10.0, 8192.0, 1.0);
          taskbarMaxWidth.visibleWhen = windowTitleSettings;
          add(std::move(taskbarMaxWidth));
        }
      } else {
        add(boolSpec("show_window_title", false));
        const WidgetSettingVisibility windowTitleSettings{"show_window_title", {"true"}};
        {
          auto windowTitleMaxWidth = intSpec("window_title_max_width", 100.0, 10.0, 200.0, 1.0);
          windowTitleMaxWidth.visibleWhen = windowTitleSettings;
          add(std::move(windowTitleMaxWidth));
        }
        {
          auto taskbarMaxWidth = intSpec("taskbar_max_width", 8192.0, 10.0, 8192.0, 1.0);
          taskbarMaxWidth.visibleWhen = windowTitleSettings;
          add(std::move(taskbarMaxWidth));
        }
      }
    } else if (type == "tray") {
      add(stringListSpec("hidden"));
      add(stringListSpec("pinned"));
      add(boolSpec("match_adjacent_spacing", false));
      add(boolSpec("drawer", false));
      {
        const WidgetSettingVisibility drawerOn{"drawer", {"true"}};
        auto cols = intSpec("drawer_columns", 3, 1.0, 5.0, 1.0);
        cols.visibleWhen = drawerOn;
        add(std::move(cols));
        auto detachedPanel = boolSpec("detached_panel", false);
        detachedPanel.visibleWhen = drawerOn;
        add(std::move(detachedPanel));
      }
    } else if (type == "volume") {
      add(segmentedSpec("device", "output", volumeDeviceOptions));
      add(stepperIntSpec("scroll_step", 5, 1.0, 25.0, 1.0, "%"));
      add(boolSpec("show_label", true));
      add(colorSpec("mute_color", "error"));
    } else if (type == "wallpaper") {
      add(glyphSpec("glyph", "wallpaper-selector"));
    } else if (type == "weather") {
      add(intSpec("max_length", 160, 40.0, 800.0, 1.0));
      add(boolSpec("show_condition", true));
    } else if (type == "workspaces") {
      const WidgetSettingVisibility pillStyleOnly{{"minimal", {"false"}}};
      for (auto& spec : commonSpecs) {
        if (spec.schema.key == "capsule_radius") {
          spec.descriptionKey = "settings.widgets.settings.capsule-radius.workspaces-description";
          spec.visibleWhen = pillStyleOnly;
          break;
        }
      }
      {
        auto minimal = boolSpec("minimal", false);
        minimal.descriptionKey = "settings.widgets.settings.minimal.workspaces-description";
        add(std::move(minimal));
      }
      {
        auto focusedOutputOnly = boolSpec("focused_output_only", false);
        focusedOutputOnly.descriptionKey = "settings.widgets.settings.focused-output-only.workspaces-description";
        add(std::move(focusedOutputOnly));
      }
      add(segmentedSpec("display", "id", workspaceDisplay));
      {
        auto labelsOnlyWhenOccupied = boolSpec("labels_only_when_occupied", false);
        labelsOnlyWhenOccupied.descriptionKey =
            "settings.widgets.settings.labels-only-when-occupied.workspaces-description";
        add(std::move(labelsOnlyWhenOccupied));
      }
      {
        auto hideWhenEmpty = boolSpec("hide_when_empty", false);
        hideWhenEmpty.descriptionKey = "settings.widgets.settings.hide-when-empty.workspaces-description";
        add(std::move(hideWhenEmpty));
      }
      {
        auto maxLabelChars = intSpec("max_label_chars", 1, 1.0, 20.0, 1.0);
        maxLabelChars.descriptionKey = "settings.widgets.settings.max-label-chars.workspaces-description";
        add(std::move(maxLabelChars));
      }
      {
        auto pillScale = doubleSpec("pill_scale", 1.0, 0.1, 1.0, 0.05);
        pillScale.descriptionKey = "settings.widgets.settings.pill-scale.workspaces-description";
        pillScale.visibleWhen = pillStyleOnly;
        add(std::move(pillScale));
      }
      {
        auto activePillSize = doubleSpec("active_pill_size", 2.2, 0.25, 8.0, 0.05);
        activePillSize.descriptionKey = "settings.widgets.settings.active-pill-size.workspaces-description";
        activePillSize.visibleWhen = pillStyleOnly;
        add(std::move(activePillSize));
      }
      {
        auto inactivePillSize = doubleSpec("inactive_pill_size", 1.0, 0.25, 8.0, 0.05);
        inactivePillSize.descriptionKey = "settings.widgets.settings.inactive-pill-size.workspaces-description";
        inactivePillSize.visibleWhen = pillStyleOnly;
        add(std::move(inactivePillSize));
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

    specs.insert(specs.end(), std::make_move_iterator(commonSpecs.begin()), std::make_move_iterator(commonSpecs.end()));
    return specs;
  }

  std::vector<WidgetSettingSpec> manifestSettingSpecs(
      const std::vector<scripting::ManifestField>& fields, const scripting::PluginTranslationCatalog* translations
  ) {
    std::vector<WidgetSettingSpec> specs;
    specs.reserve(fields.size());
    for (const auto& field : fields) {
      WidgetSettingSpec spec;
      spec.schema.key = field.key;
      if (!field.labelKey.empty()) {
        spec.literalLabel = translations != nullptr ? translations->translate(field.labelKey) : field.labelKey;
      } else {
        spec.literalLabel = field.label.empty() ? field.key : field.label;
      }
      if (!field.descriptionKey.empty()) {
        spec.literalDescription =
            translations != nullptr ? translations->translate(field.descriptionKey) : field.descriptionKey;
      } else {
        spec.literalDescription = field.description;
      }
      spec.advanced = field.advanced;
      spec.schema.minValue = field.minValue;
      spec.schema.maxValue = field.maxValue;
      spec.schema.step = field.step;

      switch (field.type) {
      case scripting::ManifestFieldType::Bool:
        spec.control = WidgetControlKind::Bool;
        spec.schema.defaultValue = field.boolDefault;
        break;
      case scripting::ManifestFieldType::Int:
        spec.control = WidgetControlKind::Int;
        spec.schema.defaultValue = static_cast<std::int64_t>(field.numberDefault);
        break;
      case scripting::ManifestFieldType::Double:
        spec.control = WidgetControlKind::Double;
        spec.schema.defaultValue = field.numberDefault;
        break;
      case scripting::ManifestFieldType::StringList:
        spec.control = WidgetControlKind::StringList;
        spec.schema.defaultValue = field.stringListDefault;
        break;
      case scripting::ManifestFieldType::File:
        spec.control = WidgetControlKind::File;
        spec.schema.defaultValue = field.stringDefault;
        spec.extensions = field.extensions;
        break;
      case scripting::ManifestFieldType::Folder:
        spec.control = WidgetControlKind::Folder;
        spec.schema.defaultValue = field.stringDefault;
        break;
      case scripting::ManifestFieldType::Select:
        spec.control = WidgetControlKind::Select;
        spec.schema.defaultValue = field.stringDefault;
        spec.literalLabels = true;
        for (const auto& opt : field.options) {
          spec.schema.enumValues.push_back(opt.value);
          std::string label = opt.label;
          if (!opt.labelKey.empty()) {
            label = translations != nullptr ? translations->translate(opt.labelKey) : opt.labelKey;
          }
          spec.options.push_back(WidgetSettingSelectOption{.value = opt.value, .labelKey = std::move(label)});
        }
        break;
      case scripting::ManifestFieldType::Color:
        spec.control = WidgetControlKind::ColorSpec;
        spec.schema.defaultValue = field.stringDefault;
        break;
      case scripting::ManifestFieldType::Glyph:
        spec.control = WidgetControlKind::Glyph;
        spec.schema.defaultValue = field.stringDefault;
        break;
      case scripting::ManifestFieldType::String:
      default:
        spec.control = WidgetControlKind::String;
        spec.schema.defaultValue = field.stringDefault;
        break;
      }
      spec.schema.type = schemaTypeForControl(spec.control);

      if (field.visibleWhen.has_value()) {
        spec.visibleWhen = WidgetSettingVisibility{field.visibleWhen->key, field.visibleWhen->values};
      }
      specs.push_back(std::move(spec));
    }
    return specs;
  }

  std::vector<WidgetSettingSpec> pluginPanelShellSettingSpecs(const scripting::PluginEntry& entry) {
    if (entry.kind != scripting::PluginEntryKind::Panel) {
      return {};
    }
    const std::string placementKey = scripting::panelShellSettingKey(entry.id, "placement");
    const std::string positionKey = scripting::panelShellSettingKey(entry.id, "position");
    const std::string openNearClickKey = scripting::panelShellSettingKey(entry.id, "open_near_click");
    const std::string entryTitle = entry.id;
    const auto entryPrefix = [&](std::string_view suffix) {
      std::string label = entryTitle;
      label.append(" · ");
      label.append(suffix);
      return label;
    };

    auto placementSpec = [&](const scripting::ManifestField* field) {
      WidgetSettingSpec spec;
      spec.schema.key = placementKey;
      spec.literalLabel = entryPrefix(tr("settings.plugins.panels.placement.label"));
      spec.literalDescription = tr("settings.plugins.panels.placement.description");
      spec.control = WidgetControlKind::Select;
      spec.segmented = true;
      spec.literalLabels = true;
      spec.schema.defaultValue = field != nullptr ? field->defaultValue() : entry.panelPlacementDefault;
      spec.options = {
          {"attached", tr("settings.options.shell.panel-placement.attached")},
          {"floating", tr("settings.options.shell.panel-placement.floating")},
      };
      spec.schema.type = schemaTypeForControl(spec.control);
      return spec;
    };

    auto positionSpec = [&](const scripting::ManifestField* field) {
      WidgetSettingSpec spec;
      spec.schema.key = positionKey;
      spec.literalLabel = entryPrefix(tr("settings.plugins.panels.position.label"));
      spec.literalDescription = tr("settings.plugins.panels.position.description");
      spec.control = WidgetControlKind::Select;
      spec.literalLabels = true;
      spec.schema.defaultValue = field != nullptr ? field->defaultValue() : entry.panelPositionDefault;
      spec.options = {
          {"auto", tr("settings.options.panel-position.auto")},
          {"center", tr("settings.options.screen-position.center")},
          {"top_left", tr("settings.options.screen-position.top-left")},
          {"top_center", tr("settings.options.screen-position.top-center")},
          {"top_right", tr("settings.options.screen-position.top-right")},
          {"center_left", tr("settings.options.screen-position.center-left")},
          {"center_right", tr("settings.options.screen-position.center-right")},
          {"bottom_left", tr("settings.options.screen-position.bottom-left")},
          {"bottom_center", tr("settings.options.screen-position.bottom-center")},
          {"bottom_right", tr("settings.options.screen-position.bottom-right")},
      };
      spec.schema.type = schemaTypeForControl(spec.control);
      spec.visibleWhen = WidgetSettingVisibility{placementKey, {"floating"}};
      return spec;
    };

    auto openNearClickSpec = [&](const scripting::ManifestField* field) {
      WidgetSettingSpec spec;
      spec.schema.key = openNearClickKey;
      spec.literalLabel = entryPrefix(tr("settings.plugins.panels.open-near-click.label"));
      spec.literalDescription = tr("settings.plugins.panels.open-near-click.description");
      spec.control = WidgetControlKind::Bool;
      spec.schema.defaultValue = field != nullptr ? field->defaultValue() : entry.panelOpenNearClickDefault;
      spec.schema.type = schemaTypeForControl(spec.control);
      spec.visibleWhen = WidgetSettingVisibility{placementKey, {"attached", "floating"}};
      return spec;
    };

    const scripting::ManifestField* placementField = nullptr;
    const scripting::ManifestField* positionField = nullptr;
    const scripting::ManifestField* openNearClickField = nullptr;
    for (const auto& field : entry.settings) {
      if (field.key == placementKey) {
        placementField = &field;
      } else if (field.key == positionKey) {
        positionField = &field;
      } else if (field.key == openNearClickKey) {
        openNearClickField = &field;
      }
    }

    return {
        placementSpec(placementField),
        positionSpec(positionField),
        openNearClickSpec(openNearClickField),
    };
  }

  std::vector<WidgetSettingSpec> widgetSettingSpecs(
      std::string_view type, const WidgetConfig* config, std::string_view shellFontFamily,
      bool supportsTaskbarWorkspaceGrouping
  ) {
    (void)config;
    if (auto pw = resolvePluginWidget(type)) {
      scripting::PluginTranslationCatalog translations;
      translations.load(pw->sourcePath.parent_path());
      std::vector<WidgetSettingSpec> specs = manifestSettingSpecs(pw->entry->settings, &translations);
      auto commonSpecs = commonWidgetSettingSpecs(shellFontFamily);
      specs.insert(
          specs.end(), std::make_move_iterator(commonSpecs.begin()), std::make_move_iterator(commonSpecs.end())
      );
      return specs;
    }
    return widgetSettingSpecs(type, shellFontFamily, supportsTaskbarWorkspaceGrouping);
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
    return findWidgetSettingSpec(widgetType, settingKey, nullptr);
  }

  std::optional<WidgetSettingSpec>
  findWidgetSettingSpec(std::string_view widgetType, std::string_view settingKey, const WidgetConfig* config) {
    const std::string key(settingKey);
    for (const auto& spec : widgetSettingSpecs(widgetType, config, "sans-serif")) {
      if (spec.schema.key == key) {
        return spec;
      }
    }
    return std::nullopt;
  }

  noctalia::config::schema::WidgetSettingSchema widgetSettingSchema(std::string_view type) {
    noctalia::config::schema::WidgetSettingSchema out;
    for (const auto& spec : widgetSettingSpecs(type, "sans-serif")) {
      out.push_back(spec.schema);
    }
    return out;
  }

  noctalia::config::schema::WidgetSettingSchema widgetSettingSchema(std::string_view type, const WidgetConfig* config) {
    noctalia::config::schema::WidgetSettingSchema out;
    for (const auto& spec : widgetSettingSpecs(type, config, "sans-serif")) {
      out.push_back(spec.schema);
    }
    return out;
  }

  std::optional<noctalia::config::schema::WidgetSettingField>
  findWidgetSettingField(std::string_view widgetType, std::string_view settingKey) {
    if (const auto spec = findWidgetSettingSpec(widgetType, settingKey)) {
      return spec->schema;
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
    if (spec->schema.type == schema::WidgetSettingType::OptionalDouble) {
      return false;
    }
    return configOverrideValueMatchesWidgetSetting(overrideValue, spec->schema.defaultValue);
  }

  bool widgetSettingOverrideIsEffective(
      std::string_view widgetName, std::string_view settingKey, const Config& withOverride,
      const Config& withoutOverride
  ) {
    const auto widgetInConfig = [](const Config& cfg, std::string_view name) -> const WidgetConfig* {
      const auto widgetIt = cfg.widgets.find(std::string(name));
      if (widgetIt == cfg.widgets.end()) {
        return nullptr;
      }
      return &widgetIt->second;
    };
    const auto valueInConfig = [&](const Config& cfg, std::string_view name,
                                   std::string_view key) -> std::optional<WidgetSettingValue> {
      const auto* widget = widgetInConfig(cfg, name);
      if (widget == nullptr) {
        return std::nullopt;
      }
      const auto settingIt = widget->settings.find(std::string(key));
      if (settingIt == widget->settings.end()) {
        return std::nullopt;
      }
      return settingIt->second;
    };
    const auto tableInConfig = [&](
                                   const Config& cfg, std::string_view name, std::string_view key
                               ) -> std::optional<std::unordered_map<std::string, std::string>> {
      const auto* widget = widgetInConfig(cfg, name);
      if (widget == nullptr) {
        return std::nullopt;
      }
      const auto tableIt = widget->tables.find(std::string(key));
      if (tableIt == widget->tables.end()) {
        return std::nullopt;
      }
      return tableIt->second;
    };

    std::string widgetType(widgetName);
    if (const auto* withWidget = widgetInConfig(withOverride, widgetName); withWidget != nullptr) {
      widgetType = withWidget->type;
    } else if (const auto* withoutWidget = widgetInConfig(withoutOverride, widgetName); withoutWidget != nullptr) {
      widgetType = withoutWidget->type;
    }

    const WidgetConfig* defaultConfig = widgetInConfig(withoutOverride, widgetName);
    if (defaultConfig == nullptr) {
      defaultConfig = widgetInConfig(withOverride, widgetName);
    }

    const auto spec = findWidgetSettingSpec(widgetType, settingKey, defaultConfig);
    if (spec.has_value() && spec->schema.type == schema::WidgetSettingType::StringMap) {
      const auto withTable = tableInConfig(withOverride, widgetName, settingKey);
      const auto withoutTable = tableInConfig(withoutOverride, widgetName, settingKey);
      if (!withTable.has_value() && !withoutTable.has_value()) {
        return false;
      }
      return withTable.value_or(std::unordered_map<std::string, std::string>{})
          != withoutTable.value_or(std::unordered_map<std::string, std::string>{});
    }
    const auto withValue = valueInConfig(withOverride, widgetName, settingKey);
    const auto withoutValue = valueInConfig(withoutOverride, widgetName, settingKey);
    if (!withValue.has_value() && !withoutValue.has_value()) {
      return false;
    }
    if (!spec.has_value()) {
      if (!withValue.has_value() || !withoutValue.has_value()) {
        return true;
      }
      return !widgetSettingValuesEqual(*withValue, *withoutValue);
    }
    if (spec->schema.type == schema::WidgetSettingType::OptionalDouble) {
      if (!withValue.has_value() || !withoutValue.has_value()) {
        return true;
      }
      return !widgetSettingValuesEqual(*withValue, *withoutValue);
    }

    const WidgetSettingValue defaultValue = spec->schema.defaultValue;
    const auto resolvedValue = [&](const Config& cfg) -> WidgetSettingValue {
      if (const auto value = valueInConfig(cfg, widgetName, settingKey); value.has_value()) {
        return *value;
      }
      return defaultValue;
    };

    return !widgetSettingValuesEqual(resolvedValue(withOverride), resolvedValue(withoutOverride));
  }

} // namespace settings
