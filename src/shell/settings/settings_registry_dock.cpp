#include "config/config_types.h"
#include "config/schema/ranges.h"
#include "i18n/i18n.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/settings_registry_common.h"
#include "system/directory_scanner.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings::registry {

  void addDockSettings(std::vector<SettingEntry>& entries, const Config& cfg, const RegistryEnvironment& env) {
    using i18n::tr;
    // Dock and per-monitor dock pages share the same controls. Override pages display effective values while
    // writing to their own table, so untouched fields continue to inherit from the global dock.
    const auto addDockPage = [&](const DockConfig& dock, std::vector<std::string> root,
                                 const std::optional<std::string>& monitorTable) {
      const auto path = [&root](std::string key) {
        auto result = root;
        result.push_back(std::move(key));
        return result;
      };
      const auto currentDock = [monitorTable](const Config& current) {
        if (monitorTable) {
          if (const auto* override = findDockMonitorOverride(current.dock, *monitorTable)) {
            return resolveDockMonitorOverride(current.dock, *override);
          }
        }
        return current.dock;
      };

      entries.push_back(makeEntry(
          SettingsSection::Dock, "general", tr("settings.schema.shared.enabled.label"),
          tr("settings.schema.dock.enabled.description"), path("enabled"), ToggleSetting{dock.enabled}, "launcher apps"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "general", tr("settings.schema.dock.active-monitor-only.label"),
          tr("settings.schema.dock.active-monitor-only.description"), path("active_monitor_only"),
          ToggleSetting{dock.activeMonitorOnly}, "monitor"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.shared.auto-hide.label"),
          tr("settings.schema.dock.auto-hide.description"), path("auto_hide"),
          autoHideModeSelect(barAutoHideMode(dock.autoHide, dock.smartAutoHide), path("smart_auto_hide")),
          "autohide smart workspace"
      ));
      {
        auto e = makeEntry(
            SettingsSection::Dock, "behavior", tr("settings.schema.dock.auto-hide-reveal-delay.label"),
            tr("settings.schema.dock.auto-hide-reveal-delay.description"), path("auto_hide_reveal_delay_ms"),
            StepperSetting{
                .value = dock.autoHideRevealDelayMs,
                .minValue = static_cast<int>(noctalia::config::schema::kDockAutoHideRevealDelayMsRange.min.value()),
                .maxValue = static_cast<int>(noctalia::config::schema::kDockAutoHideRevealDelayMsRange.max.value()),
                .step = static_cast<int>(noctalia::config::schema::kDockAutoHideRevealDelayMsRange.step.value()),
                .valueSuffix = "ms",
            },
            "autohide reveal delay dwell edge"
        );
        e.visibleWhen = [currentDock](const Config& c) { return currentDock(c).isAutoHideEnabled(); };
        entries.push_back(std::move(e));
      }
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.shared.reserve-space.label"),
          tr("settings.schema.dock.reserve-space.description"), path("reserve_space"), ToggleSetting{dock.reserveSpace},
          "exclusive zone"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.dock.layer.label"),
          tr("settings.schema.dock.layer.description"), path("layer"),
          asSegmented(plainSelect(
              {{"top", "settings.options.layer.top"}, {"overlay", "settings.options.layer.overlay"}}, dock.layer
          )),
          "layer shell z-order"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.dock.show-running.label"),
          tr("settings.schema.dock.show-running.description"), path("show_running"), ToggleSetting{dock.showRunning},
          "windows"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.dock.show-dots.label"),
          tr("settings.schema.dock.show-dots.description"), path("show_dots"), ToggleSetting{dock.showDots},
          "running dots"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "behavior", tr("settings.schema.dock.show-instance-count.label"),
          tr("settings.schema.dock.show-instance-count.description"), path("show_instance_count"),
          ToggleSetting{dock.showInstanceCount}, "badge windows"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.dock.launcher-position.label"),
          tr("settings.schema.dock.launcher-position.description"), path("launcher_position"),
          asSegmented(enumSelect(kDockLauncherPositions, dock.launcherPosition)), "launcher apps grid"
      ));
      const SettingVisibility dockLauncherEnabled = [currentDock](const Config& current) {
        const auto effective = currentDock(current);
        return effective.launcherPosition == DockLauncherPosition::Start
            || effective.launcherPosition == DockLauncherPosition::End;
      };
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "layout", tr("settings.schema.dock.launcher-icon.label"),
            tr("settings.schema.dock.launcher-icon.description"), path("launcher_icon"),
            TextSetting{.value = dock.launcherIcon, .placeholder = "grid-dots", .browseFileExtensions = {}},
            "launcher apps icon glyph"
        );
        entry.visibleWhen = dockLauncherEnabled;
        entries.push_back(std::move(entry));
      }
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "layout", tr("settings.schema.dock.launcher-custom-image.label"),
            tr("settings.schema.dock.launcher-custom-image.description"), path("launcher_custom_image"),
            TextSetting{
                .value = dock.launcherCustomImage,
                .placeholder = tr("settings.schema.dock.launcher-custom-image.placeholder"),
                .browseMode = TextSettingBrowseMode::OpenFile,
                .browseFileExtensions = DirectoryScanner::imageExtensionFilter(true),
                .browseFallbackDirectory = "/usr/share/icons",
            },
            "launcher apps image picture logo"
        );
        entry.visibleWhen = dockLauncherEnabled;
        entries.push_back(std::move(entry));
      }
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "layout", tr("settings.schema.dock.launcher-custom-image-colorize.label"),
            tr("settings.schema.dock.launcher-custom-image-colorize.description"),
            path("launcher_custom_image_colorize"), ToggleSetting{dock.launcherCustomImageColorize},
            "launcher apps image tint color"
        );
        entry.visibleWhen = dockLauncherEnabled;
        entries.push_back(std::move(entry));
      }
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.position.label"),
          tr("settings.schema.dock.position.description"), path("position"),
          asSegmented(enumSelect(kDockEdges, dock.position)), "edge"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.dock.icon-size.label"),
          tr("settings.schema.dock.icon-size.description"), path("icon_size"),
          sliderFor(dock.iconSize, noctalia::config::schema::kDockIconSizeRange, true), "apps"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.main-axis-padding.label"),
          tr("settings.schema.dock.main-axis-padding.description"), path("main_axis_padding"),
          sliderFor(dock.mainAxisPadding, noctalia::config::schema::kDockPaddingRange, true), "inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.cross-axis-padding.label"),
          tr("settings.schema.dock.cross-axis-padding.description"), path("cross_axis_padding"),
          sliderFor(dock.crossAxisPadding, noctalia::config::schema::kDockPaddingRange, true), "inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.dock.item-spacing.label"),
          tr("settings.schema.dock.item-spacing.description"), path("item_spacing"),
          sliderFor(dock.itemSpacing, noctalia::config::schema::kDockItemSpacingRange, true), "gap"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.ends-margin.label"),
          tr("settings.schema.dock.ends-margin.description"), path("margin_ends"),
          sliderFor(dock.marginEnds, noctalia::config::schema::kDockMarginEndsRange, true), "gap inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "layout", tr("settings.schema.shared.edge-margin.label"),
          tr("settings.schema.dock.edge-margin.description"), path("margin_edge"),
          sliderFor(dock.marginEdge, noctalia::config::schema::kDockMarginEdgeRange, true), "gap inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-radius.label"),
          tr("settings.schema.dock.corner-radius.description"), path("radius"),
          sliderFor(dock.radius, noctalia::config::schema::kDockRadiusRange, true), "rounded"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-top-left.label"),
          tr("settings.schema.dock.corner-top-left.description"), path("radius_top_left"),
          sliderFor(dock.radiusTopLeft, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-top-right.label"),
          tr("settings.schema.dock.corner-top-right.description"), path("radius_top_right"),
          sliderFor(dock.radiusTopRight, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-bottom-left.label"),
          tr("settings.schema.dock.corner-bottom-left.description"), path("radius_bottom_left"),
          sliderFor(dock.radiusBottomLeft, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.shared.corner-bottom-right.label"),
          tr("settings.schema.dock.corner-bottom-right.description"), path("radius_bottom_right"),
          sliderFor(dock.radiusBottomRight, noctalia::config::schema::kDockRadiusRange, true), "rounded corner", true
      ));
      {
        auto entry = makeEntry(
            SettingsSection::Dock, "shape", tr("settings.schema.dock.concave-edge-corners.label"),
            tr("settings.schema.dock.concave-edge-corners.description"), path("concave_edge_corners"),
            ToggleSetting{dock.concaveEdgeCorners}, "rounded corner carve"
        );
        entry.visibleWhen = [currentDock](const Config& current) { return currentDock(current).marginEdge == 0; };
        entries.push_back(std::move(entry));
      }
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.dock.border.label"),
          tr("settings.schema.dock.border.description"), path("border"), colorSpecPicker(dock.border), "outline color",
          true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "shape", tr("settings.schema.dock.border-width.label"),
          tr("settings.schema.dock.border-width.description"), path("border_width"),
          sliderFor(dock.borderWidth, noctalia::config::schema::kDockBorderWidthRange, false), "outline stroke", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "effects", tr("settings.schema.shared.background-opacity.label"),
          tr("settings.schema.dock.background-opacity.description"), path("background_opacity"),
          sliderFor(dock.backgroundOpacity, noctalia::config::schema::kUnitRange, false),
          "alpha transparency translucent opacity"
      ));
      if (env.backgroundEffectBlurSupported) {
        entries.push_back(makeEntry(
            SettingsSection::Dock, "effects", tr("settings.schema.dock.compositor-blur.label"),
            tr("settings.schema.dock.compositor-blur.description"), path("compositor_blur"),
            ToggleSetting{dock.compositorBlur}, "blur background effect"
        ));
      }
      entries.push_back(makeEntry(
          SettingsSection::Dock, "effects", tr("settings.schema.shared.shadow.label"),
          tr("settings.schema.dock.shadow.description"), path("shadow"), ToggleSetting{dock.shadow}, "shadow"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "effects", tr("settings.schema.dock.icon-shadow.label"),
          tr("settings.schema.dock.icon-shadow.description"), path("icon_shadow"), ToggleSetting{dock.iconShadow},
          "shadow icon"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "effects", tr("settings.schema.shared.colorize-icons.label"),
          tr("settings.schema.shared.colorize-icons.description"), path("colorize_icons"),
          ToggleSetting{dock.colorizeIcons}, "colorize tint application icons"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "effects", tr("settings.schema.dock.icon-saturation.label"),
          tr("settings.schema.dock.icon-saturation.description"), path("icon_saturation"),
          sliderFor(dock.iconSaturation, noctalia::config::schema::kUnitRange, false), "grayscale desaturate color",
          true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "focus-styling", tr("settings.schema.dock.magnification.label"),
          tr("settings.schema.dock.magnification.description"), path("magnification"),
          ToggleSetting{dock.magnification}, "magnify zoom mac"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "focus-styling", tr("settings.schema.dock.magnification-scale.label"),
          tr("settings.schema.dock.magnification-scale.description"), path("magnification_scale"),
          sliderFor(dock.magnificationScale, noctalia::config::schema::kDockMagnificationScaleRange, false),
          "magnify zoom"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "focus-styling", tr("settings.schema.dock.active-icon-scale.label"),
          tr("settings.schema.dock.active-icon-scale.description"), path("active_scale"),
          sliderFor(dock.activeScale, noctalia::config::schema::kDockActiveScaleRange, false), "focused", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "focus-styling", tr("settings.schema.dock.inactive-icon-scale.label"),
          tr("settings.schema.dock.inactive-icon-scale.description"), path("inactive_scale"),
          sliderFor(dock.inactiveScale, noctalia::config::schema::kDockInactiveScaleRange, false), "unfocused", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "focus-styling", tr("settings.schema.dock.active-icon-opacity.label"),
          tr("settings.schema.dock.active-icon-opacity.description"), path("active_opacity"),
          sliderFor(dock.activeOpacity, noctalia::config::schema::kUnitRange, false), "focused alpha", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "focus-styling", tr("settings.schema.dock.inactive-icon-opacity.label"),
          tr("settings.schema.dock.inactive-icon-opacity.description"), path("inactive_opacity"),
          sliderFor(dock.inactiveOpacity, noctalia::config::schema::kUnitRange, false), "unfocused alpha", true
      ));
      entries.push_back(makeEntry(
          SettingsSection::Dock, "pinned-apps", tr("settings.schema.dock.pinned-apps.label"),
          tr("settings.schema.dock.pinned-apps.description"), path("pinned"), ListSetting{.items = dock.pinned},
          "pinned apps dock"
      ));
    };

    addDockPage(cfg.dock, {"dock"}, std::nullopt);
    for (const auto& override : cfg.dock.monitorOverrides) {
      addDockPage(
          resolveDockMonitorOverride(cfg.dock, override), {"dock", "monitor", override.tableName}, override.tableName
      );
    }
  }

} // namespace settings::registry
