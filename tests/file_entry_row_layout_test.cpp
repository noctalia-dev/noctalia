#include "core/files/directory_scanner.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/glyph_node.h"
#include "render/scene/input_dispatcher.h"
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
#include <linux/input-event-codes.h>

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
  i18n::Service::instance().init("en");
  StubRenderer renderer;
  using filemanager::FileColumn;
  assert(filemanager::clampFileViewScale(0.5F) == filemanager::kFileViewMinScale);
  assert(filemanager::clampFileViewScale(2.0F) == filemanager::kFileViewMaxScale);
  assert(filemanager::clampFileViewScale(1.000001F) == filemanager::kFileViewDefaultScale);
  for (const float zoom : {0.8F, 1.0F, 1.8F}) {
    const auto metrics = filemanager::fileViewMetrics(zoom);
    FileEntryRow row(1.0F, metrics.listIcon);
    auto& flex = *dynamic_cast<Flex*>(row.children()[1].get());
    auto& nameCell = *flex.children()[0];
    auto& icon = *nameCell.children()[0];
    auto& image = *dynamic_cast<Image*>(nameCell.children()[1].get());
    auto& name = *dynamic_cast<Label*>(nameCell.children()[2].get());
    auto& size = *dynamic_cast<Label*>(flex.children()[1]->children()[0].get());
    auto& date = *dynamic_cast<Label*>(flex.children()[2]->children()[0].get());
    for (const float nameWidth : {160.0F, 260.0F, 480.0F}) {
      filemanager::FileColumns widths;
      widths[FileColumn::Name] = nameWidth;
      row.setColumns(filemanager::kDefaultFileColumns, widths);
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
        near(nameCell.x() + nameX, Style::spaceSm + metrics.listIcon + Style::spaceSm);
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
        near(nameCell.x() + image.x(), Style::spaceSm);
        near(nameCell.y() + image.y(), (metrics.listRowHeight - metrics.listIcon) * 0.5F);
        near(name.x(), nameX);
        near(size.x(), sizeX);
        near(date.x(), dateX);
        assert(name.x() >= image.x() + image.width() + Style::spaceSm);
        assert(nameCell.x() + name.x() + name.width() <= flex.children()[1]->x() + size.x());
      }
    }
  }

  {
    FileEntryRow row(1.0F);
    auto& flex = *dynamic_cast<Flex*>(row.children()[1].get());
    auto* nameCell = flex.children()[0].get();
    auto* sizeCell = flex.children()[1].get();
    auto* typeCell = flex.children()[3].get();
    auto* createdCell = flex.children()[4].get();
    auto* accessedCell = flex.children()[5].get();
    filemanager::FileColumns widths;
    widths[FileColumn::Name] = 220.0F;
    const std::array order{FileColumn::Type, FileColumn::Name, FileColumn::Accessed, FileColumn::Created};
    row.setColumns(order, widths);
    const FileEntry entry{
        .name = "fixture.txt", .absPath = "/nonexistent-nythera-layout-fixture/fixture.txt",
        .accessed = 1700000000, .hasAccessed = true, .type = "text/plain"
    };
    row.bind(renderer, entry, 0, 900.0F, true, false, false);
    assert(!sizeCell->visible());
    near(typeCell->x(), Style::spaceSm);
    near(nameCell->x(), typeCell->x() + widths[FileColumn::Type] + Style::spaceSm);
    near(nameCell->width(), widths[FileColumn::Name]);
    near(accessedCell->x(), nameCell->x() + nameCell->width() + Style::spaceSm);
    near(createdCell->x(), accessedCell->x() + widths[FileColumn::Accessed] + Style::spaceSm);
    assert(dynamic_cast<Label&>(*createdCell->children()[0]).text() == "—");
    assert(dynamic_cast<Label&>(*accessedCell->children()[0]).text() != "—");
    assert(!dynamic_cast<Label&>(*typeCell->children()[0]).text().empty());

    row.setColumns(filemanager::kDefaultFileColumns, widths);
    auto& size = dynamic_cast<Label&>(*sizeCell->children()[0]);
    auto folder = entry;
    folder.isDir = true;
    row.bind(renderer, folder, 0, 900.0F, false, false, false, 0);
    assert(size.text() == "0 items");
    row.bind(renderer, folder, 0, 900.0F, false, false, false, 1);
    assert(size.text() == "1 item");
    row.bind(renderer, folder, 0, 900.0F, false, false, false, 6);
    assert(size.text() == "6 items");
    row.bind(renderer, folder, 0, 900.0F, false, false, false);
    assert(size.text() == "—");
    row.bind(renderer, entry, 0, 900.0F, false, false, false);
    assert(size.text() == "0 B");
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

  ContextMenuControl menu;
  menu.setMenuWidth(200.0F);
  menu.setEntries({{.label = "Disabled", .enabled = false}, {.label = "Open"},
                   {.separator = true}, {.label = "Copy"}});
  assert(menu.highlightedIndex() == 1);
  menu.setHighlightedIndex(ContextMenuControl::NoHighlight);
  menu.layout(renderer);
  assert(menu.highlightedIndex() == ContextMenuControl::NoHighlight);
  assert(!menu.activateHighlighted());
  assert(menu.moveHighlight(1) && menu.highlightedIndex() == 1);
  assert(menu.moveHighlight(1) && menu.highlightedIndex() == 3);
  menu.setHighlightedIndex(ContextMenuControl::NoHighlight);
  assert(menu.moveHighlight(-1) && menu.highlightedIndex() == 3);

  renderer.actionGlyphAvailable = true;
  ContextMenuControl splitMenu;
  splitMenu.setMenuWidth(200.0F);
  int activations = 0;
  int submenus = 0;
  splitMenu.setOnActivate([&](const auto&) { ++activations; });
  splitMenu.setOnSubmenuOpen([&](const auto&, float) { ++submenus; });
  ContextMenuControlEntry split{
      .label = "Open Terminal Here",
      .submenuEntries = {{.label = "Terminal"}},
      .gutterIconNames = {"nythera-test-nonexistent-app-icon"},
      .splitSubmenu = true,
      .activateMainSection = true,
      .gutterFallbackGlyph = "terminal-2",
  };
  splitMenu.setEntries({split});
  splitMenu.layout(renderer);

  const auto* fallbackIcon = dynamic_cast<const FileIcon*>(splitMenu.children().front()->children().at(1).get());
  assert(fallbackIcon != nullptr);
  const auto* fallbackGlyph = dynamic_cast<const Glyph*>(fallbackIcon->children().front().get());
  assert(fallbackGlyph && fallbackGlyph->visible());
  const auto* glyphNode = dynamic_cast<const GlyphNode*>(fallbackGlyph->children().front().get());
  assert(glyphNode && glyphNode->codepoint() == GlyphRegistry::lookup("terminal-2"));

  InputDispatcher input;
  input.setSceneRoot(&splitMenu);
  const float y = (splitMenu.rowTop(0) + splitMenu.rowBottom(0)) * 0.5F;
  const auto click = [&](float pressX, float releaseX) {
    (void)input.pointerButton(pressX, y, BTN_LEFT, true, 1, 1, false);
    (void)input.pointerButton(releaseX, y, BTN_LEFT, false, 2, 2, false);
  };
  click(60, 60);
  assert(activations == 1 && submenus == 0);
  click(182, 182);
  assert(activations == 1 && submenus == 1);
  click(60, 182);
  click(182, 60);
  assert(activations == 1 && submenus == 1);
  assert(splitMenu.activateHighlighted());
  assert(splitMenu.openHighlightedSubmenu());
  assert(activations == 2 && submenus == 2);
  split.activateMainSection = false;
  splitMenu.setEntries({split});
  splitMenu.layout(renderer);
  click(60, 60);
  assert(activations == 2 && submenus == 3);
  split.enabled = false;
  splitMenu.setEntries({split});
  splitMenu.layout(renderer);
  click(60, 60);
  click(182, 182);
  assert(!splitMenu.activateHighlighted() && !splitMenu.openHighlightedSubmenu());
  assert(activations == 2 && submenus == 3);
}
