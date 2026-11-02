#pragma once

#include "core/toml.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scripting {

  enum class PluginLayerLevel : std::uint8_t {
    Background,
    Bottom,
    Top,
    Overlay,
  };

  enum class PluginLayerKeyboard : std::uint8_t {
    None,
    OnDemand,
    Exclusive,
  };

  // Anchor bits, numerically identical to zwlr_layer_surface_v1 anchors.
  namespace PluginLayerAnchor {
    inline constexpr std::uint32_t Top = 1;
    inline constexpr std::uint32_t Bottom = 2;
    inline constexpr std::uint32_t Left = 4;
    inline constexpr std::uint32_t Right = 8;
  } // namespace PluginLayerAnchor

  // Surface properties of a [[layer]] entry. Declared in plugin.toml and changed at runtime with
  // noctalia.layer.configure(). Sizes and margins are logical pixels.
  struct PluginLayerConfig {
    PluginLayerLevel layer = PluginLayerLevel::Top;
    std::uint32_t anchors = 0;
    // 0 stretches the axis when anchored to both of its edges, otherwise sizes it to the content.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    // "auto" reserves the surface's extent along its single anchored edge.
    bool exclusiveZoneAuto = false;
    std::int32_t exclusiveZone = 0;
    PluginLayerKeyboard keyboard = PluginLayerKeyboard::None;
    bool clickThrough = false;
    // "all", "focused" (resolved when shown), or an output connector name.
    std::string output = "all";

    bool operator==(const PluginLayerConfig&) const = default;
  };

  struct PluginLayerMargins {
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::int32_t left = 0;
  };

  struct PluginLayerConfigPatch {
    std::optional<PluginLayerLevel> layer;
    std::optional<std::uint32_t> anchors;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<PluginLayerMargins> margins;
    // nullopt inner value means "auto".
    std::optional<std::optional<std::int32_t>> exclusiveZone;
    std::optional<PluginLayerKeyboard> keyboard;
    std::optional<bool> clickThrough;
    std::optional<std::string> output;
  };

  enum class PluginLayerCommand : std::uint8_t {
    Show,
    Hide,
    Configure,
  };

  void applyPluginLayerPatch(PluginLayerConfig& config, const PluginLayerConfigPatch& patch);

  [[nodiscard]] std::optional<PluginLayerLevel> parsePluginLayerLevel(std::string_view value);
  [[nodiscard]] std::optional<PluginLayerKeyboard> parsePluginLayerKeyboard(std::string_view value);
  [[nodiscard]] std::optional<std::uint32_t> parsePluginLayerAnchor(std::string_view value);

  // Parses the surface keys of a [[layer]] table into `config` (keys absent keep their value).
  // Returns false and sets `error` on an invalid value.
  bool parsePluginLayerTable(const toml::table& table, PluginLayerConfig& config, std::string& error);

} // namespace scripting
