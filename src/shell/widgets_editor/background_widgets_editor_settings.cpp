#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/widget_settings_registry.h"
#include "shell/widgets_editor/background_widgets_editor.h"
#include "ui/builders.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"

#include <linux/input-event-codes.h>

namespace {

  constexpr float kInspectorWidth = 340.0f;
  constexpr float kSettingRowHeight = 34.0f;
  constexpr float kLabelWidth = 100.0f;

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
    for (const auto& cond : spec.visibleWhen->any) {
      const auto current = settingValueAsString(s, cond.key, allSpecs);
      for (const auto& val : cond.values) {
        if (val == current) {
          return true;
        }
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

  std::unique_ptr<Flex> makeRow(std::string_view labelText, std::unique_ptr<Node> control) {
    return ui::row(
        {
            .align = FlexAlign::Center,
            .justify = FlexJustify::SpaceBetween,
            .gap = Style::spaceSm,
            .minHeight = kSettingRowHeight,
            .fillWidth = true,
        },
        ui::row(
            {
                .align = FlexAlign::Center,
                .minWidth = kLabelWidth,
            },
            ui::label({
                .text = std::string(labelText),
                .fontSize = Style::fontSizeCaption,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        ),
        std::move(control)
    );
  }

  std::unique_ptr<Flex> makeToggleRow(
      std::string_view labelText, const std::string& key, bool fallback, const Settings& s,
      BackgroundWidgetsEditor* editor
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
      const Settings& s, BackgroundWidgetsEditor* editor
  ) {
    return makeRow(
        labelText,
        ui::slider({
            .minValue = minVal,
            .maxValue = maxVal,
            .step = step,
            .value = getDouble(s, key, fallback),
            .flexGrow = 1.0f,
            .onValueChanged = [editor, key](double val) { editor->applySettingChange(key, val); },
        })
    );
  }

  std::unique_ptr<Flex> makeColorSpecRow(
      std::string_view labelText, const std::string& key, std::string fallbackValue, const Settings& s,
      BackgroundWidgetsEditor* editor
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

  std::unique_ptr<Flex> makeInputRow(
      std::string_view labelText, const std::string& key, const std::string& value, const std::string& placeholder,
      BackgroundWidgetsEditor* editor
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
  makeFilePickerRow(std::string_view labelText, const std::string& key, BackgroundWidgetsEditor* editor) {
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
      BackgroundWidgetsEditor* editor
  ) {
    std::vector<std::string> labels;
    std::vector<std::string> values;
    labels.reserve(options.size());
    values.reserve(options.size());
    std::size_t selectedIndex = 0;

    for (std::size_t i = 0; i < options.size(); ++i) {
      labels.push_back(i18n::tr(options[i].labelKey));
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
      BackgroundWidgetsEditor* editor
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
            .flexGrow = 1.0f,
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
      BackgroundWidgetsEditor* editor
  ) {
    for (const auto& spec : specs) {
      if (!isSpecVisible(spec, s, specs)) {
        continue;
      }
      const auto label = i18n::tr(spec.labelKey);

      switch (spec.control) {
      case settings::WidgetControlKind::Bool: {
        const auto* defVal = std::get_if<bool>(&spec.schema.defaultValue);
        content.addChild(makeToggleRow(label, spec.schema.key, defVal != nullptr ? *defVal : false, s, editor));
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

      case settings::WidgetControlKind::Select: {
        const auto* defVal = std::get_if<std::string>(&spec.schema.defaultValue);
        const std::string fallback = defVal != nullptr ? *defVal : std::string{};
        const std::string currentValue = getStr(s, spec.schema.key, fallback);
        if (spec.segmented) {
          content.addChild(makeSegmentedRow(label, spec.schema.key, spec.options, currentValue, editor));
        } else {
          content.addChild(makeSelectRow(label, spec.schema.key, spec.options, currentValue, editor));
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

  void addSettingsSection(
      Flex& content, const std::vector<settings::WidgetSettingSpec>& specs, const Settings& s,
      BackgroundWidgetsEditor* editor, std::string_view labelKey, bool separator
  ) {
    if (!hasVisibleSpecs(specs, s)) {
      return;
    }

    addSectionHeading(content, labelKey, separator);
    addSpecSettings(content, specs, s, editor);
  }

  void addBackgroundSection(Flex& content, const Settings& s, BackgroundWidgetsEditor* editor, std::string_view type) {
    const auto specs = desktop_settings::commonDesktopWidgetSettingSpecs(type);
    addSettingsSection(content, specs, s, editor, "desktop-widgets.editor.settings.background-section", true);
  }

} // namespace

void BackgroundWidgetsEditor::applySettingChange(const std::string& key, WidgetSettingValue value) {
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

    if (view.widget != nullptr && view.widget->applySetting(key, value, state->settings, *m_renderContext)) {
      view.intrinsicWidth = std::max(1.0f, view.widget->intrinsicWidth());
      view.intrinsicHeight = std::max(1.0f, view.widget->intrinsicHeight());
      applyViewState(view, *state, false);
      updateSelectionVisuals(*surface);
      surface->surface->requestRedraw();
      return;
    }

    auto newWidget = m_factory->create(state->type, state->settings, widgetContentScale(*state));
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
    if (state->type == "audio_visualizer" || state->type == "fancy_audio_visualizer") {
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
    surface->surface->requestRedraw();
    if (key == "background") {
      requestLayout();
    }
  });
}

void BackgroundWidgetsEditor::buildInspector(
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
  addSettingsSection(
      *content, typeSpecs, selectedState.settings, this, "desktop-widgets.editor.settings.widget-section", false
  );
  addBackgroundSection(*content, selectedState.settings, this, selectedState.type);

  Flex* panelPtr = nullptr;
  Flex* handlePtr = nullptr;
  auto panel = ui::column(
      {
          .out = &panelPtr,
          .gap = 0.0f,
          .minWidth = kInspectorWidth,
          .maxWidth = kInspectorWidth,
          .configure =
              [](Flex& flex) {
                flex.setFill(colorSpecFromRole(ColorRole::Surface, 0.94f));
                flex.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
                flex.setRadius(Style::scaledRadiusXl());
                flex.setZIndex(201);
              },
      },
      ui::row(
          {
              .out = &handlePtr,
              .align = FlexAlign::Center,
              .gap = Style::spaceXs,
              .paddingV = Style::spaceXs,
              .paddingH = Style::spaceMd,
              .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.85f),
              .radius = Style::scaledRadiusLg(),
              .minHeight = Style::controlHeightSm,
              .fillWidth = true,
          },
          ui::glyph({
              .glyph = "menu-2",
              .glyphSize = 14.0f,
          }),
          ui::label({
              .text = i18n::tr("desktop-widgets.editor.settings.title"),
              .fontSize = Style::fontSizeBody,
              .fontWeight = FontWeight::Bold,
          }),
          std::move(handleArea)
      ),
      std::move(scrollView)
  );

  surface.inspector = panelPtr;
  root.addChild(std::move(panel));
  panelPtr->layout(*m_renderContext);
  handleAreaPtr->setPosition(0.0f, 0.0f);
  handleAreaPtr->setFrameSize(handlePtr->width(), handlePtr->height());

  if (!surface.inspectorPositionInitialized && surface.toolbar != nullptr) {
    surface.inspectorX = surface.toolbarX;
    surface.inspectorY = surface.toolbarY + surface.toolbar->height() + Style::spaceSm;
    surface.inspectorPositionInitialized = true;
  }
  clampInspectorPosition(surface, panelPtr->width(), panelPtr->height());
  panelPtr->setPosition(surface.inspectorX, surface.inspectorY);
}
