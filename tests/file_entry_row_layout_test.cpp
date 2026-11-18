#include "core/files/directory_scanner.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/text/glyph_registry.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_icon.h"
#include "ui/dialogs/file_view_metrics.h"
#include "ui/style.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>

namespace {
  class StubTextures final : public TextureManager {
  public:
    TextureHandle loadFromFile(const std::string&, int, bool) override { return {}; }
    TextureHandle loadFromEncodedBytes(const std::uint8_t*, std::size_t, bool) override { return {}; }
    TextureHandle loadFromRgba(const std::uint8_t*, int, int, bool) override { return {}; }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override { return {}; }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  class StubRenderer final : public Renderer {
  public:
    bool actionGlyphAvailable = true;

    TextMetrics measureText(
        std::string_view text, float size, const TextFont&, float maxWidth, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * size * 0.5F;
      const float width = maxWidth > 0.0F ? std::min(natural, maxWidth) : natural;
      return {.width = width, .right = width, .bottom = size, .lineCount = 1};
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.bottom = size}; }
    TextMetrics measureGlyph(char32_t, float size) override {
      return actionGlyphAvailable ? TextMetrics{.width = size, .right = size, .bottom = size} : TextMetrics{};
    }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { return m_textures; }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    StubTextures m_textures;
  };

  void near(float actual, float expected) { assert(std::abs(actual - expected) < 0.001F); }
} // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  assert(::setenv("NYTHERA_ASSETS_DIR", argv[1], 1) == 0);
  StubRenderer renderer;
  for (const float zoom : {0.8F, 1.0F, 1.8F}) {
    const auto metrics = filemanager::fileViewMetrics(zoom);
    FileEntryRow row(1.0F, metrics.listIcon);
    auto& flex = *dynamic_cast<Flex*>(row.children()[1].get());
    auto& icon = *flex.children()[0];
    auto& image = *dynamic_cast<Image*>(flex.children()[1].get());
    auto& name = *dynamic_cast<Label*>(flex.children()[2].get());
    auto& size = *dynamic_cast<Label*>(flex.children()[4].get());
    auto& date = *dynamic_cast<Label*>(flex.children()[5].get());
    for (const float nameWidth : {160.0F, 260.0F, 480.0F}) {
      row.setColumnWidths(nameWidth, 96.0F, 152.0F);
      // Cover recycled rows, folder/file artwork, and filenames exceeding the
      // assigned column. No texture is rendered by this geometry-only test.
      for (const bool directory : {false, true}) {
        const FileEntry entry{
            .name = std::string(100, 'x') + (directory ? "" : ".png"),
            .absPath = directory ? "/nonexistent-nythera-layout-fixture/folder"
                                 : "/nonexistent-nythera-layout-fixture/image.png",
            .isDir = directory,
        };
        row.bind(renderer, entry, 0, 900.0F, true, true, false);
        assert(icon.visible() && !image.visible());
        const float nameX = name.x();
        const float sizeX = size.x();
        const float dateX = date.x();
        near(nameX, Style::spaceSm + metrics.listIcon + Style::spaceSm);
        near(name.width(), nameWidth - metrics.listIcon - Style::spaceSm);
        near(row.rowHeight(), metrics.listRowHeight);
        near(icon.width(), metrics.listIcon);
        if (directory)
          continue;
        // Model the visibility change made by refreshThumbnail when an async
        // thumbnail becomes ready, exercising the actual retained Flex tree.
        icon.setVisible(false);
        image.setVisible(true);
        row.layout(renderer);
        near(image.width(), metrics.listIcon);
        near(image.height(), metrics.listIcon);
        near(image.x(), Style::spaceSm);
        near(image.y(), (metrics.listRowHeight - metrics.listIcon) * 0.5F);
        near(name.x(), nameX);
        near(size.x(), sizeX);
        near(date.x(), dateX);
        assert(name.x() >= image.x() + image.width() + Style::spaceSm);
        assert(name.x() + name.width() <= size.x());
      }
    }
  }

  const auto actionIcon = [&](ContextMenuControl& menu, std::string glyphName) -> const Node& {
    menu.setMenuWidth(200.0F);
    menu.setEntries({{.id = 1,
                      .label = "Copy",
                      .actionIconNames = {"edit-copy-symbolic", "edit-copy"},
                      .actionGlyph = std::move(glyphName)}});
    menu.layout(renderer);
    return *menu.children().front()->children().at(1);
  };

  assert(GlyphRegistry::contains("copy"));
  ContextMenuControl glyphMenu;
  const auto& glyph = actionIcon(glyphMenu, "copy");
  assert(dynamic_cast<const Glyph*>(&glyph) != nullptr);
  near(glyph.width(), 16.0F);
  near(glyph.height(), 16.0F);

  ContextMenuControl unknownNameMenu;
  const auto& unknownNameIcon = actionIcon(unknownNameMenu, "missing-action-icon-test");
  assert(dynamic_cast<const FileIcon*>(&unknownNameIcon) != nullptr);
  near(unknownNameIcon.x(), glyph.x());
  near(unknownNameIcon.y(), glyph.y());
  near(unknownNameIcon.width(), glyph.width());
  near(unknownNameIcon.height(), glyph.height());

  renderer.actionGlyphAvailable = false;
  ContextMenuControl missingCodepointMenu;
  assert(dynamic_cast<const FileIcon*>(&actionIcon(missingCodepointMenu, "copy")) != nullptr);
}
