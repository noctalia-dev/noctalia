#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/bar/widget_gesture.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addBarSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    // Bar: register every configured bar so global search can surface settings from all of them.
    for (const auto& bar : cfg.bars) {
      constexpr SettingsSection section = SettingsSection::Bar;
      const std::vector<std::string> root = {"bar", bar.name};
      auto path = [&](std::string key) {
        std::vector<std::string> p = root;
        p.push_back(std::move(key));
        return p;
      };
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.enabled.label"), tr("settings.schema.bar.enabled.description"),
          path("enabled"), ToggleSetting{bar.enabled}, "visible"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.position.label"),
          tr("settings.schema.bar.position.description"), path("position"), positionSelect(bar.position), "edge"
      ));
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.auto-hide.label"),
          tr("settings.schema.bar.auto-hide.description"), path("auto_hide"),
          autoHideModeSelect(barAutoHideMode(bar.autoHide, bar.smartAutoHide), path("smart_auto_hide")),
          "autohide smart workspace"
      ));
      {
        auto e = makeEntry(
            section, "general", tr("settings.schema.bar.auto-hide-delay.label"),
            tr("settings.schema.bar.auto-hide-delay.description"), path("auto_hide_delay_ms"),
            StepperSetting{
                .value = bar.autoHideDelayMs,
                .minValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.min.value()),
                .maxValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.max.value()),
                .step = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.step.value()),
                .valueSuffix = "ms",
            },
            "autohide delay wait"
        );
        e.visibleWhen = [barName = bar.name](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          return b != nullptr && b->autoHide && !b->smartAutoHide;
        };
        entries.push_back(std::move(e));
      }
      const SettingVisibility autoHideOn = [barName = bar.name](const Config& c) {
        const BarConfig* b = findBar(c, barName);
        return b != nullptr && b->isAutoHideEnabled();
      };
      {
        auto e = makeEntry(
            section, "general", tr("settings.schema.bar.show-on-workspace-switch.label"),
            tr("settings.schema.bar.show-on-workspace-switch.description"), path("show_on_workspace_switch"),
            ToggleSetting{bar.showOnWorkspaceSwitch}, "workspace reveal peek autohide"
        );
        e.visibleWhen = autoHideOn;
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.shared.reserve-space.label"),
          tr("settings.schema.bar.reserve-space.description"), path("reserve_space"), ToggleSetting{bar.reserveSpace},
          "exclusive zone"
      ));
      const SettingVisibility reserveSpaceOn = [barName = bar.name](const Config& c) {
        const BarConfig* b = findBar(c, barName);
        return b != nullptr && b->reserveSpace;
      };
      {
        auto e = makeEntry(
            section, "general", tr("settings.schema.bar.auto-hide-reserve-space.label"),
            tr("settings.schema.bar.auto-hide-reserve-space.description"), path("auto_hide_reserve_space"),
            ToggleSetting{bar.autoHideReserveSpace}, "autohide immersive exclusive zone"
        );
        e.visibleWhen = [autoHideOn, reserveSpaceOn](const Config& c) { return autoHideOn(c) && reserveSpaceOn(c); };
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "general", tr("settings.schema.bar.layer.label"), tr("settings.schema.bar.layer.description"),
          path("layer"),
          asSegmented(plainSelect(
              {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}}, bar.layer
          )),
          "layer shell z-order"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.thickness.label"), tr("settings.schema.bar.thickness.description"),
          path("thickness"), SliderSetting{bar.thickness, 10.0F, 120.0F, 1.0F, true}, "height width"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.content-scale.label"),
          tr("settings.schema.bar.content-scale.description"), path("scale"),
          SliderSetting{bar.scale, 0.5F, 4.0F, 0.05F, false}, "zoom size"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.font-scale.label"),
          tr("settings.schema.bar.font-scale.description"), path("font_scale"),
          sliderFor(bar.fontScale, noctalia::config::schema::kBarFontScaleRange, false), "text zoom size"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.ends-margin.label"),
          tr("settings.schema.bar.ends-margin.description"), path("margin_ends"), barMarginEndsSetting(bar.marginEnds),
          "gap inset"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.edge-margin.label"),
          tr("settings.schema.bar.edge-margin.description"), path("margin_edge"), barMarginStepper(bar.marginEdge),
          "gap inset"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.shared.opposite-edge-margin.label"),
          tr("settings.schema.bar.opposite-edge-margin.description"), path("margin_opposite_edge"),
          barMarginStepper(bar.marginOppositeEdge), "gap inset strut"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.content-padding.label"),
          tr("settings.schema.bar.content-padding.description"), path("padding"),
          SliderSetting{bar.padding, 0.0F, 80.0F, 1.0F, true}, "inset"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-radius.label"),
          tr("settings.schema.bar.corner-radius.description"), path("radius"),
          barReservedSlider(bar.radius, 80.0F, 1.0F, true), "rounded"
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-left.label"),
          tr("settings.schema.bar.corner-top-left.description"), path("radius_top_left"),
          barCornerSlider(bar.radiusTopLeft), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-top-right.label"),
          tr("settings.schema.bar.corner-top-right.description"), path("radius_top_right"),
          barCornerSlider(bar.radiusTopRight), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
          tr("settings.schema.bar.corner-bottom-left.description"), path("radius_bottom_left"),
          barCornerSlider(bar.radiusBottomLeft), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
          tr("settings.schema.bar.corner-bottom-right.description"), path("radius_bottom_right"),
          barCornerSlider(bar.radiusBottomRight), "rounded corner", true
      ));
      {
        auto e = makeEntry(
            section, "shape", tr("settings.schema.bar.concave-edge-corners.label"),
            tr("settings.schema.bar.concave-edge-corners.description"), path("concave_edge_corners"),
            ToggleSetting{bar.concaveEdgeCorners}, "rounded corner carve"
        );
        e.visibleWhen = [barName = bar.name](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          return b != nullptr && b->marginEdge == 0;
        };
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.bar.border.label"), tr("settings.schema.bar.border.description"),
          path("border"), colorSpecPicker(bar.border), "outline color", true
      ));
      entries.push_back(makeEntry(
          section, "shape", tr("settings.schema.bar.border-width.label"),
          tr("settings.schema.bar.border-width.description"), path("border_width"),
          barReservedSlider(bar.borderWidth, 20.0F, 0.5F, false), "outline stroke", true
      ));
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.background-opacity.label"),
          tr("settings.schema.bar.background-opacity.description"), path("background_opacity"),
          SliderSetting{bar.backgroundOpacity, 0.0F, 1.0F, 0.01F, false}, "alpha transparency translucent opacity"
      ));
      if (env.backgroundEffectBlurSupported) {
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.bar.compositor-blur.label"),
            tr("settings.schema.bar.compositor-blur.description"), path("compositor_blur"),
            ToggleSetting{bar.compositorBlur}, "blur frosted background effect wayland"
        ));
      }
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.bar.shadow.description"),
          path("shadow"), ToggleSetting{bar.shadow}, "shadow"
      ));
      entries.push_back(makeEntry(
          section, "effects", tr("settings.schema.shared.contact-shadow.label"),
          tr("settings.schema.bar.contact-shadow.description"), path("contact_shadow"),
          ToggleSetting{bar.contactShadow}, "shadow contact panel attached"
      ));
      entries.push_back(makeEntry(
          section, "layout", tr("settings.schema.bar.panel-overlap.label"),
          tr("settings.schema.bar.panel-overlap.description"), path("panel_overlap"),
          barPanelOverlapStepper(bar.panelOverlap), "seam gap overlap attached panel fractional scale", true
      ));
      const std::string barResolvedFontFamily =
          bar.fontFamily && !bar.fontFamily->empty() ? *bar.fontFamily : cfg.shell.fontFamily;
      {
        SettingControl fontFamilyControl = TextSetting{
            .value = bar.fontFamily.value_or(""), .placeholder = cfg.shell.fontFamily, .browseFileExtensions = {}
        };
        if (!env.fontFamilies.empty()) {
          fontFamilyControl = SearchPickerSetting{
              .options = env.fontFamilies,
              .selectedValue = bar.fontFamily.value_or(""),
              .placeholder = cfg.shell.fontFamily,
              .emptyText = tr("ui.controls.search-picker.empty"),
              .preferredHeight = 280.0F,
          };
        }
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.font-family.label"),
            tr("settings.schema.bar.font-family.description"), path("font_family"), std::move(fontFamilyControl),
            "typeface font"
        ));
      }
      {
        std::vector<SelectOption> fontWeightOptions;
        const auto widgetOptions =
            buildLabelFontWeightSelectOptions(barResolvedFontFamily, FontWeightSelectKind::BarDefault, bar.fontWeight);
        fontWeightOptions.reserve(widgetOptions.size());
        for (const auto& option : widgetOptions) {
          fontWeightOptions.push_back(SelectOption{option.value, tr(option.labelKey)});
        }
        SelectSetting fontWeightSelect{std::move(fontWeightOptions), std::to_string(bar.fontWeight)};
        fontWeightSelect.valueType = SelectValueType::Integer;
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.font-weight.label"),
            tr("settings.schema.bar.font-weight.description"), path("font_weight"), std::move(fontWeightSelect),
            "font text weight"
        ));
      }
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.widget-spacing.label"),
          tr("settings.schema.bar.widget-spacing.description"), path("widget_spacing"),
          SliderSetting{bar.widgetSpacing, 0.0F, 32.0F, 1.0F, true}, "gap"
      ));
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.widget-color.label"),
          tr("settings.schema.bar.widget-color.description"), path("color"), colorSpecPicker(bar.widgetColor, true),
          "color foreground", true
      ));
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.widget-icon-color.label"),
          tr("settings.schema.bar.widget-icon-color.description"), path("icon_color"),
          colorSpecPicker(bar.widgetIconColor, true), "color icon", true
      ));
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.hover-highlight.label"),
          tr("settings.schema.bar.hover-highlight.description"), path("hover_highlight"),
          ToggleSetting{bar.hoverHighlight}, "hover highlight mouse pointer"
      ));
      entries.push_back(makeEntry(
          section, "widgets", tr("settings.schema.bar.show-tooltip.label"),
          tr("settings.schema.bar.show-tooltip.description"), path("show_tooltip"), ToggleSetting{bar.showTooltip},
          "tooltip hover"
      ));
      entries.push_back(makeEntry(
          section, "capsules", tr("settings.schema.bar.widget-capsules.label"),
          tr("settings.schema.bar.widget-capsules.description"), path("capsule"),
          ToggleSetting{bar.widgetCapsuleDefault}, "pill"
      ));
      entries.push_back(makeEntry(
          section, "capsules", tr("settings.schema.bar.capsule-thickness.label"),
          tr("settings.schema.bar.capsule-thickness.description"), path("capsule_thickness"),
          SliderSetting{bar.capsuleThickness, 0.1F, 1.0F, 0.01F, false}, "pill thickness size", true
      ));
      const SettingVisibility capsuleOn = [on = bar.widgetCapsuleDefault](const Config&) { return on; };
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-radius.label"),
            tr("settings.schema.bar.capsule-radius.description"), path("capsule_radius"),
            OptionalStepperSetting{
                .value = radiusStepperValue(bar.widgetCapsuleRadius),
                .minValue = 0,
                .maxValue = 80,
                .step = 1,
                .fallbackValue = radiusStepperFallback(bar.widgetCapsuleRadius),
                .unsetLabel = tr("common.states.auto"),
                .customLabel = tr("common.states.custom")
            },
            "pill rounded radius", true
        );
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-fill.label"),
            tr("settings.schema.bar.capsule-fill.description"), path("capsule_fill"),
            colorSpecPicker(bar.widgetCapsuleFill), "color pill", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-foreground.label"),
            tr("settings.schema.bar.capsule-foreground.description"), path("capsule_foreground"),
            colorSpecPicker(bar.widgetCapsuleForeground, true), "color foreground pill", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-border.label"),
            tr("settings.schema.bar.capsule-border.description"), path("capsule_border"),
            colorSpecPicker(bar.widgetCapsuleBorder, true), "color pill outline", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-border-width.label"),
            tr("settings.schema.bar.capsule-border-width.description"), path("capsule_border_width"),
            sliderFor(bar.widgetCapsuleBorderWidth, noctalia::config::schema::kBarCapsuleBorderWidthRange, false),
            "pill outline width", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-padding.label"),
            tr("settings.schema.bar.capsule-padding.description"), path("capsule_padding"),
            SliderSetting{bar.widgetCapsulePadding, 0.0F, 48.0F, 1.0F, false}, "pill inset", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      {
        auto e = makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-opacity.label"),
            tr("settings.schema.bar.capsule-opacity.description"), path("capsule_opacity"),
            SliderSetting{bar.widgetCapsuleOpacity, 0.0F, 1.0F, 0.01F, false}, "pill alpha", true
        );
        e.visibleWhen = capsuleOn;
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.start-widgets.label"),
          tr("settings.schema.bar.start-widgets.description"), path("start"), ListSetting{.items = bar.startWidgets},
          "left"
      ));
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.center-widgets.label"),
          tr("settings.schema.bar.center-widgets.description"), path("center"), ListSetting{.items = bar.centerWidgets},
          "middle"
      ));
      entries.push_back(makeEntry(
          section, "widget-list", tr("settings.schema.bar.end-widgets.label"),
          tr("settings.schema.bar.end-widgets.description"), path("end"), ListSetting{.items = bar.endWidgets}, "right"
      ));
      {
        auto e = makeEntry(
            section, "dead-zone", tr("settings.schema.bar.dead-zone-click-through.label"),
            tr("settings.schema.bar.dead-zone-click-through.description"),
            std::vector<std::string>{"bar", bar.name, "dead_zone", "click_through"},
            ToggleSetting{bar.deadZone.clickThrough}, "bar empty margin dead zone click through pass input region"
        );
        e.visibleWhen = [autoHideOn](const Config& c) { return !autoHideOn(c); };
        entries.push_back(std::move(e));
      }
      // One row per gesture, from the same closed set widget actions use. An unset row shows the
      // built-in default as its placeholder, so "empty" never reads as "does nothing".
      for (const auto gesture : noctalia::bar::allGestures()) {
        const std::string key(noctalia::bar::gestureConfigKey(gesture));
        const auto configured = bar.deadZone.actions.find(key);
        entries.push_back(makeEntry(
            section, "dead-zone", tr(noctalia::bar::gestureLabelKey(gesture)),
            tr("settings.schema.bar.dead-zone-action.description"),
            std::vector<std::string>{"bar", bar.name, "dead_zone", "actions", key},
            GestureActionSetting{
                .gestureKey = key,
                .configured = configured != bar.deadZone.actions.end() ? configured->second : std::string{},
                .defaultAction = deadZoneDefault(gesture),
            },
            "bar empty margin dead zone action command gesture " + key
        ));
      }
    }
  }

} // namespace settings::registry
