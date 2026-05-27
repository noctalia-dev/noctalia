#include "shell/settings/settings_content.h"

#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "shell/settings/bar_widget_editor.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/keybind_recorder.h"
#include "ui/controls/label.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace settings {
  namespace {

    bool isMonitorOverrideSettingPath(const std::vector<std::string>& path) {
      return path.size() >= 5 && path[0] == "bar" && path[2] == "monitor";
    }

    bool isDockLauncherIconPath(const std::vector<std::string>& path) {
      return path.size() == 2 && path[0] == "dock" && path[1] == "launcher_icon";
    }

    bool monitorOverrideHasExplicitValue(const Config& cfg, const std::vector<std::string>& path) {
      if (!isMonitorOverrideSettingPath(path)) {
        return false;
      }

      const auto* bar = findBar(cfg, path[1]);
      if (bar == nullptr) {
        return false;
      }

      const auto* override = findMonitorOverride(*bar, path[3]);
      if (override == nullptr) {
        return false;
      }

      const std::string_view key = path.back();
      if (key == "enabled") {
        return override->enabled.has_value();
      }
      if (key == "auto_hide") {
        return override->autoHide.has_value();
      }
      if (key == "reserve_space") {
        return override->reserveSpace.has_value();
      }
      if (key == "thickness") {
        return override->thickness.has_value();
      }
      if (key == "scale") {
        return override->scale.has_value();
      }
      if (key == "margin_ends") {
        return override->marginEnds.has_value();
      }
      if (key == "margin_edge") {
        return override->marginEdge.has_value();
      }
      if (key == "padding") {
        return override->padding.has_value();
      }
      if (key == "radius") {
        return override->radius.has_value();
      }
      if (key == "radius_top_left") {
        return override->radiusTopLeft.has_value();
      }
      if (key == "radius_top_right") {
        return override->radiusTopRight.has_value();
      }
      if (key == "radius_bottom_left") {
        return override->radiusBottomLeft.has_value();
      }
      if (key == "radius_bottom_right") {
        return override->radiusBottomRight.has_value();
      }
      if (key == "background_opacity") {
        return override->backgroundOpacity.has_value();
      }
      if (key == "border") {
        return override->border.has_value();
      }
      if (key == "border_width") {
        return override->borderWidth.has_value();
      }
      if (key == "shadow") {
        return override->shadow.has_value();
      }
      if (key == "widget_spacing") {
        return override->widgetSpacing.has_value();
      }
      if (key == "capsule") {
        return override->widgetCapsuleDefault.has_value();
      }
      if (key == "capsule_fill") {
        return override->widgetCapsuleFill.has_value();
      }
      if (key == "capsule_border") {
        return override->widgetCapsuleBorderSpecified;
      }
      if (key == "capsule_foreground") {
        return override->widgetCapsuleForeground.has_value();
      }
      if (key == "color") {
        return override->widgetColor.has_value();
      }
      if (key == "capsule_groups") {
        return override->widgetCapsuleGroups.has_value();
      }
      if (key == "capsule_padding") {
        return override->widgetCapsulePadding.has_value();
      }
      if (key == "capsule_radius") {
        return override->widgetCapsuleRadius.has_value();
      }
      if (key == "capsule_opacity") {
        return override->widgetCapsuleOpacity.has_value();
      }
      if (key == "start") {
        return override->startWidgets.has_value();
      }
      if (key == "center") {
        return override->centerWidgets.has_value();
      }
      if (key == "end") {
        return override->endWidgets.has_value();
      }
      return false;
    }

    bool isBarCapsuleGroupsPath(const std::vector<std::string>& path) {
      return path.size() == 3 && path[0] == "bar" && path[2] == "capsule_groups";
    }

    bool isMonitorCapsuleGroupsPath(const std::vector<std::string>& path) {
      return path.size() == 5 && path[0] == "bar" && path[2] == "monitor" && path[4] == "capsule_groups";
    }

    bool isCapsuleGroupsPath(const std::vector<std::string>& path) {
      return isBarCapsuleGroupsPath(path) || isMonitorCapsuleGroupsPath(path);
    }

    void collectWidgetNames(std::unordered_set<std::string>& widgetNames, const std::vector<std::string>& widgets) {
      for (const auto& widget : widgets) {
        widgetNames.insert(widget);
      }
    }

    std::unordered_set<std::string> scopedBarWidgetNames(const Config& cfg, const std::vector<std::string>& path) {
      std::unordered_set<std::string> widgetNames;

      const auto* bar = path.size() >= 2 ? findBar(cfg, path[1]) : nullptr;
      if (bar == nullptr) {
        return widgetNames;
      }

      if (isBarCapsuleGroupsPath(path)) {
        collectWidgetNames(widgetNames, bar->startWidgets);
        collectWidgetNames(widgetNames, bar->centerWidgets);
        collectWidgetNames(widgetNames, bar->endWidgets);
        for (const auto& ovr : bar->monitorOverrides) {
          collectWidgetNames(widgetNames, ovr.startWidgets.value_or(bar->startWidgets));
          collectWidgetNames(widgetNames, ovr.centerWidgets.value_or(bar->centerWidgets));
          collectWidgetNames(widgetNames, ovr.endWidgets.value_or(bar->endWidgets));
        }
        return widgetNames;
      }

      const auto* ovr = path.size() >= 4 ? findMonitorOverride(*bar, path[3]) : nullptr;
      if (ovr == nullptr) {
        return widgetNames;
      }

      collectWidgetNames(widgetNames, ovr->startWidgets.value_or(bar->startWidgets));
      collectWidgetNames(widgetNames, ovr->centerWidgets.value_or(bar->centerWidgets));
      collectWidgetNames(widgetNames, ovr->endWidgets.value_or(bar->endWidgets));
      return widgetNames;
    }

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> capsuleGroupRemovalOverrides(
        const Config& cfg, const std::vector<std::string>& path, std::string_view removedGroup,
        std::vector<std::string> updatedGroups
    ) {
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      overrides.push_back({path, std::move(updatedGroups)});

      if (!isCapsuleGroupsPath(path)) {
        return overrides;
      }

      const std::string trimmedRemoved = StringUtils::trim(removedGroup);
      if (trimmedRemoved.empty()) {
        return overrides;
      }

      for (const auto& widgetName : scopedBarWidgetNames(cfg, path)) {
        const auto widgetIt = cfg.widgets.find(widgetName);
        if (widgetIt == cfg.widgets.end() || !widgetIt->second.hasSetting("capsule_group")) {
          continue;
        }
        if (StringUtils::trim(widgetIt->second.getString("capsule_group", "")) != trimmedRemoved) {
          continue;
        }
        overrides.push_back({{"widget", widgetName, "capsule_group"}, std::string()});
      }

      return overrides;
    }

    void addIdleLiveStatusPanel(Flex& section, SettingsContentContext& ctx, float scale) {
      Label* linePtr = nullptr;
      auto line = ui::label({
          .out = &linePtr,
          .text = "",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      });
      if (ctx.registerIdleLiveStatusLabel) {
        ctx.registerIdleLiveStatusLabel(linePtr);
      }
      section.addChild(std::move(line));
    }

  } // namespace

  std::size_t
  addSettingsContentSections(Flex& content, const std::vector<SettingEntry>& registry, SettingsContentContext ctx) {
    const Config& cfg = ctx.config;
    const float scale = ctx.scale;

    const auto sectionLabel = [](std::string_view section) {
      return i18n::tr("settings.navigation.sections." + std::string(section));
    };

    const auto groupLabel = [](std::string_view group) -> std::string {
      return i18n::tr("settings.navigation.groups." + std::string(group));
    };

    const auto makeSection = [&](std::string_view title, std::string_view sectionKey) -> Flex* {
      auto section = ui::column(
          {
              .align = FlexAlign::Stretch,
              .gap = Style::spaceSm * scale,
              .padding = Style::spaceLg * scale,
              .configure = [](Flex& flex) { flex.setFill(clearColorSpec()); },
          },
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
              ui::glyph({
                  .glyph = std::string(sectionGlyph(sectionKey)),
                  .glyphSize = Style::fontSizeHeader * scale,
                  .color = colorSpecFromRole(ColorRole::Primary),
              }),
              makeLabel(title, Style::fontSizeHeader * scale, colorSpecFromRole(ColorRole::Primary), FontWeight::Bold)
          )
      );
      auto* raw = section.get();
      content.addChild(std::move(section));
      return raw;
    };

    const auto addGroupLabel = [&](Flex& section, std::string_view title, bool isFirst) {
      if (title.empty()) {
        return;
      }
      if (!isFirst) {
        section.addChild(
            ui::column(
                {.align = FlexAlign::Stretch,
                 .gap = Style::spaceSm * scale,
                 .configure = [scale](Flex& flex) { flex.setPadding(Style::spaceSm * scale, 0.0f, 0.0f, 0.0f); }},
                ui::separator(),
                makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold)
            )
        );
      } else {
        section.addChild(
            makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold)
        );
      }
    };

    const auto makeResetButton = [&](const std::vector<std::string>& path) {
      return ui::button({
          .text = i18n::tr("settings.actions.reset"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [clearOverride = ctx.clearOverride, path]() { clearOverride(path); },
      });
    };

    const auto makeStatusBadge = [&](std::string_view label, const ColorSpec& fill, const ColorSpec& color,
                                     bool matchResetHeight) {
      return ui::row(
          {.align = FlexAlign::Center,
           .configure =
               [scale, fill, matchResetHeight](Flex& flex) {
                 if (matchResetHeight) {
                   flex.setMinHeight(Style::controlHeightSm * scale);
                   flex.setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
                 } else {
                   flex.setPadding(0.0f, Style::spaceXs * scale);
                 }
                 flex.setRadius(Style::scaledRadiusSm(scale));
                 flex.setFill(fill);
               }},
          makeLabel(label, Style::fontSizeCaption * scale, color, FontWeight::Bold)
      );
    };

    const auto makeOverrideBadge = [&]() {
      return makeStatusBadge(
          i18n::tr("settings.badges.override"), colorSpecFromRole(ColorRole::Primary, 0.15f),
          colorSpecFromRole(ColorRole::Primary), false
      );
    };

    const auto makeAdvancedBadge = [&]() {
      return makeStatusBadge(
          i18n::tr("settings.badges.advanced"), colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.12f),
          colorSpecFromRole(ColorRole::OnSurfaceVariant), false
      );
    };

    const auto makeOverrideResetActions = [&](const std::vector<std::string>& path) {
      return ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, makeOverrideBadge(), makeResetButton(path)
      );
    };

    const auto isTemplateEnableTogglePath = [](const std::vector<std::string>& path) {
      return path == std::vector<std::string>{"theme", "templates", "enable_builtin_templates"}
      || path == std::vector<std::string>{"theme", "templates", "enable_community_templates"};
    };

    const auto makeRow = [&](Flex& section, const SettingEntry& entry, std::unique_ptr<Node> control) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));
      const bool redundantGuiOverride =
          ctx.configService != nullptr && ctx.configService->hasOverride(entry.path) && !overridden;
      const bool monitorSetting = isMonitorOverrideSettingPath(entry.path);
      const bool monitorExplicit = monitorOverrideHasExplicitValue(cfg, entry.path) && !redundantGuiOverride;
      const bool monitorInherited = monitorSetting && !monitorExplicit;

      auto titleRow = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .fillWidth = true,
      });
      titleRow->addChild(
          makeLabel(entry.title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      if (entry.advanced) {
        titleRow->addChild(makeAdvancedBadge());
      }
      if (monitorExplicit) {
        titleRow->addChild(makeStatusBadge(
            i18n::tr("settings.badges.monitor"), colorSpecFromRole(ColorRole::Secondary, 0.15f),
            colorSpecFromRole(ColorRole::Secondary), false
        ));
      } else if (monitorInherited) {
        titleRow->addChild(makeStatusBadge(
            i18n::tr("settings.badges.inherited"), colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.12f),
            colorSpecFromRole(ColorRole::OnSurfaceVariant), false
        ));
      }
      titleRow->addChild(ui::spacer());

      ui::FlexProps copyProps{.align = FlexAlign::Start, .flexGrow = 1.0f};
      if (!isTemplateEnableTogglePath(entry.path)) {
        copyProps.gap = Style::spaceXs * scale;
      }
      auto copy = ui::column(std::move(copyProps));
      copy->addChild(std::move(titleRow));

      if (!entry.subtitle.empty()) {
        copy->addChild(makeSettingSubtitleLabel(entry.subtitle, scale));
      }

      auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      if (overridden) {
        actions->addChild(makeOverrideBadge());
        actions->addChild(makeResetButton(entry.path));
      }
      actions->addChild(std::move(control));

      auto row = ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::SpaceBetween,
           .gap = Style::spaceXs * scale,
           .configure =
               [scale](Flex& flex) {
                 flex.setPadding(2.0f * scale, 0.0f);
                 flex.setMinHeight(Style::controlHeight * scale);
               }},
          std::move(copy), std::move(actions)
      );

      section.addChild(std::move(row));
    };

    const auto makeToggle = [&](bool checked, bool enabled, const std::vector<std::string>& path,
                                std::optional<bool> clearWhenValue = std::nullopt) {
      if (enabled) {
        return ui::toggle({
            .checked = checked,
            .enabled = enabled,
            .scale = scale,
            .onChange = [configService = ctx.configService, setOverride = ctx.setOverride,
                         clearOverride = ctx.clearOverride, path, clearWhenValue](bool value) {
              if (clearWhenValue.has_value()
                  && value == *clearWhenValue
                  && configService != nullptr
                  && configService->hasOverride(path)) {
                clearOverride(path);
                return;
              }
              setOverride(path, value);
            },
        });
      }
      return ui::toggle({
          .checked = checked,
          .enabled = enabled,
          .scale = scale,
      });
    };

    const auto makeSelect = [&](const SelectSetting& setting,
                                const std::vector<std::string>& path) -> std::unique_ptr<Node> {
      if (setting.segmented) {
        std::vector<ui::SegmentedOption> segmentedOptions;
        segmentedOptions.reserve(setting.options.size());
        for (const auto& opt : setting.options) {
          segmentedOptions.push_back(ui::SegmentedOption{.label = opt.label});
        }
        auto options = setting.options;
        const bool integerValue = setting.integerValue;
        return ui::segmented({
            .options = std::move(segmentedOptions),
            .selectedIndex = optionIndex(setting.options, setting.selectedValue),
            .scale = scale,
            .onChange = [setOverride = ctx.setOverride, path, options, integerValue](std::size_t index) {
              if (index < options.size()) {
                if (integerValue) {
                  setOverride(path, static_cast<std::int64_t>(std::stoll(options[index].value)));
                } else {
                  setOverride(path, options[index].value);
                }
              }
            },
        });
      }

      const auto selectedIndex = optionIndex(setting.options, setting.selectedValue);
      const bool clearSelection = !selectedIndex.has_value() && !setting.selectedValue.empty();
      const float selectWidth = setting.preferredWidth > 0.0f ? setting.preferredWidth : 190.0f;
      auto options = setting.options;
      const bool clearOnEmpty = setting.clearOnEmpty;
      const bool integerValue = setting.integerValue;
      return ui::select({
          .options = optionLabels(setting.options),
          .selectedIndex = selectedIndex,
          .clearSelection = clearSelection,
          .placeholder = clearSelection ? std::optional<std::string>{i18n::tr(
                                              "settings.controls.select.unknown-value", "value", setting.selectedValue
                                          )}
                                        : std::nullopt,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .colorSwatchPreviews = optionSwatchPreviews(setting.options),
          .width = selectWidth * scale,
          .height = Style::controlHeight * scale,
          .onSelectionChanged = [configService = ctx.configService, clearOverride = ctx.clearOverride,
                                 setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path, options,
                                 clearOnEmpty, integerValue](std::size_t index, std::string_view /*label*/) {
            if (index < options.size()) {
              if (clearOnEmpty && options[index].value.empty()) {
                if (configService != nullptr && configService->hasOverride(path)) {
                  clearOverride(path);
                } else {
                  requestRebuild();
                }
                return;
              }
              if (integerValue) {
                setOverride(path, static_cast<std::int64_t>(std::stoll(options[index].value)));
              } else {
                setOverride(path, options[index].value);
              }
            }
          },
      });
    };

    const auto makeSlider =
        [&](double value, double minValue, double maxValue, double step, const std::vector<std::string>& path,
            bool integerValue = false,
            const std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double)>&
                linkedCommit = {}) {
          auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

          Input* valueInputPtr = nullptr;
          auto valueInput = ui::input({
              .out = &valueInputPtr,
              .value = formatSliderValue(value, integerValue),
              .fontSize = Style::fontSizeCaption * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .horizontalPadding = Style::spaceXs * scale,
              .width = 50.0f * scale,
              .height = Style::controlHeightSm * scale,
          });

          Slider* sliderPtr = nullptr;
          auto slider = ui::slider({
              .out = &sliderPtr,
              .minValue = minValue,
              .maxValue = maxValue,
              .step = step,
              .value = value,
              .trackHeight = Style::sliderTrackHeight * scale,
              .thumbSize = Style::sliderThumbSize * scale,
              .controlHeight = Style::controlHeight * scale,
              .width = Style::sliderDefaultWidth * scale,
              .height = Style::controlHeight * scale,
              .onValueChanged = [valueInputPtr, integerValue](double next) {
                valueInputPtr->setInvalid(false);
                valueInputPtr->setValue(formatSliderValue(next, integerValue));
              },
          });

          // Helper: commit either via single setOverride or as an atomic batch when linkedCommit
          // returns extra overrides (cross-field constraints).
          const auto commit = [setOverride = ctx.setOverride, setOverrides = ctx.setOverrides, path, integerValue,
                               linkedCommit](double v) {
            ConfigOverrideValue primary =
                integerValue ? ConfigOverrideValue{static_cast<std::int64_t>(std::lround(v))} : ConfigOverrideValue{v};
            if (linkedCommit) {
              auto extras = linkedCommit(v);
              if (!extras.empty()) {
                std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> all;
                all.reserve(extras.size() + 1);
                all.emplace_back(path, std::move(primary));
                for (auto& e : extras) {
                  all.push_back(std::move(e));
                }
                setOverrides(std::move(all));
                return;
              }
            }
            setOverride(path, std::move(primary));
          };

          slider->setOnDragEnd([commit, sliderPtr]() { commit(sliderPtr->value()); });

          const auto commitInputText = [commit, sliderPtr, valueInputPtr, minValue, maxValue,
                                        integerValue](const std::string& text) {
            const auto parsed = parseDoubleInput(text);
            if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
              valueInputPtr->setInvalid(true);
              return;
            }
            const double v = *parsed;
            valueInputPtr->setInvalid(false);
            sliderPtr->setValue(v);
            if (!integerValue) {
              valueInputPtr->setValue(formatSliderValue(sliderPtr->value(), false));
            }
            commit(v);
          };

          valueInput->setOnChange([valueInputPtr](const std::string& /*text*/) { valueInputPtr->setInvalid(false); });
          valueInput->setOnSubmit([commitInputText](const std::string& text) { commitInputText(text); });
          valueInput->setOnFocusLoss([commitInputText, valueInputPtr]() { commitInputText(valueInputPtr->value()); });

          // Slider first, numeric value field on the right (reset from makeRow stays left of this cluster).
          wrap->addChild(std::move(slider));
          wrap->addChild(std::move(valueInput));
          return wrap;
        };

    const auto makeText = [&](const std::string& value, const std::string& placeholder,
                              const std::vector<std::string>& path, float width = 0.0f) {
      const float inputWidth = (width > 0.0f ? width : 190.0f) * scale;
      auto input = ui::input({
          .value = value,
          .placeholder = placeholder.empty() ? i18n::tr("settings.controls.list.add-entry-placeholder") : placeholder,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .width = inputWidth,
          .height = Style::controlHeight * scale,
          .onSubmit = [setOverride = ctx.setOverride, path](const std::string& v) { setOverride(path, v); },
          .submitOnFocusLoss = true,
      });
      return input;
    };

    const auto makeTextWithPathBrowse = [&](const TextSetting& setting, const std::vector<std::string>& path) {
      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

      const float inputWidth = (setting.width > 0.0f ? setting.width : 280.0f) * scale;
      Input* inputPtr = nullptr;
      auto input = ui::input({
          .out = &inputPtr,
          .value = setting.value,
          .placeholder = setting.placeholder.empty() ? i18n::tr("settings.controls.list.add-entry-placeholder")
                                                     : setting.placeholder,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .width = inputWidth,
          .height = Style::controlHeight * scale,
          .onSubmit = [setOverride = ctx.setOverride, path](const std::string& v) { setOverride(path, v); },
          .submitOnFocusLoss = true,
      });
      wrap->addChild(std::move(input));

      const bool selectFolder = setting.browseMode == TextSettingBrowseMode::SelectFolder;
      auto browse = ui::button({
          .glyph = selectFolder ? "folder" : "file-text",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Outline,
          .minWidth = Style::controlHeight * scale,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [setOverride = ctx.setOverride, path, inputPtr, selectFolder,
                      exts = setting.browseFileExtensions]() {
            FileDialogOptions options;
            options.mode = selectFolder ? FileDialogMode::SelectFolder : FileDialogMode::Open;
            options.defaultViewMode = FileDialogViewMode::List;
            options.title = selectFolder ? i18n::tr("settings.controls.path-browse.folder-title")
                                         : i18n::tr("settings.controls.path-browse.file-title");
            if (!selectFolder) {
              options.extensions = exts;
            }
            const std::string cur = inputPtr->value();
            if (!cur.empty()) {
              std::filesystem::path p(cur);
              std::error_code ec;
              if (selectFolder) {
                if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec)) {
                  options.startDirectory = p;
                } else if (p.has_parent_path()) {
                  const auto parent = p.parent_path();
                  if (std::filesystem::exists(parent, ec)) {
                    options.startDirectory = parent;
                  }
                }
              } else {
                if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
                  options.startDirectory = p.parent_path();
                  options.defaultFilename = p.filename().string();
                } else if (p.has_parent_path() && std::filesystem::exists(p.parent_path(), ec)) {
                  options.startDirectory = p.parent_path();
                }
              }
            }
            (void)FileDialog::open(
                std::move(options), [setOverride, path, inputPtr](std::optional<std::filesystem::path> picked) {
                  if (!picked.has_value()) {
                    return;
                  }
                  const std::string s = picked->string();
                  inputPtr->setValue(s);
                  setOverride(path, s);
                }
            );
          },
      });
      wrap->addChild(std::move(browse));
      return wrap;
    };

    const auto makeGlyphText = [&](const TextSetting& setting,
                                   const std::vector<std::string>& path) -> std::unique_ptr<Node> {
      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      wrap->addChild(makeText(setting.value, setting.placeholder, path, setting.width));

      auto pickerButton = ui::button({
          .glyph = "apps",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Outline,
          .minWidth = Style::controlHeight * scale,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [setOverride = ctx.setOverride, path, currentValue = setting.value]() {
            GlyphPickerDialogOptions options;
            if (!currentValue.empty()) {
              options.initialGlyph = currentValue;
            }
            (void)GlyphPickerDialog::open(
                std::move(options), [setOverride, path](std::optional<GlyphPickerResult> result) {
                  if (!result.has_value()) {
                    return;
                  }
                  setOverride(path, result->name);
                }
            );
          },
      });
      wrap->addChild(std::move(pickerButton));
      return wrap;
    };

    const auto makeOptionalNumber = [&](const OptionalNumberSetting& setting, const std::vector<std::string>& path) {
      Input* inputPtr = nullptr;
      auto input = ui::input({
          .out = &inputPtr,
          .value = setting.value.has_value() ? std::format("{}", *setting.value) : "",
          .placeholder = setting.placeholder,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .width = 190.0f * scale,
          .height = Style::controlHeight * scale,
      });
      input->setOnChange([inputPtr](const std::string& /*text*/) { inputPtr->setInvalid(false); });
      input->setOnSubmit([configService = ctx.configService, clearOverride = ctx.clearOverride,
                          setOverride = ctx.setOverride, path, inputPtr, minValue = setting.minValue,
                          maxValue = setting.maxValue](const std::string& text) {
        if (isBlankInput(text)) {
          inputPtr->setInvalid(false);
          if (configService != nullptr && configService->hasOverride(path)) {
            clearOverride(path);
          }
          return;
        }

        const auto parsed = parseDoubleInput(text);
        if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
          inputPtr->setInvalid(true);
          return;
        }

        inputPtr->setInvalid(false);
        setOverride(path, *parsed);
      });
      return input;
    };

    const auto makeStepper = [&](const StepperSetting& setting, const std::vector<std::string>& path) {
      const int minValue = std::min(setting.minValue, setting.maxValue);
      const int maxValue = std::max(setting.minValue, setting.maxValue);
      const int currentValue = std::clamp(setting.value, minValue, maxValue);

      return ui::stepper({
          .minValue = minValue,
          .maxValue = maxValue,
          .step = setting.step,
          .value = currentValue,
          .scale = scale,
          .valueSuffix = setting.valueSuffix.empty() ? std::nullopt : std::optional<std::string>{setting.valueSuffix},
          .onValueCommitted = [setOverride = ctx.setOverride, path](int value) {
            setOverride(path, static_cast<double>(value));
          },
      });
    };

    const auto makeOptionalStepper = [&](const OptionalStepperSetting& setting, const std::vector<std::string>& path) {
      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

      const int minValue = std::min(setting.minValue, setting.maxValue);
      const int maxValue = std::max(setting.minValue, setting.maxValue);
      const int currentValue = std::clamp(setting.value.value_or(setting.fallbackValue), minValue, maxValue);

      auto segmented = ui::segmented({
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = setting.unsetLabel},
                  {.label = setting.customLabel},
              },
          .selectedIndex = static_cast<std::size_t>(setting.value.has_value() ? 1 : 0),
          .scale = scale,
          .onChange = [configService = ctx.configService, clearOverride = ctx.clearOverride,
                       requestRebuild = ctx.requestRebuild, setOverride = ctx.setOverride, path,
                       currentValue](std::size_t index) {
            if (index == 0) {
              if (configService != nullptr && configService->hasOverride(path)) {
                clearOverride(path);
              } else if (requestRebuild) {
                requestRebuild();
              }
              return;
            }
            setOverride(path, static_cast<double>(currentValue));
          },
      });

      auto stepper = ui::stepper({
          .minValue = minValue,
          .maxValue = maxValue,
          .step = setting.step,
          .value = currentValue,
          .enabled = setting.value.has_value(),
          .scale = scale,
          .onValueCommitted = [setOverride = ctx.setOverride, path](int value) {
            setOverride(path, static_cast<double>(value));
          },
      });

      wrap->addChild(std::move(segmented));
      wrap->addChild(std::move(stepper));
      return wrap;
    };

    const auto makeColorSpecPicker = [&](const ColorSpecPickerSetting& setting,
                                         const std::vector<std::string>& path) -> std::unique_ptr<Node> {
      ColorSpecSelectOptions options{
          .roles = setting.roles,
          .selectedValue = setting.selectedValue,
          .allowNone = setting.allowNone,
          .allowCustomColor = setting.allowCustomColor,
          .noneLabel = setting.noneLabel,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .width = 190.0f * scale,
      };
      return makeColorSpecSelect(
          std::move(options), [setOverride = ctx.setOverride, path](std::string value) { setOverride(path, value); },
          [configService = ctx.configService, clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild,
           path]() {
            if (configService != nullptr && configService->hasOverride(path)) {
              clearOverride(path);
            } else {
              requestRebuild();
            }
          }
      );
    };

    const auto makeSearchPickerButton = [&](const SettingEntry& entry,
                                            const SearchPickerSetting& setting) -> std::unique_ptr<Node> {
      return ui::button({
          .text = optionLabel(setting.options, setting.selectedValue),
          .glyph = "search",
          .fontSize = Style::fontSizeBody * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Outline,
          .minWidth = 190.0f * scale,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [openPopup = ctx.openSearchPickerPopup, title = entry.title, options = setting.options,
                      selectedValue = setting.selectedValue, placeholder = setting.placeholder,
                      emptyText = setting.emptyText, path = entry.path]() {
            if (openPopup) {
              openPopup(title, options, selectedValue, placeholder, emptyText, path);
            }
          },
      });
    };

    const auto makeCollectionBlock = [&](const SettingEntry& entry, bool overridden, bool reserveTitleHeight = false,
                                         bool titleMaxTwoLines = false, bool fillWidth = false, bool flexGrow = false,
                                         bool compactTitleDescription = false) {
      ui::FlexProps blockProps{
          .align = FlexAlign::Stretch,
          .configure = [scale, fillWidth, flexGrow](Flex& flex) {
            flex.setPadding(Style::spaceXs * scale, 0.0f);
            if (fillWidth) {
              flex.setFillWidth(true);
            }
            if (flexGrow) {
              flex.setFlexGrow(1.0f);
            }
          },
      };
      if (!compactTitleDescription) {
        blockProps.gap = Style::spaceXs * scale;
      }
      auto block = ui::column(std::move(blockProps));

      auto titleRow = ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .configure =
               [scale, reserveTitleHeight](Flex& flex) {
                 if (reserveTitleHeight) {
                   flex.setMinHeight(Style::controlHeightSm * scale);
                 }
               }},
          ui::label({
              .text = entry.title,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = titleMaxTwoLines ? std::optional<int>{2} : std::nullopt,
              .fontWeight = FontWeight::Bold,
          })
      );
      ui::FlexProps copyProps{.align = FlexAlign::Start, .flexGrow = 1.0f};
      if (!compactTitleDescription) {
        copyProps.gap = Style::spaceXs * scale;
      }
      auto copy = ui::column(std::move(copyProps));
      copy->addChild(std::move(titleRow));
      if (!entry.subtitle.empty()) {
        copy->addChild(makeSettingSubtitleLabel(entry.subtitle, scale));
      }

      auto header = ui::row({.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .fillWidth = true});
      header->addChild(std::move(copy));
      if (overridden) {
        header->addChild(makeOverrideResetActions(entry.path));
      }
      block->addChild(std::move(header));
      return block;
    };

    const auto makeMultiSelectBlock = [&](Flex& section, const SettingEntry& entry, const MultiSelectSetting& setting) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden);

      auto checkRow =
          ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .configure = [scale](Flex& flex) {
                     flex.setPadding(Style::spaceXs * scale, 0.0f);
                   }});

      auto options = setting.options;
      auto selected = setting.selectedValues;
      const bool requireAtLeastOne = setting.requireAtLeastOne;
      auto path = entry.path;

      for (const auto& option : options) {
        auto item = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

        const bool isSelected = std::find(selected.begin(), selected.end(), option.value) != selected.end();
        const std::string optionValue = option.value;
        auto checkbox = ui::checkbox({
            .checked = isSelected,
            .scale = scale,
            .onChange = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path, options, selected,
                         optionValue, requireAtLeastOne](bool checked) mutable {
              auto it = std::find(selected.begin(), selected.end(), optionValue);
              if (checked) {
                if (it == selected.end()) {
                  selected.push_back(optionValue);
                }
              } else {
                if (it != selected.end()) {
                  if (requireAtLeastOne && selected.size() <= 1) {
                    requestRebuild();
                    return;
                  }
                  selected.erase(it);
                }
              }
              // Preserve the option order so the override file is stable.
              std::vector<std::string> ordered;
              ordered.reserve(selected.size());
              for (const auto& opt : options) {
                if (std::find(selected.begin(), selected.end(), opt.value) != selected.end()) {
                  ordered.push_back(opt.value);
                }
              }
              setOverride(path, ordered);
            },
        });
        item->addChild(std::move(checkbox));
        item->addChild(makeLabel(
            option.label, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Normal
        ));

        checkRow->addChild(std::move(item));
      }

      block->addChild(std::move(checkRow));
      section.addChild(std::move(block));
    };

    const auto makeTemplateGridBlock = [&](Flex& section, const SettingEntry& entry,
                                           const TemplateGridSetting& setting) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden, false, false, false, false, true);

      if (setting.options.empty()) {
        block->addChild(makeSettingSubtitleLabel(setting.emptyText, scale));
        section.addChild(std::move(block));
        return;
      }

      constexpr std::size_t kTemplateCardsPerRow = 5;
      auto selected = std::make_shared<std::vector<std::string>>(setting.selectedValues);
      const auto options = std::make_shared<std::vector<SelectOption>>(setting.options);
      const auto path = entry.path;

      auto commit = [setOverride = ctx.setOverride, path, options, selected]() {
        std::vector<std::string> ordered;
        ordered.reserve(selected->size());
        for (const auto& opt : *options) {
          if (std::find(selected->begin(), selected->end(), opt.value) != selected->end()) {
            ordered.push_back(opt.value);
          }
        }
        setOverride(path, std::move(ordered));
      };

      auto grid =
          ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .configure = [scale](Flex& flex) {
                        flex.setPadding(Style::spaceMd * scale, 0.0f, 0.0f, 0.0f);
                      }});
      std::unique_ptr<Flex> row;
      std::size_t countInRow = 0;

      auto flushRow = [&]() {
        if (row == nullptr) {
          return;
        }
        while (countInRow > 0 && countInRow < kTemplateCardsPerRow) {
          row->addChild(ui::row({.fillWidth = true, .flexGrow = 1.0f}));
          ++countInRow;
        }
        grid->addChild(std::move(row));
        countInRow = 0;
      };

      for (const auto& option : *options) {
        if (row == nullptr || countInRow == kTemplateCardsPerRow) {
          flushRow();
          row = ui::row({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
        }

        const bool checked = std::find(selected->begin(), selected->end(), option.value) != selected->end();
        const std::string value = option.value;
        Button* card = nullptr;
        Label* titleLabel = nullptr;
        Label* categoryLabel = nullptr;
        Checkbox* checkbox = nullptr;
        auto checkedState = std::make_shared<bool>(checked);
        const auto cardPaletteFor = [scale](bool active) {
          return Button::ButtonPalette{
              .borderWidth = 1.0f * scale,
              .normal =
                  Button::ButtonStateColors{
                      .bg = colorSpecFromRole(
                          active ? ColorRole::Primary : ColorRole::SurfaceVariant, active ? 1.0f : 0.45f
                      ),
                      .border =
                          colorSpecFromRole(active ? ColorRole::Primary : ColorRole::Outline, active ? 0.9f : 0.45f),
                      .label = colorSpecFromRole(active ? ColorRole::OnPrimary : ColorRole::OnSurface),
                  },
              .hover =
                  Button::ButtonStateColors{
                      .bg = colorSpecFromRole(active ? ColorRole::Primary : ColorRole::Hover),
                      .border = colorSpecFromRole(active ? ColorRole::Primary : ColorRole::Hover),
                      .label = colorSpecFromRole(active ? ColorRole::OnPrimary : ColorRole::OnHover),
                  },
              .pressed =
                  Button::ButtonStateColors{
                      .bg = colorSpecFromRole(ColorRole::Primary),
                      .border = colorSpecFromRole(ColorRole::Primary),
                      .label = colorSpecFromRole(ColorRole::OnPrimary),
                  },
              .disabled =
                  Button::ButtonStateColors{
                      .bg = colorSpecFromRole(ColorRole::SurfaceVariant, 0.35f),
                      .border = colorSpecFromRole(ColorRole::Outline, 0.35f),
                      .label = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  },
              .selected = std::nullopt,
          };
        };
        auto cardNode = ui::button({
            .out = &card,
            .contentAlign = ButtonContentAlign::Start,
            .customPalette = cardPaletteFor(checked),
            .minHeight = Style::controlHeightSm * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceSm * scale,
            .gap = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .flexGrow = 1.0f,
        });
        card->addChild(
            ui::checkbox({
                .out = &checkbox,
                .checked = checked,
                .scale = scale,
                .checkedFill = colorSpecFromRole(ColorRole::Surface),
                .checkedBorder = colorSpecFromRole(ColorRole::OnPrimary),
                .checkedGlyph = colorSpecFromRole(ColorRole::Primary),
                .onChange = [selected, value, commit, checkedState, card, cardPaletteFor](bool nextChecked) mutable {
                  auto it = std::find(selected->begin(), selected->end(), value);
                  if (nextChecked) {
                    if (it == selected->end()) {
                      selected->push_back(value);
                    }
                  } else if (it != selected->end()) {
                    selected->erase(it);
                  }
                  *checkedState = nextChecked;
                  if (card != nullptr) {
                    card->setCustomPalette(cardPaletteFor(nextChecked));
                  }
                  commit();
                },
            })
        );

        auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0f});
        text->addChild(
            ui::label({
                .out = &titleLabel,
                .text = option.label,
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(checked ? ColorRole::OnPrimary : ColorRole::OnSurface),
                .maxLines = 1,
                .fontWeight = FontWeight::Medium,
            })
        );
        if (!option.description.empty()) {
          text->addChild(
              ui::label({
                  .out = &categoryLabel,
                  .text = option.description,
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(
                      checked ? ColorRole::OnPrimary : ColorRole::OnSurfaceVariant, checked ? 0.75f : 1.0f
                  ),
                  .maxLines = 1,
                  .configure = [](Label& label) { label.setCaptionStyle(); },
              })
          );
        }
        card->addChild(std::move(text));
        const auto syncNormalText = [titleLabel, categoryLabel](bool active) {
          if (titleLabel != nullptr) {
            titleLabel->setColor(colorSpecFromRole(active ? ColorRole::OnPrimary : ColorRole::OnSurface));
          }
          if (categoryLabel != nullptr) {
            categoryLabel->setColor(
                colorSpecFromRole(active ? ColorRole::OnPrimary : ColorRole::OnSurfaceVariant, active ? 0.75f : 1.0f)
            );
          }
        };
        const auto syncHoverText = [titleLabel, categoryLabel](bool active) {
          if (titleLabel != nullptr) {
            titleLabel->setColor(colorSpecFromRole(active ? ColorRole::OnPrimary : ColorRole::OnHover));
          }
          if (categoryLabel != nullptr) {
            categoryLabel->setColor(colorSpecFromRole(active ? ColorRole::OnPrimary : ColorRole::OnHover, 0.75f));
          }
        };
        const auto syncPressedText = [titleLabel, categoryLabel]() {
          if (titleLabel != nullptr) {
            titleLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary));
          }
          if (categoryLabel != nullptr) {
            categoryLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary, 0.75f));
          }
        };
        auto setTileActive = [selected, value, commit, checkedState, card, checkbox, cardPaletteFor,
                              syncNormalText](bool nextChecked) mutable {
          auto it = std::find(selected->begin(), selected->end(), value);
          if (nextChecked) {
            if (it == selected->end()) {
              selected->push_back(value);
            }
          } else if (it != selected->end()) {
            selected->erase(it);
          }
          *checkedState = nextChecked;
          if (card != nullptr) {
            card->setCustomPalette(cardPaletteFor(nextChecked));
          }
          if (checkbox != nullptr) {
            checkbox->setChecked(nextChecked);
          }
          syncNormalText(nextChecked);
          commit();
        };
        syncNormalText(*checkedState);
        card->setOnEnter([checkedState, syncHoverText]() { syncHoverText(*checkedState); });
        card->setOnLeave([checkedState, syncNormalText]() { syncNormalText(*checkedState); });
        card->setOnPress([syncPressedText](float /*localX*/, float /*localY*/, bool pressed) {
          if (!pressed) {
            return;
          }
          syncPressedText();
        });
        card->setOnClick([checkedState, setTileActive]() mutable { setTileActive(!*checkedState); });

        row->addChild(std::move(cardNode));
        ++countInRow;
      }
      flushRow();

      block->addChild(std::move(grid));
      section.addChild(std::move(block));
    };

    const auto makeListBlock = [&](Flex& section, const SettingEntry& entry, const ListSetting& list) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden);

      auto listEditor = std::make_unique<ListEditor>();
      listEditor->setScale(scale);
      listEditor->setAddPlaceholder(i18n::tr("settings.controls.list.add-entry-placeholder"));
      std::vector<ListEditorOption> suggestedOptions;
      suggestedOptions.reserve(list.suggestedOptions.size());
      for (const auto& opt : list.suggestedOptions) {
        suggestedOptions.push_back(ListEditorOption{.value = opt.value, .label = opt.label});
      }
      listEditor->setSuggestedOptions(std::move(suggestedOptions));
      listEditor->setItems(list.items);
      listEditor->setOnAddRequested([setOverride = ctx.setOverride, items = list.items,
                                     path = entry.path](std::string value) mutable {
        if (value.empty()) {
          return;
        }
        items.push_back(std::move(value));
        setOverride(path, items);
      });
      listEditor->setOnRemoveRequested([setOverride = ctx.setOverride, setOverrides = ctx.setOverrides,
                                        config = std::cref(cfg), items = list.items,
                                        path = entry.path](std::size_t index) mutable {
        if (index >= items.size()) {
          return;
        }
        const std::string removedItem = items[index];
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
        const auto overrides = capsuleGroupRemovalOverrides(config.get(), path, removedItem, items);
        if (overrides.size() == 1) {
          setOverride(path, items);
          return;
        }
        setOverrides(overrides);
      });
      listEditor->setOnMoveRequested([setOverride = ctx.setOverride, items = list.items,
                                      path = entry.path](std::size_t from, std::size_t to) mutable {
        if (from >= items.size() || to >= items.size() || from == to) {
          return;
        }
        std::swap(items[from], items[to]);
        setOverride(path, items);
      });
      block->addChild(std::move(listEditor));

      section.addChild(std::move(block));
    };

    const auto makeKeybindListBlock = [&](Flex& section, const SettingEntry& entry,
                                          const KeybindListSetting& keybinds) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden, true, true, true, true);

      auto list = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});

      // An empty list clears the override so defaults take effect again; never persist as "disabled".
      // If no GUI override exists, request a rebuild so the UI snaps back to the underlying default.
      const auto commitItems = [configService = ctx.configService, setOverride = ctx.setOverride,
                                clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild,
                                path = entry.path](std::vector<KeyChord> items) {
        if (items.empty()) {
          if (configService != nullptr && configService->hasOverride(path)) {
            if (clearOverride) {
              clearOverride(path);
            }
          } else if (requestRebuild) {
            requestRebuild();
          }
          return;
        }
        setOverride(path, items);
      };

      for (std::size_t i = 0; i < keybinds.items.size(); ++i) {
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

        auto recorder = ui::keybindRecorder({
            .chord = keybinds.items[i],
            .scale = scale,
            .unsetPlaceholder = i18n::tr("settings.controls.keybind.unset-placeholder"),
            .recordingPlaceholder = i18n::tr("settings.controls.keybind.recording-placeholder"),
            .onCommit = [commitItems, items = keybinds.items, i](KeyChord chord) mutable {
              if (i < items.size()) {
                items[i] = chord;
                commitItems(std::move(items));
              }
            },
        });
        row->addChild(std::move(recorder));

        auto removeBtn = ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [commitItems, items = keybinds.items, i]() mutable {
              if (i >= items.size()) {
                return;
              }
              items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
              commitItems(std::move(items));
            },
        });
        row->addChild(std::move(removeBtn));

        list->addChild(std::move(row));
      }

      const bool canAdd = (keybinds.maxItems == 0 || keybinds.items.size() < keybinds.maxItems);
      if (canAdd) {
        // Trailing recorder is UI-only; it only joins the persisted list once a chord is recorded.
        auto addRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

        auto addRecorder = ui::keybindRecorder({
            .scale = scale,
            .unsetPlaceholder = i18n::tr("settings.controls.keybind.add"),
            .recordingPlaceholder = i18n::tr("settings.controls.keybind.recording-placeholder"),
            .onCommit = [commitItems, items = keybinds.items](KeyChord chord) mutable {
              items.push_back(chord);
              commitItems(std::move(items));
            },
        });
        addRow->addChild(std::move(addRecorder));

        list->addChild(std::move(addRow));
      }

      block->addChild(std::move(list));

      section.addChild(std::move(block));
    };

    const auto makeShortcutListBlock = [&](Flex& section, const SettingEntry& entry,
                                           const ShortcutListSetting& shortcuts) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden);

      std::vector<std::string> itemTypes;
      itemTypes.reserve(shortcuts.items.size());
      for (const auto& item : shortcuts.items) {
        itemTypes.push_back(item.type);
      }

      std::vector<ListEditorOption> suggestedOptions;
      suggestedOptions.reserve(shortcuts.suggestedOptions.size());
      for (const auto& opt : shortcuts.suggestedOptions) {
        suggestedOptions.push_back(ListEditorOption{.value = opt.value, .label = opt.label});
      }

      auto listEditor = std::make_unique<ListEditor>();
      listEditor->setScale(scale);
      listEditor->setMaxItems(shortcuts.maxItems);
      listEditor->setAddPlaceholder(i18n::tr("settings.controls.list.add-entry-placeholder"));
      listEditor->setSuggestedOptions(std::move(suggestedOptions));
      listEditor->setItems(std::move(itemTypes));
      listEditor->setOnAddRequested([setOverride = ctx.setOverride, items = shortcuts.items,
                                     path = entry.path](std::string value) mutable {
        if (value.empty() || std::any_of(items.begin(), items.end(), [&value](const ShortcutConfig& item) {
              return item.type == value;
            })) {
          return;
        }
        items.push_back(ShortcutConfig{std::move(value)});
        setOverride(path, items);
      });
      listEditor->setOnRemoveRequested([setOverride = ctx.setOverride, items = shortcuts.items,
                                        path = entry.path](std::size_t index) mutable {
        if (index >= items.size()) {
          return;
        }
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
        setOverride(path, items);
      });
      listEditor->setOnMoveRequested([setOverride = ctx.setOverride, items = shortcuts.items,
                                      path = entry.path](std::size_t from, std::size_t to) mutable {
        if (from >= items.size() || to >= items.size() || from == to) {
          return;
        }
        std::swap(items[from], items[to]);
        setOverride(path, items);
      });
      block->addChild(std::move(listEditor));

      section.addChild(std::move(block));
    };

    const auto makeSessionActionsInlineBlock = [&](Flex& section, const SettingEntry& entry,
                                                   const SessionPanelActionsSetting& sa) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden);

      const std::vector<SelectOption> kindOptions = {
          {"lock", i18n::tr("settings.session-actions.kind.lock"), {}},
          {"logout", i18n::tr("settings.session-actions.kind.logout"), {}},
          {"suspend", i18n::tr("settings.session-actions.kind.suspend"), {}},
          {"reboot", i18n::tr("settings.session-actions.kind.reboot"), {}},
          {"shutdown", i18n::tr("settings.session-actions.kind.shutdown"), {}},
          {"command", i18n::tr("settings.session-actions.kind.command"), {}},
      };

      auto state = std::make_shared<std::vector<SessionPanelActionConfig>>(sa.items);
      const auto commit = [setOverride = ctx.setOverride, path = entry.path, state, req = ctx.requestContentRebuild]() {
        setOverride(path, *state);
        req();
      };

      const float iconBtnH = Style::controlHeight * scale;

      for (std::size_t idx = 0; idx < state->size(); ++idx) {
        auto row = ui::row({
            .align = FlexAlign::Center,
            .justify = FlexJustify::SpaceBetween,
            .gap = Style::spaceSm * scale,
            .minHeight = Style::controlHeightSm * scale,
        });

        auto summary = ui::label({
            .text = sessionActionRowSummary(kindOptions, (*state)[idx]),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        });
        row->addChild(std::move(summary));

        auto reorder = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

        auto upBtn = ui::button({
            .glyph = "chevron-up",
            .glyphSize = Style::fontSizeBody * scale,
            .enabled = idx > 0,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = iconBtnH,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [state, rowIndex = idx, commit]() {
              if (rowIndex == 0 || rowIndex >= state->size()) {
                return;
              }
              std::swap((*state)[rowIndex - 1], (*state)[rowIndex]);
              commit();
            },
        });
        reorder->addChild(std::move(upBtn));

        auto downBtn = ui::button({
            .glyph = "chevron-down",
            .glyphSize = Style::fontSizeBody * scale,
            .enabled = idx + 1 < state->size(),
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = iconBtnH,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [state, rowIndex = idx, commit]() {
              if (rowIndex + 1 >= state->size()) {
                return;
              }
              std::swap((*state)[rowIndex + 1], (*state)[rowIndex]);
              commit();
            },
        });
        reorder->addChild(std::move(downBtn));
        row->addChild(std::move(reorder));

        auto entrySettings = ui::button({
            .glyph = "settings",
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [openEntry = ctx.openSessionActionEntryEditor, rowIndex = idx]() {
              if (openEntry) {
                openEntry(rowIndex);
              }
            },
        });
        row->addChild(std::move(entrySettings));

        auto enabledToggle = ui::toggle({
            .checked = (*state)[idx].enabled,
            .scale = scale,
            .onChange = [state, rowIndex = idx, commit](bool v) {
              (*state)[rowIndex].enabled = v;
              commit();
            },
        });
        row->addChild(std::move(enabledToggle));

        block->addChild(std::move(row));
      }

      auto addBtn = ui::button({
          .text = i18n::tr("settings.session-actions.add"),
          .glyph = "add",
          .fontSize = Style::fontSizeBody * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [state, commit]() {
            state->push_back(
                SessionPanelActionConfig{
                    "command", true, "notify-send 'Noctalia' 'Custom session entry'", std::nullopt, std::nullopt,
                    SessionActionButtonVariant::Default, std::nullopt
                }
            );
            commit();
          },
      });
      block->addChild(std::move(addBtn));

      section.addChild(std::move(block));
    };

    const auto makeIdleBehaviorsInlineBlock = [&](Flex& section, const SettingEntry& entry,
                                                  const IdleBehaviorsSetting& idle) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden);

      auto state = std::make_shared<std::vector<IdleBehaviorConfig>>(idle.items);
      normalizeIdleBehaviorNames(*state);
      const auto commit = [setOverride = ctx.setOverride, path = entry.path, state, req = ctx.requestContentRebuild]() {
        normalizeIdleBehaviorNames(*state);
        setOverride(path, *state);
        req();
      };

      const float iconBtnH = Style::controlHeight * scale;
      for (std::size_t idx = 0; idx < state->size(); ++idx) {
        auto row = ui::row({
            .align = FlexAlign::Center,
            .justify = FlexJustify::SpaceBetween,
            .gap = Style::spaceSm * scale,
            .minHeight = Style::controlHeightSm * scale,
        });

        auto summary = ui::label({
            .text = idleBehaviorRowSummary((*state)[idx]),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        });
        row->addChild(std::move(summary));

        auto reorder = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

        auto upBtn = ui::button({
            .glyph = "chevron-up",
            .glyphSize = Style::fontSizeBody * scale,
            .enabled = idx > 0,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = iconBtnH,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [state, rowIndex = idx, commit]() {
              if (rowIndex == 0 || rowIndex >= state->size()) {
                return;
              }
              std::swap((*state)[rowIndex - 1], (*state)[rowIndex]);
              commit();
            },
        });
        reorder->addChild(std::move(upBtn));

        auto downBtn = ui::button({
            .glyph = "chevron-down",
            .glyphSize = Style::fontSizeBody * scale,
            .enabled = idx + 1 < state->size(),
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = iconBtnH,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [state, rowIndex = idx, commit]() {
              if (rowIndex + 1 >= state->size()) {
                return;
              }
              std::swap((*state)[rowIndex + 1], (*state)[rowIndex]);
              commit();
            },
        });
        reorder->addChild(std::move(downBtn));
        row->addChild(std::move(reorder));

        auto entrySettings = ui::button({
            .glyph = "settings",
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [openEntry = ctx.openIdleBehaviorEntryEditor, rowIndex = idx]() {
              if (openEntry) {
                openEntry(rowIndex);
              }
            },
        });
        row->addChild(std::move(entrySettings));

        auto enabledToggle = ui::toggle({
            .checked = (*state)[idx].enabled,
            .scale = scale,
            .onChange = [state, rowIndex = idx, commit](bool v) {
              (*state)[rowIndex].enabled = v;
              commit();
            },
        });
        row->addChild(std::move(enabledToggle));

        block->addChild(std::move(row));
      }

      auto addBtn = ui::button({
          .text = i18n::tr("settings.idle.behavior.add"),
          .glyph = "add",
          .fontSize = Style::fontSizeBody * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [openCreate = ctx.openIdleBehaviorCreateEditor]() {
            if (openCreate) {
              openCreate();
            }
          },
      });
      block->addChild(std::move(addBtn));

      section.addChild(std::move(block));
    };

    const auto makeControl = [&](const SettingEntry& entry) -> std::unique_ptr<Node> {
      return std::visit(
          [&](const auto& control) -> std::unique_ptr<Node> {
            using T = std::decay_t<decltype(control)>;
            if constexpr (std::is_same_v<T, ToggleSetting>) {
              return makeToggle(control.checked, control.enabled, entry.path);
            } else if constexpr (std::is_same_v<T, SelectSetting>) {
              return makeSelect(control, entry.path);
            } else if constexpr (std::is_same_v<T, SliderSetting>) {
              return makeSlider(
                  control.value, control.minValue, control.maxValue, control.step, entry.path, control.integerValue,
                  control.linkedCommit
              );
            } else if constexpr (std::is_same_v<T, TextSetting>) {
              if (isDockLauncherIconPath(entry.path)) {
                return makeGlyphText(control, entry.path);
              }
              if (control.browseMode != TextSettingBrowseMode::None) {
                return makeTextWithPathBrowse(control, entry.path);
              }
              return makeText(control.value, control.placeholder, entry.path, control.width);
            } else if constexpr (std::is_same_v<T, OptionalNumberSetting>) {
              return makeOptionalNumber(control, entry.path);
            } else if constexpr (std::is_same_v<T, OptionalStepperSetting>) {
              return makeOptionalStepper(control, entry.path);
            } else if constexpr (std::is_same_v<T, StepperSetting>) {
              return makeStepper(control, entry.path);
            } else if constexpr (std::is_same_v<T, SearchPickerSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, MultiSelectSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, TemplateGridSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, ListSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, ShortcutListSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, KeybindListSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, SessionPanelActionsSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, IdleBehaviorsSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, ButtonSetting>) {
              if (control.glyph.empty()) {
                return ui::button({
                    .text = control.label,
                    .fontSize = Style::fontSizeBody * scale,
                    .variant = ButtonVariant::Outline,
                    .minHeight = Style::controlHeight * scale,
                    .paddingV = Style::spaceSm * scale,
                    .paddingH = Style::spaceMd * scale,
                    .radius = Style::scaledRadiusMd(scale),
                    .onClick = control.action,
                });
              }
              return ui::button({
                  .text = control.label,
                  .glyph = control.glyph,
                  .fontSize = Style::fontSizeBody * scale,
                  .glyphSize = Style::fontSizeBody * scale,
                  .variant = ButtonVariant::Outline,
                  .minHeight = Style::controlHeight * scale,
                  .paddingV = Style::spaceSm * scale,
                  .paddingH = Style::spaceMd * scale,
                  .radius = Style::scaledRadiusMd(scale),
                  .onClick = control.action,
              });
            } else if constexpr (std::is_same_v<T, ColorSpecPickerSetting>) {
              return makeColorSpecPicker(control, entry.path);
            }
          },
          entry.control
      );
    };

    std::string activeSectionKey;
    std::string activeGroupKey;
    Flex* activeSection = nullptr;
    // Row-major grid state for keybind entries (see KeybindListSetting dispatch below).
    constexpr std::size_t kKeybindsPerRow = 3;
    Flex* activeKeybindRow = nullptr;
    std::size_t activeKeybindRowCount = 0;
    std::size_t visibleEntries = 0;
    const std::string normalizedSearchQuery = normalizedSettingQuery(ctx.searchQuery);

    BarWidgetEditorContext barWidgetEditorCtx{
        .config = cfg,
        .configService = ctx.configService,
        .scale = scale,
        .showAdvanced = ctx.showAdvanced,
        .showOverriddenOnly = ctx.showOverriddenOnly,
        .batteryDeviceOptions = ctx.batteryDeviceOptions,
        .editingWidgetName = ctx.editingWidgetName,
        .pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
        .pendingDeleteWidgetSettingPath = ctx.pendingDeleteWidgetSettingPath,
        .renamingWidgetName = ctx.renamingWidgetName,
        .requestRebuild = ctx.requestRebuild,
        .resetContentScroll = ctx.resetContentScroll,
        .setScrollTarget = ctx.setScrollTarget,
        .focusArea = ctx.focusArea,
        .openWidgetAddPopup = ctx.openBarWidgetAddPopup,
        .setOverride = ctx.setOverride,
        .setOverrides = ctx.setOverrides,
        .clearOverride = ctx.clearOverride,
        .renameWidgetInstance = ctx.renameWidgetInstance,
        .makeResetButton = makeResetButton,
        .makeRow = makeRow,
        .makeToggle = [&](bool checked, const std::vector<std::string>& path, std::optional<bool> clearWhenValue)
            -> std::unique_ptr<Node> { return makeToggle(checked, true, path, clearWhenValue); },
        .makeSelect = [&](const SelectSetting& setting, const std::vector<std::string>& path) -> std::unique_ptr<Node> {
          return makeSelect(setting, path);
        },
        .makeSlider = [&](double value, double minValue, double maxValue, double step,
                          const std::vector<std::string>& path, bool integerValue) -> std::unique_ptr<Node> {
          return makeSlider(value, minValue, maxValue, step, path, integerValue);
        },
        .makeOptionalNumber = [&](const OptionalNumberSetting& setting, const std::vector<std::string>& path)
            -> std::unique_ptr<Node> { return makeOptionalNumber(setting, path); },
        .makeOptionalStepper = [&](const OptionalStepperSetting& setting, const std::vector<std::string>& path)
            -> std::unique_ptr<Node> { return makeOptionalStepper(setting, path); },
        .makeText = [&](const std::string& value, const std::string& placeholder, const std::vector<std::string>& path)
            -> std::unique_ptr<Node> { return makeText(value, placeholder, path); }, // width not used in search
        .makeColorSpecPicker = [&](const ColorSpecPickerSetting& setting, const std::vector<std::string>& path)
            -> std::unique_ptr<Node> { return makeColorSpecPicker(setting, path); },
        .makeListBlock = [&](Flex& section, const SettingEntry& entry,
                             const ListSetting& list) { makeListBlock(section, entry, list); },
    };

    auto visibilityConditionMatches = [&](const SettingVisibilityCondition& cond) -> bool {
      for (const auto& other : registry) {
        if (other.path == cond.path) {
          std::string currentValue;
          if (const auto* toggle = std::get_if<ToggleSetting>(&other.control)) {
            currentValue = toggle->checked ? "true" : "false";
          } else if (const auto* select = std::get_if<SelectSetting>(&other.control)) {
            currentValue = select->selectedValue;
          }
          for (const auto& v : cond.values) {
            if (v == currentValue) {
              return true;
            }
          }
          return false;
        }
      }
      return true;
    };

    auto isEntryVisible = [&](const SettingEntry& e) -> bool {
      if (!e.visibleWhen.has_value()) {
        return true;
      }
      for (const auto& cond : e.visibleWhen->all) {
        if (!visibilityConditionMatches(cond)) {
          return false;
        }
      }
      return true;
    };

    const std::string_view selectedBarName =
        ctx.selectedBar != nullptr ? std::string_view{ctx.selectedBar->name} : std::string_view{};
    const std::string_view selectedMonitorMatch = ctx.selectedMonitorOverride != nullptr
        ? std::string_view{ctx.selectedMonitorOverride->match}
        : std::string_view{};

    for (const auto& entry : registry) {
      if (ctx.searchQuery.empty() && !ctx.selectedSection.empty() && entry.section != ctx.selectedSection) {
        continue;
      }
      if (ctx.searchQuery.empty()
          && ctx.selectedSection == "bar"
          && !settingEntryMatchesBarNavigation(entry, selectedBarName, selectedMonitorMatch)) {
        continue;
      }
      if (!ctx.showAdvanced && entry.advanced) {
        continue;
      }
      if (!isEntryVisible(entry)) {
        continue;
      }
      if (ctx.showOverriddenOnly
          && ctx.configService != nullptr
          && !ctx.configService->hasEffectiveOverride(entry.path)) {
        continue;
      }
      if (!matchesNormalizedSettingQuery(entry, normalizedSearchQuery)) {
        continue;
      }

      const std::string contentSectionKey = barSettingContentSectionKey(entry);
      if (contentSectionKey != activeSectionKey) {
        activeSectionKey = contentSectionKey;
        activeGroupKey.clear();
        activeKeybindRow = nullptr;
        activeKeybindRowCount = 0;
        std::string displayTitle;
        if (entry.section == "bar" && entry.path.size() >= 2) {
          displayTitle = i18n::tr("settings.entities.bar.label", "name", entry.path[1]);
          if (isBarMonitorOverrideSettingPath(entry.path)) {
            displayTitle += " / " + entry.path[3];
          }
        } else {
          displayTitle = sectionLabel(entry.section);
        }
        activeSection = makeSection(displayTitle, entry.section);
        if (entry.section == "idle") {
          addIdleLiveStatusPanel(*activeSection, ctx, scale);
        }
      }
      if (activeSection != nullptr) {
        if (entry.group != activeGroupKey) {
          const bool isFirstGroup = activeGroupKey.empty();
          activeGroupKey = entry.group;
          activeKeybindRow = nullptr;
          activeKeybindRowCount = 0;
          addGroupLabel(*activeSection, groupLabel(entry.group), isFirstGroup);
        }
        const bool isKeybindEntry = std::holds_alternative<KeybindListSetting>(entry.control);
        if (!isKeybindEntry) {
          activeKeybindRow = nullptr;
          activeKeybindRowCount = 0;
        }
        if (const auto* list = std::get_if<ListSetting>(&entry.control)) {
          if (isFirstBarWidgetListPath(entry.path)) {
            addBarWidgetLaneEditor(*activeSection, entry, barWidgetEditorCtx);
          } else if (!isBarWidgetListPath(entry.path)) {
            makeListBlock(*activeSection, entry, *list);
          }
        } else if (const auto* shortcuts = std::get_if<ShortcutListSetting>(&entry.control)) {
          makeShortcutListBlock(*activeSection, entry, *shortcuts);
        } else if (const auto* keybindList = std::get_if<KeybindListSetting>(&entry.control)) {
          if (activeKeybindRow == nullptr || activeKeybindRowCount >= kKeybindsPerRow) {
            auto row = ui::row({
                .align = FlexAlign::Start,
                .gap = Style::spaceMd * scale,
                .fillWidth = true,
            });
            activeKeybindRow = dynamic_cast<Flex*>(activeSection->addChild(std::move(row)));
            activeKeybindRowCount = 0;
          }
          makeKeybindListBlock(*activeKeybindRow, entry, *keybindList);
          ++activeKeybindRowCount;
        } else if (const auto* sessionActs = std::get_if<SessionPanelActionsSetting>(&entry.control)) {
          makeSessionActionsInlineBlock(*activeSection, entry, *sessionActs);
        } else if (const auto* idle = std::get_if<IdleBehaviorsSetting>(&entry.control)) {
          makeIdleBehaviorsInlineBlock(*activeSection, entry, *idle);
        } else if (const auto* picker = std::get_if<SearchPickerSetting>(&entry.control)) {
          makeRow(*activeSection, entry, makeSearchPickerButton(entry, *picker));
        } else if (const auto* multi = std::get_if<MultiSelectSetting>(&entry.control)) {
          makeMultiSelectBlock(*activeSection, entry, *multi);
        } else if (const auto* templates = std::get_if<TemplateGridSetting>(&entry.control)) {
          makeTemplateGridBlock(*activeSection, entry, *templates);
        } else {
          makeRow(*activeSection, entry, makeControl(entry));
        }
        ++visibleEntries;
      }
    }

    if (visibleEntries == 0) {
      auto emptyState = ui::column(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .gap = Style::spaceSm * scale,
           .padding = (Style::spaceLg * 2.0f) * scale,
           .minWidth = 360.0f * scale,
           .minHeight = 160.0f * scale,
           .fillWidth = true,
           .flexGrow = 2.0f,
           .configure =
               [scale](Flex& flex) {
                 flex.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.24f));
                 flex.setBorder(colorSpecFromRole(ColorRole::Outline, 0.28f), Style::borderWidth);
                 flex.setRadius(Style::scaledRadiusMd(scale));
               }},
          makeLabel(
              i18n::tr("settings.window.no-results"), Style::fontSizeHeader * scale,
              colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
          ),
          makeLabel(
              i18n::tr("settings.window.no-results-hint"), Style::fontSizeBody * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );

      content.addChild(
          ui::row(
              {.align = FlexAlign::Center, .fillWidth = true}, ui::box({.flexGrow = 0.5f}), std::move(emptyState),
              ui::box({.flexGrow = 0.5f})
          )
      );
    }

    return visibleEntries;
  }

} // namespace settings
