#include "shell/settings/settings_fingerprint.h"

#include "i18n/i18n.h"
#include "security/fingerprint_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/ring.h"
#include "ui/controls/select.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace settings {

  namespace {

    using Mode = FingerprintManager::Mode;

    // Same glyph size as the lock screen's and the polkit panel's fingerprint cue.
    constexpr float kCueGlyphSize = 72.0F;
    // Enrollment ring around that glyph.
    constexpr float kEnrollRingSize = 112.0F;
    constexpr float kEnrollRingThickness = 4.0F;

    std::unique_ptr<Label>
    textLabel(std::string text, float fontSize, ColorRole role, float scale, FontWeight weight = FontWeight::Normal) {
      return ui::label({
          .text = std::move(text),
          .fontSize = fontSize * scale,
          .fontWeight = weight,
          .color = colorSpecFromRole(role),
          .maxLines = 0,
      });
    }

    std::unique_ptr<Button> actionButton(
        std::string text, std::string glyph, bool enabled, ButtonVariant variant, float scale,
        std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .glyph = glyph.empty() ? std::nullopt : std::optional<std::string>{std::move(glyph)},
          .fontSize = Style::fontSizeBody * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = enabled,
          .variant = variant,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .gap = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Button>
    iconButton(std::string glyph, std::string tooltip, bool enabled, float scale, std::function<void()> onClick) {
      return ui::button({
          .glyph = std::move(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = enabled,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Flex> buttonRow(float scale) {
      return ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
    }

    std::unique_ptr<Glyph> smallGlyph(std::string glyph, ColorRole role, float scale) {
      return ui::glyph({
          .glyph = std::move(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(role),
      });
    }

    void addStatus(Flex& parent, const FingerprintManager& manager, float scale) {
      if (manager.status().empty()) {
        return;
      }
      const bool error = manager.statusIsError();
      auto row = ui::row({.align = FlexAlign::Start, .gap = Style::spaceSm * scale});
      row->addChild(
          smallGlyph(error ? "alert-circle" : "circle-check", error ? ColorRole::Error : ColorRole::Primary, scale)
      );
      auto text =
          textLabel(manager.status(), Style::fontSizeBody, error ? ColorRole::Error : ColorRole::OnSurface, scale);
      text->setFlexGrow(1.0F);
      row->addChild(std::move(text));
      parent.addChild(std::move(row));
    }

    std::string readerLabel(const FingerprintDevice& device) {
      return device.name
          + " · "
          + i18n::tr(
                 device.scanType == "swipe" ? "settings.fingerprint.scan-type.swipe"
                                            : "settings.fingerprint.scan-type.press"
          );
    }

    // "Reader:" with the reader's name, or a selector when there are several.
    void addReaderRow(Flex& block, FingerprintManager& manager, float scale) {
      const auto& devices = manager.devices();
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          textLabel(i18n::tr("settings.fingerprint.reader"), Style::fontSizeBody, ColorRole::OnSurfaceVariant, scale)
      );
      if (devices.size() == 1) {
        auto name = textLabel(readerLabel(devices.front()), Style::fontSizeBody, ColorRole::OnSurface, scale);
        name->setFlexGrow(1.0F);
        row->addChild(std::move(name));
      } else {
        std::vector<std::string> options;
        std::size_t selected = 0;
        for (std::size_t i = 0; i < devices.size(); ++i) {
          options.push_back(readerLabel(devices[i]));
          if (devices[i].path == manager.selectedDevice()) {
            selected = i;
          }
        }
        row->addChild(
            ui::select({
                .options = std::move(options),
                .selectedIndex = selected,
                .fontSize = Style::fontSizeBody * scale,
                .controlHeight = Style::controlHeight * scale,
                .enabled = !manager.busy(),
                .flexGrow = 1.0F,
                .onSelectionChanged = [&manager](std::size_t index, std::string_view /*label*/) {
                  if (index < manager.devices().size()) {
                    manager.selectDevice(manager.devices()[index].path);
                  }
                },
            })
        );
      }
      block.addChild(std::move(row));
    }

    // One enrolled finger: its name, a check when the last test recognized it, and delete with an
    // inline confirmation.
    std::unique_ptr<Flex> fingerRow(
        FingerprintManager& manager, const std::string& finger, std::string& pendingDelete,
        const std::function<void()>& rebuild, float scale
    ) {
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .paddingH = Style::spaceSm * scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
          .radius = Style::scaledRadiusMd(scale),
          .minHeight = Style::controlHeight * scale,
          .fillWidth = true,
      });
      row->addChild(smallGlyph("fingerprint", ColorRole::Primary, scale));
      auto name = textLabel(FingerprintManager::fingerLabel(finger), Style::fontSizeBody, ColorRole::OnSurface, scale);
      name->setFlexGrow(1.0F);
      row->addChild(std::move(name));
      if (manager.mode() == Mode::Idle && manager.matchedFinger() == finger) {
        row->addChild(smallGlyph("circle-check", ColorRole::Primary, scale));
      }

      const bool enabled = !manager.busy();
      if (pendingDelete != finger) {
        row->addChild(iconButton(
            "trash", i18n::tr("settings.fingerprint.delete"), enabled, scale, [&pendingDelete, finger, rebuild] {
              pendingDelete = finger;
              rebuild();
            }
        ));
        return row;
      }
      row->addChild(actionButton(
          i18n::tr("settings.fingerprint.confirm-delete"), "trash", enabled, ButtonVariant::Destructive, scale,
          [&pendingDelete, &manager, finger] {
            pendingDelete.clear();
            manager.deleteFinger(finger);
          }
      ));
      row->addChild(iconButton("close", i18n::tr("common.actions.cancel"), true, scale, [&pendingDelete, rebuild] {
        pendingDelete.clear();
        rebuild();
      }));
      return row;
    }

    bool allFingersEnrolled(const FingerprintManager& manager) {
      const auto enrolled = [&](std::string_view finger) {
        return std::ranges::contains(manager.enrolledFingers(), finger);
      };
      return std::ranges::all_of(fingerprint::kLeftHandFingers, enrolled)
          && std::ranges::all_of(fingerprint::kRightHandFingers, enrolled);
    }

    // The fingerprint glyph inside a ring that fills clockwise as the reader accepts scans.
    std::unique_ptr<Flex> progressIndicator(float progress, ColorRole role, float scale) {
      const float size = kEnrollRingSize * scale;
      auto indicator = ui::column({
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .minWidth = size,
          .minHeight = size,
          .maxWidth = size,
          .maxHeight = size,
      });
      auto ring = std::make_unique<Ring>();
      ring->setParticipatesInLayout(false);
      ring->setSize(size, size);
      ring->setThickness(kEnrollRingThickness * scale);
      ring->setFill(colorSpecFromRole(role));
      ring->setProgress(progress);
      indicator->addChild(std::move(ring));
      indicator->addChild(
          ui::glyph({
              .glyph = "fingerprint",
              .glyphSize = kCueGlyphSize * scale,
              .color = colorSpecFromRole(role),
          })
      );
      return indicator;
    }

    // Step 1: one column per hand, laid out as the hands sit palms down (left hand on the left).
    // The column heading names the hand, so the buttons carry the short finger name.
    void addFingerPicker(Flex& content, FingerprintManager& manager, const std::function<void()>& close, float scale) {
      static constexpr std::array<const char*, 3> kShortKeys{
          "settings.fingerprint.finger-short.thumb",
          "settings.fingerprint.finger-short.index",
          "settings.fingerprint.finger-short.middle",
      };
      content.addChild(
          textLabel(i18n::tr("settings.fingerprint.choose-finger"), Style::fontSizeBody, ColorRole::OnSurface, scale)
      );
      auto hands = ui::row({.align = FlexAlign::Start, .gap = Style::spaceSm * scale, .fillWidth = true});
      const auto addHand = [&](const char* titleKey, const auto& fingers) {
        auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F});
        column->addChild(
            textLabel(i18n::tr(titleKey), Style::fontSizeCaption, ColorRole::OnSurfaceVariant, scale, FontWeight::Bold)
        );
        for (std::size_t i = 0; i < fingers.size(); ++i) {
          const std::string finger(fingers[i]);
          const bool enrolled = std::ranges::contains(manager.enrolledFingers(), finger);
          column->addChild(actionButton(
              i18n::tr(kShortKeys[i]), enrolled ? "check" : "fingerprint", !enrolled, ButtonVariant::Outline, scale,
              [&manager, finger] { manager.startEnroll(finger); }
          ));
        }
        hands->addChild(std::move(column));
      };
      addHand("settings.fingerprint.left-hand", fingerprint::kLeftHandFingers);
      addHand("settings.fingerprint.right-hand", fingerprint::kRightHandFingers);
      content.addChild(std::move(hands));

      auto buttons = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .fillWidth = true});
      buttons->addChild(
          actionButton(i18n::tr("common.actions.cancel"), {}, true, ButtonVariant::Default, scale, close)
      );
      content.addChild(std::move(buttons));
    }

    // Step 2: the enrollment progress, drawn like the lock screen's fingerprint cue.
    void
    addEnrollProgress(Flex& content, FingerprintManager& manager, const std::function<void()>& close, float scale) {
      const bool enrolling = manager.mode() == Mode::Enrolling;
      auto title = textLabel(
          FingerprintManager::fingerLabel(manager.enrollFinger()), Style::fontSizeTitle, ColorRole::OnSurface, scale,
          FontWeight::Bold
      );
      title->setTextAlign(TextAlign::Center);
      content.addChild(std::move(title));

      const int total = manager.enrollStagesTotal();
      const int done = std::min(manager.enrollStagesDone(), std::max(total, 0));
      float progress = 0.0F;
      if (manager.enrollCompleted()) {
        progress = 1.0F;
      } else if (total > 0) {
        progress = static_cast<float>(done) / static_cast<float>(total);
      }
      const bool failed = !enrolling && !manager.enrollCompleted();

      auto indicator = ui::column({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      indicator->addChild(progressIndicator(progress, failed ? ColorRole::Error : ColorRole::Primary, scale));
      if (total > 0) {
        auto stages = textLabel(
            i18n::tr("settings.fingerprint.stages", "done", std::to_string(done), "total", std::to_string(total)),
            Style::fontSizeCaption, ColorRole::OnSurfaceVariant, scale
        );
        stages->setTextAlign(TextAlign::Center);
        indicator->addChild(std::move(stages));
      }
      content.addChild(std::move(indicator));
      addStatus(content, manager, scale);

      auto buttons = ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true}
      );
      if (enrolling) {
        buttons->addChild(
            actionButton(i18n::tr("common.actions.cancel"), {}, true, ButtonVariant::Default, scale, close)
        );
      } else {
        if (failed) {
          buttons->addChild(actionButton(
              i18n::tr("settings.fingerprint.try-again"), "refresh", true, ButtonVariant::Default, scale,
              [&manager, finger = manager.enrollFinger()] { manager.startEnroll(finger); }
          ));
        }
        buttons->addChild(
            actionButton(i18n::tr("settings.fingerprint.done"), {}, true, ButtonVariant::Primary, scale, close)
        );
      }
      content.addChild(std::move(buttons));
    }

  } // namespace

  void buildFingerprintBlock(
      Flex& block, FingerprintManager& manager, std::string& pendingDelete, const FingerprintBlockActions& actions,
      float scale
  ) {
    if (manager.devices().empty()) {
      return;
    }
    const bool busy = manager.busy();
    addReaderRow(block, manager, scale);

    block.addChild(textLabel(
        i18n::tr("settings.fingerprint.enrolled"), Style::fontSizeCaption, ColorRole::OnSurfaceVariant, scale,
        FontWeight::Bold
    ));
    if (manager.enrolledFingers().empty()) {
      block.addChild(textLabel(
          i18n::tr("settings.fingerprint.none-enrolled"), Style::fontSizeCaption, ColorRole::OnSurfaceVariant, scale
      ));
    }
    for (const auto& finger : manager.enrolledFingers()) {
      block.addChild(fingerRow(manager, finger, pendingDelete, actions.requestRebuild, scale));
    }

    if (manager.mode() != Mode::Enrolling) {
      addStatus(block, manager, scale);
    }

    auto buttons = buttonRow(scale);
    buttons->addChild(actionButton(
        i18n::tr("settings.fingerprint.add"), "add", !busy && !allFingersEnrolled(manager), ButtonVariant::Primary,
        scale, actions.openEnrollSheet
    ));
    if (manager.mode() == Mode::Verifying) {
      buttons->addChild(actionButton(
          i18n::tr("settings.fingerprint.stop-test"), "close", true, ButtonVariant::Default, scale,
          [&manager] { manager.cancel(); }
      ));
    } else {
      buttons->addChild(actionButton(
          i18n::tr("settings.fingerprint.test"), "fingerprint", !busy && !manager.enrolledFingers().empty(),
          ButtonVariant::Default, scale, [&manager] { manager.startVerify(); }
      ));
    }
    block.addChild(std::move(buttons));
  }

  void buildFingerprintEnrollSheet(
      Flex& body, FingerprintManager& manager, const std::function<void()>& close, float scale
  ) {
    auto content = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .fillWidth = true});
    if (manager.mode() == Mode::Enrolling || !manager.enrollFinger().empty()) {
      addEnrollProgress(*content, manager, close, scale);
    } else {
      addFingerPicker(*content, manager, close, scale);
    }
    body.addChild(std::move(content));
  }

} // namespace settings
