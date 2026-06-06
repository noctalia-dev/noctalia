#pragma once

#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/keybind_recorder.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/radio_button.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/search_picker.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/controls/slider.h"
#include "ui/controls/spacer.h"
#include "ui/controls/spinner.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/controls/virtual_list_view.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui {

  // out fields receive non-owning pointers into the returned subtree.
  // They are valid only while that subtree remains alive.
  struct NodeProps {
    Node** out = nullptr;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::optional<bool> clipChildren = std::nullopt;
    std::function<void(Node&)> configure = nullptr;
  };

  struct FlexProps {
    Flex** out = nullptr;
    std::optional<FlexAlign> align = std::nullopt;
    std::optional<FlexJustify> justify = std::nullopt;
    std::optional<float> gap = std::nullopt;
    std::optional<float> padding = std::nullopt;  // uniform; overridden per-axis by paddingV/paddingH
    std::optional<float> paddingV = std::nullopt; // vertical (top+bottom)
    std::optional<float> paddingH = std::nullopt; // horizontal (left+right)
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<ColorSpec> border = std::nullopt;
    std::optional<float> borderWidth = std::nullopt; // defaults to 1.0 when `border` is set
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> minHeight = std::nullopt;
    std::optional<float> maxWidth = std::nullopt;
    std::optional<float> maxHeight = std::nullopt;
    std::optional<FlexSizePolicy> widthPolicy = std::nullopt;
    std::optional<FlexSizePolicy> heightPolicy = std::nullopt;
    std::optional<bool> fillWidth = std::nullopt;
    std::optional<bool> fillHeight = std::nullopt;
    std::optional<bool> clipChildren = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Flex&)> configure = nullptr;
  };

  struct InputProps {
    Input** out = nullptr;
    std::optional<std::string> value = std::nullopt;
    std::optional<std::string> placeholder = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<float> horizontalPadding = std::nullopt;
    std::optional<bool> clearButtonEnabled = std::nullopt;
    std::optional<bool> passwordMode = std::nullopt;
    std::optional<bool> invalid = std::nullopt;
    std::optional<bool> frameVisible = std::nullopt;
    std::optional<bool> embeddedOnSolidPrimary = std::nullopt;
    std::optional<FontWeight> fontWeight = std::nullopt;
    std::optional<float> minLayoutWidth = std::nullopt;
    std::optional<TextAlign> textAlign = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(const std::string&)> onChange = nullptr;
    std::function<void(const std::string&)> onSubmit = nullptr;
    std::function<bool(std::uint32_t, std::uint32_t)> onKeyEvent = nullptr;
    std::function<void()> onFocusLoss = nullptr;
    std::optional<bool> submitOnFocusLoss = std::nullopt;
    std::function<void(Input&)> configure = nullptr;
  };

  struct ButtonProps {
    Button** out = nullptr;
    std::optional<std::string> text = std::nullopt;
    std::optional<std::string> glyph = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> glyphSize = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<bool> selected = std::nullopt;
    std::optional<ButtonContentAlign> contentAlign = std::nullopt;
    std::optional<ButtonVariant> variant = std::nullopt;
    std::optional<Button::ButtonPalette> customPalette = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<std::string> badge = std::nullopt;
    std::optional<float> badgeFontSize = std::nullopt;
    std::optional<std::string> tooltip = std::nullopt;
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> minHeight = std::nullopt;
    std::optional<float> maxWidth = std::nullopt;
    std::optional<float> maxHeight = std::nullopt;
    std::optional<float> padding = std::nullopt;
    std::optional<float> paddingV = std::nullopt;
    std::optional<float> paddingH = std::nullopt;
    std::optional<float> paddingTop = std::nullopt;
    std::optional<float> paddingRight = std::nullopt;
    std::optional<float> paddingBottom = std::nullopt;
    std::optional<float> paddingLeft = std::nullopt;
    std::optional<float> gap = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void()> onClick = nullptr;
    std::function<void()> onRightClick = nullptr;
    std::function<void(float, float, bool)> onPress = nullptr;
    std::function<void()> onMotion = nullptr;
    std::function<void(float, float)> onPointerMotion = nullptr;
    std::function<void()> onEnter = nullptr;
    std::function<void()> onLeave = nullptr;
    std::function<void(Button&)> configure = nullptr;
  };

  struct LabelProps {
    Label** out = nullptr;
    std::optional<std::string> text = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<std::string> fontFamily = std::nullopt;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> maxWidth = std::nullopt;
    std::optional<int> maxLines = std::nullopt;
    std::optional<FontWeight> fontWeight = std::nullopt;
    std::optional<TextAlign> textAlign = std::nullopt;
    std::optional<LabelBaselineMode> baselineMode = std::nullopt;
    std::optional<bool> autoScroll = std::nullopt;
    std::optional<float> autoScrollSpeed = std::nullopt;
    std::optional<bool> autoScrollOnlyWhenHovered = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Label&)> configure = nullptr;
  };

  struct BoxProps {
    Box** out = nullptr;
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> softness = std::nullopt;
    std::optional<float> cardStyleScale = std::nullopt;
    std::optional<float> cardStyleFillOpacity = std::nullopt;
    std::optional<bool> cardStyleShowBorder = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Box&)> configure = nullptr;
  };

  struct GlyphProps {
    Glyph** out = nullptr;
    std::optional<std::string> glyph = std::nullopt;
    std::optional<char32_t> codepoint = std::nullopt;
    std::optional<float> glyphSize = std::nullopt;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Glyph&)> configure = nullptr;
  };

  struct ImageProps {
    Image** out = nullptr;
    std::optional<ImageFit> fit = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> padding = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Image&)> configure = nullptr;
  };

  struct SeparatorProps {
    Separator** out = nullptr;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> thickness = std::nullopt;
    std::optional<SeparatorOrientation> orientation = std::nullopt;
    std::optional<bool> gradientEdges = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Separator&)> configure = nullptr;
  };

  struct SelectProps {
    Select** out = nullptr;
    std::optional<std::vector<std::string>> options = std::nullopt;
    std::optional<std::size_t> selectedIndex = std::nullopt;
    std::optional<bool> clearSelection = std::nullopt;
    std::optional<std::string> placeholder = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<float> horizontalPadding = std::nullopt;
    std::optional<float> glyphSize = std::nullopt;
    std::optional<std::vector<ColorSpec>> optionIndicators = std::nullopt;
    std::optional<std::vector<ColorSwatchPreview>> colorSwatchPreviews = std::nullopt;
    std::optional<bool> notifyOnReselect = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(std::size_t, std::string_view)> onSelectionChanged = nullptr;
    std::function<void(Select&)> configure = nullptr;
  };

  struct SliderProps {
    Slider** out = nullptr;
    std::optional<double> minValue = std::nullopt;
    std::optional<double> maxValue = std::nullopt;
    std::optional<double> step = std::nullopt;
    std::optional<double> value = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> trackHeight = std::nullopt;
    std::optional<float> thumbSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<bool> wheelAdjustEnabled = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(double)> onValueChanged = nullptr;
    std::function<void()> onDragEnd = nullptr;
    std::function<void(Slider&)> configure = nullptr;
  };

  struct SegmentedOption {
    std::string label = {};
    std::string glyph = {};
    std::string tooltip = {};
  };

  struct SegmentedProps {
    Segmented** out = nullptr;
    std::optional<std::vector<SegmentedOption>> options = std::nullopt;
    std::optional<std::size_t> selectedIndex = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<bool> compact = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<bool> equalSegmentWidths = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(std::size_t)> onChange = nullptr;
    std::function<void(Segmented&)> configure = nullptr;
  };

  struct ScrollViewProps {
    ScrollView** out = nullptr;
    ScrollViewState* state = nullptr;
    std::optional<bool> scrollbarVisible = std::nullopt;
    std::optional<float> viewportPaddingH = std::nullopt;
    std::optional<float> viewportPaddingV = std::nullopt;
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> softness = std::nullopt;
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> minHeight = std::nullopt;
    std::optional<bool> fillWidth = std::nullopt;
    std::optional<bool> fillHeight = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(float)> onScrollChanged = nullptr;
    std::function<void(ScrollView&)> configure = nullptr;
  };

  struct VirtualGridViewProps {
    VirtualGridView** out = nullptr;
    std::optional<std::size_t> columns = std::nullopt;
    std::optional<float> minCellWidth = std::nullopt;
    std::optional<float> cellHeight = std::nullopt;
    std::optional<bool> squareCells = std::nullopt;
    std::optional<float> columnGap = std::nullopt;
    std::optional<float> rowGap = std::nullopt;
    std::optional<std::size_t> overscanRows = std::nullopt;
    std::optional<bool> scrollbarVisible = std::nullopt;
    std::optional<float> scrollCardStyleScale = std::nullopt;
    VirtualGridAdapter* adapter = nullptr;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(std::optional<std::size_t>)> onSelectionChanged = nullptr;
    std::function<void(VirtualGridView&)> configure = nullptr;
  };

  struct VirtualListViewProps {
    VirtualListView** out = nullptr;
    std::optional<float> itemGap = std::nullopt;
    std::optional<std::size_t> overscanItems = std::nullopt;
    VirtualListAdapter* adapter = nullptr;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(VirtualListView&)> configure = nullptr;
  };

  struct SearchPickerProps {
    SearchPicker** out = nullptr;
    std::optional<std::string> placeholder = std::nullopt;
    std::optional<std::string> emptyText = std::nullopt;
    std::optional<std::string> selectedValue = std::nullopt;
    std::optional<std::vector<SearchPickerOption>> options = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(const SearchPickerOption&)> onActivated = nullptr;
    std::function<void()> onCancel = nullptr;
    std::function<void(SearchPicker&)> configure = nullptr;
  };

  struct ToggleProps {
    Toggle** out = nullptr;
    std::optional<bool> checked = std::nullopt;
    std::optional<bool> checkedImmediate = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<ToggleSize> toggleSize = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(bool)> onChange = nullptr;
    std::function<void(Toggle&)> configure = nullptr;
  };

  struct CheckboxProps {
    Checkbox** out = nullptr;
    std::optional<bool> checked = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<ColorSpec> checkedFill = std::nullopt;
    std::optional<ColorSpec> checkedBorder = std::nullopt;
    std::optional<ColorSpec> checkedGlyph = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(bool)> onChange = nullptr;
    std::function<void(Checkbox&)> configure = nullptr;
  };

  struct RadioButtonProps {
    RadioButton** out = nullptr;
    std::optional<bool> checked = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(bool)> onChange = nullptr;
    std::function<void(RadioButton&)> configure = nullptr;
  };

  struct StepperProps {
    Stepper** out = nullptr;
    std::optional<int> minValue = std::nullopt;
    std::optional<int> maxValue = std::nullopt;
    std::optional<int> step = std::nullopt;
    std::optional<int> value = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<std::string> valueSuffix = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(int)> onValueChanged = nullptr;
    std::function<void(int)> onValueCommitted = nullptr;
    std::function<void(Stepper&)> configure = nullptr;
  };

  struct KeybindRecorderProps {
    KeybindRecorder** out = nullptr;
    std::optional<KeyChord> chord = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<std::string> unsetPlaceholder = std::nullopt;
    std::optional<std::string> recordingPlaceholder = std::nullopt;
    std::optional<ModifierPolicy> modifierPolicy = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(KeyChord)> onCommit = nullptr;
    std::function<void(KeybindRecorder&)> configure = nullptr;
  };

  struct SpinnerProps {
    Spinner** out = nullptr;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> spinnerSize = std::nullopt;
    std::optional<float> thickness = std::nullopt;
    std::optional<bool> spinning = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Spinner&)> configure = nullptr;
  };

  struct ProgressBarProps {
    ProgressBar** out = nullptr;
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<ColorSpec> track = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> softness = std::nullopt;
    std::optional<ProgressBarOrientation> orientation = std::nullopt;
    std::optional<float> progress = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(ProgressBar&)> configure = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Flex> flex(FlexDirection direction, FlexProps props);
  [[nodiscard]] std::unique_ptr<Input> input(InputProps props);
  [[nodiscard]] std::unique_ptr<Button> button(ButtonProps props);
  [[nodiscard]] std::unique_ptr<Label> label(LabelProps props);
  [[nodiscard]] std::unique_ptr<Node> node(NodeProps props = {});
  [[nodiscard]] std::unique_ptr<Box> box(BoxProps props = {});
  [[nodiscard]] std::unique_ptr<Glyph> glyph(GlyphProps props = {});
  [[nodiscard]] std::unique_ptr<Image> image(ImageProps props = {});
  [[nodiscard]] std::unique_ptr<Separator> separator(SeparatorProps props = {});
  [[nodiscard]] std::unique_ptr<Select> select(SelectProps props);
  [[nodiscard]] std::unique_ptr<Slider> slider(SliderProps props);
  [[nodiscard]] std::unique_ptr<Segmented> segmented(SegmentedProps props);
  [[nodiscard]] std::unique_ptr<ScrollView> scrollView(ScrollViewProps props = {});
  [[nodiscard]] std::unique_ptr<VirtualGridView> virtualGridView(VirtualGridViewProps props);
  [[nodiscard]] std::unique_ptr<VirtualListView> virtualListView(VirtualListViewProps props);
  [[nodiscard]] std::unique_ptr<SearchPicker> searchPicker(SearchPickerProps props);
  [[nodiscard]] std::unique_ptr<Toggle> toggle(ToggleProps props);
  [[nodiscard]] std::unique_ptr<Checkbox> checkbox(CheckboxProps props);
  [[nodiscard]] std::unique_ptr<RadioButton> radioButton(RadioButtonProps props);
  [[nodiscard]] std::unique_ptr<Stepper> stepper(StepperProps props);
  [[nodiscard]] std::unique_ptr<KeybindRecorder> keybindRecorder(KeybindRecorderProps props);
  [[nodiscard]] std::unique_ptr<Spinner> spinner(SpinnerProps props = {});
  [[nodiscard]] std::unique_ptr<ProgressBar> progressBar(ProgressBarProps props = {});
  [[nodiscard]] std::unique_ptr<Spacer> spacer();

  template <typename... Children> [[nodiscard]] std::unique_ptr<Flex> row(FlexProps props, Children&&... children) {
    auto container = flex(FlexDirection::Horizontal, std::move(props));
    (container->addChild(std::forward<Children>(children)), ...);
    return container;
  }

  template <typename... Children> [[nodiscard]] std::unique_ptr<Flex> column(FlexProps props, Children&&... children) {
    auto container = flex(FlexDirection::Vertical, std::move(props));
    (container->addChild(std::forward<Children>(children)), ...);
    return container;
  }

  template <typename... Children> [[nodiscard]] std::unique_ptr<Node> node(NodeProps props, Children&&... children) {
    auto container = node(std::move(props));
    (container->addChild(std::forward<Children>(children)), ...);
    return container;
  }

} // namespace ui
