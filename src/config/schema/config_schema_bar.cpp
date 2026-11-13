#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noctalia::config::schema {

  namespace {
    // Clamp ranges shared by the concrete BarConfig fields and the parallel
    // optional BarMonitorOverride fields, declared once so the two schemas can't
    // drift apart.
    constexpr Range<std::int64_t> kBarThicknessRange{10, 300};
    constexpr Range<std::int64_t> kBarRadiusRange{0, 500};
    constexpr Range<std::int64_t> kBarPanelOverlapRange{-2, 3};
    constexpr Range<float> kBarCapsuleThicknessRange{0.1F, 1.0F};
    constexpr Range<float> kBarOpacityRange{0.0F, 1.0F};
    constexpr Range<float> kBarBorderWidthRange{0.0F, 20.0F};
    constexpr Range<float> kBarScaleRange{0.5F, 4.0F};
    constexpr Range<float> kBarCapsulePaddingRange{0.0F, 48.0F};
    constexpr Range<float> kBarCapsuleRadiusRangeF{0.0F, 80.0F};
    constexpr Range<double> kBarCapsulePaddingRangeD{0.0, 48.0};
    constexpr Range<double> kBarCapsuleRadiusRangeD{0.0, 80.0};
    constexpr Range<double> kBarCapsuleOpacityRangeD{0.0, 1.0};

    // The capsule_border pair: a bool "specified" flag plus an optional<ColorSpec>.
    // A present key (even empty) sets specified=true; an empty value means "no
    // outline" (nullopt). Emitted only when specified, as the color or empty string.
    template <typename Struct>
    Field<Struct> capsuleBorderField(
        std::optional<ColorSpec> Struct::* colorMember, bool Struct::* specifiedMember, std::string_view key
    ) {
      return custom<Struct>(
          key,
          [colorMember, specifiedMember,
           key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
            if (!tbl.contains(key)) {
              return;
            }
            auto v = tbl[key].value<std::string>();
            if (!v) {
              throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
            }
            out.*specifiedMember = true;
            if (StringUtils::trim(*v).empty()) {
              out.*colorMember = std::nullopt;
            } else {
              out.*colorMember = colorSpecFromConfigString(*v, joinPath(parentPath, key));
            }
          },
          [colorMember, specifiedMember, key](toml::table& tbl, const Struct& in) {
            if (in.*specifiedMember) {
              tbl.insert_or_assign(
                  key, (in.*colorMember).has_value() ? colorSpecToConfigString(*(in.*colorMember)) : std::string{}
              );
            }
          }
      );
    }

    const Schema<BarCapsuleGroupStyle>& barCapsuleGroupSchema() {
      static const Schema<BarCapsuleGroupStyle> s = {
          // id is trimmed; empty-id rows are dropped by the consuming keep predicate.
          custom<BarCapsuleGroupStyle>(
              "id",
              [](const toml::table& tbl, BarCapsuleGroupStyle& out, std::string_view parentPath, Diagnostics& diag) {
                if (auto v = tbl["id"].value<std::string>()) {
                  out.id = StringUtils::trim(*v);
                } else {
                  warnWrongType(tbl, "id", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const BarCapsuleGroupStyle& in) { tbl.insert_or_assign("id", in.id); }
          ),
          field(&BarCapsuleGroupStyle::enabled, "enabled"),
          field(&BarCapsuleGroupStyle::members, "members"),
          colorField(&BarCapsuleGroupStyle::fill, "fill"),
          capsuleBorderField(&BarCapsuleGroupStyle::border, &BarCapsuleGroupStyle::borderSpecified, "border"),
          field(&BarCapsuleGroupStyle::borderWidth, "border_width", kBarCapsuleBorderWidthRange),
          optionalColorField(&BarCapsuleGroupStyle::foreground, "foreground"),
          field(&BarCapsuleGroupStyle::padding, "padding", kBarCapsulePaddingRange),
          optionalFloatField(&BarCapsuleGroupStyle::radius, "radius", kBarCapsuleRadiusRangeF),
          field(&BarCapsuleGroupStyle::opacity, "opacity", kBarOpacityRange),
          field(&BarCapsuleGroupStyle::accordion, "accordion"),
          enumField(&BarCapsuleGroupStyle::accordionDirection, "accordion_direction", kBarAccordionDirections),
          optionalIntField(
              &BarCapsuleGroupStyle::accordionDurationMs, "accordion_duration", kBarCapsuleAccordionMsRange
          ),
          optionalIntField(&BarCapsuleGroupStyle::accordionDelayMs, "accordion_delay_ms", kBarCapsuleAccordionMsRange),
          optionalIntField(&BarCapsuleGroupStyle::widgetSpacing, "widget_spacing"),
      };
      return s;
    }

    // layer accepts top|overlay (concrete string member); anything else warns.
    Field<BarConfig> barLayerField() {
      return custom<BarConfig>(
          "layer",
          [](const toml::table& tbl, BarConfig& out, std::string_view parentPath, Diagnostics& diag) {
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
          [](toml::table& tbl, const BarConfig& in) { tbl.insert_or_assign("layer", in.layer); }
      );
    }

    // radius seeds all four corners; per-corner keys below override it.
    Field<BarConfig> barRadiusField() {
      return custom<BarConfig>(
          "radius",
          [](const toml::table& tbl, BarConfig& out, std::string_view parentPath, Diagnostics& diag) {
            if (auto v = tbl["radius"].value<std::int64_t>()) {
              const auto r = static_cast<std::int32_t>(std::clamp<std::int64_t>(*v, 0, 500));
              out.radius = r;
              out.radiusTopLeft = r;
              out.radiusTopRight = r;
              out.radiusBottomLeft = r;
              out.radiusBottomRight = r;
            } else {
              warnWrongType(tbl, "radius", parentPath, diag, "an integer");
            }
          },
          [](toml::table& tbl, const BarConfig& in) {
            tbl.insert_or_assign("radius", static_cast<std::int64_t>(in.radius));
          }
      );
    }

    const Schema<BarDeadZoneConfig>& barDeadZoneSchema() {
      static const Schema<BarDeadZoneConfig> s = {
          field(&BarDeadZoneConfig::actions, "actions"),
          field(&BarDeadZoneConfig::clickThrough, "click_through"),
      };
      return s;
    }

    const Schema<BarDeadZoneOverride>& barDeadZoneOverrideSchema() {
      static const Schema<BarDeadZoneOverride> s = {
          field(&BarDeadZoneOverride::actions, "actions"),
          optionalBoolField(&BarDeadZoneOverride::clickThrough, "click_through"),
      };
      return s;
    }
  } // namespace

  const Schema<BarConfig>& barFieldsSchema() {
    static const Schema<BarConfig> s = {
        field(&BarConfig::enabled, "enabled"),
        field(&BarConfig::autoHide, "auto_hide"),
        field(&BarConfig::autoHideDelayMs, "auto_hide_delay_ms", kBarAutoHideDelayMsRange),
        field(&BarConfig::smartAutoHide, "smart_auto_hide"),
        field(&BarConfig::showOnWorkspaceSwitch, "show_on_workspace_switch"),
        field(&BarConfig::reserveSpace, "reserve_space"),
        field(&BarConfig::autoHideReserveSpace, "auto_hide_reserve_space"),
        barLayerField(),
        field(&BarConfig::thickness, "thickness", kBarThicknessRange),
        field(&BarConfig::backgroundOpacity, "background_opacity", kBarOpacityRange),
        field(&BarConfig::compositorBlur, "compositor_blur"),
        colorField(&BarConfig::border, "border"),
        field(&BarConfig::borderWidth, "border_width", kBarBorderWidthRange),
        barRadiusField(),
        field(&BarConfig::radiusTopLeft, "radius_top_left", kBarRadiusRange),
        field(&BarConfig::radiusTopRight, "radius_top_right", kBarRadiusRange),
        field(&BarConfig::radiusBottomLeft, "radius_bottom_left", kBarRadiusRange),
        field(&BarConfig::radiusBottomRight, "radius_bottom_right", kBarRadiusRange),
        field(&BarConfig::concaveEdgeCorners, "concave_edge_corners"),
        field(&BarConfig::marginEnds, "margin_ends", kBarMarginEndsPercentRange),
        field(&BarConfig::marginEdge, "margin_edge"),
        field(&BarConfig::marginOppositeEdge, "margin_opposite_edge"),
        field(&BarConfig::padding, "padding"),
        field(&BarConfig::widgetSpacing, "widget_spacing"),
        field(&BarConfig::shadow, "shadow"),
        field(&BarConfig::contactShadow, "contact_shadow"),
        field(&BarConfig::panelOverlap, "panel_overlap", kBarPanelOverlapRange),
        field(&BarConfig::capsuleThickness, "capsule_thickness", kBarCapsuleThicknessRange),
        field(&BarConfig::scale, "scale", kBarScaleRange),
        field(&BarConfig::fontScale, "font_scale", kBarFontScaleRange),
        field(&BarConfig::fontWeight, "font_weight"),
        optionalTrimmedStringField(&BarConfig::fontFamily, "font_family"),
        field(&BarConfig::startWidgets, "start"),
        field(&BarConfig::centerWidgets, "center"),
        field(&BarConfig::endWidgets, "end"),
        field(&BarConfig::widgetCapsuleDefault, "capsule"),
        colorField(&BarConfig::widgetCapsuleFill, "capsule_fill"),
        optionalColorField(&BarConfig::widgetCapsuleForeground, "capsule_foreground"),
        optionalColorField(&BarConfig::widgetColor, "color"),
        optionalColorField(&BarConfig::widgetIconColor, "icon_color"),
        arrayOf<BarConfig, BarCapsuleGroupStyle>(
            &BarConfig::widgetCapsuleGroups, "capsule_group", barCapsuleGroupSchema(),
            [](const BarCapsuleGroupStyle& g) { return !g.id.empty(); }
        ),
        field(&BarConfig::widgetCapsulePadding, "capsule_padding", kBarCapsulePaddingRange),
        optionalDoubleField(&BarConfig::widgetCapsuleRadius, "capsule_radius", kBarCapsuleRadiusRangeD),
        field(&BarConfig::widgetCapsuleOpacity, "capsule_opacity", kBarOpacityRange),
        capsuleBorderField(&BarConfig::widgetCapsuleBorder, &BarConfig::widgetCapsuleBorderSpecified, "capsule_border"),
        field(&BarConfig::widgetCapsuleBorderWidth, "capsule_border_width", kBarCapsuleBorderWidthRange),
        field(&BarConfig::hoverHighlight, "hover_highlight"),
        field(&BarConfig::showTooltip, "show_tooltip"),
        subTable(&BarConfig::deadZone, "dead_zone", barDeadZoneSchema()),
        field(&BarConfig::actions, "actions"),
    };
    return s;
  }

  const Schema<BarMonitorOverride>& barMonitorOverrideSchema() {
    static const Schema<BarMonitorOverride> s = {
        field(&BarMonitorOverride::match, "match"),
        field(&BarMonitorOverride::position, "position"),
        optionalBoolField(&BarMonitorOverride::enabled, "enabled"),
        optionalBoolField(&BarMonitorOverride::autoHide, "auto_hide"),
        optionalIntField(&BarMonitorOverride::autoHideDelayMs, "auto_hide_delay_ms", kBarAutoHideDelayMsRange),
        optionalBoolField(&BarMonitorOverride::smartAutoHide, "smart_auto_hide"),
        optionalBoolField(&BarMonitorOverride::showOnWorkspaceSwitch, "show_on_workspace_switch"),
        optionalBoolField(&BarMonitorOverride::reserveSpace, "reserve_space"),
        optionalBoolField(&BarMonitorOverride::autoHideReserveSpace, "auto_hide_reserve_space"),
        // layer accepts top|overlay; anything else warns and leaves it unset.
        custom<BarMonitorOverride>(
            "layer",
            [](const toml::table& tbl, BarMonitorOverride& out, std::string_view parentPath, Diagnostics& diag) {
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
            [](toml::table& tbl, const BarMonitorOverride& in) {
              if (in.layer.has_value()) {
                tbl.insert_or_assign("layer", *in.layer);
              }
            }
        ),
        optionalIntField(&BarMonitorOverride::thickness, "thickness", kBarThicknessRange),
        optionalFloatField(&BarMonitorOverride::backgroundOpacity, "background_opacity", kBarOpacityRange),
        optionalBoolField(&BarMonitorOverride::compositorBlur, "compositor_blur"),
        optionalColorField(&BarMonitorOverride::border, "border"),
        optionalFloatField(&BarMonitorOverride::borderWidth, "border_width", kBarBorderWidthRange),
        optionalIntField(&BarMonitorOverride::radius, "radius", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusTopLeft, "radius_top_left", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusTopRight, "radius_top_right", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusBottomLeft, "radius_bottom_left", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusBottomRight, "radius_bottom_right", kBarRadiusRange),
        optionalBoolField(&BarMonitorOverride::concaveEdgeCorners, "concave_edge_corners"),
        field(&BarMonitorOverride::marginEnds, "margin_ends", kBarMarginEndsPercentRange),
        optionalIntField(&BarMonitorOverride::marginEdge, "margin_edge"),
        optionalIntField(&BarMonitorOverride::marginOppositeEdge, "margin_opposite_edge"),
        optionalIntField(&BarMonitorOverride::padding, "padding"),
        optionalIntField(&BarMonitorOverride::widgetSpacing, "widget_spacing"),
        optionalFloatField(&BarMonitorOverride::scale, "scale", kBarScaleRange),
        optionalFloatField(&BarMonitorOverride::fontScale, "font_scale", kBarFontScaleRange),
        optionalBoolField(&BarMonitorOverride::shadow, "shadow"),
        optionalBoolField(&BarMonitorOverride::contactShadow, "contact_shadow"),
        optionalIntField(&BarMonitorOverride::panelOverlap, "panel_overlap", kBarPanelOverlapRange),
        optionalFloatField(&BarMonitorOverride::capsuleThickness, "capsule_thickness", kBarCapsuleThicknessRange),
        optionalTrimmedStringField(&BarMonitorOverride::fontFamily, "font_family"),
        optionalStringVectorField(&BarMonitorOverride::startWidgets, "start"),
        optionalStringVectorField(&BarMonitorOverride::centerWidgets, "center"),
        optionalStringVectorField(&BarMonitorOverride::endWidgets, "end"),
        optionalBoolField(&BarMonitorOverride::widgetCapsuleDefault, "capsule"),
        optionalColorField(&BarMonitorOverride::widgetCapsuleFill, "capsule_fill"),
        optionalColorField(&BarMonitorOverride::widgetCapsuleForeground, "capsule_foreground"),
        optionalColorField(&BarMonitorOverride::widgetColor, "color"),
        optionalColorField(&BarMonitorOverride::widgetIconColor, "icon_color"),
        optionalDoubleField(&BarMonitorOverride::widgetCapsulePadding, "capsule_padding", kBarCapsulePaddingRangeD),
        optionalDoubleField(&BarMonitorOverride::widgetCapsuleRadius, "capsule_radius", kBarCapsuleRadiusRangeD),
        optionalDoubleField(&BarMonitorOverride::widgetCapsuleOpacity, "capsule_opacity", kBarCapsuleOpacityRangeD),
        capsuleBorderField(
            &BarMonitorOverride::widgetCapsuleBorder, &BarMonitorOverride::widgetCapsuleBorderSpecified,
            "capsule_border"
        ),
        optionalFloatField(
            &BarMonitorOverride::widgetCapsuleBorderWidth, "capsule_border_width", kBarCapsuleBorderWidthRange
        ),
        optionalBoolField(&BarMonitorOverride::hoverHighlight, "hover_highlight"),
        optionalBoolField(&BarMonitorOverride::showTooltip, "show_tooltip"),
        // capsule_group: read-only here (overrides serialize via the resolved bar).
        custom<BarMonitorOverride>(
            "capsule_group",
            [](const toml::table& tbl, BarMonitorOverride& out, std::string_view parentPath, Diagnostics& diag) {
              const auto* arr = tbl["capsule_group"].as_array();
              if (arr == nullptr) {
                warnWrongType(tbl, "capsule_group", parentPath, diag, "an array of tables");
                return;
              }
              std::vector<BarCapsuleGroupStyle> groups;
              for (std::size_t index = 0; index < arr->size(); ++index) {
                const auto& node = (*arr)[index];
                const auto* sub = node.as_table();
                if (sub == nullptr) {
                  warnWrongType(
                      diag, joinPath(parentPath, "capsule_group") + '[' + std::to_string(index) + ']', node, "a table"
                  );
                  continue;
                }
                BarCapsuleGroupStyle g{};
                readInto(*sub, g, barCapsuleGroupSchema(), joinPath(parentPath, "capsule_group"), diag);
                if (!g.id.empty()) {
                  groups.push_back(std::move(g));
                }
              }
              out.widgetCapsuleGroups = std::move(groups);
            },
            [](toml::table&, const BarMonitorOverride&) {}
        ),
        subTable(&BarMonitorOverride::deadZone, "dead_zone", barDeadZoneOverrideSchema()),
    };
    return s;
  }

} // namespace noctalia::config::schema
