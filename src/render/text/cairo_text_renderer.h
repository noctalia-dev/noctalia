#pragma once

#include "render/core/renderer.h"
#include "render/core/texture_handle.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid dragging Pango headers into every TU.
typedef struct _PangoContext PangoContext;
typedef struct _PangoFontMap PangoFontMap;
typedef struct _PangoLayout PangoLayout;

class RenderBackend;
class TextureManager;
struct Color;
struct Mat3;

// Pango/Cairo-backed text renderer.
//
// Rasterizes a shaped PangoLayout into an ARGB32 Cairo surface, uploads it as
// a premultiplied RGBA texture, and submits glyph quads to RenderBackend.
// Handles Latin, CJK, Arabic, BiDi, and COLR v1 emoji via fontconfig fallback.
//
// measure() and truncate() do not require a current render context.
// draw() needs the render backend context current (creates/binds textures).
class CairoTextRenderer {
public:
  struct TextMetrics {
    float width = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;       // negative — above baseline
    float bottom = 0.0f;    // positive — below baseline
    float inkTop = 0.0f;    // negative — visible ink above baseline
    float inkBottom = 0.0f; // positive — visible ink below baseline
    float inkLeft = 0.0f;   // visible ink left edge relative to layout origin
    float inkRight = 0.0f;  // visible ink right edge relative to layout origin
    float capHeight = 0.0f; // measured baseline-to-cap-top of 'H' (0 if unavailable)
    int lineCount = 0;      // laid-out line count (0 for empty text)
  };

  CairoTextRenderer();
  ~CairoTextRenderer();

  CairoTextRenderer(const CairoTextRenderer&) = delete;
  CairoTextRenderer& operator=(const CairoTextRenderer&) = delete;

  void initialize(RenderBackend* backend, TextureManager* textures);
  void cleanup();

  // HiDPI: raster at `scale × fontSize` pixels and shrink the quad by 1/scale.
  void setContentScale(float scale);
  void setFontFamily(std::string family);
  void notifyFontConfigChanged();

  // Drops the uploaded glyph textures (keeping CPU-side metrics) so they are
  // re-rasterized on the next draw. Used to recover from GPU memory loss across
  // suspend/resume on drivers that do not preserve VRAM. Requires the render
  // context to be current.
  void invalidateGlyphTextures();
  void abandonGlyphTextures() noexcept;

  [[nodiscard]] TextMetrics measure(
      std::string_view text, float fontSize, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {},
      TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  );
  [[nodiscard]] TextMetrics measureFont(float fontSize, FontWeight fontWeight) const;
  void measureCursorStops(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, std::vector<float>& outStops,
      FontWeight fontWeight = FontWeight::Normal
  );
  void measureCursorStopsWrapped(
      std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
      std::vector<TextCursorStop>& outStops, FontWeight fontWeight = FontWeight::Normal
  );

  void draw(
      float surfaceWidth, float surfaceHeight, float x, float baselineY, std::string_view text, float fontSize,
      const Color& color, const Mat3& transform, FontWeight fontWeight = FontWeight::Normal, float maxWidth = 0.0f,
      int maxLines = 0, TextAlign align = TextAlign::Start, std::string_view fontFamily = {},
      TextEllipsize ellipsize = TextEllipsize::End, bool useMarkup = false
  );

private:
  struct CacheKey {
    std::string text;
    std::string fontFamily;
    std::uint32_t sizeQ = 0;     // fontSize * 64 + 0.5
    std::uint32_t colorRgba = 0; // packed r<<24|g<<16|b<<8|a
    std::uint32_t maxWidthQ = 0; // maxWidth * 64 + 0.5, 0 = no limit
    std::uint16_t scaleQ = 0;    // contentScale * 64 + 0.5
    std::uint16_t maxLines = 0;  // 0 = no explicit limit (use '\n'-count fallback)
    TextAlign align = TextAlign::Start;
    TextEllipsize ellipsize = TextEllipsize::End;
    FontWeight fontWeight = FontWeight::Normal;
    bool useMarkup = false;

    bool operator==(const CacheKey& other) const noexcept;
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept;
  };

  // Color-independent key used to cache logical TextMetrics. measure() is
  // called from layout paths that run every frame on dirty surfaces; rebuilding
  // a PangoLayout for each call was the top allocation hot-spot in heaptrack.
  struct MetricsKey {
    std::string text;
    std::string fontFamily;
    std::uint32_t sizeQ = 0;
    std::uint32_t maxWidthQ = 0;
    std::uint16_t scaleQ = 0;
    std::uint16_t maxLines = 0;
    TextAlign align = TextAlign::Start;
    TextEllipsize ellipsize = TextEllipsize::End;
    FontWeight fontWeight = FontWeight::Normal;
    bool useMarkup = false;

    bool operator==(const MetricsKey& other) const noexcept;
  };
  struct MetricsKeyHash {
    std::size_t operator()(const MetricsKey& k) const noexcept;
  };

  // Color- and text-independent key for measureFont(), which derives font
  // metrics from the active family at a given size/weight/scale. measureFont()
  // runs every frame from bar layout; each underlying pango_context_get_metrics
  // call makes Pango accumulate internal cache structures that are never
  // reclaimed during the run (heaptrack: top leak, ~24MB over a 30m session).
  // Memoizing collapses ~15k calls/run down to the handful of distinct fonts.
  struct FontMetricsKey {
    std::uint32_t sizeQ = 0;
    std::uint16_t scaleQ = 0;
    FontWeight fontWeight = FontWeight::Normal;

    bool operator==(const FontMetricsKey& other) const noexcept;
  };
  struct FontMetricsKeyHash {
    std::size_t operator()(const FontMetricsKey& k) const noexcept;
  };

  // LruList is a list of pointers into map keys — we break the otherwise
  // circular type dependency (CacheEntry ↔ CacheMap) this way. CacheKey* is
  // stable because unordered_map never moves key-value nodes under insert
  // (we also reserve bucket capacity upfront to avoid rehashing).
  using LruList = std::list<const CacheKey*>;

  // A very tall text block can exceed the backend texture limit (typically
  // 4096 or 8192). We slice the Pango layout into N vertically-stacked tiles,
  // each its own texture sized within that limit. draw() emits one quad per tile;
  // tiles abut on exact buffer-pixel boundaries so there is no visible seam.
  struct Tile {
    TextureHandle texture;
    int pixelHeight = 0;  // raster pixels
    int pixelYOffset = 0; // from top of full layout, in raster pixels
  };

  struct CacheEntry {
    std::vector<Tile> tiles;
    int pixelWidth = 0;   // total raster surface pixel width
    int pixelHeight = 0;  // total raster surface pixel height (sum of tiles)
    float baselinePx = 0; // baseline from top of full layout, in raster pixels
    float inkOffsetX = 0; // raster px from surface left to logical text origin
    TextMetrics metrics;  // logical metrics in logical (unscaled) pixels
    std::size_t bytes = 0;
    bool tinted = false; // true: alpha coverage, tint in shader; false: premul RGBA
    LruList::iterator lruIt;
  };

  using CacheMap = std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>;
  using MetricsMap = std::unordered_map<MetricsKey, TextMetrics, MetricsKeyHash>;
  using FontMetricsMap = std::unordered_map<FontMetricsKey, TextMetrics, FontMetricsKeyHash>;

  // Build a PangoLayout at the given scaled size. Caller owns the layout (g_object_unref).
  PangoLayout* buildLayout(
      std::string_view text, float fontSize, FontWeight fontWeight, float maxWidthPxScaled, int maxLines,
      TextAlign align, std::string_view fontFamily = {}, TextEllipsize ellipsize = TextEllipsize::End,
      bool useMarkup = false
  ) const;
  // Render a layout into a new GL texture; fills out fields of `entry`.
  // When `tinted` is true, rasterizes as CAIRO_FORMAT_A8 and uploads alpha
  // coverage so the color is applied via u_tint at draw time. When false,
  // rasterizes as CAIRO_FORMAT_ARGB32 with `color` baked in (for COLR emoji
  // content).
  void rasterizeLayout(PangoLayout* layout, const Color& color, bool tinted, CacheEntry& entry);
  // Extract logical metrics from a laid-out PangoLayout, dividing by PANGO_SCALE and by scale.
  TextMetrics metricsFromLayout(PangoLayout* layout) const;

  CacheEntry* lookupOrRasterize(
      std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines, TextAlign align,
      const Color& color, std::string_view fontFamily = {}, TextEllipsize ellipsize = TextEllipsize::End,
      bool useMarkup = false
  );
  void touch(CacheMap::iterator it);
  void evict(CacheMap::iterator it);
  void evictIfNeeded();
  void clearCaches();
  // Re-read fontconfig when a font has been registered (by any surface) since the
  // last measure/draw, so newly loaded plugin fonts become resolvable here.
  void maybeSyncFontConfig();

  float m_contentScale = 1.0f;
  bool m_fontConfigInitialized = false;
  std::uint64_t m_syncedFontGeneration = 0;
  std::string m_fontFamily = "sans-serif";

  PangoFontMap* m_fontMap = nullptr;      // owned
  PangoContext* m_pangoContext = nullptr; // owned
  RenderBackend* m_backend = nullptr;
  TextureManager* m_textureManager = nullptr;

  CacheMap m_cache;
  LruList m_lru;
  std::size_t m_cacheBytes = 0;
  int m_glMaxTextureSize = 0; // lazy-queried on first rasterize

  MetricsMap m_metricsCache;
  mutable FontMetricsMap m_fontMetricsCache;

  static constexpr std::size_t kMaxCacheEntries = 512;
  static constexpr std::size_t kMaxCacheBytes = 32 * 1024 * 1024;
  static constexpr std::size_t kMaxMetricsEntries = 1024;
  static constexpr std::size_t kMaxFontMetricsEntries = 64;
};
