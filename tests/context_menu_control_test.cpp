#include "core/files/resource_paths.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/text_node.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/cairo_text_renderer.h"
#include "tests/test_check.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <print>
#include <string>
#include <vector>

namespace {
  class MenuRenderer final : public Renderer {
  public:
    MenuRenderer() {
      m_text.initialize(nullptr, nullptr);
      m_glyph.initialize(paths::libraryAssetPath("fonts/noctalia-tabler.ttf").string(), nullptr, nullptr);
    }

    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines, TextAlign align,
        TextEllipsize ellipsize, bool useMarkup
    ) override {
      const auto m = m_text.measure(1.0F, text, fontSize, font, maxWidth, maxLines, align, ellipsize, useMarkup);
      return TextMetrics{
          .width = m.width,
          .left = m.left,
          .right = m.right,
          .top = m.top,
          .bottom = m.bottom,
          .lineCount = m.lineCount,
          .alignOffsetX = m.alignOffsetX
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont& font) override {
      const auto m = m_text.measureFont(1.0F, fontSize, font);
      return TextMetrics{.top = m.top, .bottom = m.bottom};
    }

    void measureTextCursorStops(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<float>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStops(1.0F, text, fontSize, byteOffsets, outStops, font);
    }

    void measureTextCursorStopsWrapped(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
        std::vector<TextCursorStop>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStopsWrapped(1.0F, text, fontSize, byteOffsets, maxWidth, outStops, font);
    }

    void measureTextCharSpans(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<TextCharSpan>& outSpans, const TextFont& font
    ) override {
      m_text.measureCharSpans(1.0F, text, fontSize, byteOffsets, outSpans, font);
    }

    TextMetrics measureGlyph(char32_t codepoint, float fontSize) override {
      const auto metrics = m_glyph.measureGlyph(1.0F, codepoint, fontSize);
      return {
          .width = metrics.width,
          .left = metrics.left,
          .right = metrics.right,
          .top = metrics.top,
          .bottom = metrics.bottom
      };
    }
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    CairoTextRenderer m_text;
    CairoGlyphRenderer m_glyph;
  };

  InputArea& row(ContextMenuControl& menu, std::size_t index) {
    return dynamic_cast<InputArea&>(*menu.children().at(index));
  }
  std::vector<Label*> labels(ContextMenuControl& menu, std::size_t index) {
    std::vector<Label*> found;
    for (const auto& child : row(menu, index).children()) {
      if (auto* label = dynamic_cast<Label*>(child.get())) {
        found.push_back(label);
      }
    }
    return found;
  }
  void verifyColumns(ContextMenuControl& menu, float gap) {
    for (std::size_t i = 0; i < menu.entryCount(); ++i) {
      const auto text = labels(menu, i);
      if (text.size() == 2) {
        TEST_CHECK(text[0]->x() + text[0]->width() + gap <= text[1]->x() + 1.0F);
        TEST_CHECK(text[1]->x() + text[1]->width() <= row(menu, i).width());
      }
    }
  }
} // namespace

int main() {
  try {
    MenuRenderer renderer;
    Palette colors{};
    colors.onSurface = rgba(0.9F, 0.8F, 0.7F);
    colors.onSurfaceVariant = rgba(0.5F, 0.5F, 0.5F);
    colors.hover = rgba(0.1F, 0.7F, 0.3F);
    colors.onHover = rgba(0.05F, 0.08F, 0.1F);
    colors.error = rgba(0.9F, 0.2F, 0.3F);
    setPalette(colors);
    const std::string literal = "<b>café & 文件</b>";
    std::vector<ContextMenuControlEntry> entries{
        {.id = 0, .label = "Actions", .header = true},
        {.id = 1, .label = "Move to Trash", .shortcut = "Delete"},
        {.id = 2, .label = "", .separator = true},
        {.id = 3, .label = "Unavailable", .enabled = false, .shortcut = "Ctrl+U"},
        {.id = 4, .label = literal, .shortcut = "Ctrl+Shift+Long shortcut"},
        {.id = 5, .label = "Empty Trash", .shortcut = "Ctrl+Delete", .destructive = true},
        {.id = 6, .label = "Open With", .hasSubmenu = true, .shortcut = "Enter"},
        {.id = 7, .label = "Disabled tail", .enabled = false},
    };
    ContextMenuControl menu;
    menu.setEntries(entries);
    menu.setHighlightedIndex(0);
    TEST_CHECK(menu.highlightedIndex() == 1);
    const float width = ContextMenuControl::preferredWidth(renderer, entries);
    menu.setMenuWidth(width);
    menu.layout(renderer);
    TEST_CHECK(menu.highlightedIndex() == 1);
    verifyColumns(menu, Style::spaceMd);
    const auto literalLabels = labels(menu, 4);
    TEST_CHECK(literalLabels.front()->text() == literal);
    const auto& node = dynamic_cast<const TextNode&>(*literalLabels.front()->children().front());
    TEST_CHECK(!node.useMarkup());
    TEST_CHECK(labels(menu, 1).front()->color() == colors.onHover);
    TEST_CHECK(labels(menu, 5).front()->color() == colors.error);
    menu.setHighlightedIndex(5);
    TEST_CHECK(labels(menu, 5).front()->color() == colors.error);
    menu.setHighlightedIndex(menu.entryCount());
    TEST_CHECK(!menu.activateHighlighted());
    menu.setMenuWidth(width + 20.0F);
    menu.layout(renderer);
    TEST_CHECK(menu.highlightedIndex() == menu.entryCount());
    TEST_CHECK(menu.moveHighlight(1) && menu.highlightedIndex() == 1);
    TEST_CHECK(menu.moveHighlight(-1) && menu.highlightedIndex() == 6);
    menu.setHighlightedIndex(menu.entryCount());
    TEST_CHECK(menu.moveHighlight(-1) && menu.highlightedIndex() == 6);
    menu.setHighlightedIndex(1);
    TEST_CHECK(menu.moveHighlight(1) && menu.highlightedIndex() == 4);
    int activated = -1, submenu = -1;
    menu.setOnActivate([&](const ContextMenuControlEntry& entry) { activated = entry.id; });
    menu.setOnSubmenuOpen([&](const ContextMenuControlEntry& entry, float) { submenu = entry.id; });
    TEST_CHECK(menu.activateHighlighted() && activated == 4);
    menu.setHighlightedIndex(6);
    TEST_CHECK(menu.activateHighlighted() && submenu == 6);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&menu);
    const float x = row(menu, 4).x() + 12.0F;
    const float y = menu.rowTop(4) + 5.0F;
    dispatcher.pointerEnter(x, y, 1);
    dispatcher.pointerLeave();
    TEST_CHECK(menu.highlightedIndex() == 4);
    menu.setClearHighlightOnLeave(true);
    dispatcher.pointerEnter(x, y, 2);
    dispatcher.pointerLeave();
    TEST_CHECK(menu.highlightedIndex() == menu.entryCount());
    menu.setContentScale(1.5F);
    menu.setMenuWidth(210.0F);
    menu.layout(renderer);
    verifyColumns(menu, Style::spaceMd * 1.5F);
    TEST_CHECK(menu.highlightedIndex() == menu.entryCount());
    menu.setEntries(
        {{.label = "Heading", .header = true},
         {.label = "", .separator = true},
         {.label = "Disabled", .enabled = false}}
    );
    menu.layout(renderer);
    TEST_CHECK(!menu.moveHighlight(1) && !menu.activateHighlighted());
    menu.setEntries({});
    menu.layout(renderer);
    TEST_CHECK(!menu.moveHighlight(-1) && !menu.activateHighlighted());
    ContextMenuControl longShortcut;
    std::vector<ContextMenuControlEntry> shortLabel{{.label = "A", .shortcut = "Ctrl+Alt+Shift+Long accelerator"}};
    longShortcut.setEntries(shortLabel);
    longShortcut.setMenuWidth(ContextMenuControl::preferredWidth(renderer, shortLabel));
    longShortcut.layout(renderer);
    const auto text = labels(longShortcut, 0);
    const float natural = renderer
                              .measureText(
                                  *shortLabel.front().shortcut, Style::fontSizeCaption, {}, 0.0F, 0, TextAlign::Start,
                                  TextEllipsize::End, false
                              )
                              .width;
    TEST_CHECK(text[1]->width() + 1.0F >= natural);
    std::println("context menu checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "context_menu_control_test: {}", error.what());
    return 1;
  }
}
