#include "config/config_types.h"
#include "i18n/i18n.h"
#include "notification/notification_filter.h"
#include "notification/notification_manager.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/stepper.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace settings {

  namespace {

    void addToggleRow(
        Flex& parent, float scale, std::string label, bool checked, const std::function<void(bool)>& onChange
    ) {
      auto row = ui::row({
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = Style::spaceSm * scale,
          .minHeight = Style::controlHeightSm * scale,
      });
      row->addChild(makeLabel(
          std::move(label), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Normal
      ));
      row->addChild(
          ui::toggle({
              .checked = checked,
              .scale = scale,
              .onChange = onChange,
          })
      );
      parent.addChild(std::move(row));
    }

  } // namespace

  void buildNotificationFilterEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, NotificationFilterConfig& row, const std::function<void()>& persist
  ) {
    const float scale = ctx.scale;

    auto body = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceMd * scale,
    });

    auto matchBlock = ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
        makeLabel(
            i18n::tr("settings.notifications.filter.match-label"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        )
    );
    Input* matchPtr = nullptr;
    auto matchInput = ui::input({
        .out = &matchPtr,
        .value = row.match,
        .placeholder = i18n::tr("settings.notifications.filter.match-placeholder"),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
    });

    auto matchContentBlock = ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale},
        makeLabel(
            i18n::tr("settings.notifications.filter.match-content-label"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        )
    );
    Input* matchContentPtr = nullptr;
    auto matchContentInput = ui::input({
        .out = &matchContentPtr,
        .value = row.matchContent,
        .placeholder = i18n::tr("settings.notifications.filter.match-content-placeholder"),
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
    });

    const auto flushMatchContentFromInput = [&row, matchContentPtr]() {
      row.matchContent = StringUtils::trim(matchContentPtr->value());
      matchContentPtr->setValue(row.matchContent);
    };
    const auto flushMatchFromInput = [&row, matchPtr]() -> bool {
      row.match = normalizeNotificationMatchToken(matchPtr->value());
      if (row.match.empty()) {
        matchPtr->setInvalid(true);
        return false;
      }
      matchPtr->setInvalid(false);
      matchPtr->setValue(row.match);
      return true;
    };
    const auto persistDraft = [&row, matchPtr, flushMatchContentFromInput, persist]() {
      flushMatchContentFromInput();
      const std::string draftMatch = normalizeNotificationMatchToken(matchPtr->value());
      if (!draftMatch.empty()) {
        row.match = draftMatch;
        matchPtr->setInvalid(false);
        matchPtr->setValue(row.match);
      }
      persist();
    };
    const auto commitMatch = [flushMatchContentFromInput, flushMatchFromInput, persist]() {
      flushMatchContentFromInput();
      if (!flushMatchFromInput()) {
        return;
      }
      persist();
    };
    const auto commitMatchContent = [flushMatchContentFromInput, persist]() {
      flushMatchContentFromInput();
      persist();
    };

    matchPtr->setOnSubmit([commitMatch](const std::string& /*text*/) { commitMatch(); });
    matchPtr->setOnFocusLoss(commitMatch);
    matchContentPtr->setOnSubmit([commitMatchContent](const std::string& /*text*/) { commitMatchContent(); });
    matchContentPtr->setOnFocusLoss(commitMatchContent);
    matchBlock->addChild(std::move(matchInput));
    body->addChild(std::move(matchBlock));
    matchContentBlock->addChild(std::move(matchContentInput));
    body->addChild(std::move(matchContentBlock));

    auto flagsBlock = ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
        makeLabel(
            i18n::tr("settings.notifications.filter.flags-label"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        )
    );
    const auto urgencyEnabled = [&row](std::string_view level) {
      return row.allowedUrgencies.empty() || std::ranges::contains(row.allowedUrgencies, level);
    };
    const auto setUrgency = [&row, persistDraft](std::string_view level, bool enabled) {
      std::vector<std::string> selected;
      if (row.allowedUrgencies.empty()) {
        selected = {"low", "normal", "critical"};
      } else {
        selected = row.allowedUrgencies;
      }
      if (enabled) {
        if (!std::ranges::contains(selected, level)) {
          selected.emplace_back(level);
        }
      } else if (selected.size() <= 1) {
        return;
      } else {
        std::erase(selected, std::string(level));
      }
      row.allowedUrgencies = normalizeFilterAllowedUrgencyStrings(std::move(selected));
      persistDraft();
    };
    auto urgenciesBlock = ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale},
        makeLabel(
            i18n::tr("settings.notifications.filter.allowed-urgencies-label"), Style::fontSizeCaption * scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        )
    );
    addToggleRow(
        *urgenciesBlock, scale, i18n::tr("settings.options.notification-urgency.low"), urgencyEnabled("low"),
        [setUrgency](bool value) { setUrgency("low", value); }
    );
    addToggleRow(
        *urgenciesBlock, scale, i18n::tr("settings.options.notification-urgency.normal"), urgencyEnabled("normal"),
        [setUrgency](bool value) { setUrgency("normal", value); }
    );
    addToggleRow(
        *urgenciesBlock, scale, i18n::tr("settings.options.notification-urgency.critical"), urgencyEnabled("critical"),
        [setUrgency](bool value) { setUrgency("critical", value); }
    );
    body->addChild(std::move(urgenciesBlock));
    addToggleRow(
        *flagsBlock, scale, i18n::tr("settings.notifications.filter.show-toast"), row.showToast,
        [&row, persistDraft](bool value) {
          row.showToast = value;
          persistDraft();
        }
    );
    addToggleRow(
        *flagsBlock, scale, i18n::tr("settings.notifications.filter.save-history"), row.saveHistory,
        [&row, persistDraft](bool value) {
          row.saveHistory = value;
          persistDraft();
        }
    );
    addToggleRow(
        *flagsBlock, scale, i18n::tr("settings.notifications.filter.play-sound"), row.playSound,
        [&row, persistDraft](bool value) {
          row.playSound = value;
          persistDraft();
        }
    );
    addToggleRow(
        *flagsBlock, scale, i18n::tr("settings.notifications.filter.allow-permanent"), row.allowPermanent,
        [&row, persistDraft](bool value) {
          row.allowPermanent = value;
          persistDraft();
        }
    );

    auto durationRow = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
        .minHeight = Style::controlHeightSm * scale,
    });
    durationRow->addChild(makeLabel(
        i18n::tr("settings.notifications.filter.override-duration"), Style::fontSizeBody * scale,
        colorSpecFromRole(ColorRole::OnSurface), FontWeight::Normal
    ));

    auto durationControls = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

    Stepper* overrideStepper = nullptr;
    durationControls->addChild(
        ui::stepper(
            {.out = &overrideStepper,
             .minValue = 0,
             .maxValue = 3600000,
             .step = 1000,
             .value = row.overrideDuration.value_or(kDefaultNotificationTimeout),
             .enabled = row.overrideDuration.has_value(),
             .scale = scale,
             .valueSuffix = " ms",
             .onValueCommitted = [&row, persistDraft](int val) {
               row.overrideDuration = val;
               persistDraft();
             }}
        )
    );

    durationControls->addChild(
        ui::toggle({
            .checked = row.overrideDuration.has_value(),
            .scale = scale,
            .onChange = [&row, persistDraft, overrideStepper](bool checked) {
              if (checked) {
                row.overrideDuration = overrideStepper->value();
              } else {
                row.overrideDuration = std::nullopt;
              }
              overrideStepper->setEnabled(checked);
              persistDraft();
            },
        })
    );
    durationRow->addChild(std::move(durationControls));
    flagsBlock->addChild(std::move(durationRow));

    body->addChild(std::move(flagsBlock));

    parent.addChild(std::move(body));

    Button* applyButton = nullptr;
    auto actions = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
        ui::button({
            .out = &applyButton,
            .text = i18n::tr("common.actions.apply"),
            .glyph = "check",
            .fontSize = Style::fontSizeBody * scale,
            .glyphSize = Style::fontSizeBody * scale,
            .enabled = !normalizeNotificationMatchToken(row.match).empty(),
            .variant = ButtonVariant::Default,
            .minHeight = Style::controlHeight * scale,
            .paddingV = Style::spaceSm * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .flexGrow = 1.0F,
            .onClick = [commitMatch, applyHostedEditor = ctx.afterNotificationFilterApply,
                        closeHostedEditor = ctx.closeHostedEditor]() {
              commitMatch();
              if (applyHostedEditor) {
                applyHostedEditor();
              }
              if (closeHostedEditor) {
                closeHostedEditor();
              }
            },
        })
    );
    matchPtr->setOnChange([matchPtr, applyButton](const std::string& text) {
      matchPtr->setInvalid(false);
      applyButton->setEnabled(!normalizeNotificationMatchToken(text).empty());
    });
    parent.addChild(std::move(actions));
  }

} // namespace settings
