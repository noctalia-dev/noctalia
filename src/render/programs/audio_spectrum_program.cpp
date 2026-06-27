#include "render/programs/audio_spectrum_program.h"

#include "render/core/render_styles.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

  constexpr float kGapToBarRatio = 0.5f;

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
attribute vec4 a_color;
uniform vec2 u_surface_size;
uniform vec2 u_pixel_scale;
uniform float u_snap_to_device;
uniform mat3 u_transform;
varying vec4 v_color;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec3 pixel = u_transform * vec3(a_position, 1.0);
    if (u_snap_to_device > 0.5) {
        pixel.xy = floor(pixel.xy * u_pixel_scale + 0.5) / u_pixel_scale;
    }
    v_color = a_color;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

varying vec4 v_color;

void main() {
    gl_FragColor = v_color;
}
)";

  Color colorAt(const Color& low, const Color& high, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{
        .r = low.r + (high.r - low.r) * t,
        .g = low.g + (high.g - low.g) * t,
        .b = low.b + (high.b - low.b) * t,
        .a = low.a + (high.a - low.a) * t,
    };
  }

  void pushVertex(std::vector<GLfloat>& out, float x, float y, const Color& color) {
    const float alpha = std::clamp(color.a, 0.0f, 1.0f);
    out.push_back(x);
    out.push_back(y);
    out.push_back(color.r * alpha);
    out.push_back(color.g * alpha);
    out.push_back(color.b * alpha);
    out.push_back(alpha);
  }

  void pushQuad(std::vector<GLfloat>& out, float x0, float y0, float x1, float y1, const Color& color) {
    if (x1 <= x0 || y1 <= y0 || color.a <= 0.0f) {
      return;
    }
    pushVertex(out, x0, y0, color);
    pushVertex(out, x1, y0, color);
    pushVertex(out, x0, y1, color);
    pushVertex(out, x0, y1, color);
    pushVertex(out, x1, y0, color);
    pushVertex(out, x1, y1, color);
  }

  float snapToPixel(float value, float pixelScale) { return std::floor(value * pixelScale + 0.5f) / pixelScale; }

} // namespace

void AudioSpectrumProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_colorLocation = glGetAttribLocation(m_program.id(), "a_color");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_pixelScaleLocation = glGetUniformLocation(m_program.id(), "u_pixel_scale");
  m_snapToDeviceLocation = glGetUniformLocation(m_program.id(), "u_snap_to_device");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_positionLocation < 0
      || m_colorLocation < 0
      || m_surfaceSizeLocation < 0
      || m_pixelScaleLocation < 0
      || m_snapToDeviceLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query audio spectrum shader locations");
  }
}

void AudioSpectrumProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_colorLocation = -1;
  m_surfaceSizeLocation = -1;
  m_pixelScaleLocation = -1;
  m_snapToDeviceLocation = -1;
  m_transformLocation = -1;
  m_vertices.clear();
  m_vertices.shrink_to_fit();
}

void AudioSpectrumProgram::draw(
    float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
    const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0f || height <= 0.0f || values.empty()) {
    return;
  }

  const int valueCount = static_cast<int>(values.size());
  const int barCount = style.mirrored ? valueCount * 2 : valueCount;
  if (barCount <= 0) {
    return;
  }

  const bool horizontal = style.orientation == AudioSpectrumOrientation::Horizontal;
  const float safePixelScaleX = std::max(0.001f, pixelScaleX);
  const float safePixelScaleY = std::max(0.001f, pixelScaleY);
  const float mainPixelScale = horizontal ? safePixelScaleX : safePixelScaleY;
  const float crossPixelScale = horizontal ? safePixelScaleY : safePixelScaleX;
  const float mainAxisLen = horizontal ? width : height;
  const float crossAxisLen = horizontal ? height : width;
  const int gapCount = std::max(0, barCount - 1);
  const float weightedSlots = static_cast<float>(barCount) + static_cast<float>(gapCount) * kGapToBarRatio;
  const float devicePixel = 1.0f / mainPixelScale;
  const bool compactBars = mainAxisLen * mainPixelScale < static_cast<float>(barCount + gapCount);
  const float barThickness = compactBars
      ? mainAxisLen / static_cast<float>(barCount)
      : std::max(
            devicePixel, std::floor(mainAxisLen / std::max(1.0f, weightedSlots) * mainPixelScale) / mainPixelScale
        );
  const float gapThickness = compactBars || gapCount == 0
      ? 0.0f
      : std::max(devicePixel, std::floor(barThickness * kGapToBarRatio * mainPixelScale) / mainPixelScale);
  const float stride = barThickness + gapThickness;
  const float used = barThickness * static_cast<float>(barCount) + gapThickness * static_cast<float>(gapCount);
  const float startOffset = std::floor(std::max(0.0f, (mainAxisLen - used) * 0.5f) * mainPixelScale) / mainPixelScale;

  m_vertices.clear();
  m_vertices.reserve(static_cast<std::size_t>(barCount) * 6U * 6U);

  for (int i = 0; i < barCount; ++i) {
    const int valueIndex = style.mirrored ? (i < valueCount ? valueCount - 1 - i : i - valueCount) : i;
    const float rawValue = valueIndex >= 0 && valueIndex < valueCount
        ? std::clamp(values[static_cast<std::size_t>(valueIndex)], 0.0f, 1.0f)
        : 0.0f;
    float crossPixels = std::max(1.0f, std::floor(rawValue * crossAxisLen * crossPixelScale + 0.5f));
    if (style.centered && crossPixels > 1.0f) {
      crossPixels = std::max(2.0f, std::round(crossPixels * 0.5f) * 2.0f);
    }
    const float crossSize = crossPixels / crossPixelScale;

    float mainStart = compactBars ? startOffset + static_cast<float>(i) * stride
                                  : snapToPixel(startOffset + static_cast<float>(i) * stride, mainPixelScale);
    float mainEnd = mainStart + barThickness;
    if (mainStart < 0.0f) {
      mainEnd -= mainStart;
      mainStart = 0.0f;
    }
    if (mainEnd > mainAxisLen) {
      mainStart = std::max(0.0f, mainStart - (mainEnd - mainAxisLen));
      mainEnd = mainAxisLen;
    }
    float crossStart =
        snapToPixel(style.centered ? (crossAxisLen - crossSize) * 0.5f : crossAxisLen - crossSize, crossPixelScale);
    float crossEnd = crossStart + crossSize;
    if (crossStart < 0.0f) {
      crossEnd -= crossStart;
      crossStart = 0.0f;
    }
    if (crossEnd > crossAxisLen) {
      crossStart = std::max(0.0f, crossStart - (crossEnd - crossAxisLen));
      crossEnd = crossAxisLen;
    }
    const float t = barCount <= 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(barCount - 1);
    const Color color = colorAt(style.color1, style.color2, t);

    if (horizontal) {
      pushQuad(m_vertices, mainStart, crossStart, mainEnd, crossEnd, color);
    } else {
      pushQuad(m_vertices, crossStart, mainStart, crossEnd, mainEnd, color);
    }
  }

  if (m_vertices.empty()) {
    return;
  }

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_pixelScaleLocation, safePixelScaleX, safePixelScaleY);
  const bool canSnapToDevice = !compactBars && std::abs(transform.m[1]) < 0.0001f && std::abs(transform.m[3]) < 0.0001f;
  glUniform1f(m_snapToDeviceLocation, canSnapToDevice ? 1.0f : 0.0f);
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, transform.m.data());

  constexpr auto kStride = static_cast<GLsizei>(sizeof(GLfloat) * 6U);
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  const auto colorAttr = static_cast<GLuint>(m_colorLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, kStride, m_vertices.data());
  glVertexAttribPointer(colorAttr, 4, GL_FLOAT, GL_FALSE, kStride, m_vertices.data() + 2);
  glEnableVertexAttribArray(posAttr);
  glEnableVertexAttribArray(colorAttr);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices.size() / 6U));
  glDisableVertexAttribArray(colorAttr);
  glDisableVertexAttribArray(posAttr);
}
