#include "scripting/plugin_layer_config.h"

#include <utility>

namespace scripting {

  namespace {

    // An integer TOML value within [min, max], or nullopt.
    std::optional<std::int64_t>
    integerIn(const toml::node_view<const toml::node>& node, std::int64_t min, std::int64_t max) {
      const auto* integer = node.as_integer();
      if (integer == nullptr || integer->get() < min || integer->get() > max) {
        return std::nullopt;
      }
      return integer->get();
    }

    constexpr std::int64_t kMaxExtent = 1 << 16;

  } // namespace

  void applyPluginLayerPatch(PluginLayerConfig& config, const PluginLayerConfigPatch& patch) {
    if (patch.layer.has_value()) {
      config.layer = *patch.layer;
    }
    if (patch.anchors.has_value()) {
      config.anchors = *patch.anchors;
    }
    if (patch.width.has_value()) {
      config.width = *patch.width;
    }
    if (patch.height.has_value()) {
      config.height = *patch.height;
    }
    if (patch.margins.has_value()) {
      config.marginTop = patch.margins->top;
      config.marginRight = patch.margins->right;
      config.marginBottom = patch.margins->bottom;
      config.marginLeft = patch.margins->left;
    }
    if (patch.exclusiveZone.has_value()) {
      config.exclusiveZoneAuto = !patch.exclusiveZone->has_value();
      config.exclusiveZone = patch.exclusiveZone->value_or(0);
    }
    if (patch.keyboard.has_value()) {
      config.keyboard = *patch.keyboard;
    }
    if (patch.clickThrough.has_value()) {
      config.clickThrough = *patch.clickThrough;
    }
    if (patch.output.has_value()) {
      config.output = *patch.output;
    }
  }

  std::optional<PluginLayerLevel> parsePluginLayerLevel(std::string_view value) {
    if (value == "background") {
      return PluginLayerLevel::Background;
    }
    if (value == "bottom") {
      return PluginLayerLevel::Bottom;
    }
    if (value == "top") {
      return PluginLayerLevel::Top;
    }
    if (value == "overlay") {
      return PluginLayerLevel::Overlay;
    }
    return std::nullopt;
  }

  std::optional<PluginLayerKeyboard> parsePluginLayerKeyboard(std::string_view value) {
    if (value == "none") {
      return PluginLayerKeyboard::None;
    }
    if (value == "on_demand") {
      return PluginLayerKeyboard::OnDemand;
    }
    if (value == "exclusive") {
      return PluginLayerKeyboard::Exclusive;
    }
    return std::nullopt;
  }

  std::optional<std::uint32_t> parsePluginLayerAnchor(std::string_view value) {
    if (value == "top") {
      return PluginLayerAnchor::Top;
    }
    if (value == "bottom") {
      return PluginLayerAnchor::Bottom;
    }
    if (value == "left") {
      return PluginLayerAnchor::Left;
    }
    if (value == "right") {
      return PluginLayerAnchor::Right;
    }
    return std::nullopt;
  }

  bool parsePluginLayerTable(const toml::table& table, PluginLayerConfig& config, std::string& error) {
    if (const auto node = table["layer"]) {
      const auto* value = node.as_string();
      const auto level = value != nullptr ? parsePluginLayerLevel(value->get()) : std::nullopt;
      if (!level.has_value()) {
        error = R"(layer must be "background", "bottom", "top", or "overlay")";
        return false;
      }
      config.layer = *level;
    }

    if (const auto node = table["anchors"]) {
      const auto* array = node.as_array();
      if (array == nullptr) {
        error = R"(anchors must be an array of "top", "bottom", "left", "right")";
        return false;
      }
      std::uint32_t anchors = 0;
      for (const auto& element : *array) {
        const auto* value = element.as_string();
        const auto anchor = value != nullptr ? parsePluginLayerAnchor(value->get()) : std::nullopt;
        if (!anchor.has_value() || (anchors & *anchor) != 0) {
          error = R"(anchors must list distinct values from "top", "bottom", "left", "right")";
          return false;
        }
        anchors |= *anchor;
      }
      config.anchors = anchors;
    }

    for (const auto& [key, target] : {std::pair{"width", &config.width}, std::pair{"height", &config.height}}) {
      if (const auto node = table[key]) {
        const auto value = integerIn(node, 0, kMaxExtent);
        if (!value.has_value()) {
          error = std::string(key) + " must be an integer between 0 and " + std::to_string(kMaxExtent);
          return false;
        }
        *target = static_cast<std::uint32_t>(*value);
      }
    }

    if (const auto node = table["margins"]) {
      const auto* margins = node.as_table();
      if (margins == nullptr) {
        error = "margins must be a table with integer top, right, bottom, left";
        return false;
      }
      for (const auto& [key, value] : *margins) {
        std::int32_t* target = nullptr;
        if (key == "top") {
          target = &config.marginTop;
        } else if (key == "right") {
          target = &config.marginRight;
        } else if (key == "bottom") {
          target = &config.marginBottom;
        } else if (key == "left") {
          target = &config.marginLeft;
        }
        const auto* integer = value.as_integer();
        if (target == nullptr || integer == nullptr || integer->get() < -kMaxExtent || integer->get() > kMaxExtent) {
          error = "margins must be a table with integer top, right, bottom, left";
          return false;
        }
        *target = static_cast<std::int32_t>(integer->get());
      }
    }

    if (const auto node = table["exclusive_zone"]) {
      if (const auto* value = node.as_string(); value != nullptr && value->get() == "auto") {
        config.exclusiveZoneAuto = true;
        config.exclusiveZone = 0;
      } else if (const auto zone = integerIn(node, -1, kMaxExtent); zone.has_value()) {
        config.exclusiveZoneAuto = false;
        config.exclusiveZone = static_cast<std::int32_t>(*zone);
      } else {
        error = R"(exclusive_zone must be "auto" or an integer >= -1)";
        return false;
      }
    }

    if (const auto node = table["keyboard"]) {
      const auto* value = node.as_string();
      const auto keyboard = value != nullptr ? parsePluginLayerKeyboard(value->get()) : std::nullopt;
      if (!keyboard.has_value()) {
        error = R"(keyboard must be "none", "on_demand", or "exclusive")";
        return false;
      }
      config.keyboard = *keyboard;
    }

    if (const auto node = table["click_through"]) {
      const auto* value = node.as_boolean();
      if (value == nullptr) {
        error = "click_through must be a bool";
        return false;
      }
      config.clickThrough = value->get();
    }

    if (const auto node = table["output"]) {
      const auto* value = node.as_string();
      if (value == nullptr || value->get().empty()) {
        error = R"(output must be "all", "focused", or an output name)";
        return false;
      }
      config.output = value->get();
    }

    return true;
  }

} // namespace scripting
