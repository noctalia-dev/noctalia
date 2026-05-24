#include "shell/settings/settings_content.h"

#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "shell/settings/bar_widget_editor.h"
#include "shell/settings/color_spec_picker.h"
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

    std::unique_ptr<Label> makeLabel(
        std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal
    ) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .color = color,
          .fontWeight = fontWeight,
      });
    }

    std::optional<std::size_t> optionIndex(const std::vector<SelectOption>& options, std::string_view value) {
      for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i].value == value) {
          return i;
        }
      }
      return std::nullopt;
    }

    std::string optionLabel(const std::vector<SelectOption>& options, std::string_view value) {
      for (const auto& opt : options) {
        if (opt.value == value) {
          return opt.label;
        }
      }
      return std::string(value);
    }

    std::vector<std::string> optionLabels(const std::vector<SelectOption>& options) {
      std::vector<std::string> labels;
      labels.reserve(options.size());
      for (const auto& opt : options) {
        labels.push_back(opt.label);
      }
      return labels;
    }

    std::vector<ColorSwatchPreview> optionSwatchPreviews(const std::vector<SelectOption>& options) {
      std::vector<ColorSwatchPreview> previews;
      previews.reserve(options.size());
      for (const auto& opt : options) {
        previews.push_back(opt.preview);
      }
      return previews;
    }

    std::vector<SelectOption> sessionActionVariantOptions() {
      std::vector<SelectOption> options;
      for (const auto& variant : kSessionActionButtonVariants) {
        options.push_back(SelectOption{std::string(variant.key), i18n::tr(variant.labelKey), {}});
      }
      return options;
    }

    const char* sessionActionDefaultGlyphName(std::string_view action) {
      if (action == "lock") {
        return "lock";
      }
      if (action == "logout") {
        return "logout";
      }
      if (action == "suspend") {
        return "suspend";
      }
      if (action == "reboot") {
        return "reboot";
      }
      if (action == "shutdown") {
        return "shutdown";
      }
      return "terminal";
    }

    bool isBlankInput(std::string_view text) { return StringUtils::trim(text).empty(); }

    std::string formatSliderValue(float value, bool integerValue) {
      if (integerValue) {
        return std::format("{}", static_cast<int>(std::lround(value)));
      }
      return StringUtils::formatFixedDotDecimal(value, 2);
    }

    template <typename T> std::optional<T> parseDotDecimalInput(std::string_view text) {
      return StringUtils::parseDotDecimal<T>(text);
    }

    std::optional<float> parseFloatInput(std::string_view text) {
      const auto parsed = parseDotDecimalInput<double>(text);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      return static_cast<float>(*parsed);
    }

    std::optional<double> parseDoubleInput(std::string_view text) { return parseDotDecimalInput<double>(text); }

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

    std::string
    sessionActionRowSummary(const std::vector<SelectOption>& kindOptions, const SessionPanelActionConfig& row) {
      if (row.label.has_value() && !row.label->empty()) {
        return *row.label;
      }
      return optionLabel(kindOptions, row.action);
    }

    std::string sanitizedIdleBehaviorName(std::string_view text) {
      std::string out = StringUtils::trim(text);
      for (char& ch : out) {
        if (ch == '.' || ch == '[' || ch == ']') {
          ch = '-';
        }
      }
      return out;
    }

    std::string uniqueIdleBehaviorName(
        std::string base, const std::vector<IdleBehaviorConfig>& rows,
        std::optional<std::size_t> ignoreIndex = std::nullopt
    ) {
      base = sanitizedIdleBehaviorName(base);
      if (base.empty()) {
        base = "idle-behavior";
      }

      std::unordered_set<std::string> names;
      for (std::size_t i = 0; i < rows.size(); ++i) {
        if (ignoreIndex.has_value() && i == *ignoreIndex) {
          continue;
        }
        if (!rows[i].name.empty()) {
          names.insert(rows[i].name);
        }
      }

      if (!names.contains(base)) {
        return base;
      }
      for (int suffix = 2; suffix < 10000; ++suffix) {
        std::string candidate = std::format("{}-{}", base, suffix);
        if (!names.contains(candidate)) {
          return candidate;
        }
      }
      return base;
    }

    void normalizeIdleBehaviorNames(std::vector<IdleBehaviorConfig>& rows) {
      std::vector<IdleBehaviorConfig> normalized;
      normalized.reserve(rows.size());
      for (auto& row : rows) {
        row.name = uniqueIdleBehaviorName(row.name, normalized);
        normalized.push_back(row);
      }
      rows = std::move(normalized);
    }

    std::string idleBehaviorRowSummary(const IdleBehaviorConfig& row) {
      IdleBehaviorConfig norm = row;
      inferIdleBehaviorActionFromLegacyFields(norm);

      const auto displayName = [&]() -> std::string {
        if (norm.action == "lock") {
          return i18n::tr("settings.idle.behavior.presets.lock");
        }
        if (norm.action == "screen_off") {
          return i18n::tr("settings.idle.behavior.presets.monitor-off");
        }
        if (norm.action == "suspend") {
          return i18n::tr("settings.idle.behavior.presets.suspend");
        }
        if (row.name.empty()) {
          return i18n::tr("settings.idle.behavior.unnamed");
        }
        return row.name;
      };

      const std::string name = displayName();
      if (name.empty()) {
        return i18n::tr("settings.idle.behavior.unnamed");
      }
      if (row.timeoutSeconds <= 0) {
        return i18n::tr("settings.idle.behavior.summary-disabled-timeout", "name", name);
      }
      return i18n::tr("settings.idle.behavior.summary", "name", name, "seconds", std::to_string(row.timeoutSeconds));
    }

    void buildSessionActionEntryDetailContentImpl(
        Flex& section, SettingsContentContext& ctx, SessionPanelActionConfig& row, const std::function<void()>& persist
    ) {
      const float scale = ctx.scale;
      const std::vector<SelectOption> kindOptions = {
          {"lock", i18n::tr("settings.session-actions.kind.lock"), {}},
          {"logout", i18n::tr("settings.session-actions.kind.logout"), {}},
          {"suspend", i18n::tr("settings.session-actions.kind.suspend"), {}},
          {"reboot", i18n::tr("settings.session-actions.kind.reboot"), {}},
          {"shutdown", i18n::tr("settings.session-actions.kind.shutdown"), {}},
          {"command", i18n::tr("settings.session-actions.kind.command"), {}},
      };

      const float iconSq = Style::controlHeight * scale;
      const float iconGlyphSize = Style::fontSizeBody * scale;

      auto body = ui::row({
          .align = FlexAlign::Start,
          .gap = Style::spaceMd * scale,
          .fillWidth = true,
      });

      auto iconCol = ui::column(
          {
              .align = FlexAlign::Stretch,
              .gap = Style::spaceSm * scale,
          },
          makeLabel(
              i18n::tr("settings.session-actions.icon-label"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );

      auto glyphBtnRow = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceXs * scale,
      });

      const std::string previewGlyph = [&] {
        if (row.glyph.has_value() && !row.glyph->empty()) {
          return *row.glyph;
        }
        return std::string(sessionActionDefaultGlyphName(row.action));
      }();
      const auto previewGlyphForRow = [&row]() {
        if (row.glyph.has_value() && !row.glyph->empty()) {
          return *row.glyph;
        }
        return std::string(sessionActionDefaultGlyphName(row.action));
      };
      const auto hasCustomGlyph = [&row]() { return row.glyph.has_value() && !row.glyph->empty(); };

      Button* glyphPickBtnPtr = nullptr;
      auto glyphPickBtn = ui::button({
          .out = &glyphPickBtnPtr,
          .text = "",
          .glyph = previewGlyph,
          .glyphSize = iconGlyphSize,
          .variant = ButtonVariant::Outline,
          .minWidth = iconSq,
          .minHeight = iconSq,
          .maxWidth = iconSq,
          .maxHeight = iconSq,
          .padding = 0.0f,
          .radius = Style::scaledRadiusMd(scale),
      });

      Button* clearGlyphBtnPtr = nullptr;
      auto clearG = ui::button({
          .out = &clearGlyphBtnPtr,
          .text = i18n::tr("settings.session-actions.clear-glyph"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minHeight = iconSq,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusSm(scale),
          .visible = hasCustomGlyph(),
          .participatesInLayout = hasCustomGlyph(),
      });

      glyphPickBtn->setOnClick([&row, persist, glyphPickBtnPtr, clearGlyphBtnPtr]() {
        GlyphPickerDialogOptions options;
        options.title = i18n::tr("settings.session-actions.glyph-picker-title");
        if (row.glyph.has_value() && !row.glyph->empty()) {
          options.initialGlyph = *row.glyph;
        }
        (void)GlyphPickerDialog::open(
            std::move(options),
            [&row, persist, glyphPickBtnPtr, clearGlyphBtnPtr](std::optional<GlyphPickerResult> result) {
              if (!result.has_value()) {
                return;
              }
              row.glyph = result->name;
              glyphPickBtnPtr->setGlyph(result->name);
              clearGlyphBtnPtr->setVisible(true);
              clearGlyphBtnPtr->setParticipatesInLayout(true);
              persist();
            }
        );
      });
      glyphBtnRow->addChild(std::move(glyphPickBtn));

      clearG->setOnClick([&row, persist, glyphPickBtnPtr, clearGlyphBtnPtr, previewGlyphForRow]() {
        row.glyph = std::nullopt;
        glyphPickBtnPtr->setGlyph(previewGlyphForRow());
        clearGlyphBtnPtr->setVisible(false);
        clearGlyphBtnPtr->setParticipatesInLayout(false);
        persist();
      });
      glyphBtnRow->addChild(std::move(clearG));

      iconCol->addChild(std::move(glyphBtnRow));
      body->addChild(std::move(iconCol));

      auto fields = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * scale,
          .flexGrow = 1.0f,
      });

      fields->addChild(makeLabel(
          i18n::tr("settings.session-actions.kind-section-label"), Style::fontSizeCaption * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
      ));
      const auto selectedKindIndex = optionIndex(kindOptions, row.action);
      auto kindSelect = ui::select({
          .options = optionLabels(kindOptions),
          .selectedIndex = selectedKindIndex,
          .clearSelection = !selectedKindIndex.has_value(),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .onSelectionChanged =
              [&row, kindOptions, persist, glyphPickBtnPtr, previewGlyphForRow,
               hasCustomGlyph](std::size_t index, std::string_view /*label*/) {
                if (index < kindOptions.size()) {
                  row.action = kindOptions[index].value;
                  if (!hasCustomGlyph()) {
                    glyphPickBtnPtr->setGlyph(previewGlyphForRow());
                  }
                  persist();
                }
              },
          .configure = [](Select& select) { select.setFillWidth(true); },
      });
      fields->addChild(std::move(kindSelect));

      auto labelBlock = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
          makeLabel(
              i18n::tr("settings.session-actions.label-field"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );
      Input* labelPtr = nullptr;
      auto labelIn = ui::input({
          .out = &labelPtr,
          .value = row.label.value_or(""),
          .placeholder = i18n::tr("settings.session-actions.label-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .minLayoutWidth = 200.0f * scale,
      });
      const auto commitLabel = [&row, persist, labelPtr]() {
        const std::string t = StringUtils::trim(labelPtr->value());
        if (t.empty()) {
          row.label = std::nullopt;
        } else {
          row.label = t;
        }
        labelPtr->setInvalid(false);
        persist();
      };
      labelIn->setOnChange([labelPtr](const std::string& /*t*/) { labelPtr->setInvalid(false); });
      labelIn->setOnSubmit([commitLabel](const std::string& /*text*/) { commitLabel(); });
      labelIn->setOnFocusLoss(commitLabel);
      labelBlock->addChild(std::move(labelIn));
      fields->addChild(std::move(labelBlock));

      auto cmdBlock = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
          makeLabel(
              i18n::tr("settings.session-actions.command-label"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );
      Input* cmdPtr = nullptr;
      auto cmdIn = ui::input({
          .out = &cmdPtr,
          .value = row.command.value_or(""),
          .placeholder = i18n::tr("settings.session-actions.command-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .minLayoutWidth = 280.0f * scale,
      });
      const auto commitCommand = [&row, persist, cmdPtr]() {
        const std::string t = StringUtils::trim(cmdPtr->value());
        if (t.empty()) {
          row.command = std::nullopt;
        } else {
          row.command = t;
        }
        cmdPtr->setInvalid(false);
        persist();
      };
      cmdIn->setOnChange([cmdPtr](const std::string& /*t*/) { cmdPtr->setInvalid(false); });
      cmdIn->setOnSubmit([commitCommand](const std::string& /*text*/) { commitCommand(); });
      cmdIn->setOnFocusLoss(commitCommand);
      cmdBlock->addChild(std::move(cmdIn));
      fields->addChild(std::move(cmdBlock));

      const std::vector<SelectOption> variantOptions = sessionActionVariantOptions();
      const std::string selectedVariant(enumToKey(kSessionActionButtonVariants, row.variant));
      const auto selectedVariantIndex = optionIndex(variantOptions, selectedVariant);
      auto variantBlock = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
          makeLabel(
              i18n::tr("settings.session-actions.variant-label"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );
      auto variantSelect = ui::select({
          .options = optionLabels(variantOptions),
          .selectedIndex = selectedVariantIndex,
          .clearSelection = !selectedVariantIndex.has_value(),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .onSelectionChanged =
              [&row, variantOptions, persist](std::size_t index, std::string_view /*label*/) {
                if (index < variantOptions.size()) {
                  if (auto parsed = enumFromKey(kSessionActionButtonVariants, variantOptions[index].value)) {
                    row.variant = *parsed;
                    persist();
                  }
                }
              },
          .configure = [](Select& select) { select.setFillWidth(true); },
      });
      variantBlock->addChild(std::move(variantSelect));
      fields->addChild(std::move(variantBlock));

      auto shortcutBlock = ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
          makeLabel(
              i18n::tr("settings.session-actions.shortcut-label"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );

      KeybindRecorder* shortcutRecorderPtr = nullptr;
      auto shortcutRecorder = ui::keybindRecorder({
          .out = &shortcutRecorderPtr,
          .chord = row.shortcut,
          .scale = scale,
          .unsetPlaceholder = i18n::tr("settings.controls.keybind.unset-placeholder"),
          .recordingPlaceholder = i18n::tr("settings.controls.keybind.recording-placeholder"),
          .modifierPolicy = ModifierPolicy::Optional,
          .onCommit = [&row, persist](KeyChord chord) {
            row.shortcut = chord;
            persist();
          },
      });
      shortcutBlock->addChild(std::move(shortcutRecorder));

      if (row.shortcut.has_value()) {
        auto clearBtn = ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [&row, persist, shortcutRecorderPtr]() {
              row.shortcut = std::nullopt;
              shortcutRecorderPtr->setChord(std::nullopt);
              persist();
            },
        });
        shortcutBlock->addChild(std::move(clearBtn));
      }

      fields->addChild(std::move(shortcutBlock));

      body->addChild(std::move(fields));
      section.addChild(std::move(body));
    }

    void buildIdleBehaviorEntryDetailContentImpl(
        Flex& section, SettingsContentContext& ctx, IdleBehaviorConfig& row, const std::function<void()>& persist,
        const std::function<void()>& closeHostedEditor
    ) {
      const float scale = ctx.scale;

      const std::vector<SelectOption> idleActionOptions = {
          {"lock", i18n::tr("settings.idle.behavior.kind.lock"), {}},
          {"screen_off", i18n::tr("settings.idle.behavior.kind.screen-off"), {}},
          {"suspend", i18n::tr("settings.idle.behavior.kind.suspend"), {}},
          {"command", i18n::tr("settings.idle.behavior.kind.custom"), {}},
      };

      IdleBehaviorConfig norm = row;
      inferIdleBehaviorActionFromLegacyFields(norm);
      const bool showCustomCommands = (norm.action == "command");
      const bool showSuspendLock = (norm.action == "suspend");

      auto body = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
      });

      Flex* customCommandsRaw = nullptr;
      auto customCommandsGrp = ui::column({
          .out = &customCommandsRaw,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
          .visible = showCustomCommands,
      });

      Flex* suspendLockRaw = nullptr;
      auto suspendLockGrp = ui::row(
          {.out = &suspendLockRaw,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .fillWidth = true,
           .visible = showSuspendLock},
          ui::label({
              .text = i18n::tr("settings.idle.behavior.lock-before-suspend-label"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .fontWeight = FontWeight::Normal,
              .flexGrow = 1.0f,
          }),
          ui::toggle({
              .checked = row.lockBeforeSuspend,
              .scale = scale,
              .onChange = [&row, persist](bool v) {
                row.lockBeforeSuspend = v;
                persist();
              },
          })
      );

      const auto addCommandInput = [&](Flex& parent, std::string label, std::string placeholder, std::string& target) {
        auto block = ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
            makeLabel(
                label, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
                FontWeight::Normal
            )
        );
        Input* inputPtr = nullptr;
        auto input = ui::input({
            .out = &inputPtr,
            .value = target,
            .placeholder = placeholder,
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .horizontalPadding = Style::spaceSm * scale,
        });
        auto* targetPtr = &target;
        const auto commit = [targetPtr, persist, inputPtr]() {
          *targetPtr = StringUtils::trim(inputPtr->value());
          inputPtr->setInvalid(false);
          inputPtr->setValue(*targetPtr);
          persist();
        };
        input->setOnChange([inputPtr](const std::string& /*t*/) { inputPtr->setInvalid(false); });
        input->setOnSubmit([commit](const std::string& /*text*/) { commit(); });
        input->setOnFocusLoss(commit);
        block->addChild(std::move(input));
        parent.addChild(std::move(block));
      };

      addCommandInput(
          *customCommandsGrp, i18n::tr("settings.idle.behavior.command-label"),
          i18n::tr("settings.idle.behavior.command-placeholder"), row.command
      );

      auto resumeCommandGrp = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceMd * scale,
      });
      addCommandInput(
          *resumeCommandGrp, i18n::tr("settings.idle.behavior.resume-command-label"),
          i18n::tr("settings.idle.behavior.resume-command-placeholder"), row.resumeCommand
      );

      auto kindBlock = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          makeLabel(
              i18n::tr("settings.idle.behavior.kind-section-label"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );
      const auto selectedKindIndex = optionIndex(idleActionOptions, norm.action);
      auto kindSelect = ui::select({
          .options = optionLabels(idleActionOptions),
          .selectedIndex = selectedKindIndex,
          .clearSelection = !selectedKindIndex.has_value(),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .onSelectionChanged =
              [&row, persist, idleActionOptions, customCommandsRaw,
               suspendLockRaw](std::size_t index, std::string_view /*label*/) {
                if (index < idleActionOptions.size()) {
                  row.action = idleActionOptions[index].value;
                  if (row.action != "command") {
                    row.command.clear();
                  }
                }
                IdleBehaviorConfig n = row;
                inferIdleBehaviorActionFromLegacyFields(n);
                customCommandsRaw->setVisible(n.action == "command");
                suspendLockRaw->setVisible(n.action == "suspend");
                persist();
              },
          .configure = [](Select& select) { select.setFillWidth(true); },
      });
      kindBlock->addChild(std::move(kindSelect));
      body->addChild(std::move(kindBlock));
      body->addChild(std::move(suspendLockGrp));

      auto nameBlock = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          makeLabel(
              i18n::tr("settings.idle.behavior.name-label"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );
      Input* namePtr = nullptr;
      auto nameIn = ui::input({
          .out = &namePtr,
          .value = row.name,
          .placeholder = i18n::tr("settings.idle.behavior.name-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
      });
      const auto commitName = [&row, persist, namePtr]() {
        const std::string name = sanitizedIdleBehaviorName(namePtr->value());
        if (name.empty()) {
          namePtr->setInvalid(true);
          return;
        }
        row.name = name;
        namePtr->setInvalid(false);
        namePtr->setValue(row.name);
        persist();
      };
      nameIn->setOnChange([namePtr](const std::string& /*t*/) { namePtr->setInvalid(false); });
      nameIn->setOnSubmit([commitName](const std::string& /*text*/) { commitName(); });
      nameIn->setOnFocusLoss(commitName);
      nameBlock->addChild(std::move(nameIn));
      body->addChild(std::move(nameBlock));

      auto timeoutBlock = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          makeLabel(
              i18n::tr("settings.idle.behavior.timeout-label"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );
      Input* timeoutPtr = nullptr;
      auto timeoutIn = ui::input({
          .out = &timeoutPtr,
          .value = std::format("{}", row.timeoutSeconds),
          .placeholder = "660",
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
      });
      const auto commitTimeout = [&row, persist, timeoutPtr]() {
        const auto parsed = parseDoubleInput(timeoutPtr->value());
        if (!parsed.has_value() || *parsed < 0.0 ||
            *parsed > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
          timeoutPtr->setInvalid(true);
          return;
        }
        row.timeoutSeconds = static_cast<std::int32_t>(std::lround(*parsed));
        timeoutPtr->setInvalid(false);
        timeoutPtr->setValue(std::format("{}", row.timeoutSeconds));
        persist();
      };
      timeoutIn->setOnChange([timeoutPtr](const std::string& /*t*/) { timeoutPtr->setInvalid(false); });
      timeoutIn->setOnSubmit([commitTimeout](const std::string& /*text*/) { commitTimeout(); });
      timeoutIn->setOnFocusLoss(commitTimeout);
      timeoutBlock->addChild(std::move(timeoutIn));
      body->addChild(std::move(timeoutBlock));

      body->addChild(std::move(customCommandsGrp));
      body->addChild(std::move(resumeCommandGrp));

      section.addChild(std::move(body));

      auto actions = ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
          ui::button({
              .text = i18n::tr("common.actions.apply"),
              .glyph = "check",
              .fontSize = Style::fontSizeBody * scale,
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Default,
              .minHeight = Style::controlHeight * scale,
              .paddingV = Style::spaceSm * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Style::scaledRadiusMd(scale),
              .flexGrow = 1.0f,
              .onClick = [commitName, commitTimeout, applyHostedEditor = ctx.afterIdleBehaviorApply,
                          closeHostedEditor]() {
                commitName();
                commitTimeout();
                if (applyHostedEditor) {
                  applyHostedEditor();
                }
                if (closeHostedEditor) {
                  closeHostedEditor();
                }
              },
          })
      );
      section.addChild(std::move(actions));
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

  std::unique_ptr<Label> makeSettingSubtitleLabel(std::string_view text, float scale) {
    return ui::label({
        .text = std::string(text),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = kSettingDescriptionMaxLines,
    });
  }

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
          colorSpecFromRole(ColorRole::Primary), true
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

      auto copy = ui::column({.align = FlexAlign::Start, .gap = Style::spaceXs * scale, .flexGrow = 1.0f});
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

    const auto makeToggle = [&](bool checked, bool enabled, std::vector<std::string> path,
                                std::optional<bool> clearWhenValue = std::nullopt) {
      if (enabled) {
        return ui::toggle({
            .checked = checked,
            .enabled = enabled,
            .scale = scale,
            .onChange = [configService = ctx.configService, setOverride = ctx.setOverride,
                         clearOverride = ctx.clearOverride, path, clearWhenValue](bool value) {
              if (clearWhenValue.has_value() && value == *clearWhenValue && configService != nullptr &&
                  configService->hasOverride(path)) {
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

    const auto makeSelect = [&](const SelectSetting& setting, std::vector<std::string> path) -> std::unique_ptr<Node> {
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
        [&](float value, float minValue, float maxValue, float step, std::vector<std::string> path,
            bool integerValue = false,
            std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double)> linkedCommit =
                {}) {
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
              .onValueChanged = [valueInputPtr, integerValue](float next) {
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

          slider->setOnDragEnd([commit, sliderPtr]() { commit(static_cast<double>(sliderPtr->value())); });

          const auto commitInputText = [commit, sliderPtr, valueInputPtr, minValue, maxValue,
                                        integerValue](const std::string& text) {
            const auto parsed = parseFloatInput(text);
            if (!parsed.has_value() || *parsed < minValue || *parsed > maxValue) {
              valueInputPtr->setInvalid(true);
              return;
            }
            const float v = *parsed;
            valueInputPtr->setInvalid(false);
            sliderPtr->setValue(v);
            if (!integerValue) {
              valueInputPtr->setValue(formatSliderValue(sliderPtr->value(), false));
            }
            commit(static_cast<double>(v));
          };

          valueInput->setOnChange([valueInputPtr](const std::string& /*text*/) { valueInputPtr->setInvalid(false); });
          valueInput->setOnSubmit([commitInputText](const std::string& text) { commitInputText(text); });
          valueInput->setOnFocusLoss([commitInputText, valueInputPtr]() { commitInputText(valueInputPtr->value()); });

          // Slider first, numeric value field on the right (reset from makeRow stays left of this cluster).
          wrap->addChild(std::move(slider));
          wrap->addChild(std::move(valueInput));
          return wrap;
        };

    const auto makeText = [&](const std::string& value, const std::string& placeholder, std::vector<std::string> path,
                              float width = 0.0f) {
      const float inputWidth = (width > 0.0f ? width : 190.0f) * scale;
      return ui::input({
          .value = value,
          .placeholder = placeholder.empty() ? i18n::tr("settings.controls.list.add-entry-placeholder") : placeholder,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .width = inputWidth,
          .height = Style::controlHeight * scale,
          .onSubmit = [setOverride = ctx.setOverride, path](const std::string& v) { setOverride(path, v); },
      });
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

    const auto makeGlyphText = [&](const TextSetting& setting, std::vector<std::string> path) -> std::unique_ptr<Node> {
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

    const auto makeOptionalNumber = [&](const OptionalNumberSetting& setting, std::vector<std::string> path) {
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

    const auto makeStepper = [&](const StepperSetting& setting, std::vector<std::string> path) {
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

    const auto makeOptionalStepper = [&](const OptionalStepperSetting& setting, std::vector<std::string> path) {
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
                                         std::vector<std::string> path) -> std::unique_ptr<Node> {
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
                                         bool titleMaxTwoLines = false, bool fillWidth = false, bool flexGrow = false) {
      auto block = ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceXs * scale,
           .configure = [scale, fillWidth, flexGrow](Flex& flex) {
             flex.setPadding(2.0f * scale, 0.0f);
             if (fillWidth) {
               flex.setFillWidth(true);
             }
             if (flexGrow) {
               flex.setFlexGrow(1.0f);
             }
           }}
      );

      auto titleRow = ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm * scale,
           .fillWidth = true,
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
          }),
          ui::spacer()
      );
      if (overridden) {
        titleRow->addChild(makeOverrideResetActions(entry.path));
      }
      block->addChild(std::move(titleRow));

      if (!entry.subtitle.empty()) {
        block->addChild(makeSettingSubtitleLabel(entry.subtitle, scale));
      }
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
        .makeToggle = [&](bool checked, std::vector<std::string> path, std::optional<bool> clearWhenValue)
            -> std::unique_ptr<Node> { return makeToggle(checked, true, std::move(path), clearWhenValue); },
        .makeSelect = [&](const SelectSetting& setting, std::vector<std::string> path) -> std::unique_ptr<Node> {
          return makeSelect(setting, std::move(path));
        },
        .makeSlider = [&](float value, float minValue, float maxValue, float step, std::vector<std::string> path,
                          bool integerValue) -> std::unique_ptr<Node> {
          return makeSlider(value, minValue, maxValue, step, std::move(path), integerValue);
        },
        .makeOptionalNumber = [&](const OptionalNumberSetting& setting, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return makeOptionalNumber(setting, std::move(path)); },
        .makeOptionalStepper = [&](const OptionalStepperSetting& setting, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return makeOptionalStepper(setting, std::move(path)); },
        .makeText = [&](const std::string& value, const std::string& placeholder,
                        std::vector<std::string> path) -> std::unique_ptr<Node> {
          return makeText(value, placeholder, std::move(path));
        }, // width not used in search
        .makeColorSpecPicker = [&](const ColorSpecPickerSetting& setting, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return makeColorSpecPicker(setting, std::move(path)); },
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
      if (ctx.searchQuery.empty() && ctx.selectedSection == "bar" &&
          !settingEntryMatchesBarNavigation(entry, selectedBarName, selectedMonitorMatch)) {
        continue;
      }
      if (!ctx.showAdvanced && entry.advanced) {
        continue;
      }
      if (!isEntryVisible(entry)) {
        continue;
      }
      if (ctx.showOverriddenOnly && ctx.configService != nullptr &&
          !ctx.configService->hasEffectiveOverride(entry.path)) {
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
            activeKeybindRow = static_cast<Flex*>(activeSection->addChild(std::move(row)));
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
           .gap = Style::spaceXs * scale,
           .padding = (Style::spaceLg + Style::spaceMd) * scale,
           .configure =
               [scale](Flex& flex) {
                 flex.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.24f));
                 flex.setBorder(colorSpecFromRole(ColorRole::Outline, 0.28f), Style::borderWidth);
                 flex.setRadius(Style::scaledRadiusMd(scale));
               }},
          makeLabel(
              i18n::tr("settings.window.no-results"), Style::fontSizeBody * scale,
              colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
          ),
          makeLabel(
              i18n::tr("settings.window.no-results-hint"), Style::fontSizeCaption * scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          )
      );

      auto emptyRow = ui::row({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .fillWidth = true,
      });
      emptyRow->addChild(std::move(emptyState));
      content.addChild(std::move(emptyRow));
    }

    return visibleEntries;
  }

  void buildSessionActionEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, SessionPanelActionConfig& row, const std::function<void()>& persist
  ) {
    buildSessionActionEntryDetailContentImpl(parent, ctx, row, persist);
  }

  void buildIdleBehaviorEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, IdleBehaviorConfig& row, const std::function<void()>& persist
  ) {
    buildIdleBehaviorEntryDetailContentImpl(parent, ctx, row, persist, ctx.closeHostedEditor);
  }

} // namespace settings
