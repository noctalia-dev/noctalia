#include "config/config_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/bar/widgets/tray_widget.h"
#include "tests/test_check.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int maxLines, TextAlign, TextEllipsize,
        bool
    ) override {
      constexpr float kAdvance = 10.0F;
      const float natural = static_cast<float>(text.size()) * kAdvance;
      return TextMetrics{
          .width = natural,
          .right = natural,
          .bottom = fontSize,
          .lineCount = text.empty() ? 0 : 1,
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}

    TextMetrics measureGlyph(char32_t, float size) override {
      return TextMetrics{
          .width = size,
          .right = size,
          .bottom = size,
      };
    }

    float renderScale() const noexcept override { return 1.0F; }

    TextureManager& textureManager() override { std::abort(); }
  };

} // namespace

class TrayWidgetTestAccess {
public:
  static void setItems(TrayWidget& widget, Renderer& renderer, std::vector<TrayItemInfo> items) {
    widget.m_items = std::move(items);
    widget.rebuild(renderer);
    widget.m_rebuildPending = false;
  }

  static Flex* container(const TrayWidget& widget) { return widget.m_container; }
  static Flex* unpinnedContainer(const TrayWidget& widget) { return widget.m_unpinnedContainer; }
  static InputArea* unpinnedClip(const TrayWidget& widget) { return widget.m_unpinnedClip; }
  static Glyph* peekGlyph(const TrayWidget& widget) { return widget.m_peekGlyph; }
  static std::size_t pinnedItemCount(const TrayWidget& widget) { return widget.m_pinnedItemCount; }
  static std::size_t unpinnedItemCount(const TrayWidget& widget) { return widget.m_unpinnedItemCount; }
};

int main() {
  StubRenderer renderer;
  ConfigService config;

  const TrayItemInfo pinnedItem{.id = "app-pinned", .title = "Pinned App"};
  const TrayItemInfo unpinnedItem{.id = "app-unpinned", .title = "Unpinned App"};

  // Scenario 1: peek = true, zero items.
  {
    TrayWidget widget(
        config, nullptr,
        TrayWidget::Options{
            .pinnedItems = {"app-pinned"},
            .peek = true,
        }
    );
    widget.create();
    TrayWidgetTestAccess::setItems(widget, renderer, {});
    widget.layout(renderer, 200.0F, 32.0F);

    TEST_CHECK(!widget.root()->visible());
    TEST_CHECK(widget.root()->width() == 0.0F);
  }

  // Scenario 2: peek = true, only pinned items exist (0 unpinned items).
  // The peek glyph and unpinned clip are suppressed; root matches pinned container size.
  {
    TrayWidget widget(
        config, nullptr,
        TrayWidget::Options{
            .pinnedItems = {"app-pinned"},
            .peek = true,
        }
    );
    widget.create();
    TrayWidgetTestAccess::setItems(widget, renderer, {pinnedItem});
    widget.layout(renderer, 200.0F, 32.0F);

    TEST_CHECK(widget.root()->visible());
    TEST_CHECK(TrayWidgetTestAccess::pinnedItemCount(widget) == 1);
    TEST_CHECK(TrayWidgetTestAccess::unpinnedItemCount(widget) == 0);
    TEST_CHECK(TrayWidgetTestAccess::container(widget)->visible());
    TEST_CHECK(!TrayWidgetTestAccess::unpinnedClip(widget)->visible());
    TEST_CHECK(!TrayWidgetTestAccess::peekGlyph(widget)->visible());
    TEST_CHECK(widget.root()->width() == TrayWidgetTestAccess::container(widget)->width());
    TEST_CHECK(widget.root()->width() > 0.0F);
  }

  // Scenario 3: peek = true, glyph_position = "after", both pinned and unpinned items exist.
  // Pinned items stay permanently in place; glyph sits adjacent when collapsed and slides outward on reveal.
  {
    TrayWidget widget(
        config, nullptr,
        TrayWidget::Options{
            .pinnedItems = {"app-pinned"},
            .glyphPosition = GlyphPosition::After,
            .peek = true,
            .customItemSize = 20.0F,
        }
    );
    widget.create();
    TrayWidgetTestAccess::setItems(widget, renderer, {pinnedItem, unpinnedItem});

    TEST_CHECK(widget.peekEnabled());
    TEST_CHECK(TrayWidgetTestAccess::pinnedItemCount(widget) == 1);
    TEST_CHECK(TrayWidgetTestAccess::unpinnedItemCount(widget) == 1);

    // Collapsed state (peekProgress = 0.0F)
    widget.setPeekProgress(0.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    auto* container = TrayWidgetTestAccess::container(widget);
    auto* unpinnedClip = TrayWidgetTestAccess::unpinnedClip(widget);
    auto* peekGlyph = TrayWidgetTestAccess::peekGlyph(widget);

    TEST_CHECK(widget.root()->visible());
    TEST_CHECK(container->visible());
    TEST_CHECK(unpinnedClip->visible());
    TEST_CHECK(peekGlyph->visible());

    const float pinnedWidth = container->width();
    const float unpinnedWidth = TrayWidgetTestAccess::unpinnedContainer(widget)->width();
    const float glyphWidth = peekGlyph->width();

    TEST_CHECK(container->x() == 0.0F);
    TEST_CHECK(unpinnedClip->width() == 0.0F);
    TEST_CHECK(peekGlyph->x() > pinnedWidth);
    TEST_CHECK(widget.root()->width() == peekGlyph->x() + glyphWidth);

    const float collapsedWidth = widget.root()->width();

    // Expanded state (peekProgress = 1.0F)
    widget.setPeekProgress(1.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    // Pinned container stays at x = 0.
    TEST_CHECK(container->x() == 0.0F);
    // Unpinned clip opens between pinned container and glyph.
    TEST_CHECK(unpinnedClip->x() > pinnedWidth);
    TEST_CHECK(unpinnedClip->width() == unpinnedWidth);
    TEST_CHECK(peekGlyph->x() > unpinnedClip->x() + unpinnedWidth);
    TEST_CHECK(widget.root()->width() == peekGlyph->x() + glyphWidth);
    TEST_CHECK(widget.root()->width() > collapsedWidth);
  }

  // Scenario 4: peek = true, glyph_position = "before", both pinned and unpinned items exist.
  // Glyph is at leading end, pinned items stay at trailing end; unpinned items peek between them.
  {
    TrayWidget widget(
        config, nullptr,
        TrayWidget::Options{
            .pinnedItems = {"app-pinned"},
            .glyphPosition = GlyphPosition::Before,
            .peek = true,
            .customItemSize = 20.0F,
        }
    );
    widget.create();
    TrayWidgetTestAccess::setItems(widget, renderer, {pinnedItem, unpinnedItem});

    widget.setPeekProgress(0.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    auto* container = TrayWidgetTestAccess::container(widget);
    auto* unpinnedClip = TrayWidgetTestAccess::unpinnedClip(widget);
    auto* peekGlyph = TrayWidgetTestAccess::peekGlyph(widget);

    TEST_CHECK(peekGlyph->x() == 0.0F);
    TEST_CHECK(container->x() > peekGlyph->width());
    TEST_CHECK(unpinnedClip->width() == 0.0F);
    TEST_CHECK(widget.root()->width() == container->x() + container->width());
    TEST_CHECK(container->x() == widget.root()->width() - container->width());

    widget.setPeekProgress(1.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    // Peek glyph stays at leading end (x = 0).
    TEST_CHECK(peekGlyph->x() == 0.0F);
    // Unpinned clip opens between peek glyph and pinned items.
    TEST_CHECK(unpinnedClip->x() > peekGlyph->width());
    TEST_CHECK(unpinnedClip->width() == TrayWidgetTestAccess::unpinnedContainer(widget)->width());
    TEST_CHECK(container->x() > unpinnedClip->x() + unpinnedClip->width());
    TEST_CHECK(widget.root()->width() == container->x() + container->width());
    // Pinned items remain anchored at the trailing end.
    TEST_CHECK(container->x() == widget.root()->width() - container->width());
  }

  // Scenario 5: peek = true, glyph_position = "after", only unpinned items exist (0 pinned items).
  {
    TrayWidget widget(
        config, nullptr,
        TrayWidget::Options{
            .glyphPosition = GlyphPosition::After,
            .peek = true,
            .customItemSize = 20.0F,
        }
    );
    widget.create();
    TrayWidgetTestAccess::setItems(widget, renderer, {unpinnedItem});

    TEST_CHECK(widget.peekEnabled());
    widget.setPeekProgress(0.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    auto* peekGlyph = TrayWidgetTestAccess::peekGlyph(widget);
    TEST_CHECK(peekGlyph->x() == 0.0F);
    TEST_CHECK(widget.root()->width() == peekGlyph->width());

    widget.setPeekProgress(1.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    auto* unpinnedClip = TrayWidgetTestAccess::unpinnedClip(widget);
    TEST_CHECK(unpinnedClip->x() == 0.0F);
    TEST_CHECK(peekGlyph->x() > unpinnedClip->width());
    TEST_CHECK(widget.root()->width() == peekGlyph->x() + peekGlyph->width());
  }

  // Scenario 6: peek = true, glyph_position = "before", only unpinned items exist (0 pinned items).
  {
    TrayWidget widget(
        config, nullptr,
        TrayWidget::Options{
            .glyphPosition = GlyphPosition::Before,
            .peek = true,
            .customItemSize = 20.0F,
        }
    );
    widget.create();
    TrayWidgetTestAccess::setItems(widget, renderer, {unpinnedItem});

    TEST_CHECK(widget.peekEnabled());
    widget.setPeekProgress(0.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    auto* peekGlyph = TrayWidgetTestAccess::peekGlyph(widget);
    TEST_CHECK(peekGlyph->x() == 0.0F);
    TEST_CHECK(widget.root()->width() == peekGlyph->width());

    widget.setPeekProgress(1.0F);
    widget.layout(renderer, 200.0F, 32.0F);

    auto* unpinnedClip = TrayWidgetTestAccess::unpinnedClip(widget);
    TEST_CHECK(peekGlyph->x() == 0.0F);
    TEST_CHECK(unpinnedClip->x() > peekGlyph->width());
    TEST_CHECK(unpinnedClip->width() == TrayWidgetTestAccess::unpinnedContainer(widget)->width());
    TEST_CHECK(widget.root()->width() == unpinnedClip->x() + unpinnedClip->width());
  }

  return 0;
}
