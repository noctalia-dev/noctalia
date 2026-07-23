#include "shell/settings/settings_content.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "notification/notification_filter.h"
#include "shell/settings/bar_widget_editor.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/keybind_recorder.h"
#include "ui/controls/label.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace settings {
  namespace {

    bool isDockLauncherIconPath(const std::vector<std::string>& path) {
      return path.size() == 2 && path[0] == "dock" && path[1] == "launcher_icon";
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

  BarWidgetEditorContext makeBarWidgetEditorContext(SettingsControlFactory& factory) {
    const SettingsContentContext& ctx = factory.context();
    return BarWidgetEditorContext{
        .config = ctx.config,
        .configService = ctx.configService,
        .scale = ctx.scale,
        .showAdvanced = ctx.showAdvanced,
        .showOverriddenOnly = ctx.showOverriddenOnly,
        .batteryDeviceOptions = ctx.batteryDeviceOptions,
        .keyboardLayoutNames = ctx.keyboardLayoutNames,
        .editingWidgetName = ctx.editingWidgetName,
        .editingCapsuleGroupId = ctx.editingCapsuleGroupId,
        .selectedLaneWidgets = ctx.selectedLaneWidgets,
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
        .closeHostedEditor = ctx.closeHostedEditor,
        .openWidgetInspector = ctx.openWidgetInspectorEditor,
        .openCapsuleGroupInspector = ctx.openCapsuleGroupEditor,
        .makeResetButton = [&factory](const std::vector<std::string>& path) { return factory.makeResetButton(path); },
        .makeRow = [&factory](
                       Flex& section, const SettingEntry& entry, std::unique_ptr<Node> control
                   ) { factory.makeRow(section, entry, std::move(control)); },
        .makeToggle = [&factory](bool checked, std::vector<std::string> path, std::optional<bool> clearWhenValue)
            -> std::unique_ptr<Node> { return factory.makeToggle(checked, true, std::move(path), clearWhenValue); },
        .makeSelect = [&factory](const SelectSetting& setting, std::vector<std::string> path) -> std::unique_ptr<Node> {
          return factory.makeSelect(setting, std::move(path));
        },
        .makeSearchPicker = [&factory](
                                const SearchPickerSetting& setting, std::string title, std::vector<std::string> path
                            ) -> std::unique_ptr<Node> {
          return factory.makeSearchPicker(setting, std::move(title), std::move(path));
        },
        .makeSlider = [&factory](
                          double value, double minValue, double maxValue, double step, std::vector<std::string> path,
                          bool integerValue
                      ) -> std::unique_ptr<Node> {
          return factory.makeSlider(value, minValue, maxValue, step, std::move(path), integerValue);
        },
        .makeOptionalNumber = [&factory](const OptionalNumberSetting& setting, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return factory.makeOptionalNumber(setting, std::move(path)); },
        .makeOptionalStepper = [&factory](const OptionalStepperSetting& setting, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return factory.makeOptionalStepper(setting, std::move(path)); },
        .makeStepper = [&factory](const StepperSetting& setting, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return factory.makeStepper(setting, std::move(path)); },
        .makeText = [&factory](const std::string& value, const std::string& placeholder, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return factory.makeText(value, placeholder, std::move(path)); },
        .makeColorSpecPicker = [&factory](const ColorSpecPickerSetting& setting, std::vector<std::string> path)
            -> std::unique_ptr<Node> { return factory.makeColorSpecPicker(setting, std::move(path)); },
        .makeListBlock = [&factory](
                             Flex& section, const SettingEntry& entry, const ListSetting& list
                         ) { factory.makeListBlock(section, entry, list); },
        .makeStringMapBlock = [&factory](
                                  Flex& section, const SettingEntry& entry, const StringMapSetting& map
                              ) { factory.makeStringMapBlock(section, entry, map); },
        .supportsTaskbarWorkspaceGrouping = ctx.supportsTaskbarWorkspaceGrouping,
    };
  }

  std::size_t
  addSettingsContentSections(Flex& content, const std::vector<SettingEntry>& registry, SettingsContentContext ctx) {
    const float scale = ctx.scale;

    const auto sectionLabel = [](SettingsSection section) { return i18n::tr(settingsSectionLabelKey(section)); };

    const auto groupLabel = [](std::string_view group) -> std::string {
      return i18n::tr("settings.navigation.groups." + std::string(group));
    };

    const auto makeSection = [&](std::string_view title, SettingsSection sectionKey) -> Flex* {
      auto section = ui::column(
          {
              .align = FlexAlign::Stretch,
              .gap = Style::spaceSm * scale,
              .padding = Style::spaceLg * scale,
              .fill = clearColorSpec(),
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

    SettingsControlFactory factory(ctx);

    const auto makeRow = [&](Flex& section, const SettingEntry& entry, std::unique_ptr<Node> control) {
      factory.makeRow(section, entry, std::move(control));
    };

    const auto makeToggle = [&](bool checked, bool enabled, std::vector<std::string> path,
                                std::optional<bool> clearWhenValue = std::nullopt) {
      return factory.makeToggle(checked, enabled, std::move(path), clearWhenValue);
    };

    const auto makeSelect = [&](const SelectSetting& setting, std::vector<std::string> path) -> std::unique_ptr<Node> {
      return factory.makeSelect(setting, std::move(path));
    };

    const auto makeSlider =
        [&](double value, double minValue, double maxValue, double step, std::vector<std::string> path,
            bool integerValue = false, std::string valueSuffix = {},
            std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double)> linkedCommit =
                {},
            SliderSetting::InvertSlot invertSlot = SliderSetting::InvertSlot::None,
            bool invertEnabled = true) -> std::unique_ptr<Node> {
      return factory.makeSlider(
          value, minValue, maxValue, step, std::move(path), integerValue, std::move(linkedCommit),
          std::move(valueSuffix), invertSlot, invertEnabled
      );
    };

    const auto makeRangeSlider = [&](const RangeSliderSetting& setting,
                                     const std::vector<std::string>& lowPath) -> std::unique_ptr<Node> {
      return factory.makeRangeSlider(setting, lowPath);
    };

    const auto makeText = [&](const std::string& value, const std::string& placeholder, std::vector<std::string> path,
                              float width = 0.0f) {
      return factory.makeText(value, placeholder, std::move(path), width);
    };

    const auto makeTextWithPathBrowse = [&](const TextSetting& setting, const std::vector<std::string>& path) {
      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});

      const float inputWidth = (setting.width > 0.0f ? setting.width : 280.0f) * scale;
      Input* inputPtr = nullptr;
      auto input = ui::input({
          .out = &inputPtr,
          .value = setting.value,
          .placeholder = setting.placeholder,
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
          .variant = ButtonVariant::Default,
          .minWidth = Style::controlHeight * scale,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [setOverride = ctx.setOverride, path, inputPtr, selectFolder, exts = setting.browseFileExtensions,
                      fallbackDir = setting.browseFallbackDirectory]() {
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
            } else if (!fallbackDir.empty()) {
              std::error_code ec;
              const std::filesystem::path fallback(fallbackDir);
              if (std::filesystem::exists(fallback, ec) && std::filesystem::is_directory(fallback, ec)) {
                options.startDirectory = fallback;
              }
            }
            (void)FileDialog::open(
                std::move(options), [setOverride, path](std::optional<std::filesystem::path> picked) {
                  if (!picked.has_value()) {
                    return;
                  }
                  setOverride(path, picked->string());
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
          .variant = ButtonVariant::Default,
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
      return factory.makeOptionalNumber(setting, std::move(path));
    };

    const auto makeStepper = [&](const StepperSetting& setting, std::vector<std::string> path) {
      return factory.makeStepper(setting, std::move(path));
    };

    const auto makeOptionalStepper = [&](const OptionalStepperSetting& setting, std::vector<std::string> path) {
      return factory.makeOptionalStepper(setting, std::move(path));
    };

    const auto makeColorSpecPicker = [&](const ColorSpecPickerSetting& setting,
                                         std::vector<std::string> path) -> std::unique_ptr<Node> {
      return factory.makeColorSpecPicker(setting, std::move(path));
    };

    const auto makeSearchPickerButton = [&](const SettingEntry& entry,
                                            const SearchPickerSetting& setting) -> std::unique_ptr<Node> {
      return factory.makeSearchPicker(setting, entry.title, entry.path);
    };

    const auto makeCollectionBlock = [&](const SettingEntry& entry, bool overridden, bool reserveTitleHeight = false,
                                         bool titleMaxTwoLines = false, bool fillWidth = false, bool flexGrow = false,
                                         bool compactTitleDescription = false) {
      return factory.makeCollectionBlock(
          entry, overridden, reserveTitleHeight, titleMaxTwoLines, fillWidth, flexGrow, compactTitleDescription
      );
    };

    const auto makeMultiSelectBlock = [&](Flex& section, const SettingEntry& entry, const MultiSelectSetting& setting) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden);

      auto checkRow = ui::row(
          {.align = FlexAlign::Center,
           .wrap = true,
           .gap = Style::spaceMd * scale,
           .paddingV = Style::spaceXs * scale,
           .paddingH = 0.0f,
           .fillWidth = true}
      );

      auto options = setting.options;
      auto selected = setting.selectedValues;
      const bool requireAtLeastOne = setting.requireAtLeastOne;
      const bool persistUnselected = setting.persistUnselected;
      auto path = entry.path;

      for (const auto& option : options) {
        auto item = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

        const bool isSelected = std::ranges::contains(selected, option.value);
        const std::string optionValue = option.value;
        auto checkbox = ui::checkbox({
            .checked = isSelected,
            .scale = scale,
            .onChange = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path, options, selected,
                         optionValue, requireAtLeastOne, persistUnselected](bool checked) mutable {
              auto it = std::ranges::find(selected, optionValue);
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
              // Preserve the option order so the override file is stable. When
              // persistUnselected is set, store the unchecked complement (denylist).
              std::vector<std::string> ordered;
              ordered.reserve(options.size());
              for (const auto& opt : options) {
                if (std::ranges::contains(selected, opt.value) != persistUnselected) {
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
          if (std::ranges::contains(*selected, opt.value)) {
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

        const bool checked = std::ranges::contains(*selected, option.value);
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
                      .border = active ? colorSpecFromRole(ColorRole::Primary, 0.9f)
                                       : colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha),
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
                      .border = colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha),
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
                .fontWeight = FontWeight::Medium,
                .color = colorSpecFromRole(checked ? ColorRole::OnPrimary : ColorRole::OnSurface),
                .maxLines = 1,
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
        if (!option.tooltip.empty()) {
          card->setTooltip(option.tooltip);
        }

        row->addChild(std::move(cardNode));
        ++countInRow;
      }
      flushRow();

      block->addChild(std::move(grid));
      section.addChild(std::move(block));
    };

    const auto makeListBlock = [&](Flex& section, const SettingEntry& entry, const ListSetting& list) {
      factory.makeListBlock(section, entry, list);
    };

    const auto makeKeybindListBlock = [&](Flex& section, const SettingEntry& entry,
                                          const KeybindListSetting& keybinds) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, false, true, true, true, true, true);
      block->setClipChildren(true);
      block->setMinWidth(0.0f);
      block->setGap(Style::spaceXs * scale);

      auto list = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .fillWidth = true,
          .clipChildren = true,
      });

      const auto configureGridRecorder = [](KeybindRecorder& recorder) {
        recorder.setMinWidth(0.0f);
        recorder.setFillWidth(true);
        recorder.setClipChildren(true);
      };

      const auto keybindTabFocusKey = [&entry](std::string_view suffix) {
        std::string key;
        for (std::size_t i = 0; i < entry.path.size(); ++i) {
          if (i > 0) {
            key += '.';
          }
          key += entry.path[i];
        }
        key += suffix;
        return key;
      };

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
        auto row = ui::row({
            .align = FlexAlign::Center,
            .gap = Style::spaceXs * scale,
            .fillWidth = true,
        });

        auto recorder = ui::keybindRecorder({
            .chord = keybinds.items[i],
            .scale = scale,
            .unsetPlaceholder = i18n::tr("settings.controls.keybind.unset-placeholder"),
            .recordingPlaceholder = i18n::tr("settings.controls.keybind.recording-placeholder"),
            .flexGrow = 1.0f,
            .onCommit =
                [commitItems, items = keybinds.items, i](KeyChord chord) mutable {
                  if (i < items.size()) {
                    items[i] = chord;
                    commitItems(std::move(items));
                  }
                },
            .configure = configureGridRecorder,
        });
        row->addChild(std::move(recorder));

        auto removeBtn = ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .maxWidth = Style::controlHeightSm * scale,
            .maxHeight = Style::controlHeightSm * scale,
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
        auto addRow = ui::row({
            .align = FlexAlign::Center,
            .gap = Style::spaceXs * scale,
            .fillWidth = true,
        });

        auto addRecorder = ui::keybindRecorder({
            .scale = scale,
            .unsetPlaceholder = i18n::tr("settings.controls.keybind.add"),
            .recordingPlaceholder = i18n::tr("settings.controls.keybind.recording-placeholder"),
            .flexGrow = 1.0f,
            .onCommit =
                [commitItems, items = keybinds.items](KeyChord chord) mutable {
                  items.push_back(chord);
                  commitItems(std::move(items));
                },
            .configure =
                [configureGridRecorder, focusKey = keybindTabFocusKey(".add")](KeybindRecorder& recorder) {
                  configureGridRecorder(recorder);
                  recorder.setTabFocusKey(focusKey);
                },
        });
        addRow->addChild(std::move(addRecorder));
        // Reserve the remove-button column so the add recorder lines up with the recorded ones.
        addRow->addChild(ui::row({.width = Style::controlHeightSm * scale}));

        list->addChild(std::move(addRow));
      }

      block->addChild(std::move(list));

      if (overridden) {
        block->addChild(factory.makeOverrideResetActions(entry.path));
      }

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
        if (value.empty() || std::ranges::contains(items, value, &ShortcutConfig::type)) {
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

      const std::vector<SelectOption> kindOptions = settings::sessionActionKindOptions();

      auto state = std::make_shared<std::vector<SessionPanelActionConfig>>(sa.items);
      if (ctx.bindSessionActionsEditState) {
        ctx.bindSessionActionsEditState(state);
      }
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

        Label* summaryLabel = nullptr;
        auto summary = ui::label({
            .out = &summaryLabel,
            .text = sessionActionRowSummary(kindOptions, (*state)[idx]),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        });
        if (ctx.registerSessionActionSummaryLabel) {
          ctx.registerSessionActionSummaryLabel(idx, summaryLabel);
        }
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
            .onChange =
                [state, rowIndex = idx, commit](bool v) {
                  (*state)[rowIndex].enabled = v;
                  commit();
                },
            .configure = [idx](
                             Toggle& toggle
                         ) { toggle.setTabFocusKey("settings.session-actions." + std::to_string(idx) + ".enabled"); },
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
                    .action = "command",
                    .command = "notify-send 'Noctalia' 'Custom session entry'",
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
            .onChange =
                [state, rowIndex = idx, commit](bool v) {
                  (*state)[rowIndex].enabled = v;
                  commit();
                },
            .configure = [idx](
                             Toggle& toggle
                         ) { toggle.setTabFocusKey("settings.idle.behavior." + std::to_string(idx) + ".enabled"); },
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

    const auto makeNotificationFiltersInlineBlock = [&](Flex& section, const SettingEntry& entry,
                                                        const NotificationFiltersSetting& filters) {
      const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

      auto block = makeCollectionBlock(entry, overridden);

      auto state = std::make_shared<std::vector<NotificationFilterConfig>>(filters.items);
      normalizeNotificationFilterNames(*state);
      const auto commit = [setOverride = ctx.setOverride, path = entry.path, state, req = ctx.requestContentRebuild]() {
        normalizeNotificationFilterNames(*state);
        setOverride(path, *state);
        req();
      };

      for (std::size_t idx = 0; idx < state->size(); ++idx) {
        auto row = ui::row({
            .align = FlexAlign::Center,
            .justify = FlexJustify::SpaceBetween,
            .gap = Style::spaceSm * scale,
            .minHeight = Style::controlHeightSm * scale,
        });

        auto summary = ui::label({
            .text = notificationFilterRowSummary((*state)[idx]),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        });
        row->addChild(std::move(summary));

        auto entrySettings = ui::button({
            .glyph = "settings",
            .glyphSize = Style::fontSizeCaption * scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = Style::controlHeightSm * scale,
            .minHeight = Style::controlHeightSm * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [openEntry = ctx.openNotificationFilterEntryEditor, rowIndex = idx]() {
              if (openEntry) {
                openEntry(rowIndex);
              }
            },
        });
        row->addChild(std::move(entrySettings));

        auto enabledToggle = ui::toggle({
            .checked = (*state)[idx].enabled,
            .scale = scale,
            .onChange =
                [state, rowIndex = idx, commit](bool v) {
                  (*state)[rowIndex].enabled = v;
                  commit();
                },
            .configure =
                [idx](Toggle& toggle) {
                  toggle.setTabFocusKey("settings.notifications.filter." + std::to_string(idx) + ".enabled");
                },
        });
        row->addChild(std::move(enabledToggle));

        block->addChild(std::move(row));
      }

      auto addBtn = ui::button({
          .text = i18n::tr("settings.notifications.filter.add"),
          .glyph = "add",
          .fontSize = Style::fontSizeBody * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [openCreate = ctx.openNotificationFilterCreateEditor]() {
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
                  control.valueSuffix, control.linkedCommit, control.invertSlot, control.invertEnabled
              );
            } else if constexpr (std::is_same_v<T, RangeSliderSetting>) {
              return makeRangeSlider(control, entry.path);
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
            } else if constexpr (std::is_same_v<T, NotificationFiltersSetting>) {
              return nullptr;
            } else if constexpr (std::is_same_v<T, ButtonSetting>) {
              if (control.glyph.empty()) {
                return ui::button({
                    .text = control.label,
                    .fontSize = Style::fontSizeBody * scale,
                    .variant = ButtonVariant::Default,
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
                  .variant = ButtonVariant::Default,
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
    constexpr std::size_t kKeybindsPerRow = 2;
    Flex* activeKeybindRow = nullptr;
    std::size_t activeKeybindRowCount = 0;
    std::size_t visibleEntries = 0;
    // Very short queries (one or two letters) match hundreds of entries and building the subtree for every match is
    // costly. Cap how many results we render and hint that the list was truncated.
    constexpr std::size_t kMaxSearchResults = 50;
    bool truncated = false;
    const std::string normalizedSearchQuery = normalizedSettingQuery(ctx.searchQuery);

    BarWidgetEditorContext barWidgetEditorCtx = makeBarWidgetEditorContext(factory);

    auto isEntryVisible = [&](const SettingEntry& e) -> bool { return !e.visibleWhen || e.visibleWhen(ctx.config); };

    const std::string_view selectedBarName =
        ctx.selectedBar != nullptr ? std::string_view{ctx.selectedBar->name} : std::string_view{};
    const std::string_view selectedMonitorMatch = ctx.selectedMonitorOverride != nullptr
        ? std::string_view{ctx.selectedMonitorOverride->match}
        : std::string_view{};
    const std::optional<SettingsSection> selectedSettingsSection =
        ctx.selectedSection != "bar" ? settingsSectionFromId(ctx.selectedSection) : std::nullopt;

    // Coalesce entries by (content section, group) so each group renders once even if its entries were
    // declared non-contiguously in the registry. See coalesceByGroupKey().
    const auto entryOrder = coalesceByGroupKey(registry.size(), [&](std::size_t i) {
      return barSettingContentSectionKey(registry[i]) + '\x1f' + registry[i].group;
    });

    for (const std::size_t entryIndex : entryOrder) {
      const auto& entry = registry[entryIndex];
      if (ctx.searchQuery.empty()
          && !ctx.selectedSection.empty()
          && ctx.selectedSection != "bar"
          && (!selectedSettingsSection.has_value() || entry.section != *selectedSettingsSection)) {
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
          && !settingEntryHasEffectiveOverride(entry, *ctx.configService)) {
        continue;
      }
      if (!matchesNormalizedSettingQuery(entry, normalizedSearchQuery)) {
        continue;
      }
      // Cap only once a genuinely-matching entry is about to be rendered, so the truncation hint never
      // shows when exactly kMaxSearchResults entries matched and the remainder were filtered out anyway.
      if (!ctx.searchQuery.empty() && visibleEntries >= kMaxSearchResults) {
        truncated = true;
        break;
      }

      const std::string contentSectionKey = barSettingContentSectionKey(entry);
      if (contentSectionKey != activeSectionKey) {
        activeSectionKey = contentSectionKey;
        activeGroupKey.clear();
        activeKeybindRow = nullptr;
        activeKeybindRowCount = 0;
        std::string displayTitle;
        if (entry.section == SettingsSection::Bar && entry.path.size() >= 2) {
          displayTitle = i18n::tr("settings.entities.bar.label", "name", entry.path[1]);
          if (isBarMonitorOverrideSettingPath(entry.path)) {
            displayTitle += " / " + entry.path[3];
          }
        } else {
          displayTitle = sectionLabel(entry.section);
        }
        activeSection = makeSection(displayTitle, entry.section);
      }
      if (activeSection != nullptr) {
        if (entry.group != activeGroupKey) {
          const bool isFirstGroup = activeGroupKey.empty();
          activeGroupKey = entry.group;
          activeKeybindRow = nullptr;
          activeKeybindRowCount = 0;
          addGroupLabel(*activeSection, groupLabel(entry.group), isFirstGroup);
          if (entry.section == SettingsSection::Power && entry.group == "idle") {
            addIdleLiveStatusPanel(*activeSection, ctx, scale);
          }
        }
        if (!std::holds_alternative<KeybindListSetting>(entry.control)) {
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
        } else if (const auto* filters = std::get_if<NotificationFiltersSetting>(&entry.control)) {
          makeNotificationFiltersInlineBlock(*activeSection, entry, *filters);
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

    if (activeKeybindRow != nullptr && activeKeybindRowCount > 0 && activeKeybindRowCount < kKeybindsPerRow) {
      while (activeKeybindRowCount < kKeybindsPerRow) {
        activeKeybindRow->addChild(ui::row({.fillWidth = true, .flexGrow = 1.0f}));
        ++activeKeybindRowCount;
      }
    }

    // The Plugins section has no registry entries — it renders fully custom
    // content (addSettingsPlugins), so suppress the "no settings found" state.
    if (visibleEntries == 0 && ctx.selectedSection != "plugins") {
      auto emptyState = ui::column(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .gap = Style::spaceSm * scale,
           .padding = (Style::spaceLg * 2.0f) * scale,
           .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.24f),
           .radius = Style::scaledRadiusMd(scale),
           .border = colorSpecFromRole(ColorRole::Outline),
           .minWidth = 360.0f * scale,
           .minHeight = 160.0f * scale,
           .fillWidth = true,
           .flexGrow = 2.0f},
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

    if (truncated) {
      content.addChild(
          ui::row(
              {.align = FlexAlign::Center, .justify = FlexJustify::Center, .fillWidth = true},
              makeLabel(
                  i18n::tr("settings.window.search-truncated", "count", std::to_string(kMaxSearchResults)),
                  Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
              )
          )
      );
    }

    return visibleEntries;
  }

} // namespace settings
