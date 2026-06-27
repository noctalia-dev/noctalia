#include "shell/test/test_panel.h"

#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/render_context.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/grid_view.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

void TestPanel::create() {
  const float scale = contentScale();
  auto rootLayout = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto headerRow = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
  });

  auto header = ui::label({
      .out = &m_headerLabel,
      .text = "Test",
      .fontSize = Style::fontSizeTitle * scale,
      .color = colorSpecFromRole(ColorRole::Primary),
      .fontWeight = FontWeight::Bold,
  });
  headerRow->addChild(std::move(header));

  auto tabSwitch = ui::segmented({
      .out = &m_tabSwitch,
      .options = std::vector<ui::SegmentedOption>{{.label = "Controls"}, {.label = "Text"}},
      .selectedIndex = 0,
      .scale = scale,
      .surfaceOpacity = panelCardOpacity(),
      .onChange = [this](std::size_t index) { selectTab(index); },
  });

  headerRow->addChild(ui::row({.flexGrow = 1.0f}));
  headerRow->addChild(std::move(tabSwitch));
  headerRow->addChild(ui::row({.flexGrow = 1.0f}));

  auto closeButton = ui::button({
      .out = &m_closeButton,
      .glyph = "close",
      .glyphSize = Style::fontSizeBody * scale,
      .variant = ButtonVariant::Default,
      .surfaceOpacity = panelCardOpacity(),
      .minWidth = Style::controlHeightSm * scale,
      .minHeight = Style::controlHeightSm * scale,
      .padding = Style::spaceXs * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = []() { PanelManager::instance().closePanel(); },
  });
  headerRow->addChild(std::move(closeButton));
  rootLayout->addChild(std::move(headerRow));

  auto content = ui::row({
      .align = FlexAlign::Start,
      .gap = Style::spaceLg * scale,
      .fillWidth = true,
  });

  auto colA = ui::column({
      .align = FlexAlign::Start,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0f,
  });

  auto colB = ui::column({
      .align = FlexAlign::Start,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0f,
  });

  auto colC = ui::column({
      .align = FlexAlign::Start,
      .gap = Style::spaceMd * scale,
      .flexGrow = 1.0f,
  });

  auto makeRow = [scale]() {
    return ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * scale,
    });
  };

  auto makeCol = [scale]() {
    return ui::column({
        .align = FlexAlign::Start,
        .gap = Style::spaceXs * scale,
    });
  };

  // Each control sits in a small section: a caption-style title on top, then
  // the control underneath. This is more compact than the prior row-label
  // pattern and avoids the 150px gutter of empty space on the left.
  auto makeSection = [scale](const char* title) {
    return ui::column(
        {
            .align = FlexAlign::Start,
            .gap = Style::spaceXs * scale,
        },
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
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

    auto makeVariantButton = [scale, opacity = panelCardOpacity()](const VariantSpec& spec, bool enabled = true) {
      return ui::button({
          .text = spec.label,
          .fontSize = Style::fontSizeBody * scale,
          .enabled = enabled,
          .variant = spec.variant,
          .surfaceOpacity = opacity,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = []() {},
      });
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
    auto glyphTextButton = ui::button({
        .out = &m_glyphTextButton,
        .text = "Settings",
        .glyph = "settings",
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Default,
        .surfaceOpacity = panelCardOpacity(),
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = []() {},
    });

    auto glyphButton = ui::button({
        .out = &m_glyphButton,
        .glyph = "home",
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Default,
        .surfaceOpacity = panelCardOpacity(),
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = []() {},
    });

    auto section = makeSection("Icon buttons");
    auto row = makeRow();
    row->addChild(std::move(glyphTextButton));
    row->addChild(std::move(glyphButton));
    section->addChild(std::move(row));
    colA->addChild(std::move(section));
  }

  // Select
  {
    auto select = ui::select({
        .out = &m_select,
        .options = std::vector<std::string>{"Something", "Yop", "Anything"},
        .selectedIndex = 0,
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceMd * scale,
        .glyphSize = 14.0f * scale,
        .surfaceOpacity = panelCardOpacity(),
        .width = 220.0f * scale,
        .height = 0.0f,
    });

    auto section = makeSection("Select");
    section->setZIndex(10);
    section->addChild(std::move(select));
    colA->addChild(std::move(section));
  }

  // Input
  {
    auto input = ui::input({
        .out = &m_input,
        .placeholder = "Type something...",
        .fontSize = Style::fontSizeBody * scale,
        .controlHeight = Style::controlHeight * scale,
        .horizontalPadding = Style::spaceMd * scale,
        .surfaceOpacity = panelCardOpacity(),
        .width = 220.0f * scale,
        .height = 0.0f,
        .onChange = [this](const std::string& val) {
          if (m_inputValueLabel != nullptr) {
            m_inputValueLabel->setText(val.empty() ? "..." : val.substr(0, 16));
          }
        },
    });

    auto valueLabel = ui::label({
        .out = &m_inputValueLabel,
        .fontSize = Style::fontSizeCaption * scale,
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
    auto slider = ui::slider({
        .out = &m_slider,
        .minValue = 0.0f,
        .maxValue = 100.0f,
        .step = 1.0f,
        .value = 50.0f,
        .trackHeight = Style::sliderTrackHeight * scale,
        .thumbSize = Style::sliderThumbSize * scale,
        .controlHeight = Style::controlHeight * scale,
        .width = Style::sliderDefaultWidth * scale,
        .height = 0.0f,
        .onValueChanged = [this](double value) {
          if (m_sliderValueLabel != nullptr) {
            const int percent = static_cast<int>(std::round(value));
            m_sliderValueLabel->setText(std::to_string(percent) + "%");
          }
        },
    });

    auto valueLabel = ui::label({
        .out = &m_sliderValueLabel,
        .text = "50%",
        .fontSize = Style::fontSizeCaption * scale,
    });

    auto section = makeSection("Slider");
    auto row = makeRow();
    row->addChild(std::move(slider));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colA->addChild(std::move(section));
  }

  // ── Column B: Toggles, Segmented, Checkbox, Radio, Spinner ──────────────
  {
    auto toggle = ui::toggle({
        .out = &m_toggle,
        .checked = false,
        .toggleSize = ToggleSize::Medium,
        .scale = scale,
        .onChange = [this](bool checked) {
          if (m_toggleValueLabel != nullptr) {
            m_toggleValueLabel->setText(checked ? "true" : "false");
          }
        },
    });

    auto valueLabel = ui::label({
        .out = &m_toggleValueLabel,
        .text = "false",
        .fontSize = Style::fontSizeCaption * scale,
    });

    auto section = makeSection("Toggle");
    auto row = makeRow();
    row->addChild(std::move(toggle));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colB->addChild(std::move(section));
  }

  {
    auto valueLabel = ui::label({
        .out = &m_segmentedValueLabel,
        .text = "System",
        .fontSize = Style::fontSizeCaption * scale,
    });

    static const char* const kLabels[] = {"Light", "Dark", "System"};
    auto segmented = ui::segmented({
        .out = &m_segmented,
        .options = std::vector<ui::SegmentedOption>{{.label = "Light"}, {.label = "Dark"}, {.label = "System"}},
        .selectedIndex = 2,
        .scale = scale,
        .surfaceOpacity = panelCardOpacity(),
        .onChange = [this](std::size_t index) {
          if (m_segmentedValueLabel != nullptr && index < std::size(kLabels)) {
            m_segmentedValueLabel->setText(kLabels[index]);
          }
        },
    });

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

    auto valueLabel = ui::label({
        .out = &m_checkboxValueLabel,
        .text = "true",
        .fontSize = Style::fontSizeCaption * scale,
    });

    auto section = makeSection("Checkbox");
    auto row = makeRow();
    row->addChild(std::move(checkbox));
    row->addChild(std::move(valueLabel));
    section->addChild(std::move(row));
    colB->addChild(std::move(section));
  }

  {
    auto makeRadioOption = [scale](const char* text, std::unique_ptr<RadioButton> radio) {
      auto opt = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceXs * scale,
      });
      opt->addChild(std::move(radio));
      opt->addChild(
          ui::label({
              .text = text,
              .fontSize = Style::fontSizeBody * scale,
          })
      );
      return opt;
    };

    auto radioA = ui::radioButton({
        .out = &m_radioA,
        .checked = true,
        .scale = scale,
    });

    auto radioB = ui::radioButton({
        .out = &m_radioB,
        .scale = scale,
    });

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

    auto options = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * scale,
    });
    options->addChild(makeRadioOption("Option A", std::move(radioA)));
    options->addChild(makeRadioOption("Option B", std::move(radioB)));

    auto section = makeSection("Radio");
    section->addChild(std::move(options));
    colB->addChild(std::move(section));
  }

  {
    auto spinner = ui::spinner({
        .out = &m_spinner,
        .spinnerSize = 20.0f * scale,
        .thickness = 2.0f * scale,
    });

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
        .surfaceOpacity = panelCardOpacity(),
        .onValueChanged = [this](int v) {
          if (m_stepperValueLabel != nullptr) {
            m_stepperValueLabel->setText("onChange: " + std::to_string(v));
          }
        },
    });

    auto valueLabel = ui::label({
        .out = &m_stepperValueLabel,
        .text = "onChange: 42",
        .fontSize = Style::fontSizeCaption * scale,
    });

    auto section = makeSection("Stepper");
    section->addChild(std::move(stepper));
    section->addChild(std::move(valueLabel));
    colB->addChild(std::move(section));
  }

  // ── Column C: File dialog, Color picker, Grid view, Transforms ──────────
  {
    auto resultLabel = ui::label({
        .out = &m_fileDialogResultLabel,
        .text = "No image selected",
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxWidth = 280.0f * scale,
    });

    auto openFileDialog = ui::button({
        .out = &m_openFileDialogButton,
        .text = "Browse images...",
        .glyph = "image",
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Default,
        .surfaceOpacity = panelCardOpacity(),
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this]() {
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
        },
    });

    auto section = makeSection("File dialog");
    auto row = makeRow();
    row->addChild(std::move(openFileDialog));
    row->addChild(std::move(resultLabel));
    section->addChild(std::move(row));
    colC->addChild(std::move(section));
  }

  {
    auto resultSwatch = ui::box({
        .out = &m_colorPickerResultSwatch,
        .fill = fixedColorSpec(ColorPickerDialog::lastResult().value_or(colorForRole(ColorRole::Primary))),
        .radius = Style::scaledRadiusMd(scale),
        .width = 28.0f * scale,
        .height = 28.0f * scale,
        .configure = [scale](Box& box) {
          box.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * scale);
        },
    });

    auto openPicker = ui::button({
        .out = &m_openColorPickerButton,
        .text = "Open color picker...",
        .fontSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Default,
        .surfaceOpacity = panelCardOpacity(),
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this]() {
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
        },
    });

    auto section = makeSection("Color picker");
    auto row = makeRow();
    row->addChild(std::move(openPicker));
    row->addChild(std::move(resultSwatch));
    section->addChild(std::move(row));
    colC->addChild(std::move(section));
  }

  {
    std::string resultText = "No glyph selected";
    ColorSpec resultColor = colorSpecFromRole(ColorRole::OnSurfaceVariant);
    if (const auto last = GlyphPickerDialog::lastResult()) {
      resultText = last->name;
      resultColor = colorSpecFromRole(ColorRole::Primary);
    }

    auto resultLabel = ui::label({
        .out = &m_glyphPickerResultLabel,
        .text = std::move(resultText),
        .fontSize = Style::fontSizeCaption * scale,
        .color = resultColor,
        .maxWidth = 280.0f * scale,
    });

    auto openPicker = ui::button({
        .out = &m_openGlyphPickerButton,
        .text = "Open glyph picker...",
        .fontSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Default,
        .surfaceOpacity = panelCardOpacity(),
        .minHeight = Style::controlHeight * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this]() {
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
        },
    });

    auto section = makeSection("Glyph picker");
    auto row = makeRow();
    row->addChild(std::move(openPicker));
    row->addChild(std::move(resultLabel));
    section->addChild(std::move(row));
    colC->addChild(std::move(section));
  }

  {
    m_gridTileButtons.clear();

    auto grid = std::make_unique<GridView>();
    grid->setColumns(3);
    grid->setColumnGap(Style::spaceSm * scale);
    grid->setRowGap(Style::spaceSm * scale);
    grid->setPadding(Style::spaceXs * scale);
    grid->setSize(300.0f * scale, 0.0f);
    grid->setUniformCellSize(true);
    grid->setStretchItems(true);
    grid->setMinCellHeight(64.0f * scale);

    struct TileSpec {
      const char* glyph;
      const char* label;
    };
    const std::vector<TileSpec> tiles = {
        {"home", "Home"},         {"media-play", "Music"},      {"copy", "Gallery"},
        {"settings", "Settings"}, {"weather-cloud", "Weather"}, {"check", "Calendar"},
    };

    for (std::size_t i = 0; i < tiles.size(); ++i) {
      const auto& tileData = tiles[i];
      auto tile = ui::button({
          .text = tileData.label,
          .glyph = tileData.glyph,
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = 16.0f * scale,
          .contentAlign = ButtonContentAlign::Center,
          .variant = ButtonVariant::Default,
          .surfaceOpacity = panelCardOpacity(),
          .minHeight = 64.0f * scale,
          .padding = Style::spaceSm * scale,
          .gap = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this, tileIndex = i, label = std::string(tileData.label)]() {
                if (m_gridSelectionLabel != nullptr) {
                  m_gridSelectionLabel->setText("Selected: " + label);
                  m_gridSelectionLabel->setColor(colorSpecFromRole(ColorRole::Primary));
                }
                for (std::size_t buttonIndex = 0; buttonIndex < m_gridTileButtons.size(); ++buttonIndex) {
                  if (m_gridTileButtons[buttonIndex] != nullptr) {
                    m_gridTileButtons[buttonIndex]->setSelected(buttonIndex == tileIndex);
                  }
                }
              },
          .configure =
              [scale](Button& button) {
                button.setDirection(FlexDirection::Vertical);
                if (button.label() != nullptr) {
                  button.label()->setFontSize(Style::fontSizeCaption * scale);
                  button.label()->setMaxLines(1);
                  button.label()->setTextAlign(TextAlign::Center);
                }
              },
      });

      Button* tileButton = tile.get();
      grid->addChild(std::move(tile));
      m_gridTileButtons.push_back(tileButton);
    }

    auto statusLabel = ui::label({
        .out = &m_gridSelectionLabel,
        .text = "No grid item selected",
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    });

    auto section = makeSection("Grid view");
    section->addChild(std::move(grid));
    section->addChild(std::move(statusLabel));
    colC->addChild(std::move(section));
  }

  // Transforms
  {
    auto transformStage = ui::box({
        .out = &m_transformStage,
        .fill = colorSpecFromRole(ColorRole::Surface),
        .radius = Style::scaledRadiusLg(scale),
        .width = 280.0f * scale,
        .height = 220.0f * scale,
        .configure = [scale](Box& box) {
          box.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * scale);
        },
    });

    auto demoBox = ui::box({
        .out = &m_transformDemoBox,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
        .radius = Style::scaledRadiusLg(scale),
        .width = 180.0f * scale,
        .height = 100.0f * scale,
        .configure = [scale](Box& box) {
          box.setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * scale);
          box.setRotation(0.0f);
        },
    });

    auto demoButton = ui::button({
        .out = &m_transformDemoButton,
        .text = "Click me...",
        .glyph = "cpu-temperature",
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Primary,
        .surfaceOpacity = panelCardOpacity(),
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceLg * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this]() {
          if (m_transformHelp != nullptr) {
            m_transformHelp->setText("Transform button clicked!");
            m_transformHelp->setColor(colorSpecFromRole(ColorRole::Secondary));
          }
        },
    });
    m_transformDemoBox->addChild(std::move(demoButton));

    auto demoGlyph = ui::glyph({
        .out = &m_transformDemoGlyph,
        .glyph = "noctalia",
        .glyphSize = 24.0f * scale,
        .color = colorSpecFromRole(ColorRole::Primary),
        .configure = [scale](Glyph& glyph) {
          glyph.setPosition(150.0f * scale, 60.0f * scale);
          glyph.setRotation(std::numbers::pi_v<float> * 0.5f);
        },
    });
    m_transformDemoBox->addChild(std::move(demoGlyph));

    auto badgeBox = ui::box({
        .out = &m_transformBadgeBox,
        .fill = colorSpecFromRole(ColorRole::Primary),
        .radius = 14.0f * scale,
        .width = 28.0f * scale,
        .height = 28.0f * scale,
        .configure = [scale](Box& box) {
          box.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * scale);
        },
    });

    auto badgeLabel = ui::label({
        .out = &m_transformBadgeLabel,
        .text = "3",
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnPrimary),
    });
    m_transformBadgeBox->addChild(std::move(badgeLabel));
    m_transformDemoBox->addChild(std::move(badgeBox));
    m_transformStage->addChild(std::move(demoBox));

    auto helpLabel = ui::label({
        .out = &m_transformHelp,
        .text = "Rotated node with children.",
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    });

    auto section = makeSection("Transforms");
    section->addChild(std::move(helpLabel));
    section->addChild(std::move(transformStage));
    colC->addChild(std::move(section));
  }

  m_container = colA.get();
  content->addChild(std::move(colA));
  content->addChild(std::move(colB));
  content->addChild(std::move(colC));

  auto controlsTab = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceLg * scale,
  });
  controlsTab->addChild(std::move(content));
  m_controlsTab = controlsTab.get();

  auto textTab = buildTextLabSection(scale);
  m_textTab = textTab.get();
  m_textTab->setVisible(false);

  auto scroll = ui::scrollView({
      .out = &m_scrollView,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
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
    m_animations->animate(0.0f, 2.0f * std::numbers::pi_v<float>, 8000.0f, Easing::Linear, [this](float phase) {
      if (m_transformDemoBox != nullptr) {
        m_transformDemoBox->setRotation(phase);
        m_transformDemoBox->setScale(1.0f + 0.16f * std::sin(phase));
      }
    });
  }
}

std::unique_ptr<Flex> TestPanel::buildTextLabSection(float scale) {
  auto section = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto makeLabCard = [this, scale]() {
    return ui::column({
        .align = FlexAlign::Start,
        .gap = Style::spaceSm * scale,
        .configure = [this, scale](Flex& col) {
          col.setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
          col.setRadius(Style::scaledRadiusLg(scale));
          col.setPadding(Style::spaceMd * scale);
        },
    });
  };

  auto makeLabTitle = [scale](std::string text) {
    return ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::Bold,
    });
  };

  auto makeLabRow = [scale](FlexAlign align = FlexAlign::Center) {
    return ui::row({
        .align = align,
        .gap = Style::spaceSm * scale,
    });
  };

  auto makeTagLabel = [scale](std::string text, float minWidth) {
    return ui::label({
        .text = std::move(text),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .minWidth = minWidth * scale,
    });
  };

  // ── Section heading ────────────────────────────────────────────────
  {
    section->addChild(
        ui::label({
            .text = "Text Lab",
            .fontSize = Style::fontSizeHeader * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
            .fontWeight = FontWeight::Bold,
        })
    );
  }

  // ── Font family controls ──────────────────────────────────────────
  {
    auto row = makeLabRow();

    row->addChild(
        ui::label({
            .text = "Font family:",
            .fontSize = Style::fontSizeBody * scale,
        })
    );

    row->addChild(
        ui::input({
            .out = &m_fontFamilyInput,
            .placeholder = "e.g. sans-serif, Inter, DejaVu Sans, monospace",
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .horizontalPadding = Style::spaceMd * scale,
            .surfaceOpacity = panelCardOpacity(),
            .width = 360.0f * scale,
            .height = 0.0f,
            .onSubmit = [this](const std::string& value) { applyTestFontFamily(value); },
        })
    );

    row->addChild(
        ui::button({
            .text = "Apply",
            .fontSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Primary,
            .surfaceOpacity = panelCardOpacity(),
            .minHeight = Style::controlHeight * scale,
            .paddingV = Style::spaceSm * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this]() {
              if (m_fontFamilyInput != nullptr) {
                applyTestFontFamily(m_fontFamilyInput->value());
              }
            },
        })
    );

    static const char* const kPresets[] = {"sans-serif", "serif", "monospace"};
    for (const char* preset : kPresets) {
      row->addChild(
          ui::button({
              .text = preset,
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceSm * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this, preset]() { applyTestFontFamily(preset); },
          })
      );
    }

    row->addChild(
        ui::label({
            .out = &m_fontStatusLabel,
            .text = "Live font swap rebuilds Pango cache.",
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    section->addChild(std::move(row));
  }

  // ── Sample string used in the size ladder. Mixes ascenders, descenders,
  // ── digits, punctuation, and accents to surface vertical-metric drift.
  const std::string kSample = "Sphinx of black quartz, judge my vow — Apgjy 0123 ñÅ";

  // ── Size ladder: every Style font size, regular and bold, with a
  // ── matching-size glyph next to each label. If the label and the
  // ── glyph disagree on baseline, this row will reveal it.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Size ladder (glyph + text, regular & bold)"));

    struct SizeSpec {
      const char* name;
      float size;
    };
    const SizeSpec sizes[] = {
        {"mini", Style::fontSizeMini},   {"caption", Style::fontSizeCaption}, {"body", Style::fontSizeBody},
        {"title", Style::fontSizeTitle}, {"header", Style::fontSizeHeader},
    };

    for (const auto& s : sizes) {
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceMd * scale,
      });

      row->addChild(makeTagLabel(std::string(s.name) + " (" + std::to_string(static_cast<int>(s.size)) + ")", 110.0f));
      row->addChild(
          ui::glyph({
              .glyph = "home",
              .glyphSize = s.size * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          })
      );
      row->addChild(
          ui::label({
              .text = kSample,
              .fontSize = s.size * scale,
          })
      );
      row->addChild(
          ui::label({
              .text = "|",
              .fontSize = s.size * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      row->addChild(
          ui::label({
              .text = kSample,
              .fontSize = s.size * scale,
              .fontWeight = FontWeight::Bold,
          })
      );

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Glyph/text alignment matrix: same body text next to glyphs of
  // ── increasing size. Useful for spotting bar-style 1px drifts.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Glyph + body text alignment (varying glyph size)"));

    const float glyphSizes[] = {10.0f, 12.0f, 14.0f, 16.0f, 18.0f, 20.0f, 24.0f, 28.0f, 32.0f};
    for (float gs : glyphSizes) {
      auto row = makeLabRow();
      row->addChild(makeTagLabel("g" + std::to_string(static_cast<int>(gs)), 36.0f));
      row->addChild(
          ui::glyph({
              .glyph = "settings",
              .glyphSize = gs * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          })
      );
      row->addChild(
          ui::label({
              .text = "Hxg Apjy 0123 — body text",
              .fontSize = Style::fontSizeBody * scale,
          })
      );

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Wiggle test: identical labels repeated across a row. Any vertical
  // ── jitter shows up immediately under HiDPI snapping.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Repeat-row jitter probe (identical text repeated)"));

    const float sizes[] = {Style::fontSizeMini, Style::fontSizeCaption, Style::fontSizeBody, Style::fontSizeTitle};
    for (float fs : sizes) {
      auto row = makeLabRow();

      row->addChild(makeTagLabel("fs" + std::to_string(static_cast<int>(fs)), 40.0f));

      for (int i = 0; i < 8; ++i) {
        row->addChild(
            ui::label({
                .text = "Hgjy",
                .fontSize = fs * scale,
            })
        );
      }
      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Baseline mode test (cap-only ↔ descender swap). Latin optical mode
  // ── logical mode follows Pango metrics; ink-centered mode follows visible ink.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Baseline mode (logical vs ink-centered)"));

    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * scale,
    });

    row->addChild(
        ui::label({
            .out = &m_baselineModeLabel,
            .text = "MAR 2025",
            .fontSize = Style::fontSizeTitle * scale,
        })
    );
    row->addChild(
        ui::label({
            .text = "Apgjy",
            .fontSize = Style::fontSizeTitle * scale,
        })
    );
    row->addChild(
        ui::label({
            .text = "MAR 2025 (ink)",
            .fontSize = Style::fontSizeTitle * scale,
            .baselineMode = LabelBaselineMode::InkCentered,
        })
    );
    row->addChild(
        ui::label({
            .text = "Apgjy (ink)",
            .fontSize = Style::fontSizeTitle * scale,
            .baselineMode = LabelBaselineMode::InkCentered,
        })
    );

    auto toggleRow = makeLabRow();

    toggleRow->addChild(
        ui::label({
            .text = "first label ink centered:",
            .fontSize = Style::fontSizeCaption * scale,
        })
    );

    toggleRow->addChild(
        ui::toggle({
            .out = &m_baselineModeToggle,
            .checked = false,
            .toggleSize = ToggleSize::Small,
            .scale = scale,
            .onChange = [this](bool checked) {
              if (m_baselineModeLabel != nullptr) {
                m_baselineModeLabel->setBaselineMode(
                    checked ? LabelBaselineMode::InkCentered : LabelBaselineMode::StableLogical
                );
              }
            },
        })
    );

    col->addChild(std::move(row));
    col->addChild(std::move(toggleRow));

    section->addChild(std::move(col));
  }

  // ── Nerd Font / PUA glyph rendering test. These codepoints live in the
  // ── Unicode Private Use Area and require a Nerd Font for coverage.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Nerd Font symbols (requires a Nerd Font installed)"));

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
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceMd * scale,
      });

      row->addChild(makeTagLabel(s.codepoint, 200.0f));

      const float sizes[] = {Style::fontSizeMini, Style::fontSizeBody, Style::fontSizeTitle, Style::fontSizeHeader};
      for (float fs : sizes) {
        row->addChild(
            ui::label({
                .text = s.symbol,
                .fontSize = fs * scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
            })
        );
      }

      row->addChild(
          ui::label({
              .text = std::string(s.symbol) + " inline text",
              .fontSize = Style::fontSizeBody * scale,
          })
      );

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Elision and wrapping tests at body font size. Each row is
  // ── identical text inside boxes of decreasing width.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Elision (single line, decreasing maxWidth)"));

    const std::string longText = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor.";
    const float widths[] = {640.0f, 480.0f, 360.0f, 240.0f, 160.0f, 120.0f, 80.0f, 56.0f, 32.0f};
    for (float w : widths) {
      auto row = makeLabRow();

      row->addChild(makeTagLabel("w=" + std::to_string(static_cast<int>(w)), 56.0f));

      auto frame = ui::row(
          {
              .align = FlexAlign::Center,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceSm * scale,
              .radius = Style::scaledRadiusSm(scale),
              .border = colorSpecFromRole(ColorRole::Outline),
              .width = w * scale,
              .height = 0.0f,
          },
          ui::label({
              .text = longText,
              .fontSize = Style::fontSizeBody * scale,
              .maxWidth = w * scale - Style::spaceSm * 2.0f * scale,
              .maxLines = 1,
          })
      );
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
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Text alignment (Start / Center / End × short / medium / long)"));

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
      return ui::row(
          {
              .align = FlexAlign::Center,
              .paddingV = Style::spaceXs * scale,
              .paddingH = Style::spaceSm * scale,
              .radius = Style::scaledRadiusSm(scale),
              .border = colorSpecFromRole(ColorRole::Outline),
              .width = kBoxW * scale,
              .height = 0.0f,
          },
          ui::label({
              .text = text,
              .fontSize = Style::fontSizeBody * scale,
              .maxLines = 1,
              .textAlign = align,
              .flexGrow = 1.0f, // fill the frame so alignment has space to act
          })
      );
    };

    for (const auto& r : rows) {
      auto row = makeLabRow();

      row->addChild(makeTagLabel(r.name, 48.0f));

      row->addChild(makeAlignFrame(kShort, r.align));
      row->addChild(makeAlignFrame(kMedium, r.align));
      row->addChild(makeAlignFrame(kLong, r.align));

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Multi-line wrapping with explicit maxLines.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Wrapping (maxWidth=320, maxLines=1..4)"));

    const std::string para =
        "The quick brown fox jumps over the lazy dog while a sphinx of black quartz judges its vow.";
    for (int lines : {1, 2, 3, 4}) {
      auto row = makeLabRow(FlexAlign::Start);

      row->addChild(makeTagLabel("L=" + std::to_string(lines), 40.0f));
      row->addChild(
          ui::label({
              .text = para,
              .fontSize = Style::fontSizeBody * scale,
              .maxWidth = 320.0f * scale,
              .maxLines = lines,
          })
      );

      col->addChild(std::move(row));
    }

    section->addChild(std::move(col));
  }

  // ── Bar-style capsules (icon + short text, fixed control height).
  // ── This is the layout that has historically been most sensitive to
  // ── 1px ink-vs-metric disagreement.
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Bar-style capsules (controlHeight rows, mixed icons)"));

    struct Capsule {
      const char* glyph;
      const char* text;
    };
    const Capsule capsules[] = {
        {"home", "Home"},        {"settings", "Settings"}, {"weather-cloud", "Cloudy 18°"},
        {"media-play", "Track"}, {"check", "12 messages"}, {"cpu-temperature", "62°"},
    };

    auto row = makeLabRow();

    for (const auto& c : capsules) {
      auto pill = ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * scale,
              .paddingV = 0.0f,
              .paddingH = Style::spaceSm * scale,
              .radius = Style::scaledRadiusMd(scale),
              .border = colorSpecFromRole(ColorRole::Outline),
          },
          ui::glyph({
              .glyph = c.glyph,
              .glyphSize = Style::baseGlyphSize * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          }),
          ui::label({
              .text = c.text,
              .fontSize = Style::fontSizeBody * scale,
          })
      );

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
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Mixed sizes inline (centered cross-axis)"));

    auto row = makeLabRow();

    const float sizes[] = {
        Style::fontSizeMini, Style::fontSizeCaption, Style::fontSizeBody, Style::fontSizeTitle, Style::fontSizeHeader
    };
    for (float fs : sizes) {
      row->addChild(
          ui::label({
              .text = "Hxg" + std::to_string(static_cast<int>(fs)),
              .fontSize = fs * scale,
          })
      );
    }
    col->addChild(std::move(row));

    section->addChild(std::move(col));
  }

  // ── Auto-scrolling labels (marquee) ────────────────────────────────
  {
    auto col = makeLabCard();
    col->addChild(makeLabTitle("Auto-scroll (marquee)"));

    col->addChild(
        ui::label({
            .text = "This label scrolls automatically when the line is longer than its layout width :p",
            .fontSize = Style::fontSizeBody * scale,
            .maxWidth = 240.0f * scale,
            .autoScroll = true,
            .autoScrollSpeed = 42.0f * scale,
        })
    );

    col->addChild(
        ui::label({
            .text = "Hover this row to scroll - the marquee pauses when the pointer leaves the label.",
            .fontSize = Style::fontSizeBody * scale,
            .maxWidth = 240.0f * scale,
            .autoScroll = true,
            .autoScrollSpeed = 42.0f * scale,
            .autoScrollOnlyWhenHovered = true,
        })
    );

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
  m_gridSelectionLabel = nullptr;
  m_gridTileButtons.clear();
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

void TestPanel::onPanelCardOpacityChanged(float opacity) {
  for (Input* input : {m_input, m_fontFamilyInput}) {
    if (input != nullptr) {
      input->setSurfaceOpacity(opacity);
    }
  }
  for (Segmented* seg : {m_segmented, m_tabSwitch}) {
    if (seg != nullptr) {
      seg->setSurfaceOpacity(opacity);
    }
  }
  for (Button* btn :
       {m_closeButton, m_glyphTextButton, m_glyphButton, m_openFileDialogButton, m_openColorPickerButton,
        m_openGlyphPickerButton, m_transformDemoButton}) {
    if (btn != nullptr) {
      btn->setSurfaceOpacity(opacity);
    }
  }
  for (Button* tile : m_gridTileButtons) {
    if (tile != nullptr) {
      tile->setSurfaceOpacity(opacity);
    }
  }
  if (m_select != nullptr) {
    m_select->setSurfaceOpacity(opacity);
  }
  if (m_stepper != nullptr) {
    m_stepper->setSurfaceOpacity(opacity);
  }
}
