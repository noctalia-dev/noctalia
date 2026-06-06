#include "shell/settings/settings_control_factory.h"

#include "config/config_types.h"
#include "i18n/i18n.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/list_editor.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace settings {

  SettingsControlFactory::SettingsControlFactory(SettingsContentContext ctx)
      : m_ctx(std::move(ctx)), m_scale(m_ctx.scale) {}

  bool SettingsControlFactory::isTemplateEnableTogglePath(const std::vector<std::string>& path) {
    return path == std::vector<std::string>{"theme", "templates", "enable_builtin_templates"}
    || path == std::vector<std::string>{"theme", "templates", "enable_community_templates"};
  }

  std::unique_ptr<Button> SettingsControlFactory::makeResetButton(const std::vector<std::string>& path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeStatusBadge(
      std::string_view label, const ColorSpec& fill, const ColorSpec& color, bool matchResetHeight
  ) {
    const float scale = m_scale;
    return ui::row(
        {.align = FlexAlign::Center,
         .paddingV = matchResetHeight ? Style::spaceXs * scale : 0.0f,
         .paddingH = matchResetHeight ? Style::spaceSm * scale : Style::spaceXs * scale,
         .fill = fill,
         .radius = Style::scaledRadiusSm(scale),
         .minHeight = matchResetHeight ? std::optional<float>{Style::controlHeightSm * scale} : std::nullopt},
        makeLabel(label, Style::fontSizeCaption * scale, color, FontWeight::Bold)
    );
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeOverrideBadge() {
    return makeStatusBadge(
        i18n::tr("settings.badges.override"), colorSpecFromRole(ColorRole::Primary, 0.15f),
        colorSpecFromRole(ColorRole::Primary), false
    );
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeAdvancedBadge() {
    return makeStatusBadge(
        i18n::tr("settings.badges.advanced"), colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.12f),
        colorSpecFromRole(ColorRole::OnSurfaceVariant), false
    );
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeOverrideResetActions(const std::vector<std::string>& path) {
    const float scale = m_scale;
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, makeOverrideBadge(), makeResetButton(path)
    );
  }

  void SettingsControlFactory::makeRow(Flex& section, const SettingEntry& entry, std::unique_ptr<Node> control) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const Config& cfg = m_ctx.config;
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
         .paddingV = 2.0f * scale,
         .paddingH = 0.0f,
         .minHeight = Style::controlHeight * scale},
        std::move(copy), std::move(actions)
    );

    section.addChild(std::move(row));
  }

  std::unique_ptr<Toggle> SettingsControlFactory::makeToggle(
      bool checked, bool enabled, std::vector<std::string> path, std::optional<bool> clearWhenValue
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
  }

  std::unique_ptr<Node>
  SettingsControlFactory::makeSelect(const SelectSetting& setting, std::vector<std::string> path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
          .onChange = [setOverride = ctx.setOverride, clearOverride = ctx.clearOverride, path, options,
                       integerValue](std::size_t index) {
            if (index < options.size()) {
              if (options[index].value.empty() && integerValue) {
                clearOverride(path);
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
        .onSelectionChanged = [clearOverride = ctx.clearOverride, setOverride = ctx.setOverride, path, options,
                               clearOnEmpty, integerValue](std::size_t index, std::string_view /*label*/) {
          if (index < options.size()) {
            if (options[index].value.empty() && (clearOnEmpty || integerValue)) {
              clearOverride(path);
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
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeSlider(
      double value, double minValue, double maxValue, double step, std::vector<std::string> path, bool integerValue,
      std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double)> linkedCommit
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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

    slider->setOnDragEnd([commit, sliderPtr]() { commit(static_cast<double>(sliderPtr->value())); });

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
  }

  std::unique_ptr<Input> SettingsControlFactory::makeText(
      const std::string& value, const std::string& placeholder, std::vector<std::string> path, float width
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
  }

  std::unique_ptr<Input>
  SettingsControlFactory::makeOptionalNumber(const OptionalNumberSetting& setting, std::vector<std::string> path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
  }

  std::unique_ptr<Stepper>
  SettingsControlFactory::makeStepper(const StepperSetting& setting, std::vector<std::string> path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
          setOverride(path, static_cast<std::int64_t>(value));
        },
    });
  }

  std::unique_ptr<Flex>
  SettingsControlFactory::makeOptionalStepper(const OptionalStepperSetting& setting, std::vector<std::string> path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
        .onChange = [setOverride = ctx.setOverride, path, currentValue](std::size_t index) {
          if (index == 0) {
            setOverride(path, std::string("auto"));
            return;
          }
          setOverride(path, static_cast<std::int64_t>(currentValue));
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
          setOverride(path, static_cast<std::int64_t>(value));
        },
    });

    wrap->addChild(std::move(segmented));
    wrap->addChild(std::move(stepper));
    return wrap;
  }

  std::unique_ptr<Node>
  SettingsControlFactory::makeColorSpecPicker(const ColorSpecPickerSetting& setting, std::vector<std::string> path) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
  }

  std::unique_ptr<Flex> SettingsControlFactory::makeCollectionBlock(
      const SettingEntry& entry, bool overridden, bool reserveTitleHeight, bool titleMaxTwoLines, bool fillWidth,
      bool flexGrow, bool compactTitleDescription
  ) {
    const float scale = m_scale;
    ui::FlexProps blockProps{
        .align = FlexAlign::Stretch,
        .paddingV = Style::spaceXs * scale,
        .paddingH = 0.0f,
        .fillWidth = fillWidth ? std::optional<bool>{true} : std::nullopt,
        .flexGrow = flexGrow ? std::optional<float>{1.0f} : std::nullopt,
    };
    if (!compactTitleDescription) {
      blockProps.gap = Style::spaceXs * scale;
    }
    auto block = ui::column(std::move(blockProps));

    auto titleRow = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .minHeight = reserveTitleHeight ? std::optional<float>{Style::controlHeightSm * scale} : std::nullopt},
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
  }

  void SettingsControlFactory::makeListBlock(Flex& section, const SettingEntry& entry, const ListSetting& list) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
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
    listEditor->setOnRemoveRequested([setOverride = ctx.setOverride, items = list.items,
                                      path = entry.path](std::size_t index) mutable {
      if (index >= items.size()) {
        return;
      }
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
      setOverride(path, items);
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
  }

  void SettingsControlFactory::makeKeyValueMapBlock(
      Flex& section, const SettingEntry& entry, const KeyValueMapSetting& map
  ) {
    auto& ctx = m_ctx;
    const float scale = m_scale;
    const bool overridden = (ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(entry.path));

    auto block = makeCollectionBlock(entry, overridden);

    const auto setAndCommit = [setOverride = ctx.setOverride, clearOverride = ctx.clearOverride,
                               requestRebuild = ctx.requestRebuild,
                               path = entry.path](const std::string& key, const std::string& value) {
      auto entryPath = path;
      entryPath.push_back(key);
      if (value.empty()) {
        clearOverride(entryPath);
      } else {
        setOverride(entryPath, value);
      }
      requestRebuild();
    };

    const auto removeAndCommit = [clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild,
                                  path = entry.path](const std::string& key) {
      auto entryPath = path;
      entryPath.push_back(key);
      clearOverride(entryPath);
      requestRebuild();
    };

    std::unordered_set<std::string> suggestedSet(map.suggestedKeys.begin(), map.suggestedKeys.end());

    // Suggested keys first (sorted), then user-added keys not in suggestedKeys (sorted)
    std::vector<std::string> suggested = map.suggestedKeys;
    std::sort(suggested.begin(), suggested.end());

    std::vector<std::string> userAdded;
    for (const auto& [k, v] : map.entries) {
      if (suggestedSet.find(k) == suggestedSet.end()) {
        userAdded.push_back(k);
      }
    }
    std::sort(userAdded.begin(), userAdded.end());

    const auto addSuggestedRow = [&](const std::string& key) {
      const auto it = map.entries.find(key);
      const std::string val = it != map.entries.end() ? it->second : std::string{};
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(ui::label({
          .text = key,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }));
      row->addChild(ui::label({
          .text = "→",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }));
      row->addChild(ui::input({
          .value = val,
          .placeholder = map.valuePlaceholder,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .height = Style::controlHeight * scale,
          .flexGrow = 1.0f,
          .onSubmit = [setAndCommit, key](const std::string& newVal) { setAndCommit(key, newVal); },
          .submitOnFocusLoss = true,
      }));
      auto clearButtonWidth = Style::controlHeight * scale;
      row->addChild(ui::button({
          .glyph = val.empty() ? std::string{} : std::string{"close"},
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = clearButtonWidth,
          .minHeight = clearButtonWidth,
          .onClick = [removeAndCommit, key, val]() {
            if (!val.empty()) {
              removeAndCommit(key);
            }
          },
      }));
      block->addChild(std::move(row));
    };

    const auto addUserRow = [&](const std::string& key) {
      const std::string& val = map.entries.at(key);
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(ui::input({
          .value = key,
          .placeholder = map.keyPlaceholder,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .height = Style::controlHeight * scale,
          .flexGrow = 1.0f,
          .onSubmit = [removeAndCommit, setAndCommit, val, oldKey = key](const std::string& newKey) {
            if (newKey == oldKey || newKey.empty()) {
              return;
            }
            removeAndCommit(oldKey);
            setAndCommit(newKey, val);
          },
          .submitOnFocusLoss = true,
      }));
      row->addChild(ui::label({
          .text = "→",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }));
      row->addChild(ui::input({
          .value = val,
          .placeholder = map.valuePlaceholder,
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .height = Style::controlHeight * scale,
          .flexGrow = 1.0f,
          .onSubmit = [setAndCommit, key](const std::string& newVal) { setAndCommit(key, newVal); },
          .submitOnFocusLoss = true,
      }));
      row->addChild(ui::button({
          .glyph = "close",
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeight * scale,
          .minHeight = Style::controlHeight * scale,
          .onClick = [removeAndCommit, key]() { removeAndCommit(key); },
      }));
      block->addChild(std::move(row));
    };

    for (const auto& key : suggested) {
      addSuggestedRow(key);
    }
    for (const auto& key : userAdded) {
      addUserRow(key);
    }

    auto addRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
    Input* newKeyPtr = nullptr;
    Input* newValPtr = nullptr;
    auto newKeyInput = ui::input({
        .out = &newKeyPtr,
        .placeholder = map.keyPlaceholder,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
        .height = Style::controlHeight * scale,
        .flexGrow = 1.0f,
    });
    auto newValInput = ui::input({
        .out = &newValPtr,
        .placeholder = map.valuePlaceholder,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceSm * scale,
        .height = Style::controlHeight * scale,
        .flexGrow = 1.0f,
    });
    addRow->addChild(std::move(newKeyInput));
    addRow->addChild(ui::label({
        .text = "→",
        .fontSize = Style::fontSizeBody * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    }));
    addRow->addChild(std::move(newValInput));
    addRow->addChild(ui::button({
        .glyph = "add",
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Ghost,
        .minWidth = Style::controlHeight * scale,
        .minHeight = Style::controlHeight * scale,
        .onClick = [newKeyPtr, newValPtr, setAndCommit]() {
          if (newKeyPtr == nullptr || newValPtr == nullptr) {
            return;
          }
          const std::string k = newKeyPtr->value();
          const std::string v = newValPtr->value();
          if (k.empty()) {
            return;
          }
          setAndCommit(k, v);
        },
    }));
    block->addChild(std::move(addRow));

    section.addChild(std::move(block));
  }

} // namespace settings
