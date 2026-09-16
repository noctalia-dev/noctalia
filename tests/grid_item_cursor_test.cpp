#include "render/core/renderer.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/controls/virtual_grid_view.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
#include <string_view>
#include <vector>

namespace {

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view, float fontSize, FontWeight, float, int, TextAlign, std::string_view, TextEllipsize, bool
    ) override {
      return TextMetrics{.bottom = fontSize};
    }

    TextMetrics measureFont(float fontSize, FontWeight) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, FontWeight
    ) override {}

    TextMetrics measureGlyph(char32_t, float) override { return TextMetrics{.width = 18.0F}; }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  class StubAdapter final : public VirtualGridAdapter {
  public:
    [[nodiscard]] std::size_t itemCount() const override { return 4; }
    [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<Node>(); }
    void bindTile(Node&, std::size_t, bool, bool) override {}
  };

  bool expect(bool condition, std::string_view what) {
    std::println("{}: {}", condition ? "ok" : "FAIL", what);
    return condition;
  }

} // namespace

int main() {
  StubRenderer renderer;
  StubAdapter adapter;

  VirtualGridView grid;
  grid.setFillWidth(false);
  grid.setFillHeight(false);
  grid.setColumns(2);
  grid.setSquareCells(false);
  grid.setCellHeight(50.0F);
  grid.setColumnGap(20.0F);
  grid.setRowGap(20.0F);
  grid.setItemCursorShape(21);
  grid.setAdapter(&adapter);
  grid.setSize(240.0F, 120.0F);
  grid.layout(renderer);

  InputDispatcher dispatcher;
  std::uint32_t cursor = 0;
  dispatcher.setCursorShapeCallback([&cursor](std::uint32_t, std::uint32_t shape) { cursor = shape; });
  dispatcher.setSceneRoot(&grid);

  float cell0X = 0.0F;
  float cell0Y = 0.0F;
  float cell1X = 0.0F;
  float cell1Y = 0.0F;
  bool ok = expect(grid.absoluteAnchorForIndex(0, cell0X, cell0Y), "cell 0 anchor resolved");
  ok = expect(grid.absoluteAnchorForIndex(1, cell1X, cell1Y), "cell 1 anchor resolved") && ok;

  dispatcher.pointerEnter(cell0X, cell0Y, 1);
  ok = expect(cursor == 21, "item cursor shape applies over a cell") && ok;

  dispatcher.pointerMotion((cell0X + cell1X) * 0.5F, cell0Y, 2);
  ok = expect(cursor == 1, "gutter between cells keeps the default cursor") && ok;

  dispatcher.pointerMotion(cell1X, cell1Y, 3);
  ok = expect(cursor == 21, "item cursor shape applies to every cell") && ok;

  grid.setItemCursorShape(24);
  dispatcher.pointerMotion(cell1X + 1.0F, cell1Y, 4);
  ok = expect(cursor == 24, "shape change reaches already-created cell areas") && ok;

  dispatcher.pointerLeave();
  ok = expect(cursor == 1, "leaving the surface restores the default cursor") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
