#pragma once

#include <array>
#include <string_view>

namespace noctalia::theme {

  // Canonical list of built-in color tokens emitted in every generated palette.
  // The order is the iteration order used by json_output. Both the M3 schemes
  // and the custom schemes populate the same key set so consumers can treat
  // them interchangeably. `source_color` is the seed color used to generate
  // the scheme and is exposed for template compatibility.
  inline constexpr auto kTokens = std::to_array<std::string_view>({
      "source_color",
      "primary",
      "on_primary",
      "primary_container",
      "on_primary_container",
      "primary_fixed",
      "primary_fixed_dim",
      "on_primary_fixed",
      "on_primary_fixed_variant",
      "surface_tint",
      "secondary",
      "on_secondary",
      "secondary_container",
      "on_secondary_container",
      "secondary_fixed",
      "secondary_fixed_dim",
      "on_secondary_fixed",
      "on_secondary_fixed_variant",
      "tertiary",
      "on_tertiary",
      "tertiary_container",
      "on_tertiary_container",
      "tertiary_fixed",
      "tertiary_fixed_dim",
      "on_tertiary_fixed",
      "on_tertiary_fixed_variant",
      "error",
      "on_error",
      "error_container",
      "on_error_container",
      "surface",
      "on_surface",
      "surface_variant",
      "on_surface_variant",
      "surface_dim",
      "surface_bright",
      "surface_container_lowest",
      "surface_container_low",
      "surface_container",
      "surface_container_high",
      "surface_container_highest",
      "outline",
      "outline_variant",
      "shadow",
      "hover",
      "on_hover",
      "scrim",
      "inverse_surface",
      "inverse_on_surface",
      "inverse_primary",
      "background",
      "on_background",
      "terminal_foreground",
      "terminal_background",
      "terminal_cursor",
      "terminal_cursor_text",
      "terminal_selection_fg",
      "terminal_selection_bg",
      "terminal_normal_black",
      "terminal_normal_red",
      "terminal_normal_green",
      "terminal_normal_yellow",
      "terminal_normal_blue",
      "terminal_normal_magenta",
      "terminal_normal_cyan",
      "terminal_normal_white",
      "terminal_bright_black",
      "terminal_bright_red",
      "terminal_bright_green",
      "terminal_bright_yellow",
      "terminal_bright_blue",
      "terminal_bright_magenta",
      "terminal_bright_cyan",
      "terminal_bright_white",
  });

  inline constexpr size_t kTokenCount = kTokens.size();

} // namespace noctalia::theme
