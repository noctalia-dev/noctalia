#include "config/config_types.h"
#include "i18n/i18n.h"
#include "shell/session/session_action_meta.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/keybind_recorder.h"
#include "ui/controls/select.h"
#include "ui/controls/stepper.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    std::vector<SelectOption> sessionActionVariantOptions() {
      std::vector<SelectOption> options;
      for (const auto& variant : kSessionActionButtonVariants) {
        options.push_back(SelectOption{std::string(variant.key), i18n::tr(variant.labelKey), {}});
      }
      return options;
    }

  } // namespace

  void buildSessionActionEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, SessionPanelActionConfig& row, const std::function<void()>& persist
  ) {
    const float scale = ctx.scale;
    const std::vector<SelectOption> kindOptions = sessionActionKindOptions();

    const float iconSq = Style::controlHeight * scale;
    const float iconGlyphSize = Style::fontSizeBody * scale;
    const float captionSize = Style::fontSizeCaption * scale;
    const std::string glyphLabelText = i18n::tr("settings.session-actions.glyph-label");
    const float glyphColumnWidth =
        std::max(iconSq, static_cast<float>(glyphLabelText.size()) * captionSize * 0.62f + 2.0f * scale);

    auto body = ui::row({
        .align = FlexAlign::Start,
        .gap = Style::spaceMd * scale,
        .fillWidth = true,
    });

    auto iconCol = ui::column({
        .align = FlexAlign::Start,
        .gap = Style::spaceSm * scale,
    });
    iconCol->setMinWidth(glyphColumnWidth);
    auto glyphLabel =
        makeLabel(glyphLabelText, captionSize, colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal);
    glyphLabel->setMinWidth(glyphColumnWidth);
    iconCol->addChild(std::move(glyphLabel));

    auto glyphBtnRow = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
    });

    const std::string previewGlyph = [&] {
      if (row.glyph.has_value() && !row.glyph->empty()) {
        return *row.glyph;
      }
      return std::string(session_action::defaultGlyph(row.action));
    }();
    const auto previewGlyphForRow = [&row]() {
      if (row.glyph.has_value() && !row.glyph->empty()) {
        return *row.glyph;
      }
      return std::string(session_action::defaultGlyph(row.action));
    };
    const auto hasCustomGlyph = [&row]() { return row.glyph.has_value() && !row.glyph->empty(); };

    Button* glyphPickBtnPtr = nullptr;
    auto glyphPickBtn = ui::button({
        .out = &glyphPickBtnPtr,
        .text = "",
        .glyph = previewGlyph,
        .glyphSize = iconGlyphSize,
        .controlHeight = iconSq,
        .variant = ButtonVariant::Default,
        .minWidth = iconSq,
        .maxWidth = iconSq,
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

    Input* cmdPtr = nullptr;

    const auto commandPlaceholder = [&row]() {
      if (row.action == "command") {
        return i18n::tr("settings.session-actions.command-required-placeholder");
      }
      return i18n::tr("settings.session-actions.command-placeholder");
    };

    auto cmdBlock = ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
        makeLabel(
            i18n::tr("settings.session-actions.command-label"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        )
    );
    auto cmdIn = ui::input({
        .out = &cmdPtr,
        .value = row.command.value_or(""),
        .placeholder = commandPlaceholder(),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
        .minLayoutWidth = 280.0f * scale,
    });
    const auto commitCommand = [&row, persist, cmdPtr]() {
      const std::string t = StringUtils::trim(cmdPtr->value());
      if (row.action == "command" && t.empty()) {
        row.command = std::nullopt;
        cmdPtr->setInvalid(true);
        return;
      }
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

    const auto selectedKindIndex = optionIndex(kindOptions, row.action);
    auto kindSelect = ui::select({
        .options = optionLabels(kindOptions),
        .selectedIndex = selectedKindIndex,
        .clearSelection = !selectedKindIndex.has_value(),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .onSelectionChanged =
            [&row, kindOptions, persist, glyphPickBtnPtr, previewGlyphForRow, hasCustomGlyph, cmdPtr,
             commandPlaceholder](std::size_t index, std::string_view /*label*/) {
              if (index >= kindOptions.size()) {
                return;
              }
              const std::string& nextAction = kindOptions[index].value;
              if (nextAction == row.action) {
                return;
              }
              row.action = nextAction;
              if (cmdPtr != nullptr) {
                cmdPtr->setPlaceholder(commandPlaceholder());
                cmdPtr->setInvalid(false);
              }
              if (!hasCustomGlyph()) {
                glyphPickBtnPtr->setGlyph(previewGlyphForRow());
              }
              persist();
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

    auto countdownBlock = ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
        makeLabel(
            i18n::tr("settings.session-actions.countdown-seconds-label"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        )
    );
    const int countdownValue = std::clamp(static_cast<int>(std::lround(row.countdownSeconds)), 0, 60);
    auto countdownStepper = ui::stepper({
        .minValue = 0,
        .maxValue = 60,
        .step = 1,
        .value = countdownValue,
        .scale = scale,
        .valueSuffix = std::optional<std::string>{"s"},
        .onValueCommitted = [&row, persist](int value) {
          row.countdownSeconds = static_cast<double>(value);
          persist();
        },
    });
    countdownBlock->addChild(std::move(countdownStepper));
    fields->addChild(std::move(countdownBlock));

    body->addChild(std::move(fields));
    parent.addChild(std::move(body));
  }

} // namespace settings
