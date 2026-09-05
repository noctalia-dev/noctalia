#include "render/programs/audio_spectrum_program.h"

#include "render/core/render_styles.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

  constexpr float kGapToBarRatio = 0.5F;

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
    t = std::clamp(t, 0.0F, 1.0F);
    return Color{
        .r = low.r + (high.r - low.r) * t,
        .g = low.g + (high.g - low.g) * t,
        .b = low.b + (high.b - low.b) * t,
        .a = low.a + (high.a - low.a) * t,
    };
  }

  void pushVertex(std::vector<GLfloat>& out, float x, float y, const Color& color) {
    const float alpha = std::clamp(color.a, 0.0F, 1.0F);
    out.push_back(x);
    out.push_back(y);
    out.push_back(color.r * alpha);
    out.push_back(color.g * alpha);
    out.push_back(color.b * alpha);
    out.push_back(alpha);
  }

  void pushQuad(std::vector<GLfloat>& out, float x0, float y0, float x1, float y1, const Color& color) {
    if (x1 <= x0 || y1 <= y0 || color.a <= 0.0F) {
      return;
    }
    pushVertex(out, x0, y0, color);
    pushVertex(out, x1, y0, color);
    pushVertex(out, x0, y1, color);
    pushVertex(out, x0, y1, color);
    pushVertex(out, x1, y0, color);
    pushVertex(out, x1, y1, color);
  }

  float snapToPixel(float value, float pixelScale) { return std::floor(value * pixelScale + 0.5F) / pixelScale; }

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

void AudioSpectrumProgram::abandon() noexcept { m_program.abandon(); }

void AudioSpectrumProgram::draw(
    float surfaceWidth, float surfaceHeight, float pixelScaleX, float pixelScaleY, float width, float height,
    const AudioSpectrumStyle& style, std::span<const float> values, const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0F || height <= 0.0F || values.empty()) {
    return;
  }

  const int valueCount = static_cast<int>(values.size());
  const int barCount = style.mirrored ? valueCount * 2 : valueCount;
  if (barCount <= 0) {
    return;
  }

  const bool horizontal = style.orientation == AudioSpectrumOrientation::Horizontal;
  const float safePixelScaleX = std::max(0.001F, pixelScaleX);
  const float safePixelScaleY = std::max(0.001F, pixelScaleY);
  const float mainPixelScale = horizontal ? safePixelScaleX : safePixelScaleY;
  const float crossPixelScale = horizontal ? safePixelScaleY : safePixelScaleX;
  const float mainAxisLen = horizontal ? width : height;
  const float crossAxisLen = horizontal ? height : width;
  const int gapCount = std::max(0, barCount - 1);
  const float weightedSlots = static_cast<float>(barCount) + static_cast<float>(gapCount) * kGapToBarRatio;
  const float devicePixel = 1.0F / mainPixelScale;
  const bool compactBars = mainAxisLen * mainPixelScale < static_cast<float>(barCount + gapCount);
  const float barThickness = compactBars
      ? mainAxisLen / static_cast<float>(barCount)
      : std::max(
            devicePixel, std::floor(mainAxisLen / std::max(1.0F, weightedSlots) * mainPixelScale) / mainPixelScale
        );
  const float gapThickness = compactBars || gapCount == 0
      ? 0.0F
      : std::max(devicePixel, std::floor(barThickness * kGapToBarRatio * mainPixelScale) / mainPixelScale);
  const float stride = barThickness + gapThickness;
  const float used = barThickness * static_cast<float>(barCount) + gapThickness * static_cast<float>(gapCount);
  const float startOffset = std::floor(std::max(0.0F, (mainAxisLen - used) * 0.5F) * mainPixelScale) / mainPixelScale;

  m_vertices.clear();
  m_vertices.reserve(static_cast<std::size_t>(barCount) * 6U * 6U);

  if (style.wave) {
    constexpr int kTessellation = 4;

    std::vector<float> bandValues(static_cast<std::size_t>(barCount));
    for (int i = 0; i < barCount; ++i) {
      const int baseIndex = style.mirrored ? (i < valueCount ? valueCount - 1 - i : i - valueCount) : i;
      const int valueIndex = style.reversed ? valueCount - 1 - baseIndex : baseIndex;
      bandValues[static_cast<std::size_t>(i)] = valueIndex >= 0 && valueIndex < valueCount
          ? std::clamp(values[static_cast<std::size_t>(valueIndex)], 0.0F, 1.0F)
          : 0.0F;
    }

    // Wave renders from a fixed number of control points pooled from the band data, so
    // hill width follows the widget size and stays independent of the bands setting.
    // The baseline lifts valleys so the silhouette reads as one rolling wave, not teeth.
    constexpr int kWaveControlPoints = 32;
    constexpr float kWaveBaseline = 0.10F;
    const int pointCount = std::min(barCount, kWaveControlPoints);
    std::vector<float> wavePoints(static_cast<std::size_t>(pointCount));
    for (int i = 0; i < pointCount; ++i) {
      const int s0 = i * barCount / pointCount;
      const int s1 = std::max(s0 + 1, (i + 1) * barCount / pointCount);
      float sum = 0.0F;
      for (int j = s0; j < s1; ++j) {
        sum += bandValues[static_cast<std::size_t>(j)];
      }
      wavePoints[static_cast<std::size_t>(i)] =
          kWaveBaseline + (1.0F - kWaveBaseline) * std::pow(sum / static_cast<float>(s1 - s0), 0.4F);
    }
    bandValues = std::move(wavePoints);

    // Blur across control points so raw FFT comb structure doesn't cut narrow teeth.
    std::vector<float> smoothed(bandValues.size());
    for (int i = 0; i < pointCount; ++i) {
      const auto get = [&bandValues, pointCount](int idx) -> float {
        return bandValues[static_cast<std::size_t>(std::clamp(idx, 0, pointCount - 1))];
      };
      smoothed[static_cast<std::size_t>(i)] = 0.25F * get(i - 1) + 0.5F * get(i) + 0.25F * get(i + 1);
    }
    bandValues = std::move(smoothed);

    auto sampleValue = [&bandValues](float position) {
      const auto count = static_cast<float>(bandValues.size());
      const float clamped = std::clamp(position, 0.0F, count - 1.0F);
      const float fi = std::floor(clamped);
      const int i = static_cast<int>(fi);
      const float frac = clamped - fi;
      const auto get = [&bandValues](int idx) -> float {
        return bandValues[static_cast<std::size_t>(std::clamp(idx, 0, static_cast<int>(bandValues.size()) - 1))];
      };
      if (frac == 0.0F) {
        return get(i);
      }
      const float p0 = get(i - 1);
      const float p1 = get(i);
      const float p2 = get(i + 1);
      const float p3 = get(i + 2);
      const float t2 = frac * frac;
      const float t3 = t2 * frac;
      return (1.0F / 6.0F)
          * ((-t3 + 3.0F * t2 - 3.0F * frac + 1.0F) * p0
             + (3.0F * t3 - 6.0F * t2 + 4.0F) * p1
             + (-3.0F * t3 + 3.0F * t2 + 3.0F * frac + 1.0F) * p2
             + t3 * p3);
    };

    // Keep the polyline dense enough that wide surfaces don't reveal straight segments.
    const auto minSlices = static_cast<int>(mainAxisLen * mainPixelScale / 3.0F);
    const int totalSlices = std::clamp(minSlices, pointCount * kTessellation, 1024);
    m_vertices.reserve(static_cast<std::size_t>(totalSlices) * 24U * 6U);
    // Half-device-pixel skirt with a per-vertex alpha ramp acts as edge antialiasing;
    // the rasterizer would otherwise stair-step the curved top.
    const float aa = 0.5F / crossPixelScale;
    const float sliceWidth = mainAxisLen / static_cast<float>(totalSlices);
    // Taper the wave into the baseline at both ends instead of ending in a vertical cliff.
    constexpr float kWaveEdgeFade = 0.18F;
    const auto edgeFade = [pointCount](float position) {
      const auto smooth = [](float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0F, 1.0F);
        return t * t * (3.0F - 2.0F * t);
      };
      const float u = pointCount <= 1 ? 0.0F : position / static_cast<float>(pointCount - 1);
      return smooth(0.0F, kWaveEdgeFade, u) * smooth(1.0F, 1.0F - kWaveEdgeFade, u);
    };

    auto waveCross = [&](float position, float offset, float scale) -> float {
      const float raw = std::max(1.0F / crossPixelScale, sampleValue(position + offset) * scale * crossAxisLen);
      return raw * edgeFade(position);
    };

    const auto emitWaveLayer = [&](float offset, float scale, bool solidColor2) {
      for (int s = 0; s < totalSlices; ++s) {
        const float pos0 = static_cast<float>(s) * static_cast<float>(pointCount) / static_cast<float>(totalSlices);
        const float pos1 = static_cast<float>(s + 1) * static_cast<float>(pointCount) / static_cast<float>(totalSlices);
        const float x0 = static_cast<float>(s) * sliceWidth;
        const float x1 = static_cast<float>(s + 1) * sliceWidth;
        const float topY0 = crossAxisLen - waveCross(pos0, offset, scale);
        const float topY1 = crossAxisLen - waveCross(pos1, offset, scale);
        const float bottomY = crossAxisLen;

        const float t0 = pointCount <= 1 ? 0.0F : std::clamp(pos0 / static_cast<float>(pointCount - 1), 0.0F, 1.0F);
        const float t1 = pointCount <= 1 ? 0.0F : std::clamp(pos1 / static_cast<float>(pointCount - 1), 0.0F, 1.0F);
        Color color0 = solidColor2 ? style.color2 : colorAt(style.color1, style.color2, t0);
        Color color1 = solidColor2 ? style.color2 : colorAt(style.color1, style.color2, t1);
        Color fade0 = color0;
        fade0.a = 0.0F;
        Color fade1 = color1;
        fade1.a = 0.0F;

        if (horizontal) {
          pushVertex(m_vertices, x0, topY0 - aa, fade0);
          pushVertex(m_vertices, x1, topY1 - aa, fade1);
          pushVertex(m_vertices, x0, topY0 + aa, color0);
          pushVertex(m_vertices, x0, topY0 + aa, color0);
          pushVertex(m_vertices, x1, topY1 - aa, fade1);
          pushVertex(m_vertices, x1, topY1 + aa, color1);
          pushVertex(m_vertices, x0, topY0 + aa, color0);
          pushVertex(m_vertices, x0, bottomY, color0);
          pushVertex(m_vertices, x1, topY1 + aa, color1);
          pushVertex(m_vertices, x0, bottomY, color0);
          pushVertex(m_vertices, x1, bottomY, color1);
          pushVertex(m_vertices, x1, topY1 + aa, color1);
        } else {
          pushVertex(m_vertices, topY0 - aa, x0, fade0);
          pushVertex(m_vertices, topY1 - aa, x1, fade1);
          pushVertex(m_vertices, topY0 + aa, x0, color0);
          pushVertex(m_vertices, topY0 + aa, x0, color0);
          pushVertex(m_vertices, topY1 - aa, x1, fade1);
          pushVertex(m_vertices, topY1 + aa, x1, color1);
          pushVertex(m_vertices, topY0 + aa, x0, color0);
          pushVertex(m_vertices, bottomY, x0, color0);
          pushVertex(m_vertices, topY1 + aa, x1, color1);
          pushVertex(m_vertices, bottomY, x0, color0);
          pushVertex(m_vertices, bottomY, x1, color1);
          pushVertex(m_vertices, topY1 + aa, x1, color1);
        }
      }
    };

    // Secondary echo wave drawn in front of the main curve in the secondary color,
    // phase-shifted and scaled down so its peaks layer over the main wave.
    constexpr float kWaveSecondaryOffset = 0.3F;
    constexpr float kWaveSecondaryScale = 0.55F;
    emitWaveLayer(0.0F, 1.0F, false);
    emitWaveLayer(static_cast<float>(pointCount) * kWaveSecondaryOffset, kWaveSecondaryScale, true);
  } else {
    for (int i = 0; i < barCount; ++i) {
      const int baseIndex = style.mirrored ? (i < valueCount ? valueCount - 1 - i : i - valueCount) : i;
      const int valueIndex = style.reversed ? valueCount - 1 - baseIndex : baseIndex;
      const float rawValue = valueIndex >= 0 && valueIndex < valueCount
          ? std::clamp(values[static_cast<std::size_t>(valueIndex)], 0.0F, 1.0F)
          : 0.0F;
      float crossPixels = std::max(1.0F, std::floor(rawValue * crossAxisLen * crossPixelScale + 0.5F));
      if (style.centered && crossPixels > 1.0F) {
        crossPixels = std::max(2.0F, std::round(crossPixels * 0.5F) * 2.0F);
      }
      const float crossSize = crossPixels / crossPixelScale;

      float mainStart = compactBars ? startOffset + static_cast<float>(i) * stride
                                    : snapToPixel(startOffset + static_cast<float>(i) * stride, mainPixelScale);
      float mainEnd = mainStart + barThickness;
      if (mainStart < 0.0F) {
        mainEnd -= mainStart;
        mainStart = 0.0F;
      }
      if (mainEnd > mainAxisLen) {
        mainStart = std::max(0.0F, mainStart - (mainEnd - mainAxisLen));
        mainEnd = mainAxisLen;
      }
      float crossStart =
          snapToPixel(style.centered ? (crossAxisLen - crossSize) * 0.5F : crossAxisLen - crossSize, crossPixelScale);
      float crossEnd = crossStart + crossSize;
      if (crossStart < 0.0F) {
        crossEnd -= crossStart;
        crossStart = 0.0F;
      }
      if (crossEnd > crossAxisLen) {
        crossStart = std::max(0.0F, crossStart - (crossEnd - crossAxisLen));
        crossEnd = crossAxisLen;
      }
      const float t = barCount <= 1 ? 0.0F : static_cast<float>(i) / static_cast<float>(barCount - 1);
      const Color color = colorAt(style.color1, style.color2, t);

      if (horizontal) {
        pushQuad(m_vertices, mainStart, crossStart, mainEnd, crossEnd, color);
      } else {
        pushQuad(m_vertices, crossStart, mainStart, crossEnd, mainEnd, color);
      }
    }
  }

  if (m_vertices.empty()) {
    return;
  }

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_pixelScaleLocation, safePixelScaleX, safePixelScaleY);
  const bool canSnapToDevice = !compactBars && std::abs(transform.m[1]) < 0.0001F && std::abs(transform.m[3]) < 0.0001F;
  glUniform1f(m_snapToDeviceLocation, canSnapToDevice ? 1.0F : 0.0F);
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
