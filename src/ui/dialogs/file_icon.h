#pragma once

#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "system/icon_resolver.h"
#include "render/core/renderer.h"
#include <gio/gio.h>
#include <cmath>
#include <filesystem>

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
  }
  void setFile(const std::filesystem::path& path, bool directory) {
    if (m_path == path && m_directory == directory) return;
    m_path = path;
    m_directory = directory;
    m_loaded = false;
    m_iconNames.clear();
    markLayoutDirty();
  }
  void setGlyphSize(float size) { m_size = size; m_glyph->setGlyphSize(size); markLayoutDirty(); }
  void setColor(const Color& color) {
    m_glyph->setColor(color);
    m_image->setOpacity(color.a);
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
    if (!m_loaded || m_generation != generation || m_pixels != pixels) {
      std::string resolved;
      if (m_iconNames.empty()) {
        GFile* file = g_file_new_for_path(m_path.c_str());
        GFileInfo* info = g_file_query_info(file, "standard::icon,standard::content-type",
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
        GIcon* icon = info == nullptr ? nullptr : g_file_info_get_icon(info);
        gchar* type = nullptr;
        GIcon* fallback = nullptr;
        if (icon == nullptr) {
          type = g_content_type_guess(m_path.filename().c_str(), nullptr, 0, nullptr);
          fallback = type == nullptr ? nullptr : g_content_type_get_icon(type);
          icon = fallback;
        }
        if (icon != nullptr && G_IS_THEMED_ICON(icon)) {
          const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
          for (auto name = names; name != nullptr && *name != nullptr; ++name) m_iconNames.emplace_back(*name);
        }
        if (fallback != nullptr) g_object_unref(fallback);
        g_free(type);
        if (info != nullptr) g_object_unref(info);
        g_object_unref(file);
        m_iconNames.push_back(m_directory ? "folder" : "text-x-generic");
      }
      for (const auto& name : m_iconNames) {
        resolved = resolver.resolve(name, pixels);
        if (!resolved.empty()) break;
      }
      if (resolved.empty()) resolved = resolver.resolve("unknown", pixels);
      const bool loaded = !resolved.empty() && m_image->setSourceFile(renderer, resolved, static_cast<int>(std::ceil(m_size)));
      if (!loaded) m_image->clear(renderer);
      m_image->setVisible(loaded);
      m_glyph->setVisible(!loaded);
      m_glyph->setGlyph(m_directory ? "folder" : "file");
      m_glyph->setGlyphSize(m_size);
      m_loaded = true;
      m_generation = generation;
      m_pixels = pixels;
    }
    setSize(m_size, m_size);
    m_image->setSize(m_size, m_size);
    if (m_glyph->visible()) {
      m_glyph->measure(renderer);
      m_glyph->setPosition((m_size - m_glyph->width()) * 0.5F, (m_size - m_glyph->height()) * 0.5F);
    }
  }
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  std::filesystem::path m_path;
  std::vector<std::string> m_iconNames;
  bool m_directory = false;
  bool m_loaded = false;
  float m_size = 28.0F;
  int m_pixels = 0;
  std::uint64_t m_generation = 0;
};
