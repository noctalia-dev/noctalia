#include "render/text/cairo_glyph_renderer.h"

#include "core/log.h"
#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"

#include <cairo-ft.h>
#include <cairo.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <cstring>
#include <hb-ft.h>
#include <hb.h>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

  constexpr Logger kLog("glyph-renderer");
  constexpr std::uint32_t kSizeQuant = 64;
  constexpr std::uint32_t kScaleQuant = 64;
  constexpr float kAxisAlignedEpsilon = 0.0001F;

  inline std::uint32_t quantizeSize(float v) {
    return static_cast<std::uint32_t>(std::max(0.0F, v) * static_cast<float>(kSizeQuant) + 0.5F);
  }

  inline std::uint16_t quantizeScale(float v) {
    return static_cast<std::uint16_t>(std::max(0.0F, v) * static_cast<float>(kScaleQuant) + 0.5F);
  }

  bool isAxisAligned(const Mat3& transform) {
    return std::abs(transform.m[1]) <= kAxisAlignedEpsilon && std::abs(transform.m[3]) <= kAxisAlignedEpsilon;
  }

  float snapToBufferPixel(float value, float scale) {
    const float safeScale = std::max(1.0F, scale);
    return std::round(value * safeScale) / safeScale;
  }

  void hashCombine(std::size_t& seed, std::size_t v) { seed ^= v + 0x9E3779B97F4A7C15ULL + (seed << 12) + (seed >> 4); }

  // Fixed salt mixed into the glyph cache key hash. std::hash<integral> is the
  // identity map on most libstdc++/libc++ builds, which clusters sequential
  // codepoints into adjacent buckets and degrades the open-addressing probe
  // sequence; seeding from a non-trivial constant decorrelates the low bits.
  constexpr std::size_t kGlyphHashSalt = 0x7E4B2A9C5D3F8161ULL;

  // Hinting is disabled for icons: tabler glyphs are monoline strokes with
  // fractional widths by design. Autohinter snaps each stroke to the nearest
  // integer pixel, which visibly thins the icons. Grayscale AA without
  // hinting preserves the intended stroke thickness.
  cairo_scaled_font_t*
  create_scaled_font(cairo_font_face_t* face, cairo_font_options_t* fontOptions, float rasterSize) {
    cairo_matrix_t fontMatrix;
    cairo_matrix_init_scale(&fontMatrix, rasterSize, rasterSize);
    cairo_matrix_t ctm;
    cairo_matrix_init_identity(&ctm);
    return cairo_scaled_font_create(face, &fontMatrix, &ctm, fontOptions);
  }

  CairoGlyphRenderer::TextMetrics metrics_from_extents(const cairo_text_extents_t& extents, float invScale) {
    return CairoGlyphRenderer::TextMetrics{
        .width = static_cast<float>(extents.x_advance) * invScale,
        .left = static_cast<float>(extents.x_bearing) * invScale,
        .right = static_cast<float>(extents.x_bearing + extents.width) * invScale,
        .top = static_cast<float>(extents.y_bearing) * invScale,
        .bottom = static_cast<float>(extents.y_bearing + extents.height) * invScale,
    };
  }

  struct ShapedGlyph {
    FT_UInt index = 0;
    float xAdvance = 0.0F;
    float yAdvance = 0.0F;
    float xOffset = 0.0F;
    float yOffset = 0.0F;
  };

  bool shapeDisplayText(FT_Face face, std::string_view text, float rasterSize, std::vector<ShapedGlyph>& out) {
    out.clear();
    if (face == nullptr || text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      return false;
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(std::round(rasterSize)));
    hb_font_t* font = hb_ft_font_create_referenced(face);
    if (font == nullptr)
      return false;
    hb_ft_font_changed(font);
    hb_buffer_t* buffer = hb_buffer_create();
    if (buffer == nullptr) {
      hb_font_destroy(font);
      return false;
    }
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    out.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
      if (infos[i].codepoint == 0) {
        out.clear();
        break;
      }
      out.push_back(
          ShapedGlyph{
              .index = infos[i].codepoint,
              .xAdvance = static_cast<float>(positions[i].x_advance) / 64.0F,
              .yAdvance = static_cast<float>(positions[i].y_advance) / 64.0F,
              .xOffset = static_cast<float>(positions[i].x_offset) / 64.0F,
              .yOffset = static_cast<float>(positions[i].y_offset) / 64.0F
          }
      );
    }
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    return !out.empty();
  }

} // namespace

bool CairoGlyphRenderer::CacheKey::operator==(const CacheKey& other) const noexcept {
  return codepoint == other.codepoint
      && sizeQ == other.sizeQ
      && scaleQ == other.scaleQ
      && displayFont == other.displayFont;
}

std::size_t CairoGlyphRenderer::CacheKeyHash::operator()(const CacheKey& k) const noexcept {
  std::size_t seed = kGlyphHashSalt;
  hashCombine(seed, std::hash<char32_t>{}(k.codepoint));
  hashCombine(seed, std::hash<std::uint32_t>{}(k.sizeQ));
  hashCombine(seed, std::hash<std::uint16_t>{}(k.scaleQ));
  hashCombine(seed, std::hash<bool>{}(k.displayFont));
  return seed;
}

CairoGlyphRenderer::CairoGlyphRenderer() = default;
CairoGlyphRenderer::~CairoGlyphRenderer() { cleanup(); }

void CairoGlyphRenderer::initialize(
    const std::string& fontPath, RenderBackend* backend, TextureManager* textures, bool requireIconFont
) {
  m_backend = backend;
  m_textureManager = textures;

  if (FT_Init_FreeType(&m_ftLibrary) != 0) {
    throw std::runtime_error("CairoGlyphRenderer: FT_Init_FreeType failed");
  }
  const char* iconError = nullptr;
  if (FT_New_Face(m_ftLibrary, fontPath.c_str(), 0, &m_face) == 0) {
    m_cairoFace = cairo_ft_font_face_create_for_ft_face(m_face, 0);
    if (m_cairoFace == nullptr || cairo_font_face_status(m_cairoFace) != CAIRO_STATUS_SUCCESS) {
      if (m_cairoFace != nullptr) {
        cairo_font_face_destroy(m_cairoFace);
        m_cairoFace = nullptr;
      }
      FT_Done_Face(m_face);
      m_face = nullptr;
      iconError = "failed to create Cairo icon face";
    }
  } else {
    iconError = "failed to load icon font";
  }
  if (iconError != nullptr) {
    if (requireIconFont) {
      cleanup();
      throw std::runtime_error(std::string("CairoGlyphRenderer: ") + iconError + ": " + fontPath);
    }
    kLog.warn("{}: {}", iconError, fontPath);
  }

  m_fontOptions = cairo_font_options_create();
  cairo_font_options_set_antialias(m_fontOptions, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_style(m_fontOptions, CAIRO_HINT_STYLE_NONE);
  cairo_font_options_set_hint_metrics(m_fontOptions, CAIRO_HINT_METRICS_OFF);

  m_cache.max_load_factor(1.0F);
  m_cache.reserve(kMaxCacheEntries + 16);
}

void CairoGlyphRenderer::initializeDisplayFont(const std::string& fontPath) {
  if (m_ftLibrary == nullptr)
    throw std::runtime_error("CairoGlyphRenderer: initialize FreeType first");
  for (auto it = m_cache.begin(); it != m_cache.end();) {
    if (it->first.displayFont) {
      auto remove = it++;
      evict(remove);
    } else {
      ++it;
    }
  }
  if (m_displayFace != nullptr) {
    if (m_displayCairoFace != nullptr)
      cairo_font_face_destroy(m_displayCairoFace);
    FT_Done_Face(m_displayFace);
    m_displayFace = nullptr;
    m_displayCairoFace = nullptr;
  }
  if (FT_New_Face(m_ftLibrary, fontPath.c_str(), 0, &m_displayFace) != 0) {
    throw std::runtime_error("CairoGlyphRenderer: failed to load display font: " + fontPath);
  }
  m_displayCairoFace = cairo_ft_font_face_create_for_ft_face(m_displayFace, 0);
  if (m_displayCairoFace == nullptr || cairo_font_face_status(m_displayCairoFace) != CAIRO_STATUS_SUCCESS) {
    if (m_displayCairoFace != nullptr) {
      cairo_font_face_destroy(m_displayCairoFace);
      m_displayCairoFace = nullptr;
    }
    FT_Done_Face(m_displayFace);
    m_displayFace = nullptr;
    throw std::runtime_error("CairoGlyphRenderer: failed to create display Cairo face: " + fontPath);
  }
}

void CairoGlyphRenderer::invalidateGlyphTextures() {
  for (auto& [key, entry] : m_cache) {
    if (m_textureManager != nullptr) {
      m_textureManager->unload(entry.texture);
    }
  }
  m_cache.clear();
  m_lru.clear();
  m_cacheBytes = 0;
}

void CairoGlyphRenderer::abandonGlyphTextures() noexcept {
  m_cache.clear();
  m_lru.clear();
  m_cacheBytes = 0;
}

void CairoGlyphRenderer::cleanup() {
  for (auto& [key, entry] : m_cache) {
    if (m_textureManager != nullptr) {
      m_textureManager->unload(entry.texture);
    }
  }
  m_cache.clear();
  m_lru.clear();
  m_cacheBytes = 0;

  if (m_fontOptions != nullptr) {
    cairo_font_options_destroy(m_fontOptions);
    m_fontOptions = nullptr;
  }
  if (m_cairoFace != nullptr) {
    cairo_font_face_destroy(m_cairoFace);
    m_cairoFace = nullptr;
  }
  if (m_displayCairoFace != nullptr) {
    cairo_font_face_destroy(m_displayCairoFace);
    m_displayCairoFace = nullptr;
  }
  if (m_displayFace != nullptr) {
    FT_Done_Face(m_displayFace);
    m_displayFace = nullptr;
  }
  if (m_face != nullptr) {
    FT_Done_Face(m_face);
    m_face = nullptr;
  }
  if (m_ftLibrary != nullptr) {
    FT_Done_FreeType(m_ftLibrary);
    m_ftLibrary = nullptr;
  }
  m_backend = nullptr;
  m_textureManager = nullptr;
}

void CairoGlyphRenderer::touch(CacheMap::iterator it) { m_lru.splice(m_lru.begin(), m_lru, it->second.lruIt); }

void CairoGlyphRenderer::evict(CacheMap::iterator it) {
  if (m_textureManager != nullptr) {
    m_textureManager->unload(it->second.texture);
  }
  m_cacheBytes -= it->second.bytes;
  m_lru.erase(it->second.lruIt);
  m_cache.erase(it);
}

void CairoGlyphRenderer::evictIfNeeded() {
  while (!m_lru.empty() && (m_cache.size() > kMaxCacheEntries || m_cacheBytes > kMaxCacheBytes)) {
    auto& backKey = m_lru.back();
    auto mapIt = m_cache.find(backKey);
    if (mapIt == m_cache.end()) {
      m_lru.pop_back();
      continue;
    }
    evict(mapIt);
  }
}

CairoGlyphRenderer::TextMetrics
CairoGlyphRenderer::measureGlyph(float contentScale, char32_t codepoint, float fontSize) {
  if (m_face == nullptr || codepoint == 0 || fontSize <= 0.0F) {
    return {};
  }

  const float rasterSize = std::max(1.0F, fontSize * contentScale);
  const float invScale = 1.0F / contentScale;

  const FT_UInt glyphIndex = FT_Get_Char_Index(m_face, codepoint);
  if (glyphIndex == 0) {
    return {};
  }

  cairo_scaled_font_t* scaledFont = create_scaled_font(m_cairoFace, m_fontOptions, rasterSize);
  if (scaledFont == nullptr || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS) {
    if (scaledFont != nullptr) {
      cairo_scaled_font_destroy(scaledFont);
    }
    return {};
  }

  cairo_glyph_t glyph;
  glyph.index = glyphIndex;
  glyph.x = 0.0;
  glyph.y = 0.0;

  cairo_text_extents_t extents{};
  cairo_scaled_font_glyph_extents(scaledFont, &glyph, 1, &extents);
  const auto out =
      cairo_scaled_font_status(scaledFont) == CAIRO_STATUS_SUCCESS ? metrics_from_extents(extents, invScale) : TextMetrics{};
  cairo_scaled_font_destroy(scaledFont);
  return out;
}

CairoGlyphRenderer::TextMetrics
CairoGlyphRenderer::measureDisplayText(float contentScale, std::string_view text, float fontSize) {
  if (m_displayFace == nullptr || m_displayCairoFace == nullptr || contentScale <= 0.0F || fontSize <= 0.0F)
    return {};
  const float rasterSize = std::max(1.0F, fontSize * contentScale);
  const float invScale = 1.0F / contentScale;
  std::vector<ShapedGlyph> glyphs;
  if (!shapeDisplayText(m_displayFace, text, rasterSize, glyphs))
    return {};
  cairo_scaled_font_t* scaledFont = create_scaled_font(m_displayCairoFace, m_fontOptions, rasterSize);
  if (scaledFont == nullptr || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS) {
    if (scaledFont != nullptr)
      cairo_scaled_font_destroy(scaledFont);
    return {};
  }
  float penX = 0.0F, penY = 0.0F;
  float left = 0.0F, right = 0.0F, top = 0.0F, bottom = 0.0F;
  bool hasInk = false;
  for (const auto& shaped : glyphs) {
    cairo_glyph_t glyph{.index = shaped.index, .x = penX + shaped.xOffset, .y = -penY - shaped.yOffset};
    cairo_text_extents_t ext{};
    cairo_scaled_font_glyph_extents(scaledFont, &glyph, 1, &ext);
    if (ext.width > 0.0 || ext.height > 0.0) {
      const float gl = static_cast<float>(glyph.x) + static_cast<float>(ext.x_bearing);
      const float gr = gl + static_cast<float>(ext.width);
      const float gt = static_cast<float>(glyph.y) + static_cast<float>(ext.y_bearing);
      const float gb = gt + static_cast<float>(ext.height);
      if (!hasInk) {
        left = gl;
        right = gr;
        top = gt;
        bottom = gb;
        hasInk = true;
      } else {
        left = std::min(left, gl);
        right = std::max(right, gr);
        top = std::min(top, gt);
        bottom = std::max(bottom, gb);
      }
    }
    penX += shaped.xAdvance;
    penY += shaped.yAdvance;
  }
  cairo_scaled_font_destroy(scaledFont);
  return TextMetrics{
      .width = penX * invScale,
      .left = left * invScale,
      .right = right * invScale,
      .top = top * invScale,
      .bottom = bottom * invScale
  };
}

CairoGlyphRenderer::TextMetrics CairoGlyphRenderer::measureDisplayFont(float contentScale, float fontSize) {
  if (m_displayFace == nullptr || contentScale <= 0.0F || fontSize <= 0.0F)
    return {};
  const auto cap = measureDisplayText(contentScale, "H", fontSize);
  const float scale = fontSize / static_cast<float>(m_displayFace->units_per_EM);
  return TextMetrics{
      .width = 0.0F,
      .left = 0.0F,
      .right = 0.0F,
      .top = -static_cast<float>(m_displayFace->ascender) * scale,
      .bottom = -static_cast<float>(m_displayFace->descender) * scale,
      .capHeight = std::max(0.0F, -cap.top)
  };
}

CairoGlyphRenderer::CacheEntry* CairoGlyphRenderer::lookupOrRasterize(
    float contentScale, char32_t codepoint, float fontSize, bool displayFont, FT_UInt forcedGlyphIndex
) {
  CacheKey key;
  key.codepoint = codepoint;
  key.sizeQ = quantizeSize(fontSize);
  key.scaleQ = quantizeScale(contentScale);
  key.displayFont = displayFont;

  auto it = m_cache.find(key);
  if (it != m_cache.end()) {
    touch(it);
    return &it->second;
  }

  const float rasterSize = std::max(1.0F, fontSize * contentScale);
  FT_Face activeFace = displayFont ? m_displayFace : m_face;
  cairo_font_face_t* activeCairoFace = displayFont ? m_displayCairoFace : m_cairoFace;
  if (activeFace == nullptr || activeCairoFace == nullptr)
    return nullptr;
  FT_Set_Pixel_Sizes(activeFace, 0, static_cast<FT_UInt>(std::round(rasterSize)));

  const FT_UInt glyphIndex = forcedGlyphIndex != 0 ? forcedGlyphIndex : FT_Get_Char_Index(activeFace, codepoint);
  if (glyphIndex == 0) {
    return nullptr;
  }

  cairo_scaled_font_t* scaledFont = create_scaled_font(activeCairoFace, m_fontOptions, rasterSize);
  if (scaledFont == nullptr || cairo_scaled_font_status(scaledFont) != CAIRO_STATUS_SUCCESS) {
    if (scaledFont != nullptr) {
      cairo_scaled_font_destroy(scaledFont);
    }
    return nullptr;
  }

  cairo_glyph_t glyph;
  glyph.index = glyphIndex;
  glyph.x = 0.0;
  glyph.y = 0.0;

  cairo_text_extents_t extents;
  cairo_scaled_font_glyph_extents(scaledFont, &glyph, 1, &extents);

  // Surface size: ceil the ink rect and add a 1px margin to protect antialiased
  // edges from clipping.
  const int pad = 1;
  const int pxWidth = std::max(1, static_cast<int>(std::ceil(extents.width)) + pad * 2);
  const int pxHeight = std::max(1, static_cast<int>(std::ceil(extents.height)) + pad * 2);

  // Position the glyph so its ink rect lands within the surface with `pad` inset.
  glyph.x = -extents.x_bearing + pad;
  glyph.y = -extents.y_bearing + pad;

  CacheEntry entry{};
  entry.pixelWidth = pxWidth;
  entry.pixelHeight = pxHeight;
  entry.baselineXPx = static_cast<float>(glyph.x);
  // Baseline from top of surface = glyph.y (since cairo baseline = y offset
  // of origin, and we placed origin so that ink-top = pad).
  entry.baselinePx = static_cast<float>(glyph.y);
  entry.inkOffsetXPx = static_cast<float>(pad);
  entry.inkOffsetYPx = static_cast<float>(pad);

  // A8 (alpha coverage) rasterization: color gets applied in the shader via
  // u_tint, so the cache is color-independent. cairo draws coverage by setting
  // an opaque source on the A8 surface (rgb is ignored for A8).
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, pxWidth, pxHeight);
  cairo_t* cr = cairo_create(surface);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_scaled_font(cr, scaledFont);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
  cairo_show_glyphs(cr, &glyph, 1);
  cairo_destroy(cr);
  cairo_surface_flush(surface);

  const int stride = cairo_image_surface_get_stride(surface);
  unsigned char* data = cairo_image_surface_get_data(surface);

  // Repack tightly to pxWidth bytes per row (cairo's stride is 4-byte aligned).
  std::vector<unsigned char> tight(static_cast<std::size_t>(pxWidth) * static_cast<std::size_t>(pxHeight));
  for (int y = 0; y < pxHeight; ++y) {
    const auto row = static_cast<std::size_t>(y);
    std::memcpy(
        tight.data() + row * static_cast<std::size_t>(pxWidth), data + row * static_cast<std::size_t>(stride),
        static_cast<std::size_t>(pxWidth)
    );
  }
  cairo_surface_destroy(surface);
  cairo_scaled_font_destroy(scaledFont);

  if (m_textureManager == nullptr) {
    return nullptr;
  }

  entry.texture = m_textureManager->loadFromPixels(
      tight.data(), pxWidth, pxHeight, TextureDataFormat::Alpha, TextureFilter::Linear
  );
  if (entry.texture.id == 0) {
    return nullptr;
  }
  entry.bytes = static_cast<std::size_t>(pxWidth) * static_cast<std::size_t>(pxHeight);

  const float invScale = 1.0F / contentScale;
  entry.metrics = metrics_from_extents(extents, invScale);

  auto [ins, inserted] = m_cache.emplace(key, entry);
  m_lru.push_front(ins->first);
  ins->second.lruIt = m_lru.begin();
  m_cacheBytes += ins->second.bytes;

  evictIfNeeded();
  return &ins->second;
}

void CairoGlyphRenderer::drawGlyph(
    float contentScale, float surfaceWidth, float surfaceHeight, float x, float baselineY, char32_t codepoint,
    float fontSize, const Color& color, const Mat3& transform
) {
  if (m_face == nullptr || m_backend == nullptr || codepoint == 0) {
    return;
  }

  CacheEntry* entry = lookupOrRasterize(contentScale, codepoint, fontSize);
  if (entry == nullptr || entry->texture.id == 0) {
    return;
  }

  const float invScale = 1.0F / contentScale;
  const float quadW = static_cast<float>(entry->pixelWidth) * invScale;
  const float quadH = static_cast<float>(entry->pixelHeight) * invScale;
  const float baselineXLocal = entry->baselineXPx * invScale;
  const float baselineLocal = entry->baselinePx * invScale;

  const Mat3 localTranslation = Mat3::translation(x - baselineXLocal, baselineY - baselineLocal);
  Mat3 world = transform * localTranslation;

  // Snap the visible ink origin to the nearest buffer pixel so linear filtering
  // samples land on texel centers without the 1px texture pad biasing icon alignment.
  // Skip when the transform has rotation/skew — snapping then introduces
  // whole-pixel jumps per frame and makes animations look jittery.
  if (isAxisAligned(world)) {
    world.m[6] = snapToBufferPixel(world.m[6], contentScale);
    world.m[7] = snapToBufferPixel(world.m[7], contentScale);
  }

  m_backend->drawGlyph(
      RenderGlyphDraw{
          .texture = entry->texture.id,
          .surfaceWidth = surfaceWidth,
          .surfaceHeight = surfaceHeight,
          .width = quadW,
          .height = quadH,
          .opacity = 1.0F,
          .tint = color,
          .tinted = true,
          .transform = world,
      }
  );
}

void CairoGlyphRenderer::drawDisplayText(
    float contentScale, float surfaceWidth, float surfaceHeight, float x, float baselineY, std::string_view text,
    float fontSize, const Color& color, const Mat3& transform
) {
  if (m_displayFace == nullptr || m_displayCairoFace == nullptr || m_backend == nullptr || contentScale <= 0.0F)
    return;
  const float rasterSize = std::max(1.0F, fontSize * contentScale);
  std::vector<ShapedGlyph> glyphs;
  if (!shapeDisplayText(m_displayFace, text, rasterSize, glyphs))
    return;
  float penPxX = 0.0F, penPxY = 0.0F;
  for (const auto& shaped : glyphs) {
    CacheEntry* entry =
        lookupOrRasterize(contentScale, static_cast<char32_t>(shaped.index), fontSize, true, shaped.index);
    if (entry == nullptr || entry->texture.id == 0)
      return;
    const float invScale = 1.0F / contentScale;
    const float gx = x + (penPxX + shaped.xOffset) * invScale;
    const float quadW = static_cast<float>(entry->pixelWidth) * invScale;
    const float quadH = static_cast<float>(entry->pixelHeight) * invScale;
    const Mat3 local = Mat3::translation(
        gx - entry->baselineXPx * invScale,
        baselineY - (penPxY + shaped.yOffset) * invScale - entry->baselinePx * invScale
    );
    Mat3 world = transform * local;
    if (isAxisAligned(world)) {
      world.m[6] = snapToBufferPixel(world.m[6], contentScale);
      world.m[7] = snapToBufferPixel(world.m[7], contentScale);
    }
    m_backend->drawGlyph(
        RenderGlyphDraw{
            .texture = entry->texture.id,
            .surfaceWidth = surfaceWidth,
            .surfaceHeight = surfaceHeight,
            .width = quadW,
            .height = quadH,
            .opacity = 1.0F,
            .tint = color,
            .tinted = true,
            .transform = world
        }
    );
    penPxX += shaped.xAdvance;
    penPxY += shaped.yAdvance;
  }
}
