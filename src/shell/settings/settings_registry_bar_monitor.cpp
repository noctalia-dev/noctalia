#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/app_icon_colorization_settings.h"
#include "shell/bar/widget_gesture.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"
#include "ui/palette.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void
  addBarMonitorOverrideSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    // Bar monitor overrides (all bars).
    for (const auto& bar : cfg.bars) {
      for (const auto& ovr : bar.monitorOverrides) {
        constexpr SettingsSection section = SettingsSection::Bar;
        const std::vector<std::string> root = {"bar", bar.name, "monitor", ovr.match};
        auto monitorPath = [&](std::string key) {
          std::vector<std::string> p = root;
          p.push_back(std::move(key));
          return p;
        };

        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.enabled.label"),
            tr("settings.schema.bar.enabled.description"), monitorPath("enabled"),
            ToggleSetting{ovr.enabled.value_or(bar.enabled)}, "visible"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.position.label"),
            tr("settings.schema.bar.position.description"), monitorPath("position"),
            positionSelect(ovr.position.value_or(bar.position)), "edge"
        ));
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.auto-hide.label"),
            tr("settings.schema.bar.auto-hide.description"), monitorPath("auto_hide"),
            autoHideModeSelect(
                barAutoHideMode(ovr.autoHide.value_or(bar.autoHide), ovr.smartAutoHide.value_or(bar.smartAutoHide)),
                monitorPath("smart_auto_hide")
            ),
            "autohide smart workspace"
        ));
        {
          auto e = makeEntry(
              section, "general", tr("settings.schema.bar.auto-hide-delay.label"),
              tr("settings.schema.bar.auto-hide-delay.description"), monitorPath("auto_hide_delay_ms"),
              StepperSetting{
                  .value = ovr.autoHideDelayMs.value_or(bar.autoHideDelayMs),
                  .minValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.min.value()),
                  .maxValue = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.max.value()),
                  .step = static_cast<int>(noctalia::config::schema::kBarAutoHideDelayMsRange.step.value()),
                  .valueSuffix = "ms",
              },
              "autohide delay wait"
          );
          e.visibleWhen = [barName = bar.name, match = ovr.match](const Config& c) {
            const BarConfig* b = findBar(c, barName);
            if (b == nullptr) {
              return false;
            }
            const BarMonitorOverride* o = findMonitorOverride(*b, match);
            const bool autoHide = o != nullptr ? o->autoHide.value_or(b->autoHide) : b->autoHide;
            const bool smartAutoHide = o != nullptr ? o->smartAutoHide.value_or(b->smartAutoHide) : b->smartAutoHide;
            return autoHide && !smartAutoHide;
          };
          entries.push_back(std::move(e));
        }
        const SettingVisibility monitorAutoHideOn = [barName = bar.name, match = ovr.match](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          if (b == nullptr) {
            return false;
          }
          const BarMonitorOverride* o = findMonitorOverride(*b, match);
          return o != nullptr ? o->isAutoHideEnabled(b->autoHide, b->smartAutoHide) : b->isAutoHideEnabled();
        };
        {
          auto e = makeEntry(
              section, "general", tr("settings.schema.bar.show-on-workspace-switch.label"),
              tr("settings.schema.bar.show-on-workspace-switch.description"), monitorPath("show_on_workspace_switch"),
              ToggleSetting{ovr.showOnWorkspaceSwitch.value_or(bar.showOnWorkspaceSwitch)},
              "workspace reveal peek autohide"
          );
          e.visibleWhen = monitorAutoHideOn;
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.shared.reserve-space.label"),
            tr("settings.schema.bar.reserve-space.description"), monitorPath("reserve_space"),
            ToggleSetting{ovr.reserveSpace.value_or(bar.reserveSpace)}, "exclusive zone"
        ));
        const SettingVisibility monitorReserveSpaceOn = [barName = bar.name, match = ovr.match](const Config& c) {
          const BarConfig* b = findBar(c, barName);
          if (b == nullptr) {
            return false;
          }
          const BarMonitorOverride* o = findMonitorOverride(*b, match);
          return o != nullptr ? o->reserveSpace.value_or(b->reserveSpace) : b->reserveSpace;
        };
        {
          auto e = makeEntry(
              section, "general", tr("settings.schema.bar.auto-hide-reserve-space.label"),
              tr("settings.schema.bar.auto-hide-reserve-space.description"), monitorPath("auto_hide_reserve_space"),
              ToggleSetting{ovr.autoHideReserveSpace.value_or(bar.autoHideReserveSpace)},
              "autohide immersive exclusive zone"
          );
          e.visibleWhen = [monitorAutoHideOn, monitorReserveSpaceOn](const Config& c) {
            return monitorAutoHideOn(c) && monitorReserveSpaceOn(c);
          };
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "general", tr("settings.schema.bar.layer.label"), tr("settings.schema.bar.layer.description"),
            monitorPath("layer"),
            asSegmented(plainSelect(
                {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}},
                ovr.layer.value_or(bar.layer)
            )),
            "layer shell z-order"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.thickness.label"),
            tr("settings.schema.bar.thickness.description"), monitorPath("thickness"),
            SliderSetting{ovr.thickness.value_or(bar.thickness), 10.0F, 120.0F, 1.0F, true}, "height width"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-scale.label"),
            tr("settings.schema.bar.content-scale.description"), monitorPath("scale"),
            SliderSetting{ovr.scale.value_or(bar.scale), 0.5F, 4.0F, 0.05F, false}, "zoom size"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.font-scale.label"),
            tr("settings.schema.bar.font-scale.description"), monitorPath("font_scale"),
            sliderFor(ovr.fontScale.value_or(bar.fontScale), noctalia::config::schema::kBarFontScaleRange, false),
            "text zoom size"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.ends-margin.label"),
            tr("settings.schema.bar.ends-margin.description"), monitorPath("margin_ends"),
            barMarginEndsSetting(ovr.marginEnds.value_or(bar.marginEnds)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.edge-margin.label"),
            tr("settings.schema.bar.edge-margin.description"), monitorPath("margin_edge"),
            barMarginStepper(ovr.marginEdge.value_or(bar.marginEdge)), "gap inset"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.shared.opposite-edge-margin.label"),
            tr("settings.schema.bar.opposite-edge-margin.description"), monitorPath("margin_opposite_edge"),
            barMarginStepper(ovr.marginOppositeEdge.value_or(bar.marginOppositeEdge)), "gap inset strut"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.content-padding.label"),
            tr("settings.schema.bar.content-padding.description"), monitorPath("padding"),
            SliderSetting{ovr.padding.value_or(bar.padding), 0.0F, 80.0F, 1.0F, true}, "inset"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-radius.label"),
            tr("settings.schema.bar.corner-radius.description"), monitorPath("radius"),
            barReservedSlider(ovr.radius.value_or(bar.radius), 80.0F, 1.0F, true), "rounded"
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-left.label"),
            tr("settings.schema.bar.corner-top-left.description"), monitorPath("radius_top_left"),
            barCornerSlider(ovr.radiusTopLeft.value_or(bar.radiusTopLeft)), "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-top-right.label"),
            tr("settings.schema.bar.corner-top-right.description"), monitorPath("radius_top_right"),
            barCornerSlider(ovr.radiusTopRight.value_or(bar.radiusTopRight)), "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
            tr("settings.schema.bar.corner-bottom-left.description"), monitorPath("radius_bottom_left"),
            barCornerSlider(ovr.radiusBottomLeft.value_or(bar.radiusBottomLeft)), "rounded corner", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
            tr("settings.schema.bar.corner-bottom-right.description"), monitorPath("radius_bottom_right"),
            barCornerSlider(ovr.radiusBottomRight.value_or(bar.radiusBottomRight)), "rounded corner", true
        ));
        {
          auto e = makeEntry(
              section, "shape", tr("settings.schema.bar.concave-edge-corners.label"),
              tr("settings.schema.bar.concave-edge-corners.description"), monitorPath("concave_edge_corners"),
              ToggleSetting{ovr.concaveEdgeCorners.value_or(bar.concaveEdgeCorners)}, "rounded corner carve"
          );
          e.visibleWhen = [barName = bar.name, match = ovr.match](const Config& c) {
            const BarConfig* b = findBar(c, barName);
            if (b == nullptr) {
              return false;
            }
            const BarMonitorOverride* mo = findMonitorOverride(*b, match);
            const std::int32_t marginEdge = mo && mo->marginEdge.has_value() ? *mo->marginEdge : b->marginEdge;
            return marginEdge == 0;
          };
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border.label"), tr("settings.schema.bar.border.description"),
            monitorPath("border"), colorSpecPicker(ovr.border, true, tr("common.states.inherit")), "outline color", true
        ));
        entries.push_back(makeEntry(
            section, "shape", tr("settings.schema.bar.border-width.label"),
            tr("settings.schema.bar.border-width.description"), monitorPath("border_width"),
            barReservedSlider(ovr.borderWidth.value_or(bar.borderWidth), 20.0F, 0.5F, false), "outline stroke", true
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.background-opacity.label"),
            tr("settings.schema.bar.background-opacity.description"), monitorPath("background_opacity"),
            SliderSetting{ovr.backgroundOpacity.value_or(bar.backgroundOpacity), 0.0F, 1.0F, 0.01F, false},
            "alpha transparency translucent opacity"
        ));
        if (env.backgroundEffectBlurSupported) {
          entries.push_back(makeEntry(
              section, "effects", tr("settings.schema.bar.compositor-blur.label"),
              tr("settings.schema.bar.compositor-blur.description"), monitorPath("compositor_blur"),
              ToggleSetting{ovr.compositorBlur.value_or(bar.compositorBlur)}, "blur frosted background effect wayland"
          ));
        }
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.shadow.label"), tr("settings.schema.bar.shadow.description"),
            monitorPath("shadow"), ToggleSetting{ovr.shadow.value_or(bar.shadow)}, "shadow"
        ));
        entries.push_back(makeEntry(
            section, "effects", tr("settings.schema.shared.contact-shadow.label"),
            tr("settings.schema.bar.contact-shadow.description"), monitorPath("contact_shadow"),
            ToggleSetting{ovr.contactShadow.value_or(bar.contactShadow)}, "shadow contact panel attached"
        ));
        entries.push_back(makeEntry(
            section, "layout", tr("settings.schema.bar.panel-overlap.label"),
            tr("settings.schema.bar.panel-overlap.description"), monitorPath("panel_overlap"),
            barPanelOverlapStepper(ovr.panelOverlap.value_or(bar.panelOverlap)),
            "seam gap overlap attached panel fractional scale", true
        ));
        {
          const std::string monitorInheritedFontFamily = bar.fontFamily.value_or(cfg.shell.fontFamily);
          SettingControl fontFamilyControl = TextSetting{
              .value = ovr.fontFamily.value_or(""),
              .placeholder = monitorInheritedFontFamily,
              .browseFileExtensions = {}
          };
          if (!env.fontFamilies.empty()) {
            fontFamilyControl = SearchPickerSetting{
                .options = env.fontFamilies,
                .selectedValue = ovr.fontFamily.value_or(""),
                .placeholder = monitorInheritedFontFamily,
                .emptyText = tr("ui.controls.search-picker.empty"),
                .preferredHeight = 280.0F,
            };
          }
          entries.push_back(makeEntry(
              section, "widgets", tr("settings.schema.bar.font-family.label"),
              tr("settings.schema.bar.font-family.description"), monitorPath("font_family"),
              std::move(fontFamilyControl), "typeface font", true
          ));
        }
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-spacing.label"),
            tr("settings.schema.bar.widget-spacing.description"), monitorPath("widget_spacing"),
            SliderSetting{ovr.widgetSpacing.value_or(bar.widgetSpacing), 0.0F, 32.0F, 1.0F, true}, "gap"
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-color.label"),
            tr("settings.schema.bar.widget-color.description"), monitorPath("color"),
            colorSpecPicker(ovr.widgetColor, true, tr("common.states.inherit")), "color foreground", true
        ));
        entries.push_back(makeEntry(
            section, "widgets", tr("settings.schema.bar.widget-icon-color.label"),
            tr("settings.schema.bar.widget-icon-color.description"), monitorPath("icon_color"),
            colorSpecPicker(ovr.widgetIconColor, true, tr("common.states.inherit")), "color icon", true
        ));
        entries.push_back(makeEntry(
            section, "capsules", tr("settings.schema.bar.widget-capsules.label"),
            tr("settings.schema.bar.widget-capsules.description"), monitorPath("capsule"),
            ToggleSetting{ovr.widgetCapsuleDefault.value_or(bar.widgetCapsuleDefault)}, "pill"
        ));
        entries.push_back(makeEntry(
            section, "capsules", tr("settings.schema.bar.capsule-thickness.label"),
            tr("settings.schema.bar.capsule-thickness.description"), monitorPath("capsule_thickness"),
            SliderSetting{ovr.capsuleThickness.value_or(bar.capsuleThickness), 0.1F, 1.0F, 0.01F, false},
            "pill thickness size", true
        ));
        const SettingVisibility monitorCapsuleOn =
            [on = ovr.widgetCapsuleDefault.value_or(bar.widgetCapsuleDefault)](const Config&) { return on; };
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-radius.label"),
              tr("settings.schema.bar.capsule-radius.description"), monitorPath("capsule_radius"),
              OptionalStepperSetting{
                  .value = radiusStepperValue(ovr.widgetCapsuleRadius),
                  .minValue = 0,
                  .maxValue = 80,
                  .step = 1,
                  .fallbackValue = radiusStepperFallback(bar.widgetCapsuleRadius),
                  .unsetLabel = tr("common.states.inherit"),
                  .customLabel = tr("common.states.custom")
              },
              "pill rounded radius", true
          );
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-fill.label"),
              tr("settings.schema.bar.capsule-fill.description"), monitorPath("capsule_fill"),
              colorSpecPicker(ovr.widgetCapsuleFill, true, tr("common.states.inherit")), "color pill", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-foreground.label"),
              tr("settings.schema.bar.capsule-foreground.description"), monitorPath("capsule_foreground"),
              colorSpecPicker(ovr.widgetCapsuleForeground, true, tr("common.states.inherit")), "color foreground pill",
              true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-border.label"),
              tr("settings.schema.bar.capsule-border.description"), monitorPath("capsule_border"),
              colorSpecPicker(
                  ovr.widgetCapsuleBorderSpecified ? ovr.widgetCapsuleBorder : std::optional<ColorSpec>{}, true,
                  tr("common.states.inherit")
              ),
              "color pill outline", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-border-width.label"),
              tr("settings.schema.bar.capsule-border-width.description"), monitorPath("capsule_border_width"),
              sliderFor(
                  ovr.widgetCapsuleBorderWidth.value_or(bar.widgetCapsuleBorderWidth),
                  noctalia::config::schema::kBarCapsuleBorderWidthRange, false
              ),
              "pill outline width", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-padding.label"),
              tr("settings.schema.bar.capsule-padding.description"), monitorPath("capsule_padding"),
              SliderSetting{ovr.widgetCapsulePadding.value_or(bar.widgetCapsulePadding), 0.0F, 48.0F, 1.0F, false},
              "pill inset", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        {
          auto e = makeEntry(
              section, "capsules", tr("settings.schema.bar.capsule-opacity.label"),
              tr("settings.schema.bar.capsule-opacity.description"), monitorPath("capsule_opacity"),
              SliderSetting{ovr.widgetCapsuleOpacity.value_or(bar.widgetCapsuleOpacity), 0.0F, 1.0F, 0.01F, false},
              "pill alpha", true
          );
          e.visibleWhen = monitorCapsuleOn;
          entries.push_back(std::move(e));
        }
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.start-widgets.label"),
            tr("settings.schema.bar.start-widgets.description"), monitorPath("start"),
            ListSetting{.items = ovr.startWidgets.value_or(bar.startWidgets)}, "left"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.center-widgets.label"),
            tr("settings.schema.bar.center-widgets.description"), monitorPath("center"),
            ListSetting{.items = ovr.centerWidgets.value_or(bar.centerWidgets)}, "middle"
        ));
        entries.push_back(makeEntry(
            section, "widget-list", tr("settings.schema.bar.end-widgets.label"),
            tr("settings.schema.bar.end-widgets.description"), monitorPath("end"),
            ListSetting{.items = ovr.endWidgets.value_or(bar.endWidgets)}, "right"
        ));
        {
          std::vector<std::string> clickThroughPath = root;
          clickThroughPath.emplace_back("dead_zone");
          clickThroughPath.emplace_back("click_through");
          auto e = makeEntry(
              section, "dead-zone", tr("settings.schema.bar.dead-zone-click-through.label"),
              tr("settings.schema.bar.dead-zone-click-through.description"), std::move(clickThroughPath),
              ToggleSetting{ovr.deadZone.clickThrough.value_or(bar.deadZone.clickThrough)},
              "bar empty margin dead zone click through pass input region"
          );
          e.visibleWhen = [monitorAutoHideOn](const Config& c) { return !monitorAutoHideOn(c); };
          entries.push_back(std::move(e));
        }
        for (const auto gesture : noctalia::bar::allGestures()) {
          const std::string key(noctalia::bar::gestureConfigKey(gesture));
          std::vector<std::string> gesturePath = root;
          gesturePath.emplace_back("dead_zone");
          gesturePath.emplace_back("actions");
          gesturePath.emplace_back(key);

          const auto& overrideActions = ovr.deadZone.actions;
          std::string configured;
          if (overrideActions.has_value()) {
            const auto overridden = overrideActions->find(key);
            if (overridden != overrideActions->end()) {
              configured = overridden->second;
            }
          }
          const auto inherited = bar.deadZone.actions.find(key);
          entries.push_back(makeEntry(
              section, "dead-zone", tr(noctalia::bar::gestureLabelKey(gesture)),
              tr("settings.schema.bar.dead-zone-action.description"), std::move(gesturePath),
              GestureActionSetting{
                  .gestureKey = key,
                  .configured = std::move(configured),
                  .defaultAction =
                      inherited != bar.deadZone.actions.end() ? inherited->second : deadZoneDefault(gesture),
              },
              "bar empty margin dead zone action command gesture " + key
          ));
        }
      }
    }
  }

} // namespace settings::registry
