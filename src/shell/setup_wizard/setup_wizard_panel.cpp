#include "shell/setup_wizard/setup_wizard_panel.h"

#include "config/config_service.h"
#include "core/log.h"
#include "core/resource_paths.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "theme/builtin_palettes.h"
#include "ui/builders.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("setup-wizard");

  struct SelectOption {
    std::string_view labelKey;
    std::string_view value;
  };

  constexpr SelectOption kSetupPaletteSources[] = {
      {"settings.options.theme.source.built-in", "builtin"},
      {"settings.options.theme.source.wallpaper", "wallpaper"},
  };

  constexpr SelectOption kWallpaperSchemes[] = {
      {"theme.scheme.m3-content", "m3-content"},
      {"theme.scheme.m3-tonal-spot", "m3-tonal-spot"},
      {"theme.scheme.m3-fruit-salad", "m3-fruit-salad"},
      {"theme.scheme.m3-rainbow", "m3-rainbow"},
      {"theme.scheme.m3-monochrome", "m3-monochrome"},
      {"theme.scheme.vibrant", "vibrant"},
      {"theme.scheme.faithful", "faithful"},
      {"theme.scheme.dysfunctional", "dysfunctional"},
      {"theme.scheme.muted", "muted"},
  };

  std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal) {
    return ui::label({
        .text = std::string(text),
        .fontSize = fontSize,
        .color = color,
        .fontWeight = fontWeight,
    });
  }

  std::vector<std::string> labelsFromOptions(std::span<const SelectOption> options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& option : options) {
      labels.emplace_back(i18n::tr(option.labelKey));
    }
    return labels;
  }

  std::size_t selectedOptionIndex(std::span<const SelectOption> options, std::string_view value) {
    for (std::size_t i = 0; i < options.size(); ++i) {
      if (options[i].value == value) {
        return i;
      }
    }
    return 0;
  }

  std::vector<std::string> builtinPaletteNames() {
    std::vector<std::string> paletteNames;
    paletteNames.reserve(noctalia::theme::builtinPalettes().size());
    for (const auto& entry : noctalia::theme::builtinPalettes()) {
      paletteNames.emplace_back(entry.name);
    }
    return paletteNames;
  }

  std::size_t selectedBuiltinPaletteIndex(std::string_view name) {
    std::size_t index = 0;
    for (const auto& entry : noctalia::theme::builtinPalettes()) {
      if (entry.name == name) {
        return index;
      }
      ++index;
    }
    return 0;
  }

  std::unique_ptr<Flex> makeCard(float scale, float fillOpacity, bool showBorder) {
    return ui::column(
        {.align = FlexAlign::Stretch,
         .gap = Style::spaceMd * scale,
         .configure = [scale, fillOpacity, showBorder](Flex& card) {
           card.setPadding(Style::spaceMd * scale, Style::spaceLg * scale);
           card.setCardStyle(scale, fillOpacity, showBorder);
         }}
    );
  }

  std::unique_ptr<Flex> makeRow(float scale) {
    return ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceMd * scale,
    });
  }

  std::unique_ptr<Flex> makeTextColumn() {
    return ui::column({
        .align = FlexAlign::Start,
        .gap = 2.0f,
        .flexGrow = 1.0f,
    });
  }

} // namespace

bool SetupWizardPanel::isFirstRun(const ConfigService& config) { return config.shouldRunSetupWizard(); }

void SetupWizardPanel::create() {
  const float scale = contentScale();
  const auto& cfg = m_config->config();

  auto root = ui::column(
      {.out = &m_root,
       .align = FlexAlign::Stretch,
       .justify = FlexJustify::SpaceBetween,
       .gap = Style::spaceLg * scale,
       .configure = [scale](Flex& flex) { flex.setPadding(24.0f * scale, 28.0f * scale); }}
  );

  auto scroll = ui::scrollView({
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });

  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceLg * scale);

  // Header
  {
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale});

    header->addChild(
        ui::image({
            .out = &m_logo,
            .width = 44.0f * scale,
            .height = 44.0f * scale,
        })
    );

    auto copy = makeTextColumn();
    copy->setGap(Style::spaceXs * scale);
    copy->addChild(makeLabel(
        i18n::tr("setup-wizard.title"), 18.0f * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
    ));
    copy->addChild(makeLabel(
        i18n::tr("setup-wizard.subtitle"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));
    header->addChild(std::move(copy));
    content->addChild(std::move(header));
  }

  content->addChild(ui::separator());

  // Telemetry
  {
    auto card = makeCard(scale, panelCardOpacity(), panelBordersEnabled());

    auto row = makeRow(scale);
    {
      auto col = makeTextColumn();
      col->addChild(makeLabel(
          i18n::tr("settings.schema.shell.telemetry.label"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
      ));
      auto description = makeLabel(
          i18n::tr("settings.schema.shell.telemetry.description"), Style::fontSizeCaption * scale,
          colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      description->setMaxLines(8);
      col->addChild(std::move(description));
      row->addChild(std::move(col));
    }
    {
      row->addChild(
          ui::toggle({
              .out = &m_telemetryToggle,
              .checked = cfg.shell.telemetryEnabled,
              .scale = scale,
          })
      );
    }
    card->addChild(std::move(row));
    content->addChild(std::move(card));
  }

  // Wallpaper
  {
    auto card = makeCard(scale, panelCardOpacity(), panelBordersEnabled());

    auto row = makeRow(scale);
    {
      auto col = makeTextColumn();
      col->addChild(makeLabel(
          i18n::tr("setup-wizard.wallpaper"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface),
          FontWeight::Bold
      ));
      const std::string currentPath = m_config->getDefaultWallpaperPath();
      auto pathLabel = makeLabel(
          currentPath.empty() ? i18n::tr("setup-wizard.no-wallpaper-selected") : currentPath,
          Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      pathLabel->setMaxWidth(330.0f * scale);
      pathLabel->setMaxLines(1);
      m_wallpaperLabel = pathLabel.get();
      col->addChild(std::move(pathLabel));
      row->addChild(std::move(col));
    }
    {
      row->addChild(
          ui::button({
              .text = i18n::tr("setup-wizard.browse"),
              .glyph = "image",
              .fontSize = Style::fontSizeBody * scale,
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Outline,
              .minWidth = 112.0f * scale,
              .minHeight = Style::controlHeight * scale,
              .paddingV = Style::spaceSm * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() {
                FileDialogOptions options;
                options.mode = FileDialogMode::Open;
                options.defaultViewMode = FileDialogViewMode::Grid;
                options.title = i18n::tr("setup-wizard.select-wallpaper");
                options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif"};
                std::filesystem::path startDir;
                if (!m_wallpaperDir.empty()) {
                  startDir = m_wallpaperDir;
                } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
                  startDir = std::filesystem::path(home) / "Pictures";
                }
                options.startDirectory = std::move(startDir);
                (void)FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> result) {
                  if (!result.has_value()) {
                    return;
                  }
                  const std::string fullPath = result->string();
                  const std::string parentDir = result->parent_path().string();
                  m_wallpaperDir = parentDir;
                  if (m_wallpaperLabel != nullptr) {
                    m_wallpaperLabel->setText(fullPath);
                    m_wallpaperLabel->setColor(colorSpecFromRole(ColorRole::Primary));
                    m_wallpaperLabel->setMaxLines(1);
                  }
                  m_config->setOverride({"wallpaper", "directory"}, parentDir);
                  m_config->setWallpaperPath(std::nullopt, fullPath);
                });
              },
          })
      );
    }
    card->addChild(std::move(row));
    content->addChild(std::move(card));
  }

  // Theme
  {
    auto card = makeCard(scale, panelCardOpacity(), panelBordersEnabled());

    // Mode row
    {
      auto row = makeRow(scale);
      auto label = makeLabel(
          i18n::tr("setup-wizard.mode"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface)
      );
      label->setFlexGrow(1.0f);
      row->addChild(std::move(label));

      std::size_t modeIdx = 0;
      if (cfg.theme.mode == ThemeMode::Light) {
        modeIdx = 1;
      } else if (cfg.theme.mode == ThemeMode::Auto) {
        modeIdx = 2;
      }
      row->addChild(
          ui::select({
              .out = &m_modeSelect,
              .options =
                  std::vector<std::string>{
                      i18n::tr("settings.options.theme.mode.dark"), i18n::tr("settings.options.theme.mode.light"),
                      i18n::tr("common.states.auto")
                  },
              .selectedIndex = modeIdx,
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceMd * scale,
              .onSelectionChanged =
                  [this](std::size_t index, std::string_view /*label*/) {
                    static constexpr const char* kModes[] = {"dark", "light", "auto"};
                    if (index < 3) {
                      m_config->setOverride({"theme", "mode"}, std::string(kModes[index]));
                    }
                  },
              .configure = [scale](Select& select) { select.setMinWidth(220.0f * scale); },
          })
      );
      card->addChild(std::move(row));
    }

    // Theme source row
    {
      auto row = makeRow(scale);
      auto label = makeLabel(
          i18n::tr("settings.schema.appearance.palette-source.label"), Style::fontSizeBody * scale,
          colorSpecFromRole(ColorRole::OnSurface)
      );
      label->setFlexGrow(1.0f);
      row->addChild(std::move(label));

      // Respect the user's existing palette: seed controls from current config
      // and write no override until the user actually changes a control. The
      // wizard only offers builtin/wallpaper, so community/custom sources are
      // displayed as builtin but left untouched in config unless changed.
      m_paletteSource =
          cfg.theme.source == PaletteSource::Wallpaper ? PaletteSource::Wallpaper : PaletteSource::Builtin;
      m_builtinPalette = cfg.theme.builtinPalette;
      const std::string_view currentSource = m_paletteSource == PaletteSource::Wallpaper ? "wallpaper" : "builtin";
      row->addChild(
          ui::select({
              .out = &m_themeSourceSelect,
              .options = labelsFromOptions(kSetupPaletteSources),
              .selectedIndex = selectedOptionIndex(kSetupPaletteSources, currentSource),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceMd * scale,
              .onSelectionChanged =
                  [this](std::size_t index, std::string_view /*label*/) {
                    if (index >= std::size(kSetupPaletteSources)) {
                      return;
                    }
                    const std::string source(kSetupPaletteSources[index].value);
                    m_paletteSource = source == "wallpaper" ? PaletteSource::Wallpaper : PaletteSource::Builtin;
                    m_config->setOverride({"theme", "source"}, source);
                    configureThemeOptionSelect();
                  },
              .configure = [scale](Select& select) { select.setMinWidth(220.0f * scale); },
          })
      );
      card->addChild(std::move(row));
    }

    // Theme option row
    {
      auto row = makeRow(scale);
      auto label = makeLabel("", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface));
      label->setFlexGrow(1.0f);
      m_themeOptionLabel = label.get();
      row->addChild(std::move(label));

      row->addChild(
          ui::select({
              .out = &m_themeOptionSelect,
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .horizontalPadding = Style::spaceMd * scale,
              .configure = [scale](Select& select) { select.setMinWidth(220.0f * scale); },
          })
      );
      card->addChild(std::move(row));
      configureThemeOptionSelect();
    }

    content->addChild(std::move(card));
  }

  root->addChild(std::move(scroll));

  // Footer
  {
    auto footer = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
    });

    footer->addChild(makeLabel(
        i18n::tr("setup-wizard.footer-note"), Style::fontSizeCaption * scale,
        colorSpecFromRole(ColorRole::OnSurfaceVariant)
    ));

    footer->addChild(
        ui::button({
            .text = i18n::tr("setup-wizard.get-started"),
            .glyph = "chevron-right",
            .fontSize = Style::fontSizeBody * scale,
            .glyphSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Primary,
            .minWidth = 132.0f * scale,
            .minHeight = Style::controlHeight * scale,
            .paddingV = Style::spaceSm * scale,
            .paddingH = Style::spaceLg * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this]() { commit(); },
        })
    );
    root->addChild(std::move(footer));
  }

  setRoot(std::move(root));
}

void SetupWizardPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_logo != nullptr && !m_logo->hasImage()) {
    m_logo->setSourceFile(renderer, paths::assetPath("noctalia.svg").string(), 48 * static_cast<int>(contentScale()));
  }
  if (m_root != nullptr) {
    m_root->setPosition(0.0f, 0.0f);
    m_root->setSize(width, height);
    m_root->layout(renderer);
  }
}

void SetupWizardPanel::configureThemeOptionSelect() {
  if (m_themeOptionLabel == nullptr || m_themeOptionSelect == nullptr || m_config == nullptr) {
    return;
  }

  m_configuringThemeOptionSelect = true;
  m_themeOptionSelect->setOnSelectionChanged(nullptr);

  const auto& cfg = m_config->config();
  if (m_paletteSource == PaletteSource::Wallpaper) {
    m_themeOptionLabel->setText(i18n::tr("setup-wizard.wallpaper-scheme"));
    m_themeOptionSelect->setOptions(labelsFromOptions(kWallpaperSchemes));
    m_themeOptionSelect->setSelectedIndex(selectedOptionIndex(kWallpaperSchemes, cfg.theme.wallpaperScheme));
    m_themeOptionSelect->setOnSelectionChanged([this](std::size_t index, std::string_view /*label*/) {
      if (m_configuringThemeOptionSelect || index >= std::size(kWallpaperSchemes)) {
        return;
      }
      m_config->setOverride({"theme", "source"}, std::string("wallpaper"));
      m_config->setOverride({"theme", "wallpaper_scheme"}, std::string(kWallpaperSchemes[index].value));
    });
  } else {
    m_themeOptionLabel->setText(i18n::tr("setup-wizard.builtin-palette"));
    if (m_builtinPalette.empty()) {
      m_builtinPalette = cfg.theme.builtinPalette;
    }
    m_themeOptionSelect->setOptions(builtinPaletteNames());
    m_themeOptionSelect->setSelectedIndex(selectedBuiltinPaletteIndex(m_builtinPalette));
    m_themeOptionSelect->setOnSelectionChanged([this](std::size_t /*index*/, std::string_view name) {
      if (m_configuringThemeOptionSelect) {
        return;
      }
      m_builtinPalette = std::string(name);
      m_config->setOverride({"theme", "source"}, std::string("builtin"));
      m_config->setOverride({"theme", "builtin"}, std::string(name));
    });
  }

  m_configuringThemeOptionSelect = false;
}

void SetupWizardPanel::commit() {
  if (m_telemetryToggle != nullptr) {
    m_config->setOverride({"shell", "telemetry_enabled"}, m_telemetryToggle->checked());
  }
  // Theme/palette overrides are written live by the select callbacks only when
  // the user actually changes them, so commit must not force any defaults here.
  if (m_config != nullptr) {
    (void)m_config->markSetupWizardCompleted();
  }
  kLog.info("setup complete");
  PanelManager::instance().close();
}

void SetupWizardPanel::onClose() {
  if (m_config != nullptr) {
    (void)m_config->markSetupWizardCompleted();
  }
}
