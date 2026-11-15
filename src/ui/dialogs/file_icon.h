#pragma once

#include "render/core/renderer.h"
#include "system/icon_resolver.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/dialogs/file_artwork.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <gio/gio.h>
#include <vector>

// Shared file artwork for native file views. All theme inheritance and lookup
// caching belongs to IconResolver; Image owns renderer-compatible texture data.
class FileIcon final : public Node {
public:
  FileIcon() {
    auto glyph = std::make_unique<Glyph>();
    m_glyph = static_cast<Glyph*>(addChild(std::move(glyph)));
    auto image = std::make_unique<Image>();
    image->setFit(ImageFit::Contain);
    m_image = static_cast<Image*>(addChild(std::move(image)));
    m_image->setVisible(false);
    auto emblem = std::make_unique<Box>();
    emblem->setRadius(3.0F);
    m_emblem = static_cast<Box*>(addChild(std::move(emblem)));
    m_emblem->setVisible(false);
    auto emblemGlyph = std::make_unique<Glyph>();
    m_emblemGlyph = static_cast<Glyph*>(addChild(std::move(emblemGlyph)));
    m_emblemGlyph->setVisible(false);
  }
  void setFile(const std::filesystem::path& path, bool directory) {
    if (m_path == path && m_directory == directory)
      return;
    m_path = path;
    m_directory = directory;
    m_loaded = false;
    m_iconNames.clear();
    m_hasProvidedNames = false;
    markLayoutDirty();
  }
  void setThemedIconNames(std::vector<std::string> names) {
    m_iconNames = std::move(names);
    m_hasProvidedNames = !m_iconNames.empty();
    m_loaded = false;
    m_symlink = false;
    m_broken = false;
    markLayoutDirty();
  }
  void setGlyphSize(float size) {
    if (m_size == size)
      return;
    m_size = size;
    m_glyph->setGlyphSize(size);
    markLayoutDirty();
  }
  void setColor(const Color& color) {
    m_color = color;
    m_glyph->setColor(color);
    m_image->setOpacity(color.a);
    m_image->setForegroundTint(
        m_symbolic ? std::optional<ColorSpec>(fixedColorSpec(withAlpha(color, 1.0F))) : std::nullopt
    );
    m_emblem->setOpacity(color.a);
    m_emblemGlyph->setOpacity(color.a);
  }
  void measure(Renderer& renderer) { layout(renderer); }

private:
  LayoutSize doMeasure(Renderer&, const LayoutConstraints& constraints) override {
    return constraints.constrain({m_size, m_size});
  }
  void doLayout(Renderer& renderer) override {
    static IconResolver resolver(true);
    const int pixels = std::max(1, static_cast<int>(std::ceil(m_size * renderer.renderScale())));
    const auto generation = IconResolver::themeGeneration();
    const float emblemSize = std::clamp(m_size * 0.48F, 14.0F, 32.0F);
    if (!m_loaded || m_generation != generation || m_pixels != pixels) {
      std::string resolved;
      if (!m_hasProvidedNames) {
        const auto artwork = queryFileArtwork(m_path, m_directory);
        m_iconNames = artwork.names;
        m_symlink = artwork.symlink;
        m_broken = artwork.broken;
      }
      for (const auto& name : m_iconNames) {
        resolved = resolver.resolve(name, pixels);
        if (!resolved.empty())
          break;
      }
      if (resolved.empty())
        resolved = resolver.resolve("unknown", pixels);
      const bool loaded =
          !resolved.empty() && m_image->setSourceFile(renderer, resolved, static_cast<int>(std::ceil(m_size)));
      m_symbolic = std::filesystem::path(resolved).stem().string().ends_with("-symbolic");
      m_image->setForegroundTint(
          m_symbolic ? std::optional<ColorSpec>(fixedColorSpec(withAlpha(m_color, 1.0F))) : std::nullopt
      );
      if (!loaded)
        m_image->clear(renderer);
      m_image->setVisible(loaded);
      m_glyph->setVisible(!loaded);
      const bool trash = m_hasProvidedNames && !m_iconNames.empty() && m_iconNames.front().starts_with("user-trash");
      const bool home = m_hasProvidedNames && !m_iconNames.empty() && m_iconNames.front() == "user-home";
      m_glyph->setGlyph(trash ? "trash" : home ? "home" : m_directory && !m_broken ? "folder" : "file");
      m_glyph->setGlyphSize(m_size);
      // A palette-backed chain is legible even over detailed themed artwork.
      // Theme emblems often use a tiny arrow that disappears at list sizes.
      m_emblem->setVisible(m_symlink);
      m_emblemGlyph->setVisible(m_symlink);
      m_emblem->setFill(colorSpecFromRole(m_broken ? ColorRole::ErrorContainer : ColorRole::SurfaceContainerHigh));
      m_emblem->setBorder(colorSpecFromRole(m_broken ? ColorRole::Error : ColorRole::OutlineVariant), 1.0F);
      m_emblemGlyph->setColor(colorSpecFromRole(m_broken ? ColorRole::OnErrorContainer : ColorRole::OnSurface));
      m_emblemGlyph->setGlyph(m_broken ? "link-off" : "link");
      m_emblemGlyph->setGlyphSize(emblemSize - 2.0F);
      m_loaded = true;
      m_generation = generation;
      m_pixels = pixels;
    }
    setSize(m_size, m_size);
    m_image->setSize(m_size, m_size);
    m_image->layout(renderer);
    m_emblem->setSize(emblemSize, emblemSize);
    m_emblem->setPosition(0.0F, std::max(0.0F, m_size - emblemSize));
    if (m_emblem->visible())
      m_emblem->layout(renderer);
    if (m_emblemGlyph->visible()) {
      m_emblemGlyph->measure(renderer);
      m_emblemGlyph->setPosition(
          (emblemSize - m_emblemGlyph->width()) * 0.5F,
          std::max(0.0F, m_size - emblemSize) + (emblemSize - m_emblemGlyph->height()) * 0.5F
      );
    }
    if (m_glyph->visible()) {
      m_glyph->measure(renderer);
      m_glyph->setPosition((m_size - m_glyph->width()) * 0.5F, (m_size - m_glyph->height()) * 0.5F);
    }
  }
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Box* m_emblem = nullptr;
  Glyph* m_emblemGlyph = nullptr;
  std::filesystem::path m_path;
  std::vector<std::string> m_iconNames;
  bool m_directory = false;
  bool m_loaded = false;
  bool m_hasProvidedNames = false;
  bool m_symlink = false;
  bool m_broken = false;
  bool m_symbolic = false;
  Color m_color = rgba(1.0F, 1.0F, 1.0F, 1.0F);
  float m_size = 28.0F;
  int m_pixels = 0;
  std::uint64_t m_generation = 0;
};
