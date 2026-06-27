#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/editor/desktop_widgets_editor.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/slider.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <linux/input-event-codes.h>
#include <optional>

namespace {

  constexpr float kInspectorWidth = 520.0f;
  constexpr float kInspectorCloseSize = 22.0f;
  constexpr float kSettingRowHeight = 34.0f;
  constexpr float kSettingLabelFlexGrow = 3.0f;
  constexpr float kSettingControlFlexGrow = 2.0f;

  using Settings = std::unordered_map<std::string, WidgetSettingValue>;

  std::string getStr(const Settings& s, const std::string& key, const std::string& fallback = {}) {
    const auto it = s.find(key);
    if (it == s.end()) {
      return fallback;
    }
    if (const auto* v = std::get_if<std::string>(&it->second)) {
      return *v;
    }
    return fallback;
  }

  double getDouble(const Settings& s, const std::string& key, double fallback) {
    const auto it = s.find(key);
    if (it == s.end()) {
      return fallback;
    }
    if (const auto* v = std::get_if<double>(&it->second)) {
      return *v;
    }
    if (const auto* v = std::get_if<std::int64_t>(&it->second)) {
      return static_cast<double>(*v);
    }
    return fallback;
  }

  bool getBool(const Settings& s, const std::string& key, bool fallback) {
    const auto it = s.find(key);
    if (it == s.end()) {
      return fallback;
    }
    if (const auto* v = std::get_if<bool>(&it->second)) {
      return *v;
    }
    return fallback;
  }

  std::string settingValueAsString(
      const Settings& s, const std::string& key, const std::vector<settings::WidgetSettingSpec>& allSpecs
  ) {
    const auto it = s.find(key);
    if (it != s.end()) {
      if (const auto* vb = std::get_if<bool>(&it->second)) {
        return *vb ? "true" : "false";
      }
      if (const auto* vs = std::get_if<std::string>(&it->second)) {
        return *vs;
      }
    }
    for (const auto& spec : allSpecs) {
      if (spec.schema.key == key) {
        if (const auto* vb = std::get_if<bool>(&spec.schema.defaultValue)) {
          return *vb ? "true" : "false";
        }
        if (const auto* vs = std::get_if<std::string>(&spec.schema.defaultValue)) {
          return *vs;
        }
        break;
      }
    }
    return {};
  }

  bool isSpecVisible(
      const settings::WidgetSettingSpec& spec, const Settings& s,
      const std::vector<settings::WidgetSettingSpec>& allSpecs
  ) {
    if (!spec.visibleWhen.has_value()) {
      return true;
    }
    auto matches = [&](const std::string& key, const std::vector<std::string>& values) {
      const auto current = settingValueAsString(s, key, allSpecs);
      for (const auto& val : values) {
        if (val == current) {
          return true;
        }
      }
      return false;
    };
    for (const auto& cond : spec.visibleWhen->all) {
      if (!matches(cond.key, cond.values)) {
        return false;
      }
    }
    if (spec.visibleWhen->any.empty()) {
      return true;
    }
    for (const auto& cond : spec.visibleWhen->any) {
      if (matches(cond.key, cond.values)) {
        return true;
      }
    }
    return false;
  }

  bool hasVisibleSpecs(const std::vector<settings::WidgetSettingSpec>& specs, const Settings& s) {
    for (const auto& spec : specs) {
      if (isSpecVisible(spec, s, specs)) {
        return true;
      }
    }
    return false;
  }

  bool settingChangeAffectsInspectorVisibility(std::string_view type, std::string_view changedKey) {
    auto checkSpecs = [&](const std::vector<settings::WidgetSettingSpec>& specs) {
      for (const auto& spec : specs) {
        if (!spec.visibleWhen.has_value()) {
          continue;
        }
        for (const auto& cond : spec.visibleWhen->any) {
          if (cond.key == changedKey) {
            return true;
          }
        }
        for (const auto& cond : spec.visibleWhen->all) {
          if (cond.key == changedKey) {
            return true;
          }
        }
      }
      return false;
    };

    return checkSpecs(desktop_settings::desktopWidgetSettingSpecs(type))
        || checkSpecs(desktop_settings::commonDesktopWidgetSettingSpecs(type));
  }

  std::unique_ptr<Flex> makeRow(std::string_view labelText, std::unique_ptr<Node> control) {
    auto controlSlot = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::End,
        .gap = Style::spaceSm,
        .minWidth = 0.0f,
        .fillWidth = true,
        .flexGrow = kSettingControlFlexGrow,
    });
    controlSlot->addChild(std::move(control));

    return ui::row(
        {
            .align = FlexAlign::Center,
            .gap = Style::spaceMd,
            .minHeight = kSettingRowHeight,
            .fillWidth = true,
        },
        ui::label({
            .text = std::string(labelText),
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .minWidth = 0.0f,
            .textAlign = TextAlign::Start,
            .ellipsize = TextEllipsize::End,
            .flexGrow = kSettingLabelFlexGrow,
        }),
        std::move(controlSlot)
    );
  }

  std::unique_ptr<Flex> makeSliderControl(
      double value, double minVal, double maxVal, double step, bool integerValue, DesktopWidgetsEditor* editor,
      const std::string& key
  ) {
    Input* valueInputPtr = nullptr;
    auto valueInput = ui::input({
        .out = &valueInputPtr,
        .value = settings::formatSliderValue(value, integerValue),
        .fontSize = Style::fontSizeCaption,
        .controlHeight = Style::controlHeightSm,
        .horizontalPadding = Style::spaceXs,
        .textAlign = TextAlign::End,
        .width = 52.0f,
        .height = Style::controlHeightSm,
    });

    Slider* sliderPtr = nullptr;
    auto slider = ui::slider({
        .out = &sliderPtr,
        .minValue = minVal,
        .maxValue = maxVal,
        .step = step,
        .value = value,
        .trackHeight = Style::sliderTrackHeight,
        .thumbSize = Style::sliderThumbSize,
        .controlHeight = Style::controlHeightSm,
        .flexGrow = 1.0f,
        .onValueChanged = [valueInputPtr, integerValue, editor, key](double val) {
          valueInputPtr->setInvalid(false);
          valueInputPtr->setValue(settings::formatSliderValue(val, integerValue));
          if (integerValue) {
            editor->applySettingChange(key, static_cast<std::int64_t>(std::llround(val)));
          } else {
            editor->applySettingChange(key, val);
          }
        },
    });

    const auto commitInputText = [sliderPtr, valueInputPtr, minVal, maxVal, integerValue, editor,
                                  key](const std::string& text) {
      const auto parsed = settings::parseDoubleInput(text);
      if (!parsed.has_value() || *parsed < minVal || *parsed > maxVal) {
        valueInputPtr->setInvalid(true);
        return;
      }
      const double v = *parsed;
      valueInputPtr->setInvalid(false);
      sliderPtr->setValue(v);
      valueInputPtr->setValue(settings::formatSliderValue(sliderPtr->value(), integerValue));
      if (integerValue) {
        editor->applySettingChange(key, static_cast<std::int64_t>(std::llround(v)));
      } else {
        editor->applySettingChange(key, v);
      }
    };

    valueInput->setOnChange([valueInputPtr](const std::string& /*text*/) { valueInputPtr->setInvalid(false); });
    valueInput->setOnSubmit([commitInputText](const std::string& text) { commitInputText(text); });
    valueInput->setOnFocusLoss([commitInputText, valueInputPtr]() { commitInputText(valueInputPtr->value()); });

    return ui::row(
        {
            .align = FlexAlign::Center,
            .gap = Style::spaceSm,
            .fillWidth = true,
            .flexGrow = 1.0f,
        },
        std::move(slider), std::move(valueInput)
    );
  }

  std::unique_ptr<Flex> makeToggleRow(
      std::string_view labelText, const std::string& key, bool fallback, const Settings& s, DesktopWidgetsEditor* editor
  ) {
    return makeRow(
        labelText,
        ui::toggle({
            .checked = getBool(s, key, fallback),
            .onChange = [editor, key](bool checked) { editor->applySettingChange(key, checked); },
        })
    );
  }

  std::unique_ptr<Flex> makeSliderRow(
      std::string_view labelText, const std::string& key, double fallback, double minVal, double maxVal, double step,
      const Settings& s, DesktopWidgetsEditor* editor
  ) {
    return makeRow(labelText, makeSliderControl(getDouble(s, key, fallback), minVal, maxVal, step, false, editor, key));
  }

  // Integer-valued slider: commits std::int64_t so the stored value matches the
  // schema's Int type (plugin manifest int settings).
  std::unique_ptr<Flex> makeIntSliderRow(
      std::string_view labelText, const std::string& key, double fallback, double minVal, double maxVal, double step,
      const Settings& s, DesktopWidgetsEditor* editor
  ) {
    return makeRow(
        labelText,
        makeSliderControl(getDouble(s, key, fallback), minVal, maxVal, std::max(1.0, step), true, editor, key)
    );
  }

  std::unique_ptr<Flex> makeStepperRow(
      std::string_view labelText, const std::string& key, int fallback, int minVal, int maxVal, int step,
      const std::optional<std::string>& valueSuffix, const Settings& s, DesktopWidgetsEditor* editor
  ) {
    const int currentValue =
        std::clamp(static_cast<int>(std::llround(getDouble(s, key, static_cast<double>(fallback)))), minVal, maxVal);
    return makeRow(
        labelText,
        ui::stepper({
            .minValue = minVal,
            .maxValue = maxVal,
            .step = std::max(1, step),
            .value = currentValue,
            .valueSuffix = valueSuffix,
            .flexGrow = 1.0f,
            .onValueCommitted = [editor, key](int value) {
              editor->applySettingChange(key, static_cast<std::int64_t>(value));
            },
        })
    );
  }

  std::unique_ptr<Flex> makeColorSpecRow(
      std::string_view labelText, const std::string& key, std::string fallbackValue, const Settings& s,
      DesktopWidgetsEditor* editor
  ) {
    settings::ColorSpecSelectOptions options{
        .roles = {},
        .selectedValue = getStr(s, key, fallbackValue),
        .allowNone = false,
        .allowCustomColor = true,
        .noneLabel = {},
        .controlHeight = Style::controlHeightSm,
        .glyphSize = Style::fontSizeCaption,
        .flexGrow = true,
    };
    auto select = settings::makeColorSpecSelect(
        std::move(options), [editor, key](std::string value) { editor->applySettingChange(key, std::move(value)); },
        []() {}
    );
    return makeRow(labelText, std::move(select));
  }

  std::unique_ptr<Flex> makeGlyphRow(
      std::string_view labelText, const std::string& key, const std::string& value, DesktopWidgetsEditor* editor
  ) {
    auto input = ui::input({
        .value = value,
        .controlHeight = Style::controlHeightSm,
        .flexGrow = 1.0f,
        .onChange = [editor, key](const std::string& val) { editor->applySettingChange(key, val); },
    });
    auto picker = ui::button({
        .glyph = "apps",
        .glyphSize = Style::fontSizeBody,
        .variant = ButtonVariant::Outline,
        .minWidth = Style::controlHeightSm,
        .minHeight = Style::controlHeightSm,
        .paddingV = Style::spaceXs,
        .paddingH = Style::spaceSm,
        .onClick = [editor, key, currentValue = value]() {
          GlyphPickerDialogOptions options;
          if (!currentValue.empty()) {
            options.initialGlyph = currentValue;
          }
          (void)GlyphPickerDialog::open(std::move(options), [editor, key](std::optional<GlyphPickerResult> result) {
            if (!result.has_value()) {
              return;
            }
            editor->applySettingChange(key, result->name);
          });
        },
    });
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm,
        .fillWidth = true,
        .flexGrow = 1.0f,
    });
    row->addChild(std::move(input));
    row->addChild(std::move(picker));
    return makeRow(labelText, std::move(row));
  }

  std::unique_ptr<Flex> makeInputRow(
      std::string_view labelText, const std::string& key, const std::string& value, const std::string& placeholder,
      DesktopWidgetsEditor* editor
  ) {
    return makeRow(
        labelText,
        ui::input({
            .value = value,
            .placeholder = placeholder,
            .controlHeight = Style::controlHeightSm,
            .flexGrow = 1.0f,
            .onChange = [editor, key](const std::string& val) { editor->applySettingChange(key, val); },
        })
    );
  }

  std::unique_ptr<Flex>
  makeFilePickerRow(std::string_view labelText, const std::string& key, DesktopWidgetsEditor* editor) {
    return makeRow(
        labelText,
        ui::button({
            .text = i18n::tr("desktop-widgets.editor.settings.change-image"),
            .variant = ButtonVariant::Outline,
            .flexGrow = 1.0f,
            .onClick = [editor, key]() {
              FileDialogOptions options;
              options.mode = FileDialogMode::Open;
              options.title = i18n::tr("desktop-widgets.editor.dialogs.select-sticker-image");
              options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".svg", ".gif"};
              (void)FileDialog::open(std::move(options), [editor, key](std::optional<std::filesystem::path> result) {
                if (result) {
                  editor->applySettingChange(key, result->string());
                }
              });
            },
        })
    );
  }

  std::unique_ptr<Flex> makeSelectRow(
      std::string_view labelText, const std::string& key,
      const std::vector<settings::WidgetSettingSelectOption>& options, const std::string& currentValue,
      bool literalLabels, DesktopWidgetsEditor* editor
  ) {
    std::vector<std::string> labels;
    std::vector<std::string> values;
    labels.reserve(options.size());
    values.reserve(options.size());
    std::size_t selectedIndex = 0;

    for (std::size_t i = 0; i < options.size(); ++i) {
      labels.push_back(literalLabels ? options[i].labelKey : i18n::tr(options[i].labelKey));
      values.emplace_back(options[i].value);
      if (options[i].value == currentValue) {
        selectedIndex = i;
      }
    }

    return makeRow(
        labelText,
        ui::select({
            .options = std::move(labels),
            .selectedIndex = selectedIndex,
            .controlHeight = Style::controlHeightSm,
            .flexGrow = 1.0f,
            .onSelectionChanged = [editor, key, values = std::move(values)](std::size_t index, std::string_view) {
              if (index < values.size()) {
                editor->applySettingChange(key, values[index]);
              }
            },
        })
    );
  }

  std::unique_ptr<Flex> makeSegmentedRow(
      std::string_view labelText, const std::string& key,
      const std::vector<settings::WidgetSettingSelectOption>& options, const std::string& currentValue,
      DesktopWidgetsEditor* editor
  ) {
    std::vector<std::string> values;
    values.reserve(options.size());
    std::size_t selectedIndex = 0;

    std::vector<ui::SegmentedOption> segmentOptions;
    segmentOptions.reserve(options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
      segmentOptions.push_back({
          .label = i18n::tr(options[i].labelKey),
      });
      values.emplace_back(options[i].value);
      if (options[i].value == currentValue) {
        selectedIndex = i;
      }
    }
    return makeRow(
        labelText,
        ui::segmented({
            .options = std::move(segmentOptions),
            .selectedIndex = selectedIndex,
            .onChange = [editor, key, values = std::move(values)](std::size_t index) {
              if (index < values.size()) {
                editor->applySettingChange(key, values[index]);
              }
            },
        })
    );
  }

  void addSpecSettings(
      Flex& content, const std::vector<settings::WidgetSettingSpec>& specs, const Settings& s,
      DesktopWidgetsEditor* editor
  ) {
    for (const auto& spec : specs) {
      if (!isSpecVisible(spec, s, specs)) {
        continue;
      }
      // Plugin manifest specs carry literal labels; built-in specs carry i18n keys.
      const auto label = !spec.literalLabel.empty() ? spec.literalLabel : i18n::tr(spec.labelKey);

      switch (spec.control) {
      case settings::WidgetControlKind::Bool: {
        const auto* defVal = std::get_if<bool>(&spec.schema.defaultValue);
        content.addChild(makeToggleRow(label, spec.schema.key, defVal != nullptr ? *defVal : false, s, editor));
        break;
      }

      case settings::WidgetControlKind::Int: {
        const auto* defVal = std::get_if<std::int64_t>(&spec.schema.defaultValue);
        const int fallback = static_cast<int>(defVal != nullptr ? *defVal : 0);
        const int minVal = static_cast<int>(std::lround(spec.schema.minValue.value_or(0.0)));
        const int maxVal = static_cast<int>(std::lround(spec.schema.maxValue.value_or(std::max(fallback, 100))));
        const int step = static_cast<int>(std::max(1.0, spec.schema.step.value_or(1.0)));
        if (spec.stepper) {
          const std::optional<std::string> suffix =
              spec.valueSuffix.empty() ? std::nullopt : std::optional<std::string>{spec.valueSuffix};
          content.addChild(makeStepperRow(label, spec.schema.key, fallback, minVal, maxVal, step, suffix, s, editor));
        } else {
          content.addChild(
              makeIntSliderRow(label, spec.schema.key, static_cast<double>(fallback), minVal, maxVal, step, s, editor)
          );
        }
        break;
      }

      case settings::WidgetControlKind::Double: {
        const auto* defVal = std::get_if<double>(&spec.schema.defaultValue);
        const double fallback = defVal != nullptr ? *defVal : 0.0;
        const double minVal = spec.schema.minValue.value_or(0.0);
        const double maxVal = spec.schema.maxValue.value_or(1.0);
        content.addChild(
            makeSliderRow(label, spec.schema.key, fallback, minVal, maxVal, spec.schema.step.value_or(1.0), s, editor)
        );
        break;
      }

      case settings::WidgetControlKind::String: {
        const auto* defVal = std::get_if<std::string>(&spec.schema.defaultValue);
        const std::string fallback = defVal != nullptr ? *defVal : std::string{};
        if (spec.schema.key == "image_path") {
          content.addChild(makeFilePickerRow(label, spec.schema.key, editor));
        } else {
          content.addChild(
              makeInputRow(label, spec.schema.key, getStr(s, spec.schema.key, fallback), fallback, editor)
          );
        }
        break;
      }

      case settings::WidgetControlKind::Glyph: {
        const auto* defVal = std::get_if<std::string>(&spec.schema.defaultValue);
        const std::string fallback = defVal != nullptr ? *defVal : std::string{};
        content.addChild(makeGlyphRow(label, spec.schema.key, getStr(s, spec.schema.key, fallback), editor));
        break;
      }

      case settings::WidgetControlKind::Select: {
        const auto* defVal = std::get_if<std::string>(&spec.schema.defaultValue);
        const std::string fallback = defVal != nullptr ? *defVal : std::string{};
        const std::string currentValue = getStr(s, spec.schema.key, fallback);
        if (spec.segmented) {
          content.addChild(makeSegmentedRow(label, spec.schema.key, spec.options, currentValue, editor));
        } else {
          content.addChild(
              makeSelectRow(label, spec.schema.key, spec.options, currentValue, spec.literalLabels, editor)
          );
        }
        break;
      }

      case settings::WidgetControlKind::ColorSpec: {
        const auto* defVal = std::get_if<std::string>(&spec.schema.defaultValue);
        content.addChild(
            makeColorSpecRow(label, spec.schema.key, defVal != nullptr ? *defVal : std::string{}, s, editor)
        );
        break;
      }

      default:
        break;
      }
    }
  }

  void addSectionHeading(Flex& content, std::string_view labelKey, bool separator) {
    if (separator) {
      content.addChild(
          ui::separator({
              .orientation = SeparatorOrientation::HorizontalRule,
          })
      );
    }

    content.addChild(
        ui::label({
            .text = i18n::tr(labelKey),
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::Secondary),
            .fontWeight = FontWeight::Bold,
        })
    );
  }

  std::unique_ptr<Flex> makeResetDefaultsRow(DesktopWidgetsEditor* editor, std::function<void()> onReset) {
    return ui::row(
        {
            .justify = FlexJustify::End,
            .paddingV = Style::spaceXs,
            .fillWidth = true,
        },
        ui::button({
            .text = i18n::tr("desktop-widgets.editor.settings.reset-defaults"),
            .variant = ButtonVariant::Ghost,
            .onClick = [editor, onReset = std::move(onReset)]() {
              if (editor != nullptr) {
                onReset();
              }
            },
        })
    );
  }

  void addSettingsSection(
      Flex& content, const std::vector<settings::WidgetSettingSpec>& specs, const Settings& s,
      DesktopWidgetsEditor* editor, std::string_view labelKey, bool separator
  ) {
    if (!hasVisibleSpecs(specs, s)) {
      return;
    }

    addSectionHeading(content, labelKey, separator);
    addSpecSettings(content, specs, s, editor);
  }

  void addBackgroundSection(Flex& content, const Settings& s, DesktopWidgetsEditor* editor, std::string_view type) {
    const auto specs = desktop_settings::commonDesktopWidgetSettingSpecs(type);
    addSettingsSection(content, specs, s, editor, "desktop-widgets.editor.settings.background-section", true);
  }

} // namespace

void DesktopWidgetsEditor::applySettingChange(const std::string& key, WidgetSettingValue value) {
  deferEditorMutation([this, key, value = std::move(value)]() {
    auto* state = findWidgetState(m_selectedWidgetId);
    if (state == nullptr) {
      return;
    }
    state->settings[key] = value;

    OverlaySurface* surface = findSurfaceForWidget(m_selectedWidgetId);
    if (surface == nullptr) {
      return;
    }
    auto viewIt = surface->views.find(m_selectedWidgetId);
    if (viewIt == surface->views.end()) {
      return;
    }

    auto& view = viewIt->second;
    if (view.transformNode == nullptr) {
      return;
    }

    const bool rebuildInspector = settingChangeAffectsInspectorVisibility(state->type, key) || key == "background";

    if (view.widget != nullptr && view.widget->applySetting(key, value, state->settings, *m_renderContext)) {
      applyViewState(view, *state, true);
      updateSelectionVisuals(*surface);
      if (rebuildInspector) {
        requestLayout();
      } else if (surface->surface != nullptr) {
        surface->surface->requestLayout();
        surface->surface->requestRedraw();
      }
      return;
    }

    auto newWidget = m_factory->create(state->type, state->settings, widgetContentScale());
    if (newWidget == nullptr) {
      return;
    }

    if (view.widget != nullptr) {
      const auto& children = view.transformNode->children();
      for (const auto& child : children) {
        view.transformNode->removeChild(child.get());
        break;
      }
    }

    newWidget->create();
    if (state->type == "audio_visualizer" || state->type == "fancy_audio_visualizer" || state->type == "button") {
      newWidget->setEditorPreview(true);
    }
    newWidget->setAnimationManager(&surface->animations);
    auto* surfacePtr = surface;
    newWidget->setUpdateCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestUpdateOnly();
      }
    });
    newWidget->setLayoutCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestUpdate();
      }
    });
    newWidget->setRedrawCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestRedraw();
      }
    });
    newWidget->setFrameTickRequestCallback([surfacePtr]() {
      if (surfacePtr->surface != nullptr) {
        surfacePtr->surface->requestFrameTick();
      }
    });
    newWidget->setBox(state->boxWidth, state->boxHeight);
    newWidget->update(*m_renderContext);
    newWidget->layout(*m_renderContext);

    view.intrinsicWidth = std::max(1.0f, newWidget->intrinsicWidth());
    view.intrinsicHeight = std::max(1.0f, newWidget->intrinsicHeight());
    view.transformNode->addChild(newWidget->releaseRoot());
    view.widget = std::move(newWidget);

    applyViewState(view, *state, false);
    if ((state->type == "audio_visualizer" || state->type == "fancy_audio_visualizer") && surface->surface != nullptr) {
      surface->surface->requestFrameTick();
    }
    updateSelectionVisuals(*surface);
    if (rebuildInspector) {
      requestLayout();
    } else if (surface->surface != nullptr) {
      surface->surface->requestRedraw();
    }
  });
}

void DesktopWidgetsEditor::resetSelectedWidgetSettings() {
  deferEditorMutation([this]() {
    auto* state = findWidgetState(m_selectedWidgetId);
    if (state == nullptr) {
      return;
    }

    desktop_settings::applyAllDesktopWidgetDefaultSettings(state->settings, state->type);
    if (lockscreen_login_box::isLoginBoxWidget(*state)) {
      lockscreen_login_box::normalizeSettings(state->settings);
    }
    requestLayout();
  });
}

void DesktopWidgetsEditor::buildInspector(
    OverlaySurface& surface, Node& root, const DesktopWidgetState& selectedState
) {
  auto handleArea = std::make_unique<InputArea>();
  handleArea->setParticipatesInLayout(false);
  handleArea->setZIndex(1);
  handleArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE);
  handleArea->setOnPress([this, outputName = surface.outputName](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (data.pressed) {
      startInspectorDrag(outputName);
    } else if (m_drag.mode == DragMode::InspectorMove && m_drag.surfaceOutputName == outputName) {
      finishDrag();
    }
  });
  handleArea->setOnMotion([this, outputName = surface.outputName](const InputArea::PointerData&) {
    if (m_drag.mode == DragMode::InspectorMove && m_drag.surfaceOutputName == outputName) {
      updateDrag();
    }
  });
  auto* handleAreaPtr = handleArea.get();

  auto scrollView = ui::scrollView({
      .width = kInspectorWidth,
      .height = 0.0f,
  });

  auto* content = scrollView->content();
  content->setDirection(FlexDirection::Vertical);
  content->setGap(Style::spaceXs);
  content->setPadding(Style::spaceSm, Style::spaceMd);

  const auto typeSpecs = desktop_settings::desktopWidgetSettingSpecs(selectedState.type);
  const auto backgroundSpecs = desktop_settings::commonDesktopWidgetSettingSpecs(selectedState.type);
  addSettingsSection(
      *content, typeSpecs, selectedState.settings, this, "desktop-widgets.editor.settings.widget-section", false
  );
  addBackgroundSection(*content, selectedState.settings, this, selectedState.type);
  if (hasVisibleSpecs(typeSpecs, selectedState.settings) || hasVisibleSpecs(backgroundSpecs, selectedState.settings)) {
    content->addChild(makeResetDefaultsRow(this, [this]() { resetSelectedWidgetSettings(); }));
  }

  Flex* panelPtr = nullptr;
  Flex* handlePtr = nullptr;
  Flex* dragHandlePtr = nullptr;
  const float panelRadius = Style::scaledRadiusXl();
  auto panel = ui::column(
      {
          .out = &panelPtr,
          .align = FlexAlign::Stretch,
          .gap = 0.0f,
          .minWidth = kInspectorWidth,
          .maxWidth = kInspectorWidth,
          .clipChildren = true,
          .configure =
              [panelRadius](Flex& flex) {
                flex.setFill(colorSpecFromRole(ColorRole::Surface, 0.94f));
                flex.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
                flex.setRadius(panelRadius);
                flex.setZIndex(201);
              },
      },
      ui::row(
          {
              .out = &handlePtr,
              .align = FlexAlign::Center,
              .gap = Style::spaceSm,
              .paddingV = Style::spaceXs,
              .paddingH = Style::spaceMd,
              .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.85f),
              .minHeight = Style::controlHeightSm,
              .fillWidth = true,
              .width = kInspectorWidth,
              .configure = [panelRadius](Flex& flex) { flex.setRadii(Radii(panelRadius, panelRadius, 0.0f, 0.0f)); },
          },
          ui::row(
              {
                  .out = &dragHandlePtr,
                  .align = FlexAlign::Center,
                  .gap = Style::spaceXs,
                  .paddingV = Style::spaceXs,
                  .paddingH = Style::spaceSm,
                  .minHeight = Style::controlHeightSm,
              },
              ui::glyph({
                  .glyph = "menu-2",
                  .glyphSize = 14.0f,
              }),
              ui::label({
                  .text = desktop_settings::desktopWidgetTypeLabel(selectedState.type),
                  .fontSize = Style::fontSizeBody,
                  .fontWeight = FontWeight::Bold,
              }),
              std::move(handleArea)
          ),
          ui::spacer(),
          ui::button({
              .glyph = "close",
              .glyphSize = 12.0f,
              .controlHeight = kInspectorCloseSize,
              .variant = ButtonVariant::Ghost,
              .minWidth = kInspectorCloseSize,
              .maxWidth = kInspectorCloseSize,
              .padding = 2.0f,
              .radius = Style::scaledRadiusSm(),
              .width = kInspectorCloseSize,
              .height = kInspectorCloseSize,
              .onClick =
                  [this]() {
                    deferEditorMutation([this]() {
                      m_inspectorOpen = false;
                      requestLayout();
                    });
                  },
          })
      ),
      std::move(scrollView)
  );

  surface.inspector = panelPtr;
  root.addChild(std::move(panel));
  panelPtr->layout(*m_renderContext);
  handleAreaPtr->setPosition(0.0f, 0.0f);
  handleAreaPtr->setFrameSize(dragHandlePtr->width(), dragHandlePtr->height());

  if (!surface.inspectorPositionInitialized && surface.toolbar != nullptr) {
    surface.inspectorX = surface.toolbarX;
    surface.inspectorY = surface.toolbarY + surface.toolbar->height() + Style::spaceSm;
    surface.inspectorPositionInitialized = true;
  }
  clampInspectorPosition(surface, panelPtr->width(), panelPtr->height());
  panelPtr->setPosition(surface.inspectorX, surface.inspectorY);
}
