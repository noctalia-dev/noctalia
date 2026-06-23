#include "shell/settings/settings_content_plugins.h"

#include "config/config_types.h"
#include "i18n/i18n.h"
#include "scripting/plugin_i18n.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace settings {

  namespace {
    std::unique_ptr<Label>
    makeLabel(std::string_view text, float fontSize, ColorRole role, FontWeight weight = FontWeight::Normal) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .color = colorSpecFromRole(role),
          .fontWeight = weight,
      });
    }

    bool pluginEnabled(const scripting::PluginStatus& plugin, const SettingsPluginsContext& ctx) {
      if (ctx.config == nullptr) {
        return plugin.enabled;
      }
      return std::ranges::contains(ctx.config->plugins.enabled, plugin.id);
    }

    std::string_view pluginDisplayName(const scripting::PluginStatus& plugin) { return plugin.name; }

    std::string pluginSourceDisplayName(std::string_view source) {
      if (source == "official") {
        return "Official";
      }
      if (source == "community") {
        return "Community";
      }
      return std::string(source);
    }

    int pluginSourceOrder(std::string_view source) {
      if (source == "official") {
        return 0;
      }
      if (source == "community") {
        return 1;
      }
      return 2;
    }

    bool pluginSourceLess(std::string_view a, std::string_view b) {
      const int aOrder = pluginSourceOrder(a);
      const int bOrder = pluginSourceOrder(b);
      if (aOrder != bOrder) {
        return aOrder < bOrder;
      }
      return a < b;
    }

    std::unique_ptr<Flex> sourceRow(const PluginSourceConfig& source, const SettingsPluginsContext& ctx, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      Flex* r = row.get();

      auto info = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      info->addChild(makeLabel(
          pluginSourceDisplayName(source.name), Style::fontSizeBody * scale,
          source.enabled ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant, FontWeight::Medium
      ));
      const std::string kind = source.kind == PluginSourceKind::Git ? i18n::tr("settings.plugins.sources.kind.git")
                                                                    : i18n::tr("settings.plugins.sources.kind.path");
      info->addChild(
          makeLabel(kind + " · " + source.location, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant)
      );
      r->addChild(std::move(info));

      if (source.enabled && source.kind == PluginSourceKind::Git) {
        r->addChild(
            ui::button({
                .glyph = "refresh",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.plugins.sources.update"),
                .onClick = [cb = ctx.updateSource, name = source.name]() {
                  if (cb) {
                    cb(name);
                  }
                },
            })
        );
      }
      r->addChild(
          ui::button({
              .glyph = "settings",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("settings.plugins.sources.edit"),
              .onClick = [cb = ctx.editSource, source]() {
                if (cb) {
                  cb(source);
                }
              },
          })
      );
      r->addChild(
          ui::toggle({
              .checked = source.enabled,
              .scale = scale,
              .onChange = [cb = ctx.setSourceEnabled, source](bool on) {
                if (cb) {
                  cb(source, on);
                }
              },
          })
      );
      return row;
    }

    std::unique_ptr<Flex> makeSourceBadge(std::string_view label, float scale) {
      return ui::row(
          {.align = FlexAlign::Center,
           .paddingH = Style::spaceXs * scale,
           .fill = colorSpecFromRole(ColorRole::Primary, 0.15f),
           .radius = Style::scaledRadiusSm(scale)},
          ui::label({
              .text = std::string(label),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .fontWeight = FontWeight::Bold,
          })
      );
    }

    std::unique_ptr<Flex>
    pluginRow(const scripting::PluginStatus& plugin, const SettingsPluginsContext& ctx, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      Flex* r = row.get();
      const bool enabled = pluginEnabled(plugin, ctx);

      r->addChild(
          ui::glyph({
              .glyph = plugin.icon.empty() ? std::string("apps") : plugin.icon,
              .glyphSize = Style::fontSizeHeader * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .width = Style::controlHeightSm * scale,
              .height = Style::controlHeightSm * scale,
          })
      );

      auto info = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      auto title = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
      const std::string version = plugin.version.empty() ? std::string("?") : plugin.version;
      title->addChild(
          makeLabel(pluginDisplayName(plugin), Style::fontSizeBody * scale, ColorRole::OnSurface, FontWeight::Medium)
      );
      if (plugin.source == "official") {
        title->addChild(makeSourceBadge(i18n::tr("settings.badges.official"), scale));
      } else if (!plugin.source.empty()) {
        title->addChild(makeLabel(
            pluginSourceDisplayName(plugin.source), Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
        ));
      }
      title->addChild(makeLabel("v" + version, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant));
      if (!plugin.compatible) {
        title->addChild(makeLabel(
            i18n::tr("settings.plugins.plugins.requires-newer-noctalia"), Style::fontSizeMini * scale, ColorRole::Error,
            FontWeight::Bold
        ));
      }
      if (plugin.deprecated) {
        title->addChild(makeLabel(
            i18n::tr("settings.plugins.plugins.deprecated"), Style::fontSizeMini * scale, ColorRole::Secondary,
            FontWeight::Bold
        ));
      }
      info->addChild(std::move(title));
      if (!plugin.description.empty()) {
        info->addChild(makeLabel(plugin.description, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant));
      }
      if (!plugin.dependencies.empty()) {
        info->addChild(makeLabel(
            i18n::tr("settings.plugins.plugins.requires", "dependencies", StringUtils::join(plugin.dependencies, ", ")),
            Style::fontSizeCaption * scale, ColorRole::Secondary
        ));
      }
      r->addChild(std::move(info));

      const auto* manifest = scripting::PluginRegistry::instance().findManifest(plugin.id);
      if (enabled && manifest != nullptr && !manifest->settings.empty() && ctx.onConfigure) {
        r->addChild(
            ui::button({
                .glyph = "settings",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.plugins.plugins.configure"),
                .onClick = [cb = ctx.onConfigure, id = plugin.id]() {
                  if (cb) {
                    cb(id);
                  }
                },
            })
        );
      }

      r->addChild(
          ui::toggle({
              .checked = enabled,
              .enabled = enabled || plugin.compatible,
              .scale = scale,
              .onChange = [cb = ctx.setEnabled, id = plugin.id](bool on) {
                if (cb) {
                  cb(id, on);
                }
              },
          })
      );
      return row;
    }

    // ── Per-plugin settings editor ─────────────────────────────────────────

    std::string valueAsString(const WidgetSettingValue& value) {
      if (const auto* s = std::get_if<std::string>(&value)) {
        return *s;
      }
      if (const auto* b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*i);
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return std::to_string(*d);
      }
      return {};
    }

    std::vector<std::string> valueAsStringList(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::vector<std::string>>(&value)) {
        return *v;
      }
      return {};
    }

    bool valueAsBool(const WidgetSettingValue& value) {
      if (const auto* b = std::get_if<bool>(&value)) {
        return *b;
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i != 0;
      }
      return false;
    }

    std::int64_t valueAsInt(const WidgetSettingValue& value) {
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i;
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(std::llround(*d));
      }
      return 0;
    }

    double valueAsDouble(const WidgetSettingValue& value) {
      if (const auto* d = std::get_if<double>(&value)) {
        return *d;
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
      }
      return 0.0;
    }

    // Current value for a plugin setting: the override if present, else the manifest default.
    WidgetSettingValue
    pluginSettingValue(const Config& cfg, const std::string& pluginId, const WidgetSettingSpec& spec) {
      const auto pluginIt = cfg.plugins.pluginSettings.find(pluginId);
      if (pluginIt != cfg.plugins.pluginSettings.end()) {
        const auto keyIt = pluginIt->second.find(spec.schema.key);
        if (keyIt != pluginIt->second.end()) {
          return keyIt->second;
        }
      }
      return spec.schema.defaultValue;
    }

    bool pluginSettingVisible(
        const Config& cfg, const std::string& pluginId, const WidgetSettingSpec& spec,
        const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      if (!spec.visibleWhen.has_value()) {
        return true;
      }
      const auto currentString = [&](const std::string& key) -> std::string {
        const auto depIt =
            std::ranges::find_if(allSpecs, [&](const WidgetSettingSpec& s) { return s.schema.key == key; });
        if (depIt == allSpecs.end()) {
          return {};
        }
        return valueAsString(pluginSettingValue(cfg, pluginId, *depIt));
      };
      const auto matches = [&](const WidgetSettingVisibilityCondition& cond) {
        const std::string value = currentString(cond.key);
        return std::ranges::contains(cond.values, value);
      };
      // Visible when any `any` alternative matches (or none declared) AND every `all` condition matches.
      const auto& vis = *spec.visibleWhen;
      const bool anyOk = vis.any.empty() || std::ranges::any_of(vis.any, matches);
      const bool allOk = std::ranges::all_of(vis.all, matches);
      return anyOk && allOk;
    }

    std::unique_ptr<Node> pluginSettingControl(
        SettingsControlFactory& factory, const WidgetSettingSpec& spec, const WidgetSettingValue& value,
        const std::vector<std::string>& path
    ) {
      switch (spec.control) {
      case WidgetControlKind::Bool: {
        std::optional<bool> clearWhenValue;
        if (const auto* defaultBool = std::get_if<bool>(&spec.schema.defaultValue)) {
          clearWhenValue = *defaultBool;
        }
        return factory.makeToggle(valueAsBool(value), true, path, clearWhenValue);
      }
      case WidgetControlKind::Int: {
        const double minValue = spec.schema.minValue.value_or(0.0);
        const double maxValue = spec.schema.maxValue.value_or(100.0);
        return factory.makeSlider(
            static_cast<double>(valueAsInt(value)), minValue, maxValue, spec.schema.step.value_or(1.0), path,
            /*integerValue=*/true
        );
      }
      case WidgetControlKind::Double: {
        const double minValue = spec.schema.minValue.value_or(0.0);
        const double maxValue = spec.schema.maxValue.value_or(1.0);
        return factory.makeSlider(
            valueAsDouble(value), minValue, maxValue, spec.schema.step.value_or(1.0), path, false
        );
      }
      case WidgetControlKind::Select: {
        std::vector<SelectOption> options;
        options.reserve(spec.options.size());
        for (const auto& option : spec.options) {
          options.push_back(
              SelectOption{option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
          );
        }
        SelectSetting selectSetting{std::move(options), valueAsString(value)};
        if (const auto* defaultString = std::get_if<std::string>(&spec.schema.defaultValue)) {
          selectSetting.clearOnEmpty = defaultString->empty();
        }
        return factory.makeSelect(selectSetting, path);
      }
      case WidgetControlKind::ColorSpec: {
        ColorSpecPickerSetting pickerSetting;
        pickerSetting.selectedValue = valueAsString(value);
        pickerSetting.allowNone = spec.advanced;
        pickerSetting.allowCustomColor = spec.allowCustomColor;
        return factory.makeColorSpecPicker(pickerSetting, path);
      }
      case WidgetControlKind::StringList:
        return nullptr;
      case WidgetControlKind::String:
      case WidgetControlKind::File:
      case WidgetControlKind::Folder:
      case WidgetControlKind::Glyph:
      default:
        return factory.makeText(valueAsString(value), {}, path);
      }
    }

  } // namespace

  void buildPluginSettingsEditor(
      Flex& body, const Config& cfg, SettingsControlFactory& factory, const std::string& pluginId,
      const scripting::PluginManifest& manifest, bool showAdvanced, float scale
  ) {
    scripting::PluginTranslationCatalog translations;
    if (const auto pluginDir = scripting::PluginRegistry::instance().findPluginDir(pluginId)) {
      translations.load(*pluginDir);
    }
    const auto specs = settings::manifestSettingSpecs(manifest.settings, &translations);
    bool rendered = false;
    for (const auto& spec : specs) {
      if (spec.advanced && !showAdvanced) {
        continue;
      }
      if (!pluginSettingVisible(cfg, pluginId, spec, specs)) {
        continue;
      }
      const std::vector<std::string> path = {"plugin_settings", pluginId, spec.schema.key};
      const WidgetSettingValue value = pluginSettingValue(cfg, pluginId, spec);
      SettingEntry entry{
          .section = SettingsSection::Bar,
          .group = "plugin-settings",
          .title = spec.literalLabel,
          .subtitle = spec.literalDescription,
          .path = path,
          .control = TextSetting{},
          .advanced = spec.advanced,
          .searchText = {},
          .visibleWhen = std::nullopt,
      };
      if (spec.control == WidgetControlKind::StringList) {
        factory.makeListBlock(body, entry, ListSetting{.items = valueAsStringList(value)});
      } else {
        factory.makeRow(body, entry, pluginSettingControl(factory, spec, value, path));
      }
      rendered = true;
    }
    if (!rendered) {
      body.addChild(makeLabel(
          i18n::tr("settings.plugins.settings.empty"), Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
      ));
    }
  }

  void addSettingsPlugins(Flex& content, SettingsPluginsContext ctx) {
    if (ctx.selectedSection != "plugins") {
      return;
    }
    const float scale = ctx.scale;

    auto sectionCol = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceLg * scale,
        .fill = clearColorSpec(),
        .fillWidth = true,
    });
    Flex* section = sectionCol.get();
    content.addChild(std::move(sectionCol));

    section->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
            ui::glyph({
                .glyph = "puzzle",
                .glyphSize = Style::fontSizeHeader * scale,
                .color = colorSpecFromRole(ColorRole::Primary),
            }),
            makeLabel(
                i18n::tr("settings.navigation.sections.plugins"), Style::fontSizeHeader * scale, ColorRole::Primary,
                FontWeight::Bold
            )
        )
    );

    // ── Sources ──────────────────────────────────────────────────────────
    auto sourcesHeader = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
    sourcesHeader->addChild(makeLabel(
        i18n::tr("settings.plugins.sources.title"), Style::fontSizeBody * scale, ColorRole::Secondary, FontWeight::Bold
    ));
    sourcesHeader->addChild(ui::spacer());
    sourcesHeader->addChild(
        ui::button({
            .text = i18n::tr("settings.plugins.sources.add"),
            .glyph = "add",
            .fontSize = Style::fontSizeCaption * scale,
            .glyphSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Outline,
            .onClick = [cb = ctx.addSource]() {
              if (cb) {
                cb();
              }
            },
        })
    );
    section->addChild(std::move(sourcesHeader));
    if (ctx.sources.empty()) {
      section->addChild(makeLabel(
          i18n::tr("settings.plugins.sources.empty"), Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
      ));
    }
    std::vector<PluginSourceConfig> sources = ctx.sources;
    std::ranges::stable_sort(sources, [](const auto& a, const auto& b) { return pluginSourceLess(a.name, b.name); });
    for (const auto& source : sources) {
      section->addChild(sourceRow(source, ctx, scale));
    }

    section->addChild(ui::separator({.spacing = Style::spaceSm * scale}));

    // ── Plugins ──────────────────────────────────────────────────────────
    section->addChild(makeLabel(
        i18n::tr("settings.plugins.plugins.title"), Style::fontSizeBody * scale, ColorRole::Secondary, FontWeight::Bold
    ));
    if (ctx.pluginsLoading) {
      section->addChild(makeLabel(
          ctx.plugins.empty() ? i18n::tr("settings.plugins.plugins.loading")
                              : i18n::tr("settings.plugins.plugins.refreshing"),
          Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
      ));
    } else if (ctx.plugins.empty()) {
      section->addChild(makeLabel(
          i18n::tr("settings.plugins.plugins.empty"), Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
      ));
    }
    std::vector<scripting::PluginStatus> plugins = ctx.plugins;
    std::ranges::sort(plugins, [&](const auto& a, const auto& b) {
      const std::string_view aName = pluginDisplayName(a);
      const std::string_view bName = pluginDisplayName(b);
      if (aName != bName) {
        return aName < bName;
      }
      if (a.source != b.source) {
        return pluginSourceLess(a.source, b.source);
      }
      return a.id < b.id;
    });
    for (const auto& plugin : plugins) {
      section->addChild(pluginRow(plugin, ctx, scale));
    }
  }

} // namespace settings
