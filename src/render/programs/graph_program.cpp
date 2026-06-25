#include "render/programs/graph_program.h"

#include "render/core/render_styles.h"
#include "render/core/texture_handle.h"

#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
varying vec2 v_texcoord;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_texcoord = a_position;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShader[] = R"(
precision highp float;

uniform sampler2D u_data_source;
uniform vec4 u_line_color1;
uniform vec4 u_line_color2;
uniform vec4 u_line_color3;
uniform float u_count1;
uniform float u_count2;
uniform float u_count3;
uniform float u_scroll1;
uniform float u_scroll2;
uniform float u_scroll3;
uniform float u_line_width;
uniform float u_graph_fill_opacity;
uniform float u_tex_width;
uniform float u_res_x;
uniform float u_res_y;
uniform float u_aa_size;
varying vec2 v_texcoord;

float fetchData(float idx, int ch) {
    float i = clamp(idx, 0.0, u_tex_width - 1.0);
    float u = (floor(i) + 0.5) / u_tex_width;
    vec4 t = texture2D(u_data_source, vec2(u, 0.5));
    if (ch == 0) return t.r;
    if (ch == 1) return t.g;
    return t.b;
}

float cubicHermite(float y0, float y1, float y2, float y3, float t) {
    float m1 = (y2 - y0) * 0.25;
    float m2 = (y3 - y1) * 0.25;
    float t2 = t * t;
    float t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * y1
         + (t3 - 2.0 * t2 + t) * m1
         + (-2.0 * t3 + 3.0 * t2) * y2
         + (t3 - t2) * m2;
}

float graphPadNorm() {
    float linePad = (u_line_width * 0.5 + u_aa_size * 2.0 + 1.0) / max(u_res_y, 1.0);
    // Keep a little extra headroom so Hermite overshoot near the extrema stays visible.
    return min(0.18, max(linePad + 0.04, 0.06));
}

float mapToPlotRange(float y) {
    float pad = graphPadNorm();
    return pad + (1.0 - 2.0 * pad) * y;
}

float evalCurve(float dataIdx, int ch) {
    float i = floor(dataIdx);
    float t = dataIdx - i;
    return mapToPlotRange(cubicHermite(
        fetchData(i - 1.0, ch),
        fetchData(i, ch),
        fetchData(i + 1.0, ch),
        fetchData(i + 2.0, ch),
        t
    ));
}

float segDistSq(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float len2 = dot(ab, ab);
    float t = len2 > 0.0 ? clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    vec2 proj = a + t * ab;
    vec2 d = p - proj;
    return dot(d, d);
}

float curveDistance(float dataIdx, float pixStep, float normY, int ch) {
    vec2 frag = vec2(0.0, normY * u_res_y);

    float px = -2.0;
    float py = evalCurve(dataIdx - 2.0 * pixStep, ch) * u_res_y;
    vec2 d0 = frag - vec2(px, py);
    float best = dot(d0, d0);

    for (int i = 1; i <= 16; i++) {
        float cx = -2.0 + float(i) * 0.25;
        float cy = evalCurve(dataIdx + cx * pixStep, ch) * u_res_y;
        best = min(best, segDistSq(frag, vec2(px, py), vec2(cx, cy)));
        px = cx;
        py = cy;
    }

    return sqrt(best);
}

float lineCoverage(float dataIdx, float pixStep, float normY, int ch, float halfW) {
    float cyPrev = evalCurve(dataIdx - pixStep, ch);
    float cyNext = evalCurve(dataIdx + pixStep, ch);
    float slope = (cyNext - cyPrev) * 0.5 * u_res_y;
    float aa = max(u_aa_size * 2.0,
                   (abs(slope) + 1.0) * inversesqrt(slope * slope + 1.0) * u_aa_size * 2.0);
    float coverage = smoothstep(halfW + aa, halfW, curveDistance(dataIdx, pixStep, normY, ch));

    if (abs(slope) > 4.0) {
        // Near-vertical segments alias badly if we only sample the line at the
        // pixel center while the graph scrolls horizontally.
        float sub = pixStep * 0.3333333;
        float left = smoothstep(halfW + aa, halfW, curveDistance(dataIdx - sub, pixStep, normY, ch));
        float right = smoothstep(halfW + aa, halfW, curveDistance(dataIdx + sub, pixStep, normY, ch));
        coverage = (coverage + left + right) / 3.0;
    }

    return coverage;
}

vec4 blendOver(vec4 src, vec4 dst) {
    return src + dst * (1.0 - src.a);
}

void main() {
    vec2 uv = v_texcoord;
    float normY = 1.0 - uv.y;
    float pad = graphPadNorm();
    float plotTop = 1.0 - pad;

    vec4 result = vec4(0.0);
    float halfW = u_line_width * 0.5;

    if (u_count1 >= 4.0) {
        float segs = u_count1 - 3.0;
        float di = 2.0 + u_scroll1 + uv.x * segs;
        float pixStep = segs / u_res_x;
        float cy = evalCurve(di, 0);

        if (u_graph_fill_opacity > 0.0 && normY >= pad && normY <= min(cy, plotTop)) {
            float a = u_graph_fill_opacity * ((normY - pad) / max(plotTop - pad, 0.0001)) * u_line_color1.a;
            result = blendOver(vec4(u_line_color1.rgb * a, a), result);
        }

        float sa = lineCoverage(di, pixStep, normY, 0, halfW) * u_line_color1.a;
        result = blendOver(vec4(u_line_color1.rgb * sa, sa), result);
    }

    if (u_count2 >= 4.0) {
        float segs = u_count2 - 3.0;
        float di = 2.0 + u_scroll2 + uv.x * segs;
        float pixStep = segs / u_res_x;
        float cy = evalCurve(di, 1);

        if (u_graph_fill_opacity > 0.0 && normY >= pad && normY <= min(cy, plotTop)) {
            float a = u_graph_fill_opacity * ((normY - pad) / max(plotTop - pad, 0.0001)) * u_line_color2.a;
            result = blendOver(vec4(u_line_color2.rgb * a, a), result);
        }

        float sa = lineCoverage(di, pixStep, normY, 1, halfW) * u_line_color2.a;
        result = blendOver(vec4(u_line_color2.rgb * sa, sa), result);
    }

    if (u_count3 >= 4.0) {
        float segs = u_count3 - 3.0;
        float di = 2.0 + u_scroll3 + uv.x * segs;
        float pixStep = segs / u_res_x;
        float cy = evalCurve(di, 2);

        if (u_graph_fill_opacity > 0.0 && normY >= pad && normY <= min(cy, plotTop)) {
            float a = u_graph_fill_opacity * ((normY - pad) / max(plotTop - pad, 0.0001)) * u_line_color3.a;
            result = blendOver(vec4(u_line_color3.rgb * a, a), result);
        }

        float sa = lineCoverage(di, pixStep, normY, 2, halfW) * u_line_color3.a;
        result = blendOver(vec4(u_line_color3.rgb * sa, sa), result);
    }

    gl_FragColor = result;
}
)";

} // namespace

void GraphProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShader, kFragmentShader);
  const auto id = m_program.id();

  m_positionLoc = glGetAttribLocation(id, "a_position");
  m_surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  m_quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  m_transformLoc = glGetUniformLocation(id, "u_transform");
  m_lineColor1Loc = glGetUniformLocation(id, "u_line_color1");
  m_lineColor2Loc = glGetUniformLocation(id, "u_line_color2");
  m_lineColor3Loc = glGetUniformLocation(id, "u_line_color3");
  m_count1Loc = glGetUniformLocation(id, "u_count1");
  m_count2Loc = glGetUniformLocation(id, "u_count2");
  m_count3Loc = glGetUniformLocation(id, "u_count3");
  m_scroll1Loc = glGetUniformLocation(id, "u_scroll1");
  m_scroll2Loc = glGetUniformLocation(id, "u_scroll2");
  m_scroll3Loc = glGetUniformLocation(id, "u_scroll3");
  m_lineWidthLoc = glGetUniformLocation(id, "u_line_width");
  m_graphFillOpacityLoc = glGetUniformLocation(id, "u_graph_fill_opacity");
  m_texWidthLoc = glGetUniformLocation(id, "u_tex_width");
  m_resXLoc = glGetUniformLocation(id, "u_res_x");
  m_resYLoc = glGetUniformLocation(id, "u_res_y");
  m_aaSizeLoc = glGetUniformLocation(id, "u_aa_size");
  m_dataSourceLoc = glGetUniformLocation(id, "u_data_source");

  if (m_positionLoc < 0 || m_surfaceSizeLoc < 0 || m_quadSizeLoc < 0 || m_transformLoc < 0 || m_dataSourceLoc < 0) {
    throw std::runtime_error("failed to query graph shader locations");
  }
}

void GraphProgram::destroy() {
  m_program.destroy();
  m_positionLoc = -1;
  m_surfaceSizeLoc = -1;
  m_quadSizeLoc = -1;
  m_transformLoc = -1;
  m_lineColor1Loc = -1;
  m_lineColor2Loc = -1;
  m_lineColor3Loc = -1;
  m_count1Loc = -1;
  m_count2Loc = -1;
  m_count3Loc = -1;
  m_scroll1Loc = -1;
  m_scroll2Loc = -1;
  m_scroll3Loc = -1;
  m_lineWidthLoc = -1;
  m_graphFillOpacityLoc = -1;
  m_texWidthLoc = -1;
  m_resXLoc = -1;
  m_resYLoc = -1;
  m_aaSizeLoc = -1;
  m_dataSourceLoc = -1;
}

void GraphProgram::draw(
    TextureId dataTexture, int texWidth, float surfaceWidth, float surfaceHeight, float width, float height,
    const GraphStyle& style, const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0f || height <= 0.0f || dataTexture == 0) {
    return;
  }

  static constexpr std::array<GLfloat, 12> kQuad = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLoc, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLoc, width, height);
  glUniformMatrix3fv(m_transformLoc, 1, GL_FALSE, transform.m.data());

  glUniform4f(m_lineColor1Loc, style.lineColor1.r, style.lineColor1.g, style.lineColor1.b, style.lineColor1.a);
  glUniform4f(m_lineColor2Loc, style.lineColor2.r, style.lineColor2.g, style.lineColor2.b, style.lineColor2.a);
  glUniform4f(m_lineColor3Loc, style.lineColor3.r, style.lineColor3.g, style.lineColor3.b, style.lineColor3.a);
  glUniform1f(m_count1Loc, style.count1);
  glUniform1f(m_count2Loc, style.count2);
  glUniform1f(m_count3Loc, style.count3);
  glUniform1f(m_scroll1Loc, style.scroll1);
  glUniform1f(m_scroll2Loc, style.scroll2);
  glUniform1f(m_scroll3Loc, style.scroll3);
  glUniform1f(m_lineWidthLoc, style.lineWidth);
  glUniform1f(m_graphFillOpacityLoc, style.graphFillOpacity);
  glUniform1f(m_texWidthLoc, static_cast<float>(texWidth));
  glUniform1f(m_resXLoc, width);
  glUniform1f(m_resYLoc, height);
  glUniform1f(m_aaSizeLoc, style.aaSize);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(dataTexture.value()));
  glUniform1i(m_dataSourceLoc, 0);

  const auto posAttr = static_cast<GLuint>(m_positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
