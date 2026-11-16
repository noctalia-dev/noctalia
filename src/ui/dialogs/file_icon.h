#pragma once

#include "filemanager/directory_metadata.h"
#include "render/core/renderer.h"
#include "system/icon_resolver.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/dialogs/file_artwork.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <gio/gio.h>
#include <memory>
#include <vector>

// Shared file artwork for native file views. All theme inheritance and lookup
// caching belongs to IconResolver; Image owns renderer-compatible texture data.
class FileIcon final : public Node {
  friend class FileIconTestAccess;

public:
  static void invalidateDirectoryMetadata() { s_directoryMetadataGeneration.fetch_add(1, std::memory_order_relaxed); }
  ~FileIcon() override {
    *m_alive = false;
    if (m_metadataCancel)
      g_cancellable_cancel(m_metadataCancel.get());
  }
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
    m_directoryMetadataGeneration = s_directoryMetadataGeneration.load(std::memory_order_relaxed);
    if (m_metadataCancel)
      g_cancellable_cancel(m_metadataCancel.get());
    m_metadataCancel.reset();
    m_loaded = false;
    m_iconNames.clear();
    m_baseIconNames.clear();
    m_hasProvidedNames = false;
    m_normalArtworkLoaded = false;
    ++m_metadataGeneration;
    m_metadataChecked = false;
    m_metadataIcon.clear();
    if (directory)
      queryDirectoryMetadata();
    markLayoutDirty();
  }
  void setThemedIconNames(std::vector<std::string> names) {
    m_baseIconNames = std::move(names);
    m_hasProvidedNames = !m_baseIconNames.empty();
    m_iconNames = filemanager::prioritizeDirectoryMetadataIcon(m_metadataIcon, m_baseIconNames);
    m_loaded = false;
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
    const auto metadataGeneration = s_directoryMetadataGeneration.load(std::memory_order_relaxed);
    if (m_directoryMetadataGeneration != metadataGeneration) {
      m_directoryMetadataGeneration = metadataGeneration;
      if (m_metadataCancel)
        g_cancellable_cancel(m_metadataCancel.get());
      m_metadataCancel.reset();
      m_metadataIcon.clear();
      m_metadataChecked = false;
      m_iconNames = m_baseIconNames;
      m_normalArtworkLoaded = false;
      m_loaded = false;
      if (m_directory)
        queryDirectoryMetadata();
    }
    const float emblemSize = std::clamp(m_size * 0.48F, 14.0F, 32.0F);
    if (!m_loaded || m_generation != generation || m_pixels != pixels) {
      std::string resolved;
      if (!m_normalArtworkLoaded) {
        const auto artwork = m_path.empty() ? FileArtwork{.names = {m_directory ? "folder" : "text-x-generic"}}
                                            : queryFileArtwork(m_path, m_directory);
        m_symlink = artwork.symlink;
        m_broken = artwork.broken;
        if (!m_hasProvidedNames)
          m_baseIconNames = artwork.names;
        m_normalArtworkLoaded = true;
        m_iconNames = filemanager::prioritizeDirectoryMetadataIcon(m_metadataIcon, m_baseIconNames);
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
  void queryDirectoryMetadata() {
    if (m_metadataChecked || m_path.empty() || m_path.string().starts_with("trash:/"))
      return;
    m_metadataChecked = true;
    struct Request {
      FileIcon* icon;
      std::weak_ptr<bool> alive;
      std::uint64_t generation;
      std::filesystem::path directory;
      std::shared_ptr<GCancellable> cancel;
      std::uint64_t directoryGeneration;
    };
    struct StreamRequest {
      Request request;
      GFileInputStream* stream;
      std::array<char, 65537> contents{};
    };
    m_metadataCancel = std::shared_ptr<GCancellable>(g_cancellable_new(), g_object_unref);
    auto* data = new Request{this,   m_alive,          m_metadataGeneration,
                             m_path, m_metadataCancel, s_directoryMetadataGeneration.load(std::memory_order_relaxed)};
    GFile* file = g_file_new_for_path((m_path / ".directory").c_str());
    g_file_query_info_async(
        file, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
        G_PRIORITY_DEFAULT, data->cancel.get(),
        [](GObject* source, GAsyncResult* result, gpointer userData) {
          std::unique_ptr<Request> queryRequest(static_cast<Request*>(userData));
          GError* error = nullptr;
          GFileInfo* info = g_file_query_info_finish(G_FILE(source), result, &error);
          const bool bounded = isBoundedDirectoryMetadataFile(info);
          g_clear_object(&info);
          g_clear_error(&error);
          if (!bounded)
            return;
          g_file_read_async(
              G_FILE(source), G_PRIORITY_DEFAULT, queryRequest->cancel.get(),
              [](GObject* metadataFile, GAsyncResult* readResult, gpointer readData) {
                std::unique_ptr<Request> openRequest(static_cast<Request*>(readData));
                GError* readError = nullptr;
                auto* stream = g_file_read_finish(G_FILE(metadataFile), readResult, &readError);
                g_clear_error(&readError);
                if (!stream)
                  return;
                auto* streamRequest = new StreamRequest{*openRequest, stream, {}};
                g_input_stream_read_all_async(
                    G_INPUT_STREAM(stream), streamRequest->contents.data(), streamRequest->contents.size(),
                    G_PRIORITY_DEFAULT, openRequest->cancel.get(),
                    [](GObject* input, GAsyncResult* bytesResult, gpointer streamData) {
                      std::unique_ptr<StreamRequest> readRequest(static_cast<StreamRequest*>(streamData));
                      gsize length = 0;
                      GError* bytesError = nullptr;
                      const bool read =
                          g_input_stream_read_all_finish(G_INPUT_STREAM(input), bytesResult, &length, &bytesError);
                      g_clear_error(&bytesError);
                      const auto alive = readRequest->request.alive.lock();
                      auto* icon = readRequest->request.icon;
                      if (alive
                          && *alive
                          && icon->m_metadataGeneration == readRequest->request.generation
                          && readRequest->request.directoryGeneration
                              == s_directoryMetadataGeneration.load(std::memory_order_relaxed)
                          && icon->m_path == readRequest->request.directory
                          && read
                          && length <= 65536) {
                        if (const auto metadataIcon =
                                filemanager::directoryMetadataIcon(readRequest->contents.data(), length)) {
                          auto names = filemanager::directoryMetadataIconNames(
                              readRequest->contents.data(), length,
                              icon->m_baseIconNames.empty() ? std::vector<std::string>{"folder", "text-x-generic"}
                                                            : icon->m_baseIconNames
                          );
                          icon->m_metadataIcon = *metadataIcon;
                          icon->m_iconNames = std::move(names);
                          icon->m_loaded = false;
                          icon->markLayoutDirty();
                        }
                      }
                      // Transfer ownership before forming call arguments: argument
                      // evaluation order must not release readRequest before dereferencing it.
                      auto* closingRequest = readRequest.release();
                      g_input_stream_close_async(
                          G_INPUT_STREAM(closingRequest->stream), G_PRIORITY_DEFAULT, nullptr,
                          [](GObject* closing, GAsyncResult* closeResult, gpointer closeData) {
                            std::unique_ptr<StreamRequest> closeRequest(static_cast<StreamRequest*>(closeData));
                            GError* closeError = nullptr;
                            g_input_stream_close_finish(G_INPUT_STREAM(closing), closeResult, &closeError);
                            g_clear_error(&closeError);
                            g_object_unref(closeRequest->stream);
                          },
                          closingRequest
                      );
                    },
                    streamRequest
                );
              },
              new Request(*queryRequest)
          );
        },
        data
    );
    g_object_unref(file);
  }
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Box* m_emblem = nullptr;
  Glyph* m_emblemGlyph = nullptr;
  std::filesystem::path m_path;
  std::vector<std::string> m_iconNames;
  std::vector<std::string> m_baseIconNames;
  bool m_directory = false;
  bool m_loaded = false;
  bool m_hasProvidedNames = false;
  bool m_normalArtworkLoaded = false;
  bool m_symlink = false;
  bool m_broken = false;
  bool m_symbolic = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  std::shared_ptr<GCancellable> m_metadataCancel;
  std::uint64_t m_metadataGeneration = 0;
  std::uint64_t m_directoryMetadataGeneration = 0;
  bool m_metadataChecked = false;
  std::string m_metadataIcon;
  Color m_color = rgba(1.0F, 1.0F, 1.0F, 1.0F);
  float m_size = 28.0F;
  int m_pixels = 0;
  std::uint64_t m_generation = 0;
  inline static std::atomic_uint64_t s_directoryMetadataGeneration{0};
};
