#include "ui/builders.h"

#include "ui/controls/scroll_view.h"

#include <utility>

namespace ui {

  namespace {

    template <typename Control, typename Props> void applyNodeProps(Control& control, const Props& props) {
      if (props.width.has_value() || props.height.has_value()) {
        control.setSize(props.width.value_or(control.width()), props.height.value_or(control.height()));
      }
      if (props.flexGrow.has_value()) {
        control.setFlexGrow(*props.flexGrow);
      }
      if (props.opacity.has_value()) {
        control.setOpacity(*props.opacity);
      }
      if (props.visible.has_value()) {
        control.setVisible(*props.visible);
      }
      if (props.participatesInLayout.has_value()) {
        control.setParticipatesInLayout(*props.participatesInLayout);
      }
    }

    template <typename Props> void applyFlexProps(Flex& flex, FlexDirection direction, const Props& props) {
      flex.setDirection(direction);
      if (props.align.has_value()) {
        flex.setAlign(*props.align);
      }
      if (props.justify.has_value()) {
        flex.setJustify(*props.justify);
      }
      if (props.gap.has_value()) {
        flex.setGap(*props.gap);
      }
      if (props.padding.has_value()) {
        flex.setPadding(*props.padding);
      }
      if (props.minWidth.has_value()) {
        flex.setMinWidth(*props.minWidth);
      }
      if (props.minHeight.has_value()) {
        flex.setMinHeight(*props.minHeight);
      }
      if (props.maxWidth.has_value()) {
        flex.setMaxWidth(*props.maxWidth);
      }
      if (props.maxHeight.has_value()) {
        flex.setMaxHeight(*props.maxHeight);
      }
      if (props.widthPolicy.has_value()) {
        flex.setWidthPolicy(*props.widthPolicy);
      }
      if (props.heightPolicy.has_value()) {
        flex.setHeightPolicy(*props.heightPolicy);
      }
      if (props.fillWidth.has_value()) {
        flex.setFillWidth(*props.fillWidth);
      }
      if (props.fillHeight.has_value()) {
        flex.setFillHeight(*props.fillHeight);
      }
      if (props.clipChildren.has_value()) {
        flex.setClipChildren(*props.clipChildren);
      }
      applyNodeProps(flex, props);
    }

  } // namespace

  std::unique_ptr<Flex> makeFlex(FlexDirection direction, FlexProps props) {
    auto flex = std::make_unique<Flex>();
    applyFlexProps(*flex, direction, props);
    if (props.configure) {
      props.configure(*flex);
    }
    if (props.out != nullptr) {
      *props.out = flex.get();
    }
    return flex;
  }

  std::unique_ptr<Input> input(InputProps props) {
    auto control = std::make_unique<Input>();
    if (props.value.has_value()) {
      control->setValue(*props.value);
    }
    if (props.placeholder.has_value()) {
      control->setPlaceholder(*props.placeholder);
    }
    if (props.fontSize.has_value()) {
      control->setFontSize(*props.fontSize);
    }
    if (props.controlHeight.has_value()) {
      control->setControlHeight(*props.controlHeight);
    }
    if (props.horizontalPadding.has_value()) {
      control->setHorizontalPadding(*props.horizontalPadding);
    }
    if (props.clearButtonEnabled.has_value()) {
      control->setClearButtonEnabled(*props.clearButtonEnabled);
    }
    if (props.passwordMode.has_value()) {
      control->setPasswordMode(*props.passwordMode);
    }
    if (props.invalid.has_value()) {
      control->setInvalid(*props.invalid);
    }
    if (props.frameVisible.has_value()) {
      control->setFrameVisible(*props.frameVisible);
    }
    if (props.embeddedOnSolidPrimary.has_value()) {
      control->setEmbeddedOnSolidPrimary(*props.embeddedOnSolidPrimary);
    }
    if (props.fontWeight.has_value()) {
      control->setFontWeight(*props.fontWeight);
    }
    if (props.minLayoutWidth.has_value()) {
      control->setMinLayoutWidth(*props.minLayoutWidth);
    }
    if (props.textAlign.has_value()) {
      control->setTextAlign(*props.textAlign);
    }
    if (props.onChange) {
      control->setOnChange(std::move(props.onChange));
    }
    if (props.onSubmit) {
      control->setOnSubmit(std::move(props.onSubmit));
    }
    if (props.onKeyEvent) {
      control->setOnKeyEvent(std::move(props.onKeyEvent));
    }
    if (props.onFocusLoss) {
      control->setOnFocusLoss(std::move(props.onFocusLoss));
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Button> button(ButtonProps props) {
    auto control = std::make_unique<Button>();
    if (props.text.has_value()) {
      control->setText(*props.text);
    }
    if (props.glyph.has_value()) {
      control->setGlyph(*props.glyph);
    }
    if (props.fontSize.has_value()) {
      control->setFontSize(*props.fontSize);
    }
    if (props.glyphSize.has_value()) {
      control->setGlyphSize(*props.glyphSize);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.selected.has_value()) {
      control->setSelected(*props.selected);
    }
    if (props.contentAlign.has_value()) {
      control->setContentAlign(*props.contentAlign);
    }
    if (props.variant.has_value()) {
      control->setVariant(*props.variant);
    }
    if (props.customPalette.has_value()) {
      control->setCustomPalette(std::move(*props.customPalette));
    }
    if (props.onClick) {
      control->setOnClick(std::move(props.onClick));
    }
    if (props.onRightClick) {
      control->setOnRightClick(std::move(props.onRightClick));
    }
    if (props.onPress) {
      control->setOnPress(std::move(props.onPress));
    }
    if (props.onMotion) {
      control->setOnMotion(std::move(props.onMotion));
    }
    if (props.onPointerMotion) {
      control->setOnPointerMotion(std::move(props.onPointerMotion));
    }
    if (props.onEnter) {
      control->setOnEnter(std::move(props.onEnter));
    }
    if (props.onLeave) {
      control->setOnLeave(std::move(props.onLeave));
    }
    if (props.badge.has_value()) {
      control->setBadge(*props.badge);
    }
    if (props.badgeFontSize.has_value()) {
      control->setBadgeFontSize(*props.badgeFontSize);
    }
    if (props.tooltip.has_value()) {
      control->setTooltip(*props.tooltip);
    }
    if (props.minWidth.has_value()) {
      control->setMinWidth(*props.minWidth);
    }
    if (props.minHeight.has_value()) {
      control->setMinHeight(*props.minHeight);
    }
    if (props.maxWidth.has_value()) {
      control->setMaxWidth(*props.maxWidth);
    }
    if (props.maxHeight.has_value()) {
      control->setMaxHeight(*props.maxHeight);
    }
    if (props.padding.has_value() || props.paddingV.has_value() || props.paddingH.has_value() ||
        props.paddingTop.has_value() || props.paddingRight.has_value() || props.paddingBottom.has_value() ||
        props.paddingLeft.has_value()) {
      const float allPadding = props.padding.value_or(0.0f);
      const float verticalPadding = props.paddingV.value_or(allPadding);
      const float horizontalPadding = props.paddingH.value_or(allPadding);
      control->setPadding(
          props.paddingTop.value_or(verticalPadding), props.paddingRight.value_or(horizontalPadding),
          props.paddingBottom.value_or(verticalPadding), props.paddingLeft.value_or(horizontalPadding)
      );
    }
    if (props.gap.has_value()) {
      control->setGap(*props.gap);
    }
    if (props.radius.has_value()) {
      control->setRadius(*props.radius);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Label> label(LabelProps props) {
    auto control = std::make_unique<Label>();
    if (props.text.has_value()) {
      control->setText(*props.text);
    }
    if (props.fontSize.has_value()) {
      control->setFontSize(*props.fontSize);
    }
    if (props.fontFamily.has_value()) {
      control->setFontFamily(std::move(*props.fontFamily));
    }
    if (props.color.has_value()) {
      control->setColor(*props.color);
    }
    if (props.minWidth.has_value()) {
      control->setMinWidth(*props.minWidth);
    }
    if (props.maxWidth.has_value()) {
      control->setMaxWidth(*props.maxWidth);
    }
    if (props.maxLines.has_value()) {
      control->setMaxLines(*props.maxLines);
    }
    if (props.fontWeight.has_value()) {
      control->setFontWeight(*props.fontWeight);
    }
    if (props.textAlign.has_value()) {
      control->setTextAlign(*props.textAlign);
    }
    if (props.baselineMode.has_value()) {
      control->setBaselineMode(*props.baselineMode);
    }
    if (props.autoScroll.has_value()) {
      control->setAutoScroll(*props.autoScroll);
    }
    if (props.autoScrollSpeed.has_value()) {
      control->setAutoScrollSpeed(*props.autoScrollSpeed);
    }
    if (props.autoScrollOnlyWhenHovered.has_value()) {
      control->setAutoScrollOnlyWhenHovered(*props.autoScrollOnlyWhenHovered);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Box> box(BoxProps props) {
    auto control = std::make_unique<Box>();
    if (props.cardStyleScale.has_value()) {
      control->setCardStyle(
          *props.cardStyleScale, props.cardStyleFillOpacity.value_or(1.0f), props.cardStyleShowBorder.value_or(true)
      );
    }
    if (props.fill.has_value()) {
      control->setFill(*props.fill);
    }
    if (props.radius.has_value()) {
      control->setRadius(*props.radius);
    }
    if (props.softness.has_value()) {
      control->setSoftness(*props.softness);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Glyph> glyph(GlyphProps props) {
    auto control = std::make_unique<Glyph>();
    if (props.glyph.has_value()) {
      (void)control->setGlyph(*props.glyph);
    }
    if (props.codepoint.has_value()) {
      (void)control->setCodepoint(*props.codepoint);
    }
    if (props.glyphSize.has_value()) {
      control->setGlyphSize(*props.glyphSize);
    }
    if (props.color.has_value()) {
      control->setColor(*props.color);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Image> image(ImageProps props) {
    auto control = std::make_unique<Image>();
    if (props.fit.has_value()) {
      control->setFit(*props.fit);
    }
    if (props.radius.has_value()) {
      control->setRadius(*props.radius);
    }
    if (props.padding.has_value()) {
      control->setPadding(*props.padding);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Separator> separator(SeparatorProps props) {
    auto control = std::make_unique<Separator>();
    if (props.color.has_value()) {
      control->setColor(*props.color);
    }
    if (props.thickness.has_value()) {
      control->setThickness(*props.thickness);
    }
    if (props.orientation.has_value()) {
      control->setOrientation(*props.orientation);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Select> select(SelectProps props) {
    auto control = std::make_unique<Select>();
    if (props.options.has_value()) {
      control->setOptions(std::move(*props.options));
    }
    if (props.selectedIndex.has_value()) {
      control->setSelectedIndex(*props.selectedIndex);
    }
    if (props.clearSelection.value_or(false)) {
      control->clearSelection();
    }
    if (props.placeholder.has_value()) {
      control->setPlaceholder(*props.placeholder);
    }
    if (props.fontSize.has_value()) {
      control->setFontSize(*props.fontSize);
    }
    if (props.controlHeight.has_value()) {
      control->setControlHeight(*props.controlHeight);
    }
    if (props.horizontalPadding.has_value()) {
      control->setHorizontalPadding(*props.horizontalPadding);
    }
    if (props.glyphSize.has_value()) {
      control->setGlyphSize(*props.glyphSize);
    }
    if (props.optionIndicators.has_value()) {
      control->setOptionIndicators(std::move(*props.optionIndicators));
    }
    if (props.colorSwatchPreviews.has_value()) {
      control->setColorSwatchPreviews(std::move(*props.colorSwatchPreviews));
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.onSelectionChanged) {
      control->setOnSelectionChanged(std::move(props.onSelectionChanged));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Slider> slider(SliderProps props) {
    auto control = std::make_unique<Slider>();
    if (props.minValue.has_value() || props.maxValue.has_value()) {
      control->setRange(props.minValue.value_or(control->minValue()), props.maxValue.value_or(control->maxValue()));
    }
    if (props.step.has_value()) {
      control->setStep(*props.step);
    }
    if (props.value.has_value()) {
      control->setValue(*props.value);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.trackHeight.has_value()) {
      control->setTrackHeight(*props.trackHeight);
    }
    if (props.thumbSize.has_value()) {
      control->setThumbSize(*props.thumbSize);
    }
    if (props.controlHeight.has_value()) {
      control->setControlHeight(*props.controlHeight);
    }
    if (props.wheelAdjustEnabled.has_value()) {
      control->setWheelAdjustEnabled(*props.wheelAdjustEnabled);
    }
    if (props.onValueChanged) {
      control->setOnValueChanged(std::move(props.onValueChanged));
    }
    if (props.onDragEnd) {
      control->setOnDragEnd(std::move(props.onDragEnd));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Segmented> segmented(SegmentedProps props) {
    auto control = std::make_unique<Segmented>();
    if (props.fontSize.has_value()) {
      control->setFontSize(*props.fontSize);
    }
    if (props.scale.has_value()) {
      control->setScale(*props.scale);
    }
    if (props.compact.has_value()) {
      control->setCompact(*props.compact);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.equalSegmentWidths.has_value()) {
      control->setEqualSegmentWidths(*props.equalSegmentWidths);
    }
    if (props.options.has_value()) {
      std::size_t index = 0;
      for (const auto& option : *props.options) {
        control->addOption(option.label, option.glyph);
        if (!option.tooltip.empty()) {
          control->setOptionTooltip(index, option.tooltip);
        }
        ++index;
      }
    }
    if (props.selectedIndex.has_value()) {
      control->setSelectedIndex(*props.selectedIndex);
    }
    if (props.onChange) {
      control->setOnChange(std::move(props.onChange));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<ScrollView> scrollView(ScrollViewProps props) {
    auto control = std::make_unique<ScrollView>();
    if (props.state != nullptr) {
      control->bindState(props.state);
    }
    if (props.scrollbarVisible.has_value()) {
      control->setScrollbarVisible(*props.scrollbarVisible);
    }
    if (props.viewportPaddingH.has_value()) {
      control->setViewportPaddingH(*props.viewportPaddingH);
    }
    if (props.viewportPaddingV.has_value()) {
      control->setViewportPaddingV(*props.viewportPaddingV);
    }
    if (props.fill.has_value()) {
      control->setFill(*props.fill);
    }
    if (props.radius.has_value()) {
      control->setRadius(*props.radius);
    }
    if (props.softness.has_value()) {
      control->setSoftness(*props.softness);
    }
    if (props.minWidth.has_value()) {
      control->setMinWidth(*props.minWidth);
    }
    if (props.minHeight.has_value()) {
      control->setMinHeight(*props.minHeight);
    }
    if (props.fillWidth.has_value()) {
      control->setFillWidth(*props.fillWidth);
    }
    if (props.fillHeight.has_value()) {
      control->setFillHeight(*props.fillHeight);
    }
    if (props.onScrollChanged) {
      control->setOnScrollChanged(std::move(props.onScrollChanged));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<VirtualGridView> virtualGridView(VirtualGridViewProps props) {
    auto control = std::make_unique<VirtualGridView>();
    if (props.columns.has_value()) {
      control->setColumns(*props.columns);
    }
    if (props.minCellWidth.has_value()) {
      control->setMinCellWidth(*props.minCellWidth);
    }
    if (props.cellHeight.has_value()) {
      control->setCellHeight(*props.cellHeight);
    }
    if (props.squareCells.has_value()) {
      control->setSquareCells(*props.squareCells);
    }
    if (props.columnGap.has_value()) {
      control->setColumnGap(*props.columnGap);
    }
    if (props.rowGap.has_value()) {
      control->setRowGap(*props.rowGap);
    }
    if (props.overscanRows.has_value()) {
      control->setOverscanRows(*props.overscanRows);
    }
    if (props.scrollbarVisible.has_value()) {
      control->scrollView().setScrollbarVisible(*props.scrollbarVisible);
    }
    if (props.scrollCardStyleScale.has_value()) {
      control->scrollView().setCardStyle(*props.scrollCardStyleScale);
    }
    if (props.adapter != nullptr) {
      control->setAdapter(props.adapter);
    }
    applyNodeProps(*control, props);
    if (props.onSelectionChanged) {
      control->setOnSelectionChanged(std::move(props.onSelectionChanged));
    }
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<VirtualListView> virtualListView(VirtualListViewProps props) {
    auto control = std::make_unique<VirtualListView>();
    if (props.itemGap.has_value()) {
      control->setItemGap(*props.itemGap);
    }
    if (props.overscanItems.has_value()) {
      control->setOverscanItems(*props.overscanItems);
    }
    if (props.adapter != nullptr) {
      control->setAdapter(props.adapter);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<SearchPicker> searchPicker(SearchPickerProps props) {
    auto control = std::make_unique<SearchPicker>();
    if (props.placeholder.has_value()) {
      control->setPlaceholder(*props.placeholder);
    }
    if (props.emptyText.has_value()) {
      control->setEmptyText(*props.emptyText);
    }
    if (props.selectedValue.has_value()) {
      control->setSelectedValue(*props.selectedValue);
    }
    if (props.options.has_value()) {
      control->setOptions(std::move(*props.options));
    }
    if (props.onActivated) {
      control->setOnActivated(std::move(props.onActivated));
    }
    if (props.onCancel) {
      control->setOnCancel(std::move(props.onCancel));
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Toggle> toggle(ToggleProps props) {
    auto control = std::make_unique<Toggle>();
    if (props.scale.has_value()) {
      control->setScale(*props.scale);
    }
    if (props.toggleSize.has_value()) {
      control->setToggleSize(*props.toggleSize);
    }
    if (props.checkedImmediate.has_value()) {
      control->setCheckedImmediate(*props.checkedImmediate);
    } else if (props.checked.has_value()) {
      control->setChecked(*props.checked);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.onChange) {
      control->setOnChange(std::move(props.onChange));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Checkbox> checkbox(CheckboxProps props) {
    auto control = std::make_unique<Checkbox>();
    if (props.checked.has_value()) {
      control->setChecked(*props.checked);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.scale.has_value()) {
      control->setScale(*props.scale);
    }
    if (props.onChange) {
      control->setOnChange(std::move(props.onChange));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<RadioButton> radioButton(RadioButtonProps props) {
    auto control = std::make_unique<RadioButton>();
    if (props.scale.has_value()) {
      control->setScale(*props.scale);
    }
    if (props.checked.has_value()) {
      control->setChecked(*props.checked);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.onChange) {
      control->setOnChange(std::move(props.onChange));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Stepper> stepper(StepperProps props) {
    auto control = std::make_unique<Stepper>();
    if (props.scale.has_value()) {
      control->setScale(*props.scale);
    }
    if (props.minValue.has_value() || props.maxValue.has_value()) {
      control->setRange(props.minValue.value_or(control->minValue()), props.maxValue.value_or(control->maxValue()));
    }
    if (props.step.has_value()) {
      control->setStep(*props.step);
    }
    if (props.valueSuffix.has_value()) {
      control->setValueSuffix(std::move(*props.valueSuffix));
    }
    if (props.value.has_value()) {
      control->setValue(*props.value);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.onValueChanged) {
      control->setOnValueChanged(std::move(props.onValueChanged));
    }
    if (props.onValueCommitted) {
      control->setOnValueCommitted(std::move(props.onValueCommitted));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<KeybindRecorder> keybindRecorder(KeybindRecorderProps props) {
    auto control = std::make_unique<KeybindRecorder>();
    if (props.scale.has_value()) {
      control->setScale(*props.scale);
    }
    if (props.chord.has_value()) {
      control->setChord(*props.chord);
    }
    if (props.unsetPlaceholder.has_value()) {
      control->setUnsetPlaceholder(*props.unsetPlaceholder);
    }
    if (props.recordingPlaceholder.has_value()) {
      control->setRecordingPlaceholder(*props.recordingPlaceholder);
    }
    if (props.modifierPolicy.has_value()) {
      control->setModifierPolicy(*props.modifierPolicy);
    }
    if (props.enabled.has_value()) {
      control->setEnabled(*props.enabled);
    }
    if (props.onCommit) {
      control->setOnCommit(std::move(props.onCommit));
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Spinner> spinner(SpinnerProps props) {
    auto control = std::make_unique<Spinner>();
    if (props.color.has_value()) {
      control->setColor(*props.color);
    }
    if (props.spinnerSize.has_value()) {
      control->setSpinnerSize(*props.spinnerSize);
    }
    if (props.thickness.has_value()) {
      control->setThickness(*props.thickness);
    }
    if (props.spinning.has_value()) {
      if (*props.spinning) {
        control->start();
      } else {
        control->stop();
      }
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Chip> chip(ChipProps props) {
    auto control = std::make_unique<Chip>();
    if (props.text.has_value()) {
      control->setText(*props.text);
    }
    if (props.active.has_value()) {
      control->setActive(*props.active);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<ProgressBar> progressBar(ProgressBarProps props) {
    auto control = std::make_unique<ProgressBar>();
    if (props.fill.has_value()) {
      control->setFill(*props.fill);
    }
    if (props.track.has_value()) {
      control->setTrack(*props.track);
    }
    if (props.radius.has_value()) {
      control->setRadius(*props.radius);
    }
    if (props.softness.has_value()) {
      control->setSoftness(*props.softness);
    }
    if (props.orientation.has_value()) {
      control->setOrientation(*props.orientation);
    }
    if (props.progress.has_value()) {
      control->setProgress(*props.progress);
    }
    applyNodeProps(*control, props);
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

  std::unique_ptr<Spacer> spacer() { return std::make_unique<Spacer>(); }

} // namespace ui
