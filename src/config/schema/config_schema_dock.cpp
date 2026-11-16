#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"

#include <string_view>

namespace noctalia::config::schema {

  namespace {
    const Schema<DockMonitorOverride>& dockMonitorOverrideSchema() {
      static const Schema<DockMonitorOverride> s = {
          field(&DockMonitorOverride::match, "match"),
          optionalBoolField(&DockMonitorOverride::enabled, "enabled"),
          optionalEnumField(&DockMonitorOverride::position, "position", kDockEdges),
          optionalBoolField(&DockMonitorOverride::activeMonitorOnly, "active_monitor_only"),
          optionalIntField(&DockMonitorOverride::iconSize, "icon_size", kDockIconSizeRange),
          optionalBoolField(&DockMonitorOverride::iconShadow, "icon_shadow"),
          optionalFloatField(&DockMonitorOverride::iconSaturation, "icon_saturation", kUnitRange),
          optionalBoolField(&DockMonitorOverride::colorizeIcons, "colorize_icons"),
          optionalIntField(&DockMonitorOverride::mainAxisPadding, "main_axis_padding", kDockPaddingRange),
          optionalIntField(&DockMonitorOverride::crossAxisPadding, "cross_axis_padding", kDockPaddingRange),
          optionalIntField(&DockMonitorOverride::itemSpacing, "item_spacing", kDockItemSpacingRange),
          optionalFloatField(&DockMonitorOverride::backgroundOpacity, "background_opacity", kUnitRange),
          optionalBoolField(&DockMonitorOverride::compositorBlur, "compositor_blur"),
          optionalColorField(&DockMonitorOverride::border, "border"),
          optionalFloatField(&DockMonitorOverride::borderWidth, "border_width", kDockBorderWidthRange),
          optionalIntField(&DockMonitorOverride::radius, "radius", kDockRadiusRange),
          optionalIntField(&DockMonitorOverride::radiusTopLeft, "radius_top_left", kDockRadiusRange),
          optionalIntField(&DockMonitorOverride::radiusTopRight, "radius_top_right", kDockRadiusRange),
          optionalIntField(&DockMonitorOverride::radiusBottomLeft, "radius_bottom_left", kDockRadiusRange),
          optionalIntField(&DockMonitorOverride::radiusBottomRight, "radius_bottom_right", kDockRadiusRange),
          optionalBoolField(&DockMonitorOverride::concaveEdgeCorners, "concave_edge_corners"),
          optionalIntField(&DockMonitorOverride::marginEnds, "margin_ends", kDockMarginEndsRange),
          optionalIntField(&DockMonitorOverride::marginEdge, "margin_edge", kDockMarginEdgeRange),
          optionalBoolField(&DockMonitorOverride::shadow, "shadow"),
          optionalBoolField(&DockMonitorOverride::showRunning, "show_running"),
          optionalBoolField(&DockMonitorOverride::autoHide, "auto_hide"),
          optionalBoolField(&DockMonitorOverride::smartAutoHide, "smart_auto_hide"),
          optionalIntField(
              &DockMonitorOverride::autoHideRevealDelayMs, "auto_hide_reveal_delay_ms", kDockAutoHideRevealDelayMsRange
          ),
          custom<DockMonitorOverride>(
              "layer",
              [](const toml::table& tbl, DockMonitorOverride& out, std::string_view parentPath, Diagnostics& diag) {
                if (auto v = tbl["layer"].value<std::string>()) {
                  if (*v == "top" || *v == "overlay") {
                    out.layer = *v;
                  } else {
                    diag.warn(joinPath(parentPath, "layer"), "expected top or overlay, got \"" + *v + "\"");
                  }
                } else {
                  warnWrongType(tbl, "layer", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const DockMonitorOverride& in) {
                if (in.layer) {
                  tbl.insert_or_assign("layer", *in.layer);
                }
              }
          ),
          optionalBoolField(&DockMonitorOverride::reserveSpace, "reserve_space"),
          optionalFloatField(&DockMonitorOverride::activeScale, "active_scale", kDockActiveScaleRange),
          optionalFloatField(&DockMonitorOverride::inactiveScale, "inactive_scale", kDockInactiveScaleRange),
          optionalBoolField(&DockMonitorOverride::magnification, "magnification"),
          optionalFloatField(
              &DockMonitorOverride::magnificationScale, "magnification_scale", kDockMagnificationScaleRange
          ),
          optionalFloatField(&DockMonitorOverride::activeOpacity, "active_opacity", kUnitRange),
          optionalFloatField(&DockMonitorOverride::inactiveOpacity, "inactive_opacity", kUnitRange),
          optionalBoolField(&DockMonitorOverride::showDots, "show_dots"),
          optionalBoolField(&DockMonitorOverride::showInstanceCount, "show_instance_count"),
          optionalEnumField(&DockMonitorOverride::launcherPosition, "launcher_position", kDockLauncherPositions),
          field(&DockMonitorOverride::launcherIcon, "launcher_icon"),
          optionalPathStringField(&DockMonitorOverride::launcherCustomImage, "launcher_custom_image"),
          optionalBoolField(&DockMonitorOverride::launcherCustomImageColorize, "launcher_custom_image_colorize"),
          optionalStringVectorField(&DockMonitorOverride::pinned, "pinned"),
      };
      return s;
    }
  } // namespace

  const Schema<DockConfig>& dockSchema() {
    static const Schema<DockConfig> s = {
        field(&DockConfig::enabled, "enabled"),
        enumField(&DockConfig::position, "position", kDockEdges),
        field(&DockConfig::activeMonitorOnly, "active_monitor_only"),
        field(&DockConfig::iconSize, "icon_size", kDockIconSizeRange),
        field(&DockConfig::iconShadow, "icon_shadow"),
        field(&DockConfig::iconSaturation, "icon_saturation", kUnitRange),
        field(&DockConfig::colorizeIcons, "colorize_icons"),
        field(&DockConfig::mainAxisPadding, "main_axis_padding", kDockPaddingRange),
        field(&DockConfig::crossAxisPadding, "cross_axis_padding", kDockPaddingRange),
        field(&DockConfig::itemSpacing, "item_spacing", kDockItemSpacingRange),
        field(&DockConfig::backgroundOpacity, "background_opacity", kUnitRange),
        field(&DockConfig::compositorBlur, "compositor_blur"),
        colorField(&DockConfig::border, "border"),
        field(&DockConfig::borderWidth, "border_width", kDockBorderWidthRange),
        custom<DockConfig>(
            "radius",
            [](const toml::table& tbl, DockConfig& out, std::string_view parentPath, Diagnostics& diag) {
              if (auto v = tbl["radius"].value<std::int64_t>()) {
                const auto radius = static_cast<std::int32_t>(applyRange<std::int64_t>(*v, kDockRadiusRange));
                out.radius = radius;
                out.radiusTopLeft = radius;
                out.radiusTopRight = radius;
                out.radiusBottomLeft = radius;
                out.radiusBottomRight = radius;
              } else {
                warnWrongType(tbl, "radius", parentPath, diag, "an integer");
              }
            },
            [](toml::table& tbl, const DockConfig& in) {
              tbl.insert_or_assign("radius", static_cast<std::int64_t>(in.radius));
            }
        ),
        field(&DockConfig::radiusTopLeft, "radius_top_left", kDockRadiusRange),
        field(&DockConfig::radiusTopRight, "radius_top_right", kDockRadiusRange),
        field(&DockConfig::radiusBottomLeft, "radius_bottom_left", kDockRadiusRange),
        field(&DockConfig::radiusBottomRight, "radius_bottom_right", kDockRadiusRange),
        field(&DockConfig::concaveEdgeCorners, "concave_edge_corners"),
        field(&DockConfig::marginEnds, "margin_ends", kDockMarginEndsRange),
        field(&DockConfig::marginEdge, "margin_edge", kDockMarginEdgeRange),
        field(&DockConfig::shadow, "shadow"),
        field(&DockConfig::showRunning, "show_running"),
        field(&DockConfig::autoHide, "auto_hide"),
        field(&DockConfig::smartAutoHide, "smart_auto_hide"),
        field(&DockConfig::autoHideRevealDelayMs, "auto_hide_reveal_delay_ms", kDockAutoHideRevealDelayMsRange),
        custom<DockConfig>(
            "layer",
            [](const toml::table& tbl, DockConfig& out, std::string_view parentPath, Diagnostics& diag) {
              if (auto v = tbl["layer"].value<std::string>()) {
                if (*v == "top" || *v == "overlay") {
                  out.layer = *v;
                } else {
                  diag.warn(joinPath(parentPath, "layer"), "expected top or overlay, got \"" + *v + "\"");
                }
              } else {
                warnWrongType(tbl, "layer", parentPath, diag, "a string");
              }
            },
            [](toml::table& tbl, const DockConfig& in) { tbl.insert_or_assign("layer", in.layer); }
        ),
        field(&DockConfig::reserveSpace, "reserve_space"),
        field(&DockConfig::activeScale, "active_scale", kDockActiveScaleRange),
        field(&DockConfig::inactiveScale, "inactive_scale", kDockInactiveScaleRange),
        field(&DockConfig::magnification, "magnification"),
        field(&DockConfig::magnificationScale, "magnification_scale", kDockMagnificationScaleRange),
        field(&DockConfig::activeOpacity, "active_opacity", kUnitRange),
        field(&DockConfig::inactiveOpacity, "inactive_opacity", kUnitRange),
        field(&DockConfig::showDots, "show_dots"),
        field(&DockConfig::showInstanceCount, "show_instance_count"),
        enumField(&DockConfig::launcherPosition, "launcher_position", kDockLauncherPositions),
        field(&DockConfig::launcherIcon, "launcher_icon"),
        pathStringField(&DockConfig::launcherCustomImage, "launcher_custom_image"),
        field(&DockConfig::launcherCustomImageColorize, "launcher_custom_image_colorize"),
        field(&DockConfig::pinned, "pinned"),
        namedMap<DockConfig, DockMonitorOverride>(
            &DockConfig::monitorOverrides, "monitor", dockMonitorOverrideSchema(),
            [](DockMonitorOverride& out, std::string_view name) {
              out.tableName = name;
              out.match = name;
            },
            [](const DockMonitorOverride& in) { return in.tableName; }
        ),
    };
    return s;
  }

} // namespace noctalia::config::schema
