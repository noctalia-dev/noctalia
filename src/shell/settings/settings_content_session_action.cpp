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
    const float controlHeight = Style::controlHeight * scale;
    const std::vector<SelectOption> kindOptions = sessionActionKindOptions();

    // Every field is a caption label stacked over its control, with one gap inside
    // a field and a wider one between fields, so all labels and controls share the
    // same left edge and rhythm.
    const auto field = [scale](std::string label, std::unique_ptr<Node> control, float grow) {
      auto block = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = grow},
          makeLabel(
              std::move(label), Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
              FontWeight::Normal
          )
      );
      block->addChild(std::move(control));
      return block;
    };
    const auto pair = [scale](std::unique_ptr<Node> left, std::unique_ptr<Node> right) {
      auto line = ui::row({.align = FlexAlign::Start, .gap = Style::spaceMd * scale, .fillWidth = true});
      line->addChild(std::move(left));
      line->addChild(std::move(right));
      return line;
    };

    auto fields = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
        .fillWidth = true,
    });

    // Glyph + behavior
    const auto previewGlyphForRow = [&row]() {
      if (row.glyph.has_value() && !row.glyph->empty()) {
        return *row.glyph;
      }
      return std::string(session_action::defaultGlyph(row.action));
    };
    const auto hasCustomGlyph = [&row]() { return row.glyph.has_value() && !row.glyph->empty(); };

    auto glyphBtnRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

    Button* glyphPickBtnPtr = nullptr;
    auto glyphPickBtn = ui::button({
        .out = &glyphPickBtnPtr,
        .text = "",
        .glyph = previewGlyphForRow(),
        .glyphSize = Style::fontSizeBody * scale,
        .controlHeight = controlHeight,
        .variant = ButtonVariant::Default,
        .minWidth = controlHeight,
        .maxWidth = controlHeight,
        .padding = 0.0F,
        .radius = Style::scaledRadiusMd(scale),
    });

    Button* clearGlyphBtnPtr = nullptr;
    auto clearG = ui::button({
        .out = &clearGlyphBtnPtr,
        .text = i18n::tr("settings.session-actions.clear-glyph"),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Ghost,
        .minHeight = controlHeight,
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

    Input* cmdPtr = nullptr;
    const auto commandPlaceholder = [&row]() {
      if (row.action == "command") {
        return i18n::tr("settings.session-actions.command-required-placeholder");
      }
      return i18n::tr("settings.session-actions.command-placeholder");
    };

    // Shell command
    auto cmdIn = ui::input({
        .out = &cmdPtr,
        .value = row.command.value_or(""),
        .placeholder = commandPlaceholder(),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = controlHeight,
        .horizontalPadding = Style::spaceSm * scale,
        .minLayoutWidth = 280.0F * scale,
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

    const auto selectedKindIndex = optionIndex(kindOptions, row.action);
    auto kindSelect = ui::select({
        .options = optionLabels(kindOptions),
        .selectedIndex = selectedKindIndex,
        .clearSelection = !selectedKindIndex.has_value(),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = controlHeight,
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

    fields->addChild(pair(
        field(i18n::tr("settings.session-actions.glyph-label"), std::move(glyphBtnRow), 0.0F),
        field(i18n::tr("settings.session-actions.kind-section-label"), std::move(kindSelect), 1.0F)
    ));

    // Label
    Input* labelPtr = nullptr;
    auto labelIn = ui::input({
        .out = &labelPtr,
        .value = row.label.value_or(""),
        .placeholder = i18n::tr("settings.session-actions.label-placeholder"),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = controlHeight,
        .horizontalPadding = Style::spaceSm * scale,
        .minLayoutWidth = 200.0F * scale,
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
    fields->addChild(field(i18n::tr("settings.session-actions.label-field"), std::move(labelIn), 0.0F));

    fields->addChild(field(i18n::tr("settings.session-actions.command-label"), std::move(cmdIn), 0.0F));

    // Button style + countdown
    const std::vector<SelectOption> variantOptions = sessionActionVariantOptions();
    const std::string selectedVariant(enumToKey(kSessionActionButtonVariants, row.variant));
    const auto selectedVariantIndex = optionIndex(variantOptions, selectedVariant);
    auto variantSelect = ui::select({
        .options = optionLabels(variantOptions),
        .selectedIndex = selectedVariantIndex,
        .clearSelection = !selectedVariantIndex.has_value(),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = controlHeight,
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

    fields->addChild(pair(
        field(i18n::tr("settings.session-actions.variant-label"), std::move(variantSelect), 1.0F),
        field(i18n::tr("settings.session-actions.countdown-seconds-label"), std::move(countdownStepper), 1.0F)
    ));

    // Shortcuts: the clear button always holds its slot so both recorders keep the
    // same width whether or not a chord is set.
    const auto shortcutControl = [&row, persist, scale, controlHeight](
                                     std::optional<KeyChord> SessionPanelActionConfig::* member, ModifierPolicy policy
                                 ) {
      auto line = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .fillWidth = true});

      KeybindRecorder* recorderPtr = nullptr;
      Button* clearBtnPtr = nullptr;
      auto recorder = ui::keybindRecorder({
          .out = &recorderPtr,
          .chord = row.*member,
          .scale = scale,
          .unsetPlaceholder = i18n::tr("settings.controls.keybind.unset-placeholder"),
          .recordingPlaceholder = i18n::tr("settings.controls.keybind.recording-placeholder"),
          .modifierPolicy = policy,
          .flexGrow = 1.0F,
          .configure = [controlHeight](KeybindRecorder& control) { control.setMinHeight(controlHeight); },
      });
      auto clearBtn = ui::button({
          .out = &clearBtnPtr,
          .glyph = "close",
          .glyphSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = controlHeight,
          .minHeight = controlHeight,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .visible = (row.*member).has_value(),
      });
      recorderPtr->setOnCommit([&row, member, persist, clearBtnPtr](KeyChord chord) {
        row.*member = chord;
        clearBtnPtr->setVisible(true);
        persist();
      });
      clearBtnPtr->setOnClick([&row, member, persist, recorderPtr, clearBtnPtr]() {
        row.*member = std::nullopt;
        recorderPtr->setChord(std::nullopt);
        clearBtnPtr->setVisible(false);
        persist();
      });
      line->addChild(std::move(recorder));
      line->addChild(std::move(clearBtn));
      return line;
    };

    fields->addChild(pair(
        field(
            i18n::tr("settings.session-actions.shortcut-label"),
            shortcutControl(&SessionPanelActionConfig::shortcut, ModifierPolicy::Optional), 1.0F
        ),
        // Required: bare or Shift+printable chords would be typed into the password field instead.
        field(
            i18n::tr("settings.session-actions.lockscreen-shortcut-label"),
            shortcutControl(&SessionPanelActionConfig::lockscreenShortcut, ModifierPolicy::Required), 1.0F
        )
    ));

    parent.addChild(std::move(fields));
  }

} // namespace settings
