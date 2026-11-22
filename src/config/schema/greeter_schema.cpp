#include "config/schema/greeter_schema.h"

#include "config/color_spec.h"
#include "config/schema/widget_schema.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "time/time_format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace noctalia::config::schema {
  namespace {
    constexpr std::array<std::string_view, 9> kAnchors = {
        "top-left",     "top-center",  "top-right",     "center-left",  "center",
        "center-right", "bottom-left", "bottom-center", "bottom-right",
    };
    constexpr std::array<std::string_view, 3> kLoginModes = {"auto", "full", "minimal"};
    constexpr std::array<std::string_view, 5> kCoreIds = {"login", "branding", "keyboard", "appearance", "power"};
    constexpr std::array<std::string_view, 5> kWidgetTypes = {"clock", "label", "sticker", "sysmon", "button"};
    constexpr std::array<std::string_view, 6> kSysmonStats = {"",         "cpu_usage", "cpu_temp",
                                                              "cpu_freq", "ram_pct",   "swap_pct"};

    bool validInstanceId(std::string_view id) {
      const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
      };
      return !id.empty()
          && id.size() <= 64
          && alnum(id.front())
          && std::ranges::all_of(id, [&](char c) { return alnum(c) || c == '_' || c == '-'; })
          && !std::ranges::contains(kCoreIds, id)
          && id != "clock";
    }

    double normalizedRotation(double value) {
      constexpr double pi = std::numbers::pi;
      double result = std::fmod(value, 2 * pi);
      if (result >= pi) {
        result -= 2 * pi;
      }
      if (result < -pi) {
        result += 2 * pi;
      }
      return result;
    }

    struct Reader {
      const toml::table& table;
      std::string path;
      Diagnostics& diag;

      void error(std::string_view key, std::string message) const {
        diag.error(joinPath(path, key), std::move(message), "greeter.invalid-profile");
      }
      void keys(std::initializer_list<std::string_view> allowed) const {
        for (const auto& [key, node] : table) {
          (void)node;
          if (std::ranges::find(allowed, key.str()) == allowed.end()) {
            error(key.str(), "unknown greeter setting");
          }
        }
      }
      const toml::table* sub(std::string_view key) const {
        const auto* result = table[key].as_table();
        if (table.contains(key) && result == nullptr) {
          error(key, "expected a table");
        }
        return result;
      }
      std::optional<std::string> string(std::string_view key, bool required = false) const {
        if (!table.contains(key) && !required) {
          return std::nullopt;
        }
        auto result = table[key].value<std::string>();
        if (!result) {
          error(key, "expected a string");
        }
        return result;
      }
      std::optional<std::string> output() const {
        auto result = string("output");
        if (result && result->empty()) {
          error("output", "output must be a nonempty connector or stable identity");
        }
        return result;
      }
      std::optional<bool> boolean(std::string_view key) const {
        if (!table.contains(key)) {
          return std::nullopt;
        }
        auto result = table[key].value_exact<bool>();
        if (!result) {
          error(key, "expected a boolean");
        }
        return result;
      }
      std::optional<double>
      number(std::string_view key, double min, double max, bool required = false, bool natural = false) const {
        if (!table.contains(key) && !required) {
          return std::nullopt;
        }
        auto result = table[key].value<double>();
        if (!table[key].is_number()
            || !result
            || !std::isfinite(*result)
            || ((!natural || *result != 0) && (*result < min || *result > max))) {
          error(
              key,
              "expected a finite number in "
                  + std::to_string(min)
                  + ".."
                  + std::to_string(max)
                  + (natural ? " or 0 for natural size" : "")
          );
          return std::nullopt;
        }
        return result;
      }
      std::optional<double> rotation() const {
        if (!table.contains("rotation")) {
          return std::nullopt;
        }
        const auto result = table["rotation"].value<double>();
        if (!table["rotation"].is_number() || !result || !std::isfinite(*result)) {
          error("rotation", "expected finite radians");
          return std::nullopt;
        }
        return normalizedRotation(*result);
      }
      template <typename Enum, std::size_t Size>
      std::optional<Enum>
      enumeration(std::string_view key, const std::array<std::string_view, Size>& values, bool required = false) const {
        auto value = string(key, required);
        if (!value) {
          return std::nullopt;
        }
        const auto it = std::ranges::find(values, *value);
        if (it == values.end()) {
          error(key, "unknown value: " + *value);
          return std::nullopt;
        }
        return static_cast<Enum>(std::distance(values.begin(), it));
      }
    };

    GreeterCoreOverride readCore(const Reader& r, std::string_view id) {
      r.keys({"visible", "anchor", "x", "y", "width", "output", "rotation", "flip_x", "flip_y", "scale"});
      GreeterCoreOverride core;
      core.visible = r.boolean("visible");
      core.anchor = r.enumeration<GreeterAnchor>("anchor", kAnchors);
      core.x = r.number("x", -8192, 8192);
      core.y = r.number("y", -8192, 8192);
      core.width = r.number("width", id == "login" ? 280 : 32, id == "login" ? 640 : 1600);
      core.output = r.output();
      core.rotation = r.rotation();
      core.flipX = r.boolean("flip_x");
      core.flipY = r.boolean("flip_y");
      core.scale = r.number("scale", 0.5, 4);
      if (id == "login") {
        if (core.visible == false) {
          r.error("visible", "login cannot be hidden");
        }
        if (r.table.contains("output")) {
          r.error("output", "login must remain on every greeter output");
        }
      }
      return core;
    }

    WidgetSettingSchema widgetSchema(std::string_view type) {
      if (!std::ranges::contains(kWidgetTypes, type)) {
        return {};
      }
      auto fields = desktop_settings::desktopWidgetSettingSchema(type);
      if (type == "button") {
        std::erase_if(fields, [](const auto& field) { return field.key == "command"; });
        fields.push_back(
            {.key = "action",
             .type = WidgetSettingType::Enum,
             .defaultValue = std::string("none"),
             .enumValues = {"none", "power", "appearance", "sessions", "suspend", "reboot", "shutdown", "firmware"}}
        );
      } else if (type == "sysmon") {
        std::erase_if(fields, [](const auto& field) {
          return field.key == "interface" || field.key.starts_with("network_");
        });
        for (auto& field : fields) {
          if (field.key == "stat" || field.key == "stat2") {
            std::erase_if(field.enumValues, [](const auto& value) {
              return !std::ranges::contains(kSysmonStats, value);
            });
          }
        }
      }
      return fields;
    }

    std::optional<WidgetSettingValue> readSetting(const Reader& r, const WidgetSettingField& field) {
      const auto node = r.table[field.key];
      switch (field.type) {
      case WidgetSettingType::Bool:
        if (auto value = r.boolean(field.key)) {
          return *value;
        }
        return std::nullopt;
      case WidgetSettingType::Int:
        if (!node.is_integer()) {
          r.error(field.key, "expected an integer");
          return std::nullopt;
        }
        [[fallthrough]];
      case WidgetSettingType::Double:
      case WidgetSettingType::OptionalDouble: {
        const auto value = r.number(field.key, field.minValue.value_or(-1.0e30), field.maxValue.value_or(1.0e30));
        if (!value) {
          return std::nullopt;
        }
        if (field.type == WidgetSettingType::Int) {
          return *node.value<std::int64_t>();
        }
        return *value;
      }
      case WidgetSettingType::String:
      case WidgetSettingType::Enum:
      case WidgetSettingType::Color: {
        auto value = r.string(field.key);
        if (!value) {
          return std::nullopt;
        }
        if (field.type == WidgetSettingType::Enum && !std::ranges::contains(field.enumValues, *value)) {
          r.error(field.key, "unsupported value for this greeter widget: " + *value);
          return std::nullopt;
        }
        if (field.type == WidgetSettingType::Color) {
          const auto* defaultColor = std::get_if<std::string>(&field.defaultValue);
          if (value->empty() && defaultColor != nullptr && defaultColor->empty()) {
            return *value;
          }
          try {
            colorSpecFromConfigString(*value);
          } catch (const std::exception& e) {
            r.error(field.key, e.what());
            return std::nullopt;
          }
        }
        return *value;
      }
      case WidgetSettingType::StringList:
      case WidgetSettingType::StringMap:
        r.error(field.key, "setting type is unavailable in the greeter catalog");
        return std::nullopt;
      }
      return std::nullopt;
    }

    GreeterWidgetConfig readWidget(const Reader& r, std::string id) {
      GreeterWidgetConfig widget;
      widget.id = std::move(id);
      widget.type = r.string("type", true).value_or("");
      const bool supported = std::ranges::contains(kWidgetTypes, widget.type);
      if (!supported) {
        r.error("type", "type is unavailable in the greeter catalog: " + widget.type);
      }
      widget.enabled = r.boolean("enabled").value_or(true);
      widget.output = r.output();
      widget.boxWidth = r.number("box_width", 16, 4096, false, true).value_or(0);
      widget.boxHeight = r.number("box_height", 16, 4096, false, true).value_or(0);
      widget.rotation = r.rotation().value_or(0);
      widget.flipX = r.boolean("flip_x").value_or(false);
      widget.flipY = r.boolean("flip_y").value_or(false);
      const auto placement = r.string("placement", true);
      if (placement == "anchor") {
        r.keys(
            {"type", "enabled", "output", "box_width", "box_height", "rotation", "flip_x", "flip_y", "settings",
             "placement", "anchor", "x", "y"}
        );
        widget.placement = GreeterAnchorPlacement{
            r.enumeration<GreeterAnchor>("anchor", kAnchors, true).value_or(GreeterAnchor::Center),
            r.number("x", -8192, 8192).value_or(0), r.number("y", -8192, 8192).value_or(0)
        };
      } else if (placement == "relative") {
        r.keys(
            {"type", "enabled", "output", "box_width", "box_height", "rotation", "flip_x", "flip_y", "settings",
             "placement", "cx", "cy", "placement_width", "placement_height"}
        );
        widget.placement = GreeterRelativePlacement{
            r.number("cx", -65536, 65536, true).value_or(0), r.number("cy", -65536, 65536, true).value_or(0),
            r.number("placement_width", 1, 65536, true).value_or(1),
            r.number("placement_height", 1, 65536, true).value_or(1)
        };
      } else if (placement) {
        r.error("placement", "expected anchor or relative");
      }
      if (const auto* settings = r.sub("settings"); settings != nullptr && supported) {
        const Reader sr{*settings, joinPath(r.path, "settings"), r.diag};
        const auto fields = widgetSchema(widget.type);
        for (const auto& [key, node] : *settings) {
          (void)node;
          const auto field = std::ranges::find(fields, key.str(), &WidgetSettingField::key);
          if (field == fields.end()) {
            sr.error(key.str(), "setting is unavailable for this greeter widget");
            continue;
          }
          if (auto value = readSetting(sr, *field)) {
            if (widget.type == "clock" && key == "timezone" && !isValidTimezone(std::get<std::string>(*value))) {
              sr.error(key.str(), "unknown timezone: " + std::get<std::string>(*value));
            }
            widget.settings.emplace(key.str(), std::move(*value));
          }
        }
      }
      return widget;
    }

    GreeterWidgetCollection readCollection(const Reader& r) {
      r.keys({"widget_order", "widget"});
      GreeterWidgetCollection result;
      if (r.table.empty()) {
        return result;
      }
      result.instances.emplace();
      const auto* order = r.table["widget_order"].as_array();
      if (order == nullptr) {
        r.error("widget_order", "a custom collection requires an explicit widget_order array");
      }
      const auto* records = r.sub("widget");
      if (order != nullptr && order->size() > 64) {
        r.error("widget_order", "at most 64 widget instances are allowed");
      }
      if (records != nullptr && records->size() > 64) {
        r.error("widget", "at most 64 widget instances are allowed");
      }
      std::map<std::string, GreeterWidgetConfig> parsed;
      if (records != nullptr && records->size() <= 64) {
        for (const auto& [id, node] : *records) {
          const auto path = joinPath(joinPath(r.path, "widget"), id.str());
          if (!validInstanceId(id.str())) {
            r.diag.error(path, "invalid or reserved greeter widget instance ID");
          }
          if (const auto* table = node.as_table()) {
            parsed.emplace(id.str(), readWidget(Reader{*table, path, r.diag}, std::string(id.str())));
          } else {
            r.diag.error(path, "expected a widget table");
          }
        }
      }
      std::set<std::string> seen;
      if (order != nullptr && order->size() <= 64) {
        for (const auto& node : *order) {
          const auto id = node.value<std::string>();
          if (!id || !validInstanceId(*id) || !seen.insert(*id).second) {
            r.error("widget_order", "expected unique, valid widget instance IDs");
            continue;
          }
          if (const auto it = parsed.find(*id); it != parsed.end()) {
            result.instances->push_back(it->second);
          } else {
            r.error("widget_order", "missing widget record: " + *id);
          }
        }
      }
      for (const auto& [id, widget] : parsed) {
        (void)widget;
        if (!seen.contains(id)) {
          r.error("widget_order", "unlisted widget record: " + id);
        }
      }
      return result;
    }

    template <typename T> void insertOptional(toml::table& table, std::string_view key, const std::optional<T>& value) {
      if (value) {
        table.insert_or_assign(key, *value);
      }
    }
    template <typename Enum, std::size_t Size>
    std::string_view enumName(Enum value, const std::array<std::string_view, Size>& names) {
      const auto index = static_cast<std::size_t>(value);
      return index < names.size() ? names[index] : "";
    }
    toml::table writeWidget(const GreeterWidgetConfig& widget) {
      toml::table table{{"type", widget.type},          {"enabled", widget.enabled},
                        {"box_width", widget.boxWidth}, {"box_height", widget.boxHeight},
                        {"rotation", widget.rotation},  {"flip_x", widget.flipX},
                        {"flip_y", widget.flipY}};
      insertOptional(table, "output", widget.output);
      if (const auto* anchor = std::get_if<GreeterAnchorPlacement>(&widget.placement)) {
        table.insert_or_assign("placement", "anchor");
        table.insert_or_assign("anchor", enumName(anchor->anchor, kAnchors));
        table.insert_or_assign("x", anchor->x);
        table.insert_or_assign("y", anchor->y);
      } else {
        const auto& relative = std::get<GreeterRelativePlacement>(widget.placement);
        table.insert_or_assign("placement", "relative");
        table.insert_or_assign("cx", relative.cx);
        table.insert_or_assign("cy", relative.cy);
        table.insert_or_assign("placement_width", relative.placementWidth);
        table.insert_or_assign("placement_height", relative.placementHeight);
      }
      if (!widget.settings.empty()) {
        toml::table settings;
        for (const auto& [key, value] : widget.settings) {
          std::visit(
              [&](const auto& concrete) {
                using T = std::decay_t<decltype(concrete)>;
                if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                  toml::array array;
                  for (const auto& entry : concrete) {
                    array.push_back(entry);
                  }
                  settings.insert_or_assign(key, std::move(array));
                } else if constexpr (std::is_same_v<T, WidgetSettingStringMap>) {
                  toml::table map;
                  for (const auto& [name, entry] : concrete) {
                    map.insert_or_assign(name, entry);
                  }
                  settings.insert_or_assign(key, std::move(map));
                } else {
                  settings.insert_or_assign(key, concrete);
                }
              },
              value
          );
        }
        table.insert_or_assign("settings", std::move(settings));
      }
      return table;
    }
  } // namespace

  std::optional<GreeterProfile>
  readGreeterConfig(const toml::table& table, std::string_view path, Diagnostics& diagnostics) {
    const auto first = diagnostics.entries.size();
    const Reader r{table, std::string(path), diagnostics};
    r.keys({"layout", "widgets"});
    GreeterProfile config;
    if (const auto* layout = r.sub("layout")) {
      config.layout.emplace();
      const Reader lr{*layout, joinPath(path, "layout"), diagnostics};
      config.layout->loginMode = lr.enumeration<GreeterLoginMode>("login_mode", kLoginModes);
      for (const auto& [id, node] : *layout) {
        if (id == "outputs") {
          const auto* outputs = node.as_table();
          if (!outputs || outputs->size() > 32) {
            lr.error("outputs", "expected at most 32 output layouts");
            continue;
          }
          for (const auto& [name, output] : *outputs) {
            const auto* local = output.as_table();
            if (name.str().empty() || name.str().size() > 512 || !local || local->contains("outputs")) {
              lr.error("outputs", "invalid output layout");
              continue;
            }
            const toml::table wrapped{{"layout", *local}};
            if (auto profile =
                    readGreeterConfig(wrapped, joinPath(lr.path, "outputs." + std::string(name.str())), diagnostics)) {
              config.layout->outputs.emplace(name.str(), std::move(*profile->layout));
            }
          }
          continue;
        }
        if (id == "login_mode") {
          continue;
        }
        if (!std::ranges::contains(kCoreIds, id.str())) {
          lr.error(id.str(), "unknown greeter core component");
          continue;
        }
        if (const auto* component = node.as_table()) {
          config.layout->components.emplace(
              id.str(), readCore(Reader{*component, joinPath(lr.path, id.str()), diagnostics}, id.str())
          );
        } else {
          lr.error(id.str(), "expected a component table");
        }
      }
    }
    if (const auto* widgets = r.sub("widgets")) {
      config.widgets = readCollection(Reader{*widgets, joinPath(path, "widgets"), diagnostics});
    }
    if (diagnostics.entries.size() != first) {
      for (auto index = first; index < diagnostics.entries.size(); ++index) {
        diagnostics.entries[index].recoveryScope = Diagnostics::RecoveryScope::Component;
        diagnostics.entries[index].ownerPath = std::string(path);
      }
      return std::nullopt;
    }
    return config;
  }

  WidgetSettingSchema greeterWidgetSettingSchema(std::string_view type) { return widgetSchema(type); }

  toml::table writeGreeterConfig(const GreeterProfile& config) {
    toml::table table;
    if (config.layout) {
      toml::table layout;
      if (config.layout->loginMode) {
        layout.insert_or_assign("login_mode", enumName(*config.layout->loginMode, kLoginModes));
      }
      for (const auto& [id, core] : config.layout->components) {
        toml::table component;
        insertOptional(component, "visible", core.visible);
        if (core.anchor) {
          component.insert_or_assign("anchor", enumName(*core.anchor, kAnchors));
        }
        insertOptional(component, "x", core.x);
        insertOptional(component, "y", core.y);
        insertOptional(component, "width", core.width);
        insertOptional(component, "output", core.output);
        insertOptional(component, "rotation", core.rotation);
        insertOptional(component, "flip_x", core.flipX);
        insertOptional(component, "flip_y", core.flipY);
        insertOptional(component, "scale", core.scale);
        layout.insert_or_assign(id, std::move(component));
      }
      if (!config.layout->outputs.empty()) {
        toml::table outputs;
        for (const auto& [name, local] : config.layout->outputs) {
          GreeterProfile nested;
          nested.layout = local;
          auto serialized = writeGreeterConfig(nested);
          outputs.insert(name, std::move(*serialized["layout"].as_table()));
        }
        layout.insert("outputs", std::move(outputs));
      }
      table.insert_or_assign("layout", std::move(layout));
    }
    if (config.widgets) {
      toml::table collection;
      if (config.widgets->instances) {
        toml::array order;
        toml::table widgets;
        for (const auto& widget : *config.widgets->instances) {
          order.push_back(widget.id);
          widgets.insert_or_assign(widget.id, writeWidget(widget));
        }
        collection.insert_or_assign("widget_order", std::move(order));
        if (!widgets.empty()) {
          collection.insert_or_assign("widget", std::move(widgets));
        }
      }
      table.insert_or_assign("widgets", std::move(collection));
    }
    return table;
  }
} // namespace noctalia::config::schema
