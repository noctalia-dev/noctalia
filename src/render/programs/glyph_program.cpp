#include "render/programs/glyph_program.h"

#include "render/core/color.h"
#include "render/core/texture_handle.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

  // Positions a unit quad, applies a pixel-space transform, converts to NDC.
  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec2 a_texcoord;
uniform vec2 u_surface_size;
uniform vec2 u_size;
uniform mat3 u_transform;
varying vec2 v_texcoord;
varying vec2 v_local;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_texcoord = a_texcoord;
    v_local = local;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  // Fragment shader: three modes selected by u_tint_mode.
  //   mode 0 (RGBA):     texture stores a premultiplied RGBA glyph; scale by opacity.
  //   mode 1 (tint):     texture stores an alpha coverage mask; the output is
  //                      premul(tint) * coverage * opacity.
  //   mode 2 (gradient): alpha coverage again, but the colour comes from four
  //                      stops evaluated across the whole laid-out text block,
  //                      optionally with a bounded halo behind the ink.
  // All paths output premultiplied to match the pipeline-wide
  // GL_ONE / GL_ONE_MINUS_SRC_ALPHA blend mode.
  //
  // ponytail: the halo is a fixed 3x3 tap ring scaled by the requested radius,
  // not a separable blur. That is what the 8 px ceiling (kMaxTextGlowRadius) buys
  // — a wider halo would band visibly, and the upgrade path is a two-pass
  // separable blur into an offscreen target if a caller ever needs one. The taps
  // also cannot cross a tile seam: a layout taller than the max texture size
  // rasterizes into several textures and each is sampled independently.
  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform sampler2D u_texture;
uniform float u_opacity;
uniform vec4 u_tint;        // straight (non-premul) rgba
uniform float u_tint_mode;  // 0 = RGBA texture, 1 = alpha coverage, 2 = gradient
uniform vec2 u_gradient_direction;
uniform float u_gradient_offset;
uniform vec4 u_gradient_stops;
uniform vec4 u_gradient_color0;
uniform vec4 u_gradient_color1;
uniform vec4 u_gradient_color2;
uniform vec4 u_gradient_color3;
uniform vec2 u_gradient_span;        // full laid-out text block, logical px
uniform vec2 u_gradient_quad_origin; // this quad's origin inside that block
uniform float u_glow_radius;         // texels; 0 disables the halo
uniform vec2 u_texel_size;           // 1 / texture size, in UV units
varying vec2 v_texcoord;
varying vec2 v_local;

// Coverage from the alpha channel, with anything outside this draw's UV window
// reading as empty. The window matters because the glow path deliberately draws
// a quad larger than the glyph.
float coverage_at(vec2 uv) {
    vec2 clamped = clamp(uv, vec2(0.0), vec2(1.0));
    if (clamped != uv) {
        return 0.0;
    }
    return texture2D(u_texture, uv).a;
}

float gradient_segment_t(float position, float start, float end) {
    return clamp((position - start) / max(end - start, 0.0001), 0.0, 1.0);
}

// Identical rules to the rect shader's gradient_fill: monotonise the stops, then
// pick one segment and interpolate inside it. Note the deliberate absence of a
// clamp on `position` — samples outside the stop range resolve to the endpoint
// colours, which is exactly what lets an animated offset carry the crest off the
// end of the text and back.
vec4 gradient_color(float position) {
    vec4 stops = clamp(u_gradient_stops, vec4(0.0), vec4(1.0));
    stops.y = max(stops.y, stops.x);
    stops.z = max(stops.z, stops.y);
    stops.w = max(stops.w, stops.z);

    vec4 c0 = u_gradient_color0;
    vec4 c1 = u_gradient_color1;
    vec4 c2 = u_gradient_color2;
    vec4 c3 = u_gradient_color3;

    if (position <= stops.y) {
        return mix(c0, c1, gradient_segment_t(position, stops.x, stops.y));
    }
    if (position <= stops.z) {
        return mix(c1, c2, gradient_segment_t(position, stops.y, stops.z));
    }
    return mix(c2, c3, gradient_segment_t(position, stops.z, stops.w));
}

void main() {
    if (u_tint_mode > 1.5) {
        // Gradient position runs across the whole text block, so vertical tiles
        // and wrapped lines continue one ramp instead of restarting per quad.
        vec2 block = max(u_gradient_span, vec2(1e-5));
        vec2 uv_block = (v_local + u_gradient_quad_origin) / block;
        float gradient_pos = dot(uv_block, u_gradient_direction) - u_gradient_offset;
        vec4 fill = gradient_color(gradient_pos);

        float ink = coverage_at(v_texcoord);
        float halo = 0.0;
        if (u_glow_radius > 0.0) {
            // Fixed 8-tap ring plus centre; radius scales the ring, not the count.
            vec2 r = u_texel_size * u_glow_radius;
            halo += coverage_at(v_texcoord + vec2(-r.x, -r.y));
            halo += coverage_at(v_texcoord + vec2(0.0, -r.y));
            halo += coverage_at(v_texcoord + vec2(r.x, -r.y));
            halo += coverage_at(v_texcoord + vec2(-r.x, 0.0));
            halo += coverage_at(v_texcoord + vec2(r.x, 0.0));
            halo += coverage_at(v_texcoord + vec2(-r.x, r.y));
            halo += coverage_at(v_texcoord + vec2(0.0, r.y));
            halo += coverage_at(v_texcoord + vec2(r.x, r.y));
            halo = clamp(halo * 0.125, 0.0, 1.0);
        }

        // Halo sits behind the ink and never brightens it past its own alpha.
        float combined = clamp(ink + halo * (1.0 - ink) * 0.6, 0.0, 1.0);
        float alpha = combined * fill.a * u_opacity;
        gl_FragColor = vec4(fill.rgb * alpha, alpha);
        return;
    }

    vec4 c = texture2D(u_texture, v_texcoord);
    if (u_tint_mode > 0.5) {
        float coverage = c.a * u_tint.a * u_opacity;
        gl_FragColor = vec4(u_tint.rgb * coverage, coverage);
    } else {
        gl_FragColor = vec4(c.rgb * u_opacity, c.a * u_opacity);
    }
}
)";

} // namespace

void GlyphProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_texCoordLocation = glGetAttribLocation(m_program.id(), "a_texcoord");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_rectLocation = glGetUniformLocation(m_program.id(), "u_size");
  m_opacityLocation = glGetUniformLocation(m_program.id(), "u_opacity");
  m_samplerLocation = glGetUniformLocation(m_program.id(), "u_texture");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");
  m_tintLocation = glGetUniformLocation(m_program.id(), "u_tint");
  m_tintModeLocation = glGetUniformLocation(m_program.id(), "u_tint_mode");
  m_gradientDirectionLocation = glGetUniformLocation(m_program.id(), "u_gradient_direction");
  m_gradientOffsetLocation = glGetUniformLocation(m_program.id(), "u_gradient_offset");
  m_gradientStopsLocation = glGetUniformLocation(m_program.id(), "u_gradient_stops");
  m_gradientColor0Location = glGetUniformLocation(m_program.id(), "u_gradient_color0");
  m_gradientColor1Location = glGetUniformLocation(m_program.id(), "u_gradient_color1");
  m_gradientColor2Location = glGetUniformLocation(m_program.id(), "u_gradient_color2");
  m_gradientColor3Location = glGetUniformLocation(m_program.id(), "u_gradient_color3");
  m_gradientSpanLocation = glGetUniformLocation(m_program.id(), "u_gradient_span");
  m_gradientQuadOriginLocation = glGetUniformLocation(m_program.id(), "u_gradient_quad_origin");
  m_glowRadiusLocation = glGetUniformLocation(m_program.id(), "u_glow_radius");
  m_texelSizeLocation = glGetUniformLocation(m_program.id(), "u_texel_size");

  if (m_positionLocation < 0
      || m_texCoordLocation < 0
      || m_surfaceSizeLocation < 0
      || m_rectLocation < 0
      || m_opacityLocation < 0
      || m_samplerLocation < 0
      || m_transformLocation < 0
      || m_tintLocation < 0
      || m_tintModeLocation < 0
      || m_gradientDirectionLocation < 0
      || m_gradientOffsetLocation < 0
      || m_gradientStopsLocation < 0
      || m_gradientColor0Location < 0
      || m_gradientColor1Location < 0
      || m_gradientColor2Location < 0
      || m_gradientColor3Location < 0
      || m_gradientSpanLocation < 0
      || m_gradientQuadOriginLocation < 0
      || m_glowRadiusLocation < 0
      || m_texelSizeLocation < 0) {
    throw std::runtime_error("failed to query color glyph shader locations");
  }
}

void GlyphProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_texCoordLocation = -1;
  m_surfaceSizeLocation = -1;
  m_rectLocation = -1;
  m_opacityLocation = -1;
  m_samplerLocation = -1;
  m_transformLocation = -1;
  m_tintLocation = -1;
  m_tintModeLocation = -1;
  m_gradientDirectionLocation = -1;
  m_gradientOffsetLocation = -1;
  m_gradientStopsLocation = -1;
  m_gradientColor0Location = -1;
  m_gradientColor1Location = -1;
  m_gradientColor2Location = -1;
  m_gradientColor3Location = -1;
  m_gradientSpanLocation = -1;
  m_gradientQuadOriginLocation = -1;
  m_glowRadiusLocation = -1;
  m_texelSizeLocation = -1;
}

void GlyphProgram::abandon() noexcept { m_program.abandon(); }

void GlyphProgram::bindCommon(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Mat3& transform
) const {
  const std::array<GLfloat, 12> positions = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  const std::array<GLfloat, 12> texcoords = {
      u0, v0, u1, v0, u0, v1, u0, v1, u1, v0, u1, v1,
  };

  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_rectLocation, width, height);
  glUniform1f(m_opacityLocation, opacity);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, transform.m.data());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture.value()));
  glUniform1i(m_samplerLocation, 0);
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  const auto texAttr = static_cast<GLuint>(m_texCoordLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, positions.data());
  glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, 0, texcoords.data());
  glEnableVertexAttribArray(posAttr);
  glEnableVertexAttribArray(texAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
  glDisableVertexAttribArray(texAttr);
}

void GlyphProgram::draw(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0F || height <= 0.0F) {
    return;
  }
  glUseProgram(m_program.id());
  glUniform1f(m_tintModeLocation, 0.0F);
  glUniform4f(m_tintLocation, 1.0F, 1.0F, 1.0F, 1.0F);
  bindCommon(texture, surfaceWidth, surfaceHeight, width, height, u0, v0, u1, v1, opacity, transform);
}

void GlyphProgram::drawTinted(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const Color& tint, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0F || height <= 0.0F) {
    return;
  }
  glUseProgram(m_program.id());
  glUniform1f(m_tintModeLocation, 1.0F);
  glUniform4f(m_tintLocation, tint.r, tint.g, tint.b, tint.a);
  bindCommon(texture, surfaceWidth, surfaceHeight, width, height, u0, v0, u1, v1, opacity, transform);
}

void GlyphProgram::drawGradient(
    TextureId texture, float surfaceWidth, float surfaceHeight, float width, float height, float u0, float v0, float u1,
    float v1, float opacity, const TextGradientStyle& gradient, float fullWidth, float fullHeight, float tileOffsetY,
    float contentScale, const Mat3& transform
) const {
  if (!m_program.isValid() || texture == 0 || width <= 0.0F || height <= 0.0F) {
    return;
  }

  // Quad expansion. The glyph must keep its original footprint inside the larger
  // quad, so growing the size and shifting the transform is only two thirds of
  // the job — the texcoords have to be scaled outward by the same ratio, which
  // is what maps the padding band to UVs outside 0..1 where coverage_at() reads
  // it as empty. Skip the remap and the glyph silently stretches instead.
  const float pad = std::max(0.0F, gradient.glowRadius);
  const float paddedWidth = width + pad * 2.0F;
  const float paddedHeight = height + pad * 2.0F;
  const float uSpan = u1 - u0;
  const float vSpan = v1 - v0;
  const float padU = width > 0.0F ? uSpan * (pad / width) : 0.0F;
  const float padV = height > 0.0F ? vSpan * (pad / height) : 0.0F;
  const Mat3 paddedTransform = transform * Mat3::translation(-pad, -pad);

  glUseProgram(m_program.id());
  glUniform1f(m_tintModeLocation, 2.0F);
  glUniform4f(m_tintLocation, 1.0F, 1.0F, 1.0F, 1.0F);

  // The axis carries a corner bias that keeps the projected block spanning
  // exactly 0..1 along it; ui.gradient adds it to the offset and so must this.
  const GradientAxis axis = gradientAxisForDegrees(gradient.angleDeg);
  glUniform2f(m_gradientDirectionLocation, axis.x, axis.y);
  glUniform1f(m_gradientOffsetLocation, gradient.offset + axis.bias);
  glUniform4f(
      m_gradientStopsLocation, gradient.stops[0].position, gradient.stops[1].position, gradient.stops[2].position,
      gradient.stops[3].position
  );
  glUniform4f(
      m_gradientColor0Location, gradient.stops[0].color.r, gradient.stops[0].color.g, gradient.stops[0].color.b,
      gradient.stops[0].color.a
  );
  glUniform4f(
      m_gradientColor1Location, gradient.stops[1].color.r, gradient.stops[1].color.g, gradient.stops[1].color.b,
      gradient.stops[1].color.a
  );
  glUniform4f(
      m_gradientColor2Location, gradient.stops[2].color.r, gradient.stops[2].color.g, gradient.stops[2].color.b,
      gradient.stops[2].color.a
  );
  glUniform4f(
      m_gradientColor3Location, gradient.stops[3].color.r, gradient.stops[3].color.g, gradient.stops[3].color.b,
      gradient.stops[3].color.a
  );
  glUniform2f(m_gradientSpanLocation, std::max(fullWidth, 1e-5F), std::max(fullHeight, 1e-5F));
  // v_local is relative to the padded quad, so the origin absorbs the padding.
  glUniform2f(m_gradientQuadOriginLocation, -pad, tileOffsetY - pad);

  // The kernel taps in texels. A logical radius therefore converts through the
  // content scale, or the halo tightens by a quarter on a 1.25 output.
  const float scale = contentScale > 0.0F ? contentScale : 1.0F;
  glUniform1f(m_glowRadiusLocation, pad * scale);
  const float texWidth = uSpan > 0.0F ? (width * scale) / uSpan : 1.0F;
  const float texHeight = vSpan > 0.0F ? (height * scale) / vSpan : 1.0F;
  glUniform2f(m_texelSizeLocation, 1.0F / std::max(texWidth, 1.0F), 1.0F / std::max(texHeight, 1.0F));

  bindCommon(
      texture, surfaceWidth, surfaceHeight, paddedWidth, paddedHeight, u0 - padU, v0 - padV, u1 + padU, v1 + padV,
      opacity, paddedTransform
  );
}
