#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_window.h"
#include "theme/fixed_palette.h"
#include "theme/palette.h"
#include "theme/tokens.h"
#include "ui/builders.h"
#include "ui/controls/grid_tile.h"
#include "ui/controls/virtual_grid_view.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    struct PaletteColor {
      std::string name;
      std::optional<std::uint32_t> argb;
      std::string hex;
    };

    std::optional<std::size_t> terminalAnsiIndex(std::string_view token) {
      std::size_t index = 0;
      for (const auto& group : noctalia::theme::kTerminalAnsiGroupTokenKeys) {
        for (const auto name : noctalia::theme::kTerminalAnsiColorJsonKeys) {
          if (token == std::format("{}_{}", group.tokenPrefix, name)) {
            return index;
          }
          ++index;
        }
      }
      return std::nullopt;
    }

    class PaletteColorTile final : public GridTile {
    public:
      explicit PaletteColorTile(float scale) {
        setDirection(FlexDirection::Vertical);
        setAlign(FlexAlign::Stretch);
        setGap(Style::spaceXs * scale);
        setPadding(Style::spaceSm * scale);
        setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45F));
        setBorder(colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha), Style::borderWidth);
        setRadius(Style::scaledRadiusMd(scale));
        addChild(
            ui::row(
                {.out = &m_swatch,
                 .align = FlexAlign::Start,
                 .justify = FlexJustify::End,
                 .padding = Style::spaceXs * scale,
                 .radius = Style::scaledRadiusSm(scale),
                 .border = colorSpecFromRole(ColorRole::Outline),
                 .borderWidth = Style::borderWidth,
                 .fillWidth = true,
                 .height = 52.0F * scale},
                ui::row(
                    {.out = &m_terminalBadge,
                     .paddingV = Style::spaceXs * scale * 0.5F,
                     .paddingH = Style::spaceXs * scale,
                     .fill = colorSpecFromRole(ColorRole::Surface, 0.95F),
                     .radius = Style::scaledRadiusSm(scale),
                     .visible = false,
                     .participatesInLayout = false},
                    ui::label({
                        .out = &m_terminalNumber,
                        .fontSize = Style::fontSizeMini * scale,
                        .fontWeight = FontWeight::Medium,
                        .color = colorSpecFromRole(ColorRole::OnSurface),
                    })
                )
            )
        );
        addChild(
            ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .fillWidth = true, .height = 32.0F * scale},
                ui::label({
                    .out = &m_name,
                    .fontSize = Style::fontSizeCaption * scale,
                    .fontWeight = FontWeight::Medium,
                    .color = colorSpecFromRole(ColorRole::OnSurface),
                    .maxLines = 2,
                    .ellipsize = TextEllipsize::End,
                    .flexGrow = 1.0F,
                }),
                ui::button({
                    .out = &m_copyName,
                    .glyph = "copy",
                    .glyphSize = Style::fontSizeCaption * scale,
                    .variant = ButtonVariant::Ghost,
                    .minWidth = 28.0F * scale,
                    .minHeight = 28.0F * scale,
                    .padding = Style::spaceXs * scale,
                })
            )
        );
        addChild(
            ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .fillWidth = true, .height = 28.0F * scale},
                ui::label({
                    .out = &m_hex,
                    .fontSize = Style::fontSizeCaption * scale,
                    .fontFamily = "monospace",
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                    .flexGrow = 1.0F,
                }),
                ui::button({
                    .out = &m_copyHex,
                    .glyph = "copy",
                    .glyphSize = Style::fontSizeCaption * scale,
                    .variant = ButtonVariant::Ghost,
                    .tooltip = i18n::tr("settings.palette-inspector.copy-hex"),
                    .minWidth = 28.0F * scale,
                    .minHeight = 28.0F * scale,
                    .padding = Style::spaceXs * scale,
                })
            )
        );
      }

      void bind(const PaletteColor& color, const std::function<void(std::string)>& copy) {
        m_name->setText(color.name);
        m_swatch->setFill(color.argb.has_value() ? fixedColorSpec(rgbHex(*color.argb)) : clearColorSpec());
        const auto ansiIndex = terminalAnsiIndex(color.name);
        m_terminalBadge->setVisible(ansiIndex.has_value());
        m_terminalBadge->setParticipatesInLayout(ansiIndex.has_value());
        m_terminalNumber->setText(
            ansiIndex.has_value()
                ? i18n::tr("settings.palette-inspector.terminal-number", "number", *ansiIndex + 1, "index", *ansiIndex)
                : std::string{}
        );
        m_copyName->setTooltip(i18n::tr("settings.palette-inspector.copy-name") + ": " + color.name);
        m_copyName->setOnClick([copy, name = color.name]() { copy(name); });
        m_hex->setText(color.argb.has_value() ? color.hex : i18n::tr("settings.palette-inspector.missing"));
        m_copyHex->setEnabled(color.argb.has_value());
        m_copyHex->setOnClick([copy, value = color.hex]() { copy(value); });
      }

    private:
      Flex* m_swatch = nullptr;
      Flex* m_terminalBadge = nullptr;
      Label* m_terminalNumber = nullptr;
      Label* m_name = nullptr;
      Label* m_hex = nullptr;
      Button* m_copyName = nullptr;
      Button* m_copyHex = nullptr;
    };

  } // namespace

  class PaletteInspectorContent final : public VirtualGridAdapter {
  public:
    PaletteInspectorContent(float scale, std::function<void(std::string)> copy)
        : m_scale(scale), m_copy(std::move(copy)) {}

    void setColors(const std::unordered_map<std::string, std::uint32_t>& colors, std::string_view mode) {
      m_colors.clear();
      m_colors.reserve(noctalia::theme::kTokenCount);
      for (const auto token : noctalia::theme::kTokens) {
        PaletteColor color{.name = std::string(token), .argb = std::nullopt, .hex = {}};
        if (const auto it = colors.find(color.name); it != colors.end()) {
          color.argb = it->second;
          color.hex = std::format("#{:06x}", it->second & 0xFFFFFFU);
        }
        m_colors.push_back(std::move(color));
      }
      m_mode = mode;
      syncDescription();
      applyFilter();
    }

    void populateBody(Flex& body) {
      uiAssertNotRendering("PaletteInspectorContent::populateBody");
      m_grid = nullptr;
      m_description = nullptr;
      m_empty = nullptr;
      body.setGap(Style::spaceSm * m_scale);
      body.addChild(
          ui::label({
              .out = &m_description,
              .fontSize = Style::fontSizeCaption * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 0,
          })
      );
      syncDescription();
      auto toolbar = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale, .fillWidth = true});
      toolbar->addChild(
          ui::input({
              .value = m_search,
              .placeholder = i18n::tr("settings.palette-inspector.search-placeholder"),
              .fontSize = Style::fontSizeBody * m_scale,
              .clearButtonEnabled = true,
              .flexGrow = 1.0F,
              .onChange = [this](const std::string& value) {
                m_search = value;
                applyFilter();
                m_grid->scrollToIndex(0);
              },
          })
      );
      toolbar->addChild(
          ui::segmented({
              .options =
                  std::vector<ui::SegmentedOption>{
                      {.label = i18n::tr("settings.palette-inspector.filter-all")},
                      {.label = i18n::tr("settings.palette-inspector.filter-palette")},
                      {.label = i18n::tr("settings.palette-inspector.filter-terminal")},
                  },
              .selectedIndex = m_category,
              .fontSize = Style::fontSizeCaption * m_scale,
              .scale = m_scale,
              .compact = true,
              .onChange = [this](std::size_t index) {
                m_category = index;
                applyFilter();
                m_grid->scrollToIndex(0);
              },
          })
      );
      body.addChild(std::move(toolbar));
      body.addChild(
          ui::label({
              .out = &m_empty,
              .text = i18n::tr("settings.palette-inspector.empty"),
              .fontSize = Style::fontSizeBody * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = m_filtered.empty(),
              .participatesInLayout = m_filtered.empty(),
          })
      );
      body.addChild(
          ui::virtualGridView({
              .out = &m_grid,
              .state = &m_scrollState,
              .contentScale = m_scale,
              .minCellWidth = 224.0F * m_scale,
              .cellHeight = 140.0F * m_scale,
              .squareCells = false,
              .columnGap = Style::spaceMd * m_scale,
              .rowGap = Style::spaceMd * m_scale,
              .adapter = this,
              .flexGrow = 1.0F,
          })
      );
    }

    [[nodiscard]] std::size_t itemCount() const override { return m_filtered.size(); }

    [[nodiscard]] std::unique_ptr<Node> createTile() override {
      auto tile = std::make_unique<PaletteColorTile>(m_scale);
      // Keep card buttons above the grid's cell input area.
      tile->setZIndex(m_grid->focusArea()->zIndex() + 1);
      return tile;
    }

    void bindTile(Node& tile, std::size_t index, bool /*selected*/, bool /*hovered*/) override {
      static_cast<PaletteColorTile&>(tile).bind(m_colors[m_filtered[index]], m_copy);
    }

  private:
    void syncDescription() {
      if (m_description != nullptr) {
        m_description->setText(
            i18n::tr(
                "settings.palette-inspector.description", "mode",
                i18n::tr(m_mode == "light" ? "settings.options.theme.mode.light" : "settings.options.theme.mode.dark")
            )
        );
      }
    }

    void applyFilter() {
      m_filtered.clear();
      const auto query = StringUtils::toLower(m_search);
      for (std::size_t i = 0; i < m_colors.size(); ++i) {
        const bool terminal = m_colors[i].name.starts_with("terminal_");
        if ((m_category == 1 && terminal) || (m_category == 2 && !terminal)) {
          continue;
        }
        if (m_colors[i].name.contains(query) || m_colors[i].hex.contains(query)) {
          m_filtered.push_back(i);
        }
      }
      if (m_empty != nullptr) {
        m_empty->setVisible(m_filtered.empty());
        m_empty->setParticipatesInLayout(m_filtered.empty());
      }
      if (m_grid != nullptr) {
        m_grid->notifyDataChanged();
      }
    }

    float m_scale;
    std::function<void(std::string)> m_copy;
    std::vector<PaletteColor> m_colors;
    std::vector<std::size_t> m_filtered;
    std::string m_mode;
    std::string m_search;
    std::size_t m_category = 0;
    ScrollViewState m_scrollState;
    VirtualGridView* m_grid = nullptr;
    Label* m_description = nullptr;
    Label* m_empty = nullptr;
  };

} // namespace settings

void SettingsWindow::setResolvedPalette(const noctalia::theme::GeneratedPalette& generated, std::string_view mode) {
  const auto& colors = mode == "light" ? generated.light : generated.dark;
  if (m_templateColorMode == mode && m_templateColors == colors) {
    return;
  }
  m_templateColors = colors;
  m_templateColorMode = mode;
  if (m_paletteInspectorContent != nullptr) {
    m_paletteInspectorContent->setColors(m_templateColors, mode);
    m_editorSheetModal->requestLayout();
  }
}

void SettingsWindow::openPaletteInspector() {
  if (m_surface == nullptr || m_templateColorMode.empty()) {
    return;
  }
  if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
    m_editorSheetModal->close();
  }
  if (m_editorSheetModal == nullptr) {
    m_editorSheetModal = std::make_unique<settings::SettingsSheetModal>();
    m_editorSheetModal->initialize(m_modalHost, [this]() { dismissOpenSelectDropdown(); });
  }
  const float scale = uiScale();
  auto content = std::make_shared<settings::PaletteInspectorContent>(scale, [this](std::string value) {
    if (m_clipboardService != nullptr && m_clipboardService->copyText(value)) {
      m_editorSheetModal->setStatusMessage(i18n::tr("settings.palette-inspector.copied", "value", value), false);
    } else {
      m_editorSheetModal->setStatusMessage(i18n::tr("settings.palette-inspector.copy-failed"), true);
    }
  });
  content->setColors(m_templateColors, m_templateColorMode);
  m_paletteInspectorContent = content;
  m_editorSheetModal->open(
      settings::SettingsSheetRequest{
          .sheetTitle = i18n::tr("settings.palette-inspector.title"),
          .populateSheetBody = [content](Flex& body) { content->populateBody(body); },
          .scale = scale,
          .minWidth = 800.0F,
          .maxWidth = 1120.0F,
          .parentFraction = 0.9F,
          .fillParentHeight = true,
          .scrollableBody = false,
          .onClosed = [this]() { m_paletteInspectorContent.reset(); },
      }
  );
}
