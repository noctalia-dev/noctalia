#include "config/config_types.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {

  void buildIdleBehaviorEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, IdleBehaviorConfig& row, const std::function<void()>& persist
  ) {
    const float scale = ctx.scale;

    const std::vector<SelectOption> idleActionOptions = {
        {"lock", i18n::tr("settings.idle.behavior.kind.lock"), {}},
        {"screen_off", i18n::tr("settings.idle.behavior.kind.screen-off"), {}},
        {"suspend", i18n::tr("settings.idle.behavior.kind.suspend"), {}},
        {"lock_and_suspend", i18n::tr("settings.idle.behavior.kind.lock-and-suspend"), {}},
        {"command", i18n::tr("settings.idle.behavior.kind.custom"), {}},
    };

    IdleBehaviorConfig norm = row;
    normalizeIdleBehaviorAction(norm);
    const bool showCustomCommands = (norm.action == "command");

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

    const auto addCommandInput = [&](Flex& section, std::string label, std::string placeholder, std::string& target) {
      auto block = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
          makeLabel(
              label, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
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
      section.addChild(std::move(block));
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

    Flex* nameBlockRaw = nullptr;
    auto nameBlock = ui::column(
        {.out = &nameBlockRaw,
         .align = FlexAlign::Stretch,
         .gap = Style::spaceXs * scale,
         .visible = showCustomCommands},
        makeLabel(
            i18n::tr("settings.idle.behavior.name-label"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        )
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
             nameBlockRaw](std::size_t index, std::string_view /*label*/) {
              if (index < idleActionOptions.size()) {
                row.action = idleActionOptions[index].value;
                if (row.action != "command") {
                  row.command.clear();
                }
                // "suspend" and "lock_and_suspend" are distinct choices; pin lockBeforeSuspend so the
                // explicit selection wins over normalizeIdleBehaviorAction's suspend -> lock_and_suspend fold.
                row.lockBeforeSuspend = (row.action == "lock_and_suspend");
              }
              IdleBehaviorConfig n = row;
              normalizeIdleBehaviorAction(n);
              customCommandsRaw->setVisible(n.action == "command");
              nameBlockRaw->setVisible(n.action == "command");
              persist();
            },
        .configure = [](Select& select) { select.setFillWidth(true); },
    });
    kindBlock->addChild(std::move(kindSelect));
    body->addChild(std::move(kindBlock));

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
        .value = StringUtils::formatDotDecimal(row.timeoutSeconds),
        .placeholder = "660",
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
    });
    const auto commitTimeout = [&row, persist, timeoutPtr]() {
      const auto parsed = parseDoubleInput(timeoutPtr->value());
      constexpr double kMaxIdleTimeoutSeconds = static_cast<double>(std::numeric_limits<std::uint32_t>::max()) / 1000.0;
      if (!parsed.has_value() || *parsed < 0.0 || *parsed > kMaxIdleTimeoutSeconds) {
        timeoutPtr->setInvalid(true);
        return;
      }
      row.timeoutSeconds = *parsed;
      timeoutPtr->setInvalid(false);
      timeoutPtr->setValue(StringUtils::formatDotDecimal(row.timeoutSeconds));
      persist();
    };
    timeoutIn->setOnChange([timeoutPtr](const std::string& /*t*/) { timeoutPtr->setInvalid(false); });
    timeoutIn->setOnSubmit([commitTimeout](const std::string& /*text*/) { commitTimeout(); });
    timeoutIn->setOnFocusLoss(commitTimeout);
    timeoutBlock->addChild(std::move(timeoutIn));
    body->addChild(std::move(timeoutBlock));

    body->addChild(std::move(customCommandsGrp));
    body->addChild(std::move(resumeCommandGrp));

    parent.addChild(std::move(body));

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
                        closeHostedEditor = ctx.closeHostedEditor]() {
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
    parent.addChild(std::move(actions));
  }

} // namespace settings
