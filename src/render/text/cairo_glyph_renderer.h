#pragma once

#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/core/texture_handle.h"

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

// FreeType forwards
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

// Cairo forwards
typedef struct _cairo_font_face cairo_font_face_t;
typedef struct _cairo_font_options cairo_font_options_t;

class RenderBackend;
class TextureManager;

// Direct FreeType + Cairo renderer for single codepoints in a dedicated font
// (tabler.ttf icon font). No Pango, no shaping, no fallback — the icon font
// MUST be used, never substituted.
class CairoGlyphRenderer {
public:
  struct TextMetrics {
    float width = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;    // negative — above baseline
    float bottom = 0.0f; // positive — below baseline
  };

  CairoGlyphRenderer();
  ~CairoGlyphRenderer();

  CairoGlyphRenderer(const CairoGlyphRenderer&) = delete;
  CairoGlyphRenderer& operator=(const CairoGlyphRenderer&) = delete;

  void initialize(const std::string& fontPath, RenderBackend* backend, TextureManager* textures);
  void cleanup();

  void setContentScale(float scale);

  [[nodiscard]] TextMetrics measureGlyph(char32_t codepoint, float fontSize);

  void drawGlyph(
      float surfaceWidth, float surfaceHeight, float x, float baselineY, char32_t codepoint, float fontSize,
      const Color& color, const Mat3& transform
  );

private:
  struct CacheKey {
    char32_t codepoint = 0;
    std::uint32_t sizeQ = 0;
    std::uint16_t scaleQ = 0;

    bool operator==(const CacheKey& other) const noexcept;
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept;
  };

  using LruList = std::list<CacheKey>;

  struct CacheEntry {
    TextureHandle texture;
    int pixelWidth = 0;
    int pixelHeight = 0;
    float baselineXPx = 0.0f; // baseline from left of surface, raster pixels
    float baselinePx = 0.0f;  // baseline from top of surface, raster pixels
    float inkOffsetXPx = 0.0f;
    float inkOffsetYPx = 0.0f;
    TextMetrics metrics;
    std::size_t bytes = 0;
    LruList::iterator lruIt;
  };

  using CacheMap = std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>;

  CacheEntry* lookupOrRasterize(char32_t codepoint, float fontSize);
  void touch(CacheMap::iterator it);
  void evict(CacheMap::iterator it);
  void evictIfNeeded();

  float m_contentScale = 1.0f;

  FT_Library m_ftLibrary = nullptr;
  FT_Face m_face = nullptr;
  cairo_font_face_t* m_cairoFace = nullptr;
  cairo_font_options_t* m_fontOptions = nullptr;
  RenderBackend* m_backend = nullptr;
  TextureManager* m_textureManager = nullptr;

  CacheMap m_cache;
  LruList m_lru;
  std::size_t m_cacheBytes = 0;

  static constexpr std::size_t kMaxCacheEntries = 512;
  static constexpr std::size_t kMaxCacheBytes = 8 * 1024 * 1024;
};
