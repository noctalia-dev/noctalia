#include "shell/test/test_panel.h"

#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/render_context.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/grid_tile.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/radio_button.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/spinner.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

void TestPanel::create() {
  const float scale = contentScale();
  auto rootLayout = std::make_unique<Flex>();
  rootLayout->setDirection(FlexDirection::Vertical);
  rootLayout->setGap(Style::spaceMd * scale);
  rootLayout->setAlign(FlexAlign::Stretch);

  auto headerRow = std::make_unique<Flex>();
  headerRow->setDirection(FlexDirection::Horizontal);
  headerRow->setAlign(FlexAlign::Center);
  headerRow->setJustify(FlexJustify::SpaceBetween);
  headerRow->setGap(Style::spaceSm * scale);

  auto header = std::make_unique<Label>();
  header->setText("Test");
  header->setFontWeight(FontWeight::Bold);
  header->setFontSize(Style::fontSizeTitle * scale);
  header->setColor(colorSpecFromRole(ColorRole::Primary));
  m_headerLabel = header.get();
  headerRow->addChild(std::move(header));

  auto tabSwitch = std::make_unique<Segmented>();
  tabSwitch->setScale(scale);
  tabSwitch->addOption("Controls");
  tabSwitch->addOption("Text");
  tabSwitch->setSelectedIndex(0);
  tabSwitch->setOnChange([this](std::size_t index) { selectTab(index); });
  m_tabSwitch = tabSwitch.get();

  auto headerSpacerL = std::make_unique<Flex>();
  headerSpacerL->setFlexGrow(1.0f);
  headerRow->addChild(std::move(headerSpacerL));
  headerRow->addChild(std::move(tabSwitch));
  auto headerSpacerR = std::make_unique<Flex>();
  headerSpacerR->setFlexGrow(1.0f);
  headerRow->addChild(std::move(headerSpacerR));

  auto closeButton = std::make_unique<Button>();
  closeButton->setGlyph("close");
  closeButton->setVariant(ButtonVariant::Default);
  closeButton->setGlyphSize(Style::fontSizeBody * scale);
  closeButton->setMinWidth(Style::controlHeightSm * scale);
  closeButton->setMinHeight(Style::controlHeightSm * scale);
  closeButton->setPadding(Style::spaceXs * scale);
  closeButton->setRadius(Style::scaledRadiusMd(scale));
  closeButton->setOnClick([]() { PanelManager::instance().closePanel(); });
  m_closeButton = closeButton.get();
  headerRow->addChild(std::move(closeButton));
  rootLayout->addChild(std::move(headerRow));

  auto content = std::make_unique<Flex>();
  content->setDirection(FlexDirection::Horizontal);
  content->setGap(Style::spaceLg * scale);
  content->setAlign(FlexAlign::Start);
  content->setFillWidth(true);

  auto colA = std::make_unique<Flex>();
  colA->setDirection(FlexDirection::Vertical);
  colA->setGap(Style::spaceMd * scale);
  colA->setAlign(FlexAlign::Start);
  colA->setFlexGrow(1.0f);

  auto colB = std::make_unique<Flex>();
  colB->setDirection(FlexDirection::Vertical);
  colB->setGap(Style::spaceMd * scale);
  colB->setAlign(FlexAlign::Start);
  colB->setFlexGrow(1.0f);

  auto colC = std::make_unique<Flex>();
  colC->setDirection(FlexDirection::Vertical);
  colC->setGap(Style::spaceMd * scale);
  colC->setAlign(FlexAlign::Start);
  colC->setFlexGrow(1.0f);

  auto makeRow = [scale]() {
    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setGap(Style::spaceMd * scale);
    row->setAlign(FlexAlign::Center);
    return row;
  };

  auto makeCol = [scale]() {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setGap(Style::spaceXs * scale);
    col->setAlign(FlexAlign::Start);
    return col;
  };

  // Each control sits in a small section: a caption-style title on top, then
  // the control underneath. This is more compact than the prior row-label
  // pattern and avoids the 150px gutter of empty space on the left.
  auto makeSection = [scale](const char* title) {
    auto section = std::make_unique<Flex>();
    section->setDirection(FlexDirection::Vertical);
    section->setGap(Style::spaceXs * scale);
    section->setAlign(FlexAlign::Start);
    auto label = std::make_unique<Label>();
    label->setText(title);
    label->setFontSize(Style::fontSizeCaption * scale);
    label->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    section->addChild(std::move(label));
    return section;
  };

  // ── Column A: Buttons + Icon buttons ────────────────────────────────────
  {
    struct VariantSpec {
      const char* label;
      ButtonVariant variant;
    };
    const std::vector<VariantSpec> variants = {
        {"Default", ButtonVariant::Default},     {"Primary", ButtonVariant::Primary},
        {"Secondary", ButtonVariant::Secondary}, {"Destructive", ButtonVariant::Destructive},
        {"Outline", ButtonVariant::Outline},     {"Ghost", ButtonVariant::Ghost},
    };

    auto makeVariantButton = [scale](const VariantSpec& spec, bool enabled = true) {
      auto btn = std::make_unique<Button>();
      btn->setText(spec.label);
      btn->setFontSize(Style::fontSizeBody * scale);
      btn->setVariant(spec.variant);
      btn->setMinHeight(Style::controlHeight * scale);
      btn->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
      btn->setRadius(Style::scaledRadiusMd(scale));
      btn->setOnClick([]() {});
      btn->setEnabled(enabled);
      return btn;
    };

    auto buttonsSection = makeSection("Buttons");
    auto buttonsCol = makeCol();
    buttonsCol->setGap(Style::spaceXs * scale);
    constexpr std::size_t kPerRow = 3;
    for (bool enabled : {true, false}) {
      for (std::size_t base = 0; base < variants.size(); base += kPerRow) {
        auto row = makeRow();
        for (std::size_t i = base; i < base + kPerRow && i < variants.size(); ++i) {
          row->addChild(makeVariantButton(variants[i], enabled));
        }
        buttonsCol->addChild(std::move(row));
      }
    }
    buttonsSection->addChild(std::move(buttonsCol));
    colA->addChild(std::move(buttonsSection));
  }

  {
    auto glyphTextButton = std::make_unique<Button>();
    glyphTextButton->setText("Settings");
    glyphTextButton->setGlyph("settings");
    glyphTextButton->setFontSize(Style::fontSizeBody * scale);
    glyphTextButton->setGlyphSize(Style::fontSizeBody * scale);
    glyphTextButton->setVariant(ButtonVariant::Default);
    glyphTextButton->setMinHeight(Style::controlHeight * scale);
    glyphTextButton->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    glyphTextButton->setRadius(Style::scaledRadiusMd(scale));
    glyphTextButton->setOnClick([]() {});
    m_glyphTextButton = glyphTextButton.get();

    auto glyphButton = std::make_unique<Button>();
    glyphButton->setGlyph("home");
    glyphButton->setGlyphSize(Style::fontSizeBody * scale);
    glyphButton->setVariant(ButtonVariant::Default);
    glyphButton->setMinHeight(Style::controlHeight * scale);
    glyphButton->setPadding(
        Style::spaceSm * scale, Style::spaceMd * scale, Style::spaceSm * scale, Style::spaceMd * scale
    );
    glyphButton->setRadius(Style::scaledRadiusMd(scale));
    glyphButton->setOnClick([]() {});
    m_glyphButton = glyphButton.get();

    auto section = makeSection("Icon buttons");
    auto row = makeRow();
    row->addChild(std::move(glyphTextButton));
    row->addChild(std::move(glyphButton));
    section->addChild(std::move(row));
    colA->addChild(std::move(section));
  }

  // Select
  {
    auto select = std::make_unique<Select>();
    select->setSize(220.0f * scale, 0.0f);
    select->setFontSize(Style::fontSizeBody * scale);
    select->setControlHeight(Style::controlHeight * scale);
    select->setHorizontalPadding(Style::spaceMd * scale);
    select->setGlyphSize(14.0f * scale);
    select->setOptions({"Something", "Yop", "Anything"});
    select->setSelectedIndex(0);
    m_select = select.get();

    auto section = makeSection("Select");
    section->setZIndex(10);
    section->addChild(std::move(select));
    colA->addChild(std::move(section));
  }

  // Input
  {
    auto input = std::make_unique<Input>();
    input->setPlaceholder("Type something...");
    input->setSize(220.0f * scale, 0.0f);
    input->setFontSize(Style::fontSizeBody * scale);
    input->setControlHeight(Style::controlHeight * scale);
    input->setHorizontalPadding(Style::spaceMd * scale);
    m_input = input.get();

    auto valueLabel = std::make_unique<Label>();
    valueLabel->setCaptionStyle();
    valueLabel->setFontSize(Style::fontSizeCaption * scale);
    m_inputValueLabel = valueLabel.get();

    input->setOnChange([this](const std::string& val) {
      if (m_inputValueLabel != nullptr) {
        m_inputValueLabel->setText(val.empty() ? "..." : val.substr(0, 16));
      }
    });

    auto section = makeSection("Input");
    auto row = makeRow();
    row->addChild(std::move(input));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colA->addChild(std::move(section));
  }

  // Slider
  {
    auto slider = std::make_unique<Slider>();
    slider->setRange(0.0f, 100.0f);
    slider->setStep(1.0f);
    slider->setValue(50.0f);
    slider->setSize(Style::sliderDefaultWidth * scale, 0.0f);
    slider->setControlHeight(Style::controlHeight * scale);
    slider->setTrackHeight(Style::sliderTrackHeight * scale);
    slider->setThumbSize(Style::sliderThumbSize * scale);
    slider->setOnValueChanged([this](float value) {
      if (m_sliderValueLabel != nullptr) {
        const int percent = static_cast<int>(std::round(value));
        m_sliderValueLabel->setText(std::to_string(percent) + "%");
      }
    });
    m_slider = slider.get();

    auto valueLabel = std::make_unique<Label>();
    valueLabel->setText("50%");
    valueLabel->setCaptionStyle();
    valueLabel->setFontSize(Style::fontSizeCaption * scale);
    m_sliderValueLabel = valueLabel.get();

    auto section = makeSection("Slider");
    auto row = makeRow();
    row->addChild(std::move(slider));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colA->addChild(std::move(section));
  }

  // ── Column B: Toggles, Segmented, Checkbox, Radio, Spinner ──────────────
  {
    auto toggle = std::make_unique<Toggle>();
    toggle->setToggleSize(ToggleSize::Medium);
    toggle->setScale(scale);
    toggle->setChecked(false);
    toggle->setOnChange([this](bool checked) {
      if (m_toggleValueLabel != nullptr) {
        m_toggleValueLabel->setText(checked ? "true" : "false");
      }
    });
    m_toggle = toggle.get();

    auto valueLabel = std::make_unique<Label>();
    valueLabel->setText("false");
    valueLabel->setCaptionStyle();
    valueLabel->setFontSize(Style::fontSizeCaption * scale);
    m_toggleValueLabel = valueLabel.get();

    auto section = makeSection("Toggle");
    auto row = makeRow();
    row->addChild(std::move(toggle));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colB->addChild(std::move(section));
  }

  {
    auto segmented = std::make_unique<Segmented>();
    segmented->setScale(scale);
    segmented->addOption("Light");
    segmented->addOption("Dark");
    segmented->addOption("System");
    segmented->setSelectedIndex(2);

    auto valueLabel = std::make_unique<Label>();
    valueLabel->setText("System");
    valueLabel->setCaptionStyle();
    valueLabel->setFontSize(Style::fontSizeCaption * scale);
    m_segmentedValueLabel = valueLabel.get();

    static const char* const kLabels[] = {"Light", "Dark", "System"};
    segmented->setOnChange([this](std::size_t index) {
      if (m_segmentedValueLabel != nullptr && index < std::size(kLabels)) {
        m_segmentedValueLabel->setText(kLabels[index]);
      }
    });
    m_segmented = segmented.get();

    auto section = makeSection("Segmented");
    auto row = makeRow();
    row->addChild(std::move(segmented));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colB->addChild(std::move(section));
  }

  {
    auto checkbox = ui::checkbox({
        .out = &m_checkbox,
        .checked = true,
        .scale = scale,
        .onChange = [this](bool checked) {
          if (m_checkboxValueLabel != nullptr) {
            m_checkboxValueLabel->setText(checked ? "true" : "false");
          }
        },
    });

    auto valueLabel = std::make_unique<Label>();
    valueLabel->setText("true");
    valueLabel->setCaptionStyle();
    valueLabel->setFontSize(Style::fontSizeCaption * scale);
    m_checkboxValueLabel = valueLabel.get();

    auto section = makeSection("Checkbox");
    auto row = makeRow();
    row->addChild(std::move(checkbox));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colB->addChild(std::move(section));
  }

  {
    auto makeRadioOption = [scale](const char* text, std::unique_ptr<RadioButton> radio) {
      auto opt = std::make_unique<Flex>();
      opt->setDirection(FlexDirection::Horizontal);
      opt->setAlign(FlexAlign::Center);
      opt->setGap(Style::spaceXs * scale);
      opt->addChild(std::move(radio));
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setFontSize(Style::fontSizeBody * scale);
      opt->addChild(std::move(label));
      return opt;
    };

    auto radioA = std::make_unique<RadioButton>();
    radioA->setScale(scale);
    radioA->setChecked(true);
    m_radioA = radioA.get();

    auto radioB = std::make_unique<RadioButton>();
    radioB->setScale(scale);
    m_radioB = radioB.get();

    if (m_radioA != nullptr) {
      m_radioA->setOnChange([this](bool checked) {
        if (!checked || m_radioB == nullptr) {
          return;
        }
        m_radioA->setChecked(true);
        m_radioB->setChecked(false);
      });
    }
    if (m_radioB != nullptr) {
      m_radioB->setOnChange([this](bool checked) {
        if (!checked || m_radioA == nullptr) {
          return;
        }
        m_radioA->setChecked(false);
        m_radioB->setChecked(true);
      });
    }

    auto options = std::make_unique<Flex>();
    options->setDirection(FlexDirection::Horizontal);
    options->setAlign(FlexAlign::Center);
    options->setGap(Style::spaceMd * scale);
    options->addChild(makeRadioOption("Option A", std::move(radioA)));
    options->addChild(makeRadioOption("Option B", std::move(radioB)));

    auto section = makeSection("Radio");
    section->addChild(std::move(options));
    colB->addChild(std::move(section));
  }

  {
    auto spinner = std::make_unique<Spinner>();
    spinner->setSpinnerSize(20.0f * scale);
    spinner->setThickness(2.0f * scale);
    m_spinner = spinner.get();

    auto section = makeSection("Spinner");
    section->addChild(std::move(spinner));
    colB->addChild(std::move(section));
  }

  {
    auto stepper = ui::stepper({
        .out = &m_stepper,
        .minValue = 0,
        .maxValue = 199,
        .step = 1,
        .value = 42,
        .scale = scale,
        .onValueChanged = [this](int v) {
          if (m_stepperValueLabel != nullptr) {
            m_stepperValueLabel->setText("onChange: " + std::to_string(v));
          }
        },
    });

    auto valueLabel = std::make_unique<Label>();
    valueLabel->setText("onChange: 42");
    valueLabel->setCaptionStyle();
    valueLabel->setFontSize(Style::fontSizeCaption * scale);
    m_stepperValueLabel = valueLabel.get();

    auto section = makeSection("Stepper");
    section->addChild(std::move(stepper));
    section->addChild(std::move(valueLabel));
    colB->addChild(std::move(section));
  }

  // ── Column C: File dialog, Color picker, Grid view, Transforms ──────────
  {
    auto resultLabel = std::make_unique<Label>();
    resultLabel->setText("No image selected");
    resultLabel->setCaptionStyle();
    resultLabel->setFontSize(Style::fontSizeCaption * scale);
    resultLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    resultLabel->setMaxWidth(280.0f * scale);
    m_fileDialogResultLabel = resultLabel.get();

    auto openFileDialog = std::make_unique<Button>();
    openFileDialog->setText("Browse images...");
    openFileDialog->setGlyph("image");
    openFileDialog->setFontSize(Style::fontSizeBody * scale);
    openFileDialog->setGlyphSize(Style::fontSizeBody * scale);
    openFileDialog->setVariant(ButtonVariant::Default);
    openFileDialog->setMinHeight(Style::controlHeight * scale);
    openFileDialog->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    openFileDialog->setRadius(Style::scaledRadiusMd(scale));
    openFileDialog->setOnClick([this]() {
      FileDialogOptions options;
      options.mode = FileDialogMode::Open;
      options.title = "Select Image";
      options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif"};
      (void)FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> result) {
        if (m_fileDialogResultLabel == nullptr) {
          return;
        }
        if (result.has_value()) {
          m_fileDialogResultLabel->setText(result->string());
          m_fileDialogResultLabel->setColor(colorSpecFromRole(ColorRole::Primary));
        } else {
          m_fileDialogResultLabel->setText("Cancelled");
          m_fileDialogResultLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        }
      });
    });
    m_openFileDialogButton = openFileDialog.get();

    auto section = makeSection("File dialog");
    auto row = makeRow();
    row->addChild(std::move(openFileDialog));
    row->addChild(std::move(resultLabel));
    section->addChild(std::move(row));
    colC->addChild(std::move(section));
  }

  {
    auto resultSwatch = std::make_unique<Box>();
    resultSwatch->setSize(28.0f * scale, 28.0f * scale);
    resultSwatch->setRadius(Style::scaledRadiusMd(scale));
    resultSwatch->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * scale);
    if (const auto last = ColorPickerDialog::lastResult()) {
      resultSwatch->setFill(*last);
    } else {
      resultSwatch->setFill(colorForRole(ColorRole::Primary));
    }
    m_colorPickerResultSwatch = resultSwatch.get();

    auto openPicker = std::make_unique<Button>();
    openPicker->setText("Open color picker…");
    openPicker->setFontSize(Style::fontSizeBody * scale);
    openPicker->setVariant(ButtonVariant::Default);
    openPicker->setMinHeight(Style::controlHeight * scale);
    openPicker->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    openPicker->setRadius(Style::scaledRadiusMd(scale));
    openPicker->setOnClick([this]() {
      ColorPickerDialogOptions options;
      if (const auto last = ColorPickerDialog::lastResult()) {
        options.initialColor = *last;
      }
      (void)ColorPickerDialog::open(std::move(options), [this](std::optional<Color> result) {
        if (!result.has_value() || m_colorPickerResultSwatch == nullptr) {
          return;
        }
        m_colorPickerResultSwatch->setFill(*result);
      });
    });
    m_openColorPickerButton = openPicker.get();

    auto section = makeSection("Color picker");
    auto row = makeRow();
    row->addChild(std::move(openPicker));
    row->addChild(std::move(resultSwatch));
    section->addChild(std::move(row));
    colC->addChild(std::move(section));
  }

  {
    auto resultLabel = std::make_unique<Label>();
    if (const auto last = GlyphPickerDialog::lastResult()) {
      resultLabel->setText(last->name);
      resultLabel->setColor(colorSpecFromRole(ColorRole::Primary));
    } else {
      resultLabel->setText("No glyph selected");
      resultLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    }
    resultLabel->setCaptionStyle();
    resultLabel->setFontSize(Style::fontSizeCaption * scale);
    resultLabel->setMaxWidth(280.0f * scale);
    m_glyphPickerResultLabel = resultLabel.get();

    auto openPicker = std::make_unique<Button>();
    openPicker->setText("Open glyph picker...");
    openPicker->setFontSize(Style::fontSizeBody * scale);
    openPicker->setVariant(ButtonVariant::Default);
    openPicker->setMinHeight(Style::controlHeight * scale);
    openPicker->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    openPicker->setRadius(Style::scaledRadiusMd(scale));
    openPicker->setOnClick([this]() {
      GlyphPickerDialogOptions options;
      if (const auto last = GlyphPickerDialog::lastResult()) {
        options.initialGlyph = last->name;
      }
      (void)GlyphPickerDialog::open(std::move(options), [this](std::optional<GlyphPickerResult> result) {
        if (!result.has_value()) {
          return;
        }
        if (m_glyphPickerResultLabel != nullptr) {
          m_glyphPickerResultLabel->setText(result->name);
          m_glyphPickerResultLabel->setColor(colorSpecFromRole(ColorRole::Primary));
        }
        if (m_glyphButton != nullptr) {
          m_glyphButton->setGlyph(result->name);
        }
      });
    });
    m_openGlyphPickerButton = openPicker.get();

    auto section = makeSection("Glyph picker");
    auto row = makeRow();
    row->addChild(std::move(openPicker));
    row->addChild(std::move(resultLabel));
    section->addChild(std::move(row));
    colC->addChild(std::move(section));
  }

  {
    auto grid = std::make_unique<GridView>();
    grid->setColumns(3);
    grid->setColumnGap(Style::spaceSm * scale);
    grid->setRowGap(Style::spaceSm * scale);
    grid->setPadding(Style::spaceXs * scale);
    grid->setSize(300.0f * scale, 0.0f);
    grid->setUniformCellSize(true);
    grid->setMinCellHeight(64.0f * scale);

    struct TileSpec {
      const char* glyph;
      const char* label;
      bool accent;
    };
    const std::vector<TileSpec> tiles = {
        {"home", "Home", false},         {"media-play", "Music", true},       {"copy", "Gallery", false},
        {"settings", "Settings", false}, {"weather-cloud", "Weather", false}, {"check", "Calendar", false},
    };

    for (const auto& tileData : tiles) {
      auto tile = std::make_unique<GridTile>();
      tile->setDirection(FlexDirection::Vertical);
      tile->setAlign(FlexAlign::Center);
      tile->setJustify(FlexJustify::Center);
      tile->setGap(Style::spaceXs * scale);
      tile->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
      if (tileData.accent) {
        tile->setRadius(Style::scaledRadiusMd(scale));
        tile->setFill(colorSpecFromRole(ColorRole::Primary));
        tile->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      } else {
        tile->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
        tile->setRadius(Style::scaledRadiusMd(scale));
      }

      auto icon = std::make_unique<Glyph>();
      icon->setGlyph(tileData.glyph);
      icon->setGlyphSize(16.0f * scale);
      icon->setColor(colorSpecFromRole(tileData.accent ? ColorRole::OnPrimary : ColorRole::OnSurface));
      tile->addChild(std::move(icon));

      auto label = std::make_unique<Label>();
      label->setText(tileData.label);
      label->setCaptionStyle();
      label->setFontSize(Style::fontSizeCaption * scale);
      label->setColor(colorSpecFromRole(tileData.accent ? ColorRole::OnPrimary : ColorRole::OnSurfaceVariant));
      tile->addChild(std::move(label));

      grid->addChild(std::move(tile));
    }

    auto section = makeSection("Grid view");
    section->addChild(std::move(grid));
    colC->addChild(std::move(section));
  }

  // Transforms
  {
    auto transformStage = std::make_unique<Box>();
    transformStage->setSize(280.0f * scale, 220.0f * scale);
    transformStage->setFill(colorSpecFromRole(ColorRole::Surface));
    transformStage->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * scale);
    transformStage->setRadius(Style::scaledRadiusLg(scale));
    m_transformStage = transformStage.get();

    auto demoBox = std::make_unique<Box>();
    demoBox->setSize(180.0f * scale, 100.0f * scale);
    demoBox->setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
    demoBox->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * scale);
    demoBox->setRadius(Style::scaledRadiusLg(scale));
    demoBox->setRotation(0.0f);
    m_transformDemoBox = demoBox.get();

    auto demoButton = std::make_unique<Button>();
    demoButton->setText("Click me...");
    demoButton->setGlyph("cpu-temperature");
    demoButton->setFontSize(Style::fontSizeBody * scale);
    demoButton->setGlyphSize(Style::fontSizeBody * scale);
    demoButton->setVariant(ButtonVariant::Primary);
    demoButton->setPadding(Style::spaceSm * scale, Style::spaceLg * scale);
    demoButton->setRadius(Style::scaledRadiusMd(scale));
    demoButton->setOnClick([this]() {
      if (m_transformHelp != nullptr) {
        m_transformHelp->setText("Transform button clicked!");
        m_transformHelp->setColor(colorSpecFromRole(ColorRole::Secondary));
      }
    });
    m_transformDemoButton = demoButton.get();
    m_transformDemoBox->addChild(std::move(demoButton));

    auto demoGlyph = std::make_unique<Glyph>();
    demoGlyph->setGlyph("noctalia");
    demoGlyph->setPosition(150.0f * scale, 60.0f * scale);
    demoGlyph->setGlyphSize(24.0f * scale);
    demoGlyph->setColor(colorSpecFromRole(ColorRole::Primary));
    demoGlyph->setRotation(static_cast<float>(M_PI) * 0.5f);
    m_transformDemoGlyph = demoGlyph.get();
    m_transformDemoBox->addChild(std::move(demoGlyph));

    auto badgeBox = std::make_unique<Box>();
    badgeBox->setSize(28.0f * scale, 28.0f * scale);
    badgeBox->setFill(colorSpecFromRole(ColorRole::Primary));
    badgeBox->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * scale);
    badgeBox->setRadius(14.0f * scale);
    m_transformBadgeBox = badgeBox.get();

    auto badgeLabel = std::make_unique<Label>();
    badgeLabel->setText("3");
    badgeLabel->setFontSize(Style::fontSizeCaption * scale);
    badgeLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary));
    m_transformBadgeLabel = badgeLabel.get();
    m_transformBadgeBox->addChild(std::move(badgeLabel));
    m_transformDemoBox->addChild(std::move(badgeBox));
    m_transformStage->addChild(std::move(demoBox));

    auto helpLabel = std::make_unique<Label>();
    helpLabel->setText("Rotated node with children.");
    helpLabel->setFontSize(Style::fontSizeCaption * scale);
    helpLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    m_transformHelp = helpLabel.get();

    auto section = makeSection("Transforms");
    section->addChild(std::move(helpLabel));
    section->addChild(std::move(transformStage));
    colC->addChild(std::move(section));
  }

  m_container = colA.get();
  content->addChild(std::move(colA));
  content->addChild(std::move(colB));
  content->addChild(std::move(colC));

  auto controlsTab = std::make_unique<Flex>();
  controlsTab->setDirection(FlexDirection::Vertical);
  controlsTab->setAlign(FlexAlign::Stretch);
  controlsTab->setGap(Style::spaceLg * scale);
  controlsTab->addChild(std::move(content));
  m_controlsTab = controlsTab.get();

  auto textTab = buildTextLabSection(scale);
  m_textTab = textTab.get();
  m_textTab->setVisible(false);

  auto scroll = std::make_unique<ScrollView>();
  scroll->setScrollbarVisible(true);
  scroll->setViewportPaddingH(0.0f);
  scroll->setViewportPaddingV(0.0f);
  scroll->clearFill();
  scroll->clearBorder();
  scroll->setFlexGrow(1.0f);
  m_scrollView = scroll.get();
  auto* scrollContent = scroll->content();
  scrollContent->setDirection(FlexDirection::Vertical);
  scrollContent->setAlign(FlexAlign::Stretch);
  scrollContent->setGap(Style::spaceLg * scale);
  scrollContent->addChild(std::move(controlsTab));
  scrollContent->addChild(std::move(textTab));
  rootLayout->addChild(std::move(scroll));

  setRoot(std::move(rootLayout));

  // Propagate animation manager to all controls in the tree
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  // Start spinner after animation manager is propagated
  if (m_spinner != nullptr) {
    m_spinner->start();
  }

  if (m_animations != nullptr && m_transformDemoBox != nullptr) {
    m_animations->animate(0.0f, 2.0f * static_cast<float>(M_PI), 8000.0f, Easing::Linear, [this](float phase) {
      if (m_transformDemoBox != nullptr) {
        m_transformDemoBox->setRotation(phase);
        m_transformDemoBox->setScale(1.0f + 0.16f * std::sin(phase));
      }
    });
  }
}

std::unique_ptr<Flex> TestPanel::buildTextLabSection(float scale) {
  auto section = std::make_unique<Flex>();
  section->setDirection(FlexDirection::Vertical);
  section->setAlign(FlexAlign::Stretch);
  section->setGap(Style::spaceMd * scale);

  // ── Section heading ────────────────────────────────────────────────
  {
    auto heading = std::make_unique<Label>();
    heading->setText("Text Lab");
    heading->setFontWeight(FontWeight::Bold);
    heading->setFontSize(Style::fontSizeHeader * scale);
    heading->setColor(colorSpecFromRole(ColorRole::Primary));
    section->addChild(std::move(heading));
  }

  // ── Font family controls ──────────────────────────────────────────
  {
    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setGap(Style::spaceSm * scale);

    auto familyLabel = std::make_unique<Label>();
    familyLabel->setText("Font family:");
    familyLabel->setFontSize(Style::fontSizeBody * scale);
    row->addChild(std::move(familyLabel));

    auto input = std::make_unique<Input>();
    input->setPlaceholder("e.g. sans-serif, Inter, DejaVu Sans, monospace");
    input->setSize(360.0f * scale, 0.0f);
    input->setFontSize(Style::fontSizeBody * scale);
    input->setControlHeight(Style::controlHeight * scale);
    input->setHorizontalPadding(Style::spaceMd * scale);
    input->setOnSubmit([this](const std::string& value) { applyTestFontFamily(value); });
    m_fontFamilyInput = input.get();
    row->addChild(std::move(input));

    auto applyBtn = std::make_unique<Button>();
    applyBtn->setText("Apply");
    applyBtn->setVariant(ButtonVariant::Primary);
    applyBtn->setFontSize(Style::fontSizeBody * scale);
    applyBtn->setMinHeight(Style::controlHeight * scale);
    applyBtn->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    applyBtn->setRadius(Style::scaledRadiusMd(scale));
    applyBtn->setOnClick([this]() {
      if (m_fontFamilyInput != nullptr) {
        applyTestFontFamily(m_fontFamilyInput->value());
      }
    });
    row->addChild(std::move(applyBtn));

    static const char* const kPresets[] = {"sans-serif", "serif", "monospace"};
    for (const char* preset : kPresets) {
      auto btn = std::make_unique<Button>();
      btn->setText(preset);
      btn->setVariant(ButtonVariant::Ghost);
      btn->setFontSize(Style::fontSizeCaption * scale);
      btn->setMinHeight(Style::controlHeightSm * scale);
      btn->setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
      btn->setRadius(Style::scaledRadiusMd(scale));
      btn->setOnClick([this, preset]() { applyTestFontFamily(preset); });
      row->addChild(std::move(btn));
    }

    auto status = std::make_unique<Label>();
    status->setCaptionStyle();
    status->setFontSize(Style::fontSizeCaption * scale);
    status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    status->setText("Live font swap rebuilds Pango cache.");
    m_fontStatusLabel = status.get();
    row->addChild(std::move(status));

    section->addChild(std::move(row));
  }

  // ── Sample string used in the size ladder. Mixes ascenders, descenders,
  // ── digits, punctuation, and accents to surface vertical-metric drift.
  const std::string kSample = "Sphinx of black quartz, judge my vow — Apgjy 0123 ñÅ";

  // ── Size ladder: every Style font size, regular and bold, with a
  // ── matching-size glyph next to each label. If the label and the
  // ── glyph disagree on baseline, this row will reveal it.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Size ladder (glyph + text, regular & bold)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    struct SizeSpec {
      const char* name;
      float size;
    };
    const SizeSpec sizes[] = {
        {"mini", Style::fontSizeMini},   {"caption", Style::fontSizeCaption}, {"body", Style::fontSizeBody},
        {"title", Style::fontSizeTitle}, {"header", Style::fontSizeHeader},
    };

    for (const auto& s : sizes) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(Style::spaceMd * scale);

      auto tag = std::make_unique<Label>();
      tag->setText(std::string(s.name) + " (" + std::to_string(static_cast<int>(s.size)) + ")");
      tag->setFontSize(Style::fontSizeCaption * scale);
      tag->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      tag->setMinWidth(110.0f * scale);
      row->addChild(std::move(tag));

      auto glyph = std::make_unique<Glyph>();
      glyph->setGlyph("home");
      glyph->setGlyphSize(s.size * scale);
      glyph->setColor(colorSpecFromRole(ColorRole::Primary));
      row->addChild(std::move(glyph));

      auto regular = std::make_unique<Label>();
      regular->setText(kSample);
      regular->setFontSize(s.size * scale);
      row->addChild(std::move(regular));

      auto sep = std::make_unique<Label>();
      sep->setText("|");
      sep->setFontSize(s.size * scale);
      sep->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      row->addChild(std::move(sep));

      auto bold = std::make_unique<Label>();
      bold->setText(kSample);
      bold->setFontWeight(FontWeight::Bold);
      bold->setFontSize(s.size * scale);
      row->addChild(std::move(bold));

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Glyph/text alignment matrix: same body text next to glyphs of
  // ── increasing size. Useful for spotting bar-style 1px drifts.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Glyph + body text alignment (varying glyph size)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    const float glyphSizes[] = {10.0f, 12.0f, 14.0f, 16.0f, 18.0f, 20.0f, 24.0f, 28.0f, 32.0f};
    for (float gs : glyphSizes) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(Style::spaceSm * scale);

      auto tag = std::make_unique<Label>();
      tag->setText("g" + std::to_string(static_cast<int>(gs)));
      tag->setFontSize(Style::fontSizeCaption * scale);
      tag->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      tag->setMinWidth(36.0f * scale);
      row->addChild(std::move(tag));

      auto glyph = std::make_unique<Glyph>();
      glyph->setGlyph("settings");
      glyph->setGlyphSize(gs * scale);
      glyph->setColor(colorSpecFromRole(ColorRole::Primary));
      row->addChild(std::move(glyph));

      auto label = std::make_unique<Label>();
      label->setText("Hxg Apjy 0123 — body text");
      label->setFontSize(Style::fontSizeBody * scale);
      row->addChild(std::move(label));

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Wiggle test: identical labels repeated across a row. Any vertical
  // ── jitter shows up immediately under HiDPI snapping.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Repeat-row jitter probe (identical text repeated)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    const float sizes[] = {Style::fontSizeMini, Style::fontSizeCaption, Style::fontSizeBody, Style::fontSizeTitle};
    for (float fs : sizes) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(Style::spaceSm * scale);

      auto tag = std::make_unique<Label>();
      tag->setText("fs" + std::to_string(static_cast<int>(fs)));
      tag->setFontSize(Style::fontSizeCaption * scale);
      tag->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      tag->setMinWidth(40.0f * scale);
      row->addChild(std::move(tag));

      for (int i = 0; i < 8; ++i) {
        auto lbl = std::make_unique<Label>();
        lbl->setText("Hgjy");
        lbl->setFontSize(fs * scale);
        row->addChild(std::move(lbl));
      }
      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Baseline mode test (cap-only ↔ descender swap). Latin optical mode
  // ── logical mode follows Pango metrics; ink-centered mode follows visible ink.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Baseline mode (logical vs ink-centered)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setGap(Style::spaceMd * scale);

    auto stable = std::make_unique<Label>();
    stable->setText("MAR 2025");
    stable->setFontSize(Style::fontSizeTitle * scale);
    m_baselineModeLabel = stable.get();
    row->addChild(std::move(stable));

    auto descender = std::make_unique<Label>();
    descender->setText("Apgjy");
    descender->setFontSize(Style::fontSizeTitle * scale);
    row->addChild(std::move(descender));

    auto plain = std::make_unique<Label>();
    plain->setText("MAR 2025 (ink)");
    plain->setFontSize(Style::fontSizeTitle * scale);
    plain->setBaselineMode(LabelBaselineMode::InkCentered);
    row->addChild(std::move(plain));

    auto plainDesc = std::make_unique<Label>();
    plainDesc->setText("Apgjy (ink)");
    plainDesc->setFontSize(Style::fontSizeTitle * scale);
    plainDesc->setBaselineMode(LabelBaselineMode::InkCentered);
    row->addChild(std::move(plainDesc));

    auto toggleRow = std::make_unique<Flex>();
    toggleRow->setDirection(FlexDirection::Horizontal);
    toggleRow->setAlign(FlexAlign::Center);
    toggleRow->setGap(Style::spaceSm * scale);

    auto toggleLabel = std::make_unique<Label>();
    toggleLabel->setText("first label ink centered:");
    toggleLabel->setCaptionStyle();
    toggleLabel->setFontSize(Style::fontSizeCaption * scale);
    toggleRow->addChild(std::move(toggleLabel));

    auto toggle = std::make_unique<Toggle>();
    toggle->setToggleSize(ToggleSize::Small);
    toggle->setScale(scale);
    toggle->setChecked(false);
    toggle->setOnChange([this](bool checked) {
      if (m_baselineModeLabel != nullptr) {
        m_baselineModeLabel->setBaselineMode(
            checked ? LabelBaselineMode::InkCentered : LabelBaselineMode::StableLogical
        );
      }
    });
    m_baselineModeToggle = toggle.get();
    toggleRow->addChild(std::move(toggle));

    col->addChild(std::move(row));
    col->addChild(std::move(toggleRow));

    section->addChild(std::move(col));
  }

  // ── Nerd Font / PUA glyph rendering test. These codepoints live in the
  // ── Unicode Private Use Area and require a Nerd Font for coverage.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Nerd Font symbols (requires a Nerd Font installed)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    struct NerdSpec {
      const char* codepoint;
      const char* symbol;
    };
    const NerdSpec symbols[] = {
        {"U+E612 nf-seti-folder", "\xee\x98\x92"}, {"U+E615 nf-seti-home", "\xee\x98\x95"},
        {"U+F001 nf-fa-music", "\xef\x80\x81"},    {"U+F008 nf-fa-film", "\xef\x80\x88"},
        {"U+F013 nf-fa-cog", "\xef\x80\x93"},      {"U+F015 nf-fa-home", "\xef\x80\x95"},
        {"U+F0E0 nf-fa-envelope", "\xef\x83\xa0"}, {"U+F120 nf-fa-terminal", "\xef\x84\xa0"},
        {"U+F1D3 nf-fa-git", "\xef\x87\x93"},      {"U+F268 nf-fa-chrome", "\xef\x89\xa8"},
        {"U+F308 nf-linux-tux", "\xef\x8c\x88"},   {"U+F489 nf-oct-terminal", "\xef\x92\x89"},
    };

    for (const auto& s : symbols) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(Style::spaceMd * scale);

      auto tag = std::make_unique<Label>();
      tag->setText(s.codepoint);
      tag->setFontSize(Style::fontSizeCaption * scale);
      tag->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      tag->setMinWidth(200.0f * scale);
      row->addChild(std::move(tag));

      const float sizes[] = {Style::fontSizeMini, Style::fontSizeBody, Style::fontSizeTitle, Style::fontSizeHeader};
      for (float fs : sizes) {
        auto lbl = std::make_unique<Label>();
        lbl->setText(s.symbol);
        lbl->setFontSize(fs * scale);
        lbl->setColor(colorSpecFromRole(ColorRole::OnSurface));
        row->addChild(std::move(lbl));
      }

      auto mixed = std::make_unique<Label>();
      mixed->setText(std::string(s.symbol) + " inline text");
      mixed->setFontSize(Style::fontSizeBody * scale);
      row->addChild(std::move(mixed));

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Elision and wrapping tests at body font size. Each row is
  // ── identical text inside boxes of decreasing width.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Elision (single line, decreasing maxWidth)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    const std::string longText = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor.";
    const float widths[] = {640.0f, 480.0f, 360.0f, 240.0f, 160.0f, 120.0f, 80.0f, 56.0f, 32.0f};
    for (float w : widths) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(Style::spaceSm * scale);

      auto tag = std::make_unique<Label>();
      tag->setText("w=" + std::to_string(static_cast<int>(w)));
      tag->setFontSize(Style::fontSizeCaption * scale);
      tag->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      tag->setMinWidth(56.0f * scale);
      row->addChild(std::move(tag));

      auto frame = std::make_unique<Flex>();
      frame->setDirection(FlexDirection::Horizontal);
      frame->setAlign(FlexAlign::Center);
      frame->setSize(w * scale, 0.0f);
      frame->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
      frame->setRadius(Style::scaledRadiusSm(scale));
      frame->setPadding(Style::spaceXs * scale, Style::spaceSm * scale);

      auto lbl = std::make_unique<Label>();
      lbl->setText(longText);
      lbl->setFontSize(Style::fontSizeBody * scale);
      lbl->setMaxWidth(w * scale - Style::spaceSm * 2.0f * scale);
      lbl->setMaxLines(1);
      frame->addChild(std::move(lbl));
      row->addChild(std::move(frame));

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Text alignment: Start / Center / End with short, medium, and long
  // ── (eliding) text in same-width framed boxes. Each row is one alignment
  // ── mode; each column is a different text length. The long column should
  // ── always show the ellipsis; the short and medium columns show where the
  // ── text lands relative to the box edges.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Text alignment (Start / Center / End × short / medium / long)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    constexpr float kBoxW = 200.0f;
    const std::string kShort = "Hi";
    const std::string kMedium = "The quick brown fox";
    const std::string kLong = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod.";

    struct AlignRow {
      const char* name;
      TextAlign align;
    };
    const AlignRow rows[] = {
        {"Start", TextAlign::Start},
        {"Center", TextAlign::Center},
        {"End", TextAlign::End},
    };

    auto makeAlignFrame = [&](const std::string& text, TextAlign align) {
      auto frame = std::make_unique<Flex>();
      frame->setDirection(FlexDirection::Horizontal);
      frame->setAlign(FlexAlign::Center);
      frame->setSize(kBoxW * scale, 0.0f);
      frame->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
      frame->setRadius(Style::scaledRadiusSm(scale));
      frame->setPadding(Style::spaceXs * scale, Style::spaceSm * scale);

      auto lbl = std::make_unique<Label>();
      lbl->setText(text);
      lbl->setFontSize(Style::fontSizeBody * scale);
      lbl->setMaxLines(1);
      lbl->setTextAlign(align);
      lbl->setFlexGrow(1.0f); // fill the frame so alignment has space to act
      frame->addChild(std::move(lbl));
      return frame;
    };

    for (const auto& r : rows) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(Style::spaceSm * scale);

      auto tag = std::make_unique<Label>();
      tag->setText(r.name);
      tag->setFontSize(Style::fontSizeCaption * scale);
      tag->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      tag->setMinWidth(48.0f * scale);
      row->addChild(std::move(tag));

      row->addChild(makeAlignFrame(kShort, r.align));
      row->addChild(makeAlignFrame(kMedium, r.align));
      row->addChild(makeAlignFrame(kLong, r.align));

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Multi-line wrapping with explicit maxLines.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Wrapping (maxWidth=320, maxLines=1..4)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    const std::string para =
        "The quick brown fox jumps over the lazy dog while a sphinx of black quartz judges its vow.";
    for (int lines : {1, 2, 3, 4}) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Start);
      row->setGap(Style::spaceSm * scale);

      auto tag = std::make_unique<Label>();
      tag->setText("L=" + std::to_string(lines));
      tag->setFontSize(Style::fontSizeCaption * scale);
      tag->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      tag->setMinWidth(40.0f * scale);
      row->addChild(std::move(tag));

      auto lbl = std::make_unique<Label>();
      lbl->setText(para);
      lbl->setFontSize(Style::fontSizeBody * scale);
      lbl->setMaxWidth(320.0f * scale);
      lbl->setMaxLines(lines);
      row->addChild(std::move(lbl));

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Bar-style capsules (icon + short text, fixed control height).
  // ── This is the layout that has historically been most sensitive to
  // ── 1px ink-vs-metric disagreement.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Bar-style capsules (controlHeight rows, mixed icons)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    struct Capsule {
      const char* glyph;
      const char* text;
    };
    const Capsule capsules[] = {
        {"home", "Home"},        {"settings", "Settings"}, {"weather-cloud", "Cloudy 18°"},
        {"media-play", "Track"}, {"check", "12 messages"}, {"cpu-temperature", "62°"},
    };

    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setGap(Style::spaceSm * scale);

    for (const auto& c : capsules) {
      auto pill = std::make_unique<Flex>();
      pill->setDirection(FlexDirection::Horizontal);
      pill->setAlign(FlexAlign::Center);
      pill->setGap(Style::spaceXs * scale);
      pill->setRadius(Style::scaledRadiusMd(scale));
      pill->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
      pill->setPadding(0.0f, Style::spaceSm * scale);

      auto glyph = std::make_unique<Glyph>();
      glyph->setGlyph(c.glyph);
      glyph->setGlyphSize(Style::barGlyphSize * scale);
      glyph->setColor(colorSpecFromRole(ColorRole::Primary));
      pill->addChild(std::move(glyph));

      auto lbl = std::make_unique<Label>();
      lbl->setText(c.text);
      lbl->setFontSize(Style::fontSizeBody * scale);
      pill->addChild(std::move(lbl));

      row->addChild(std::move(pill));
    }
    col->addChild(std::move(row));

    section->addChild(std::move(col));
  }

  // ── Mixed sizes inline: lays multiple labels of different sizes next
  // ── to each other on the same baseline. If FlexAlign::Center is using
  // ── ink rather than the cap line, smaller text will float relative to
  // ── larger.
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Mixed sizes inline (centered cross-axis)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    auto row = std::make_unique<Flex>();
    row->setDirection(FlexDirection::Horizontal);
    row->setAlign(FlexAlign::Center);
    row->setGap(Style::spaceSm * scale);

    const float sizes[] = {
        Style::fontSizeMini, Style::fontSizeCaption, Style::fontSizeBody, Style::fontSizeTitle, Style::fontSizeHeader
    };
    for (float fs : sizes) {
      auto lbl = std::make_unique<Label>();
      lbl->setText("Hxg" + std::to_string(static_cast<int>(fs)));
      lbl->setFontSize(fs * scale);
      row->addChild(std::move(lbl));
    }
    col->addChild(std::move(row));

    section->addChild(std::move(col));
  }

  // ── Auto-scrolling labels (marquee) ────────────────────────────────
  {
    auto col = std::make_unique<Flex>();
    col->setDirection(FlexDirection::Vertical);
    col->setAlign(FlexAlign::Start);
    col->setGap(Style::spaceSm * scale);
    col->setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
    col->setRadius(Style::scaledRadiusLg(scale));
    col->setPadding(Style::spaceMd * scale);

    auto title = std::make_unique<Label>();
    title->setText("Auto-scroll (marquee)");
    title->setFontWeight(FontWeight::Bold);
    title->setFontSize(Style::fontSizeBody * scale);
    col->addChild(std::move(title));

    auto marquee = std::make_unique<Label>();
    marquee->setText("This label scrolls automatically when the line is longer than its layout width :p");
    marquee->setFontSize(Style::fontSizeBody * scale);
    marquee->setMaxWidth(240.0f * scale);
    marquee->setAutoScroll(true);
    marquee->setAutoScrollSpeed(42.0f * scale);
    col->addChild(std::move(marquee));

    auto marqueeHover = std::make_unique<Label>();
    marqueeHover->setText("Hover this row to scroll - the marquee pauses when the pointer leaves the label.");
    marqueeHover->setFontSize(Style::fontSizeBody * scale);
    marqueeHover->setMaxWidth(240.0f * scale);
    marqueeHover->setAutoScroll(true);
    marqueeHover->setAutoScrollSpeed(42.0f * scale);
    marqueeHover->setAutoScrollOnlyWhenHovered(true);
    col->addChild(std::move(marqueeHover));

    section->addChild(std::move(col));
  }

  return section;
}

void TestPanel::selectTab(std::size_t index) {
  if (m_controlsTab != nullptr) {
    m_controlsTab->setVisible(index == 0);
  }
  if (m_textTab != nullptr) {
    m_textTab->setVisible(index == 1);
  }
  PanelManager::instance().requestLayout();
}

void TestPanel::applyTestFontFamily(const std::string& family) {
  std::string trimmed = family;
  while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
    trimmed.erase(trimmed.begin());
  }
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
    trimmed.pop_back();
  }
  if (trimmed.empty()) {
    trimmed = "sans-serif";
  }
  auto* ctx = PanelManager::instance().renderContext();
  if (ctx == nullptr) {
    return;
  }
  ctx->setTextFontFamily(trimmed);
  if (m_fontStatusLabel != nullptr) {
    m_fontStatusLabel->setText("Active: " + trimmed);
    m_fontStatusLabel->setColor(colorSpecFromRole(ColorRole::Primary));
  }
  PanelManager::instance().requestLayout();
}

void TestPanel::onClose() {
  m_container = nullptr;
  m_headerLabel = nullptr;
  m_sliderValueLabel = nullptr;
  m_toggleValueLabel = nullptr;
  m_checkboxValueLabel = nullptr;
  m_select = nullptr;
  m_glyphTextButton = nullptr;
  m_glyphButton = nullptr;
  m_glyphBox = nullptr;
  m_glyph = nullptr;
  m_transformStage = nullptr;
  m_transformDemoBox = nullptr;
  m_transformDemoGlyph = nullptr;
  m_transformDemoButton = nullptr;
  m_transformBadgeBox = nullptr;
  m_transformBadgeLabel = nullptr;
  m_slider = nullptr;
  m_toggle = nullptr;
  m_checkbox = nullptr;
  m_radioA = nullptr;
  m_radioB = nullptr;
  m_spinner = nullptr;
  m_stepper = nullptr;
  m_stepperValueLabel = nullptr;
  m_input = nullptr;
  m_inputValueLabel = nullptr;
  m_openFileDialogButton = nullptr;
  m_fileDialogResultLabel = nullptr;
  m_transformHelp = nullptr;
  m_colorPickerResultSwatch = nullptr;
  m_openColorPickerButton = nullptr;
  m_openGlyphPickerButton = nullptr;
  m_glyphPickerResultLabel = nullptr;
  m_segmented = nullptr;
  m_segmentedValueLabel = nullptr;
  m_closeButton = nullptr;
  m_scrollView = nullptr;
  m_fontFamilyInput = nullptr;
  m_fontStatusLabel = nullptr;
  m_baselineModeLabel = nullptr;
  m_baselineModeToggle = nullptr;
  m_controlsTab = nullptr;
  m_textTab = nullptr;
  m_tabSwitch = nullptr;
}

void TestPanel::doLayout(Renderer& renderer, float width, float height) {
  if (root() == nullptr) {
    return;
  }
  root()->setSize(width, height);
  root()->layout(renderer);

  if (m_glyph != nullptr && m_glyphBox != nullptr) {
    m_glyph->measure(renderer);
    m_glyph->setPosition(
        std::round((m_glyphBox->width() - m_glyph->width()) * 0.5f),
        std::round((m_glyphBox->height() - m_glyph->height()) * 0.5f)
    );
  }
  if (m_transformStage != nullptr && m_transformDemoBox != nullptr) {
    m_transformDemoBox->setPosition(
        std::round((m_transformStage->width() - m_transformDemoBox->width()) * 0.5f),
        std::round((m_transformStage->height() - m_transformDemoBox->height()) * 0.5f)
    );
  }
  if (m_transformDemoBox != nullptr && m_transformDemoButton != nullptr) {
    m_transformDemoButton->layout(renderer);
    m_transformDemoButton->setPosition(
        std::round((m_transformDemoBox->width() - m_transformDemoButton->width()) * 0.5f),
        std::round((m_transformDemoBox->height() - m_transformDemoButton->height()) * 0.5f)
    );
  }
  if (m_transformDemoBox != nullptr && m_transformDemoGlyph != nullptr) {
    m_transformDemoGlyph->measure(renderer);
    m_transformDemoGlyph->setPosition(
        18.0f * contentScale(), std::round((m_transformDemoBox->height() - m_transformDemoGlyph->height()) * 0.85f)
    );
  }
  if (m_transformDemoBox != nullptr && m_transformBadgeBox != nullptr) {
    m_transformBadgeBox->setPosition(
        m_transformDemoBox->width() - m_transformBadgeBox->width() - 12.0f * contentScale(), 12.0f * contentScale()
    );
  }
  if (m_transformBadgeBox != nullptr && m_transformBadgeLabel != nullptr) {
    m_transformBadgeLabel->measure(renderer);
    m_transformBadgeLabel->setPosition(
        std::round((m_transformBadgeBox->width() - m_transformBadgeLabel->width()) * 0.5f),
        std::round((m_transformBadgeBox->height() - m_transformBadgeLabel->height()) * 0.5f) - 1.0f * contentScale()
    );
  }
}

void TestPanel::doUpdate(Renderer& /*renderer*/) {}
