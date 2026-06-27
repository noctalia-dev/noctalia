#include "render/programs/fancy_audio_visualizer_program.h"

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

uniform sampler2D u_audio_source;
uniform float u_time;
uniform float u_item_width;
uniform float u_item_height;
uniform vec4 u_primary_color;
uniform vec4 u_secondary_color;
uniform float u_sensitivity;
uniform float u_rotation_speed;
uniform float u_bar_width;
uniform float u_ring_opacity;
uniform float u_corner_radius;
uniform float u_bloom_intensity;
uniform float u_mode;
uniform float u_wave_thickness;
uniform float u_inner_diameter;
varying vec2 v_texcoord;

#define TWOPI 6.28318530718
#define PI 3.14159265359
#define NBARS 32.0
#define MAX_VISUAL_RADIUS 0.95

bool hasRings() { return u_mode >= 2.0; }
bool hasBars() { return u_mode == 0.0 || u_mode == 3.0 || u_mode >= 5.0; }
bool hasWave() { return u_mode == 1.0 || u_mode == 4.0 || u_mode >= 5.0; }

float getAudio(float pos) {
    return texture2D(u_audio_source, vec2(clamp(pos, 0.0, 1.0), 0.5)).r;
}

float smoothAudio(float pos) {
    float idx = pos * (NBARS - 1.0);
    float fracPart = fract(idx);
    float i0 = floor(idx) / (NBARS - 1.0);
    float i1 = ceil(idx) / (NBARS - 1.0);
    return mix(getAudio(i0), getAudio(i1), fracPart) * u_sensitivity;
}

float getBass() { return smoothAudio(0.05); }
float getMid() { return smoothAudio(0.3); }
float getHighMid() { return smoothAudio(0.6); }
float getTreble() { return smoothAudio(0.9); }

float roundedBoxSDF(vec2 center, vec2 size, float radius) {
    vec2 q = abs(center) - size + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float catmullAudio(float mirroredPos) {
    float bandPos = mirroredPos * (NBARS - 1.0);
    float fband1 = floor(bandPos);
    float fband0 = max(fband1 - 1.0, 0.0);
    float fband2 = min(fband1 + 1.0, NBARS - 1.0);
    float fband3 = min(fband1 + 2.0, NBARS - 1.0);

    float t = fract(bandPos);
    float p0 = getAudio(fband0 / (NBARS - 1.0)) * u_sensitivity;
    float p1 = getAudio(fband1 / (NBARS - 1.0)) * u_sensitivity;
    float p2 = getAudio(fband2 / (NBARS - 1.0)) * u_sensitivity;
    float p3 = getAudio(fband3 / (NBARS - 1.0)) * u_sensitivity;

    float t2 = t * t;
    float t3 = t2 * t;
    float smoothed = 0.5 * (
        (2.0 * p1) +
        (-p0 + p2) * t +
        (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
        (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3
    );
    return max(smoothed, 0.0);
}

void addRings(inout vec4 color, float theta, float d, float iTime, float bass, float mid, float highMid, float treble) {
    float innerRadius = u_inner_diameter / 2.0;

    if (d < innerRadius * 0.6) {
        float wave = mid * 0.8;
        float ripple = sin(d * 25.0 + wave * 15.0 - iTime * 2.0);
        if (ripple > 0.7) {
            float intensity = clamp(mid * 0.6, 0.0, 0.4);
            color = max(color, u_secondary_color * intensity * u_ring_opacity);
        }
    }

    float energyRad = innerRadius * 0.65;
    float energyThickness = 0.015 + clamp(highMid * 0.02, 0.0, 0.03);
    if (d > energyRad - energyThickness && d < energyRad + energyThickness) {
        float segmentAngle = theta * 8.0 + highMid * 3.0 + iTime;
        if (mod(segmentAngle, 1.0) < 0.6) {
            float alpha = clamp(highMid * 2.0, 0.3, 1.0) * u_ring_opacity;
            color = max(color, mix(u_primary_color, u_secondary_color, 0.5) * alpha);
        }
    }

    float particleRad = innerRadius * 0.75;
    if (d > particleRad - 0.02 && d < particleRad + 0.02) {
        float particleAngle = theta + treble * 2.0 + iTime * 0.5;
        float particleSpacing = TWOPI / 16.0;
        if (mod(particleAngle, particleSpacing) < 0.15) {
            float brightness = 0.5 + clamp(treble * 1.5, 0.0, 0.5);
            color = max(color, u_secondary_color * brightness * u_ring_opacity);
        }
    }

    float gridRad = innerRadius * 0.85;
    if (d > gridRad - 0.012 && d < gridRad + 0.012) {
        float gridAngle = theta + iTime * u_rotation_speed;
        float gridDensity = 0.08 + clamp(mid * 0.05, 0.0, 0.1);
        if (mod(gridAngle, 0.2) < gridDensity) {
            vec4 gridColor = u_primary_color * 0.7 * u_ring_opacity;
            gridColor.rgb += vec3(0.1, 0.15, 0.2) * clamp(mid, 0.0, 0.8);
            color = max(color, gridColor);
        }
    }

    float accentRad = innerRadius * 0.92;
    float pulse = clamp(bass * 0.08, 0.0, 0.05);
    if (d > accentRad - pulse - 0.008 && d < accentRad + pulse + 0.015) {
        float colorShift = clamp(bass * 0.5, 0.0, 1.0);
        vec4 ringColor = mix(u_secondary_color * 0.7, u_primary_color, colorShift);
        ringColor.a = u_ring_opacity * max(u_primary_color.a, u_secondary_color.a);
        ringColor.rgb *= 1.0 + bass * 0.3;
        color = max(color, ringColor);
    }

    float outerRad = innerRadius + bass * 0.05;
    if (d > outerRad - 0.008 && d < outerRad + 0.008) {
        vec4 outerColor = u_primary_color * u_ring_opacity;
        outerColor.rgb += vec3(0.2, 0.3, 0.4) * clamp(treble * 0.5, 0.0, 0.3);
        outerColor.rgb *= 1.0 + bass * 0.4;
        color = max(color, outerColor);
    }
}

vec4 computePolarWave(vec2 uv, float iTime, float bass, float mid, float highMid, float treble) {
    float aspect = u_item_width / u_item_height;
    vec2 centered = (uv - 0.5) * 2.0;
    centered.x *= aspect;

    float theta = atan(centered.y, centered.x);
    float d = length(centered);
    float innerRadius = u_inner_diameter / 2.0;
    float baseRadius = 0.35;

    vec4 color = vec4(0.0);
    if (hasRings()) {
        addRings(color, theta, d, iTime, bass, mid, highMid, treble);
    }

    if (hasWave()) {
        float adjustedTheta = theta + PI + iTime * u_rotation_speed * 0.2;
        float normalizedAngle = mod(adjustedTheta, TWOPI) / TWOPI;
        float mirroredPos = normalizedAngle < 0.5 ? normalizedAngle * 2.0 : (1.0 - normalizedAngle) * 2.0;
        float smoothedAudio = catmullAudio(mirroredPos);
        float waveRadius = min(baseRadius + smoothedAudio * 0.5, MAX_VISUAL_RADIUS);

        if (d >= innerRadius && d <= waveRadius) {
            float fillFactor = (d - innerRadius) / max(waveRadius - innerRadius, 0.001);
            vec3 fillColor = mix(u_primary_color.rgb * 0.8, u_secondary_color.rgb, fillFactor);
            fillColor *= 1.0 + bass * 0.3;
            float fillAlpha = clamp(mix(0.4, 1.0, fillFactor) * u_wave_thickness, 0.0, 1.0);
            color = max(color, vec4(fillColor, fillAlpha * max(u_primary_color.a, u_secondary_color.a)));
        }

        float edgeThickness = u_wave_thickness * 0.025;
        float distToEdge = abs(d - waveRadius);
        if (distToEdge < edgeThickness) {
            float edgeFactor = 1.0 - smoothstep(0.0, edgeThickness, distToEdge);
            vec3 edgeColor = u_secondary_color.rgb * (1.2 + smoothedAudio * 0.5);
            if (smoothedAudio > 0.5) {
                edgeColor += vec3(0.3, 0.4, 0.5) * (smoothedAudio - 0.5);
            }
            color = max(color, vec4(edgeColor, edgeFactor * u_secondary_color.a));
        }
    }

    return color;
}

vec4 computeBars(vec2 uv, float iTime, float bass, float mid, float highMid, float treble) {
    float aspect = u_item_width / u_item_height;
    vec2 centered = (uv - 0.5) * 2.0;
    centered.x *= aspect;

    float theta = atan(centered.y, centered.x);
    float d = length(centered);
    float innerRadius = u_inner_diameter / 2.0;
    float baseRadius = 0.35;

    vec4 color = vec4(0.0);
    if (hasRings()) {
        addRings(color, theta, d, iTime, bass, mid, highMid, treble);
    }

    if (hasBars() && d > innerRadius) {
        float section = TWOPI / (NBARS * 2.0);
        float center = section / 2.0;
        float adjustedTheta = theta + PI + iTime * u_rotation_speed * 0.2;
        float m = mod(adjustedTheta, section);
        float ym = d * sin(center - m);
        float barW = u_bar_width * 0.015;

        if (abs(ym) < barW) {
            float circlePos = mod(adjustedTheta, TWOPI) / TWOPI;
            float mirroredPos = circlePos < 0.5 ? circlePos * 2.0 : (1.0 - circlePos) * 2.0;
            float v = smoothAudio(mirroredPos);
            float wave = sin(theta * 3.0 + mid * 5.0) * clamp(mid * 0.03, 0.0, 0.05);
            v = max(v + wave, 0.0);

            float barStart = innerRadius;
            float barEnd = min(baseRadius + v * 0.5, MAX_VISUAL_RADIUS);
            if (d >= barStart && d <= barEnd) {
                float heightFactor = (d - barStart) / max(barEnd - barStart, 0.001);
                vec3 bottomColor = u_primary_color.rgb * 0.6;
                vec3 middleColor = u_primary_color.rgb;
                vec3 topColor = u_secondary_color.rgb;
                vec3 barColor = heightFactor < 0.5
                    ? mix(bottomColor, middleColor, heightFactor * 2.0)
                    : mix(middleColor, topColor, (heightFactor - 0.5) * 2.0);

                barColor *= 1.0 + bass * 0.4;
                if (heightFactor > 0.85) {
                    barColor += vec3(0.3, 0.4, 0.5) * clamp(treble * 0.8, 0.0, 0.5);
                }

                float edgeFactor = 1.0 - smoothstep(barW * 0.7, barW, abs(ym));
                color = max(color, vec4(barColor, edgeFactor * max(u_primary_color.a, u_secondary_color.a)));
            }
        }
    }

    return color;
}

void addBloom(inout vec4 color, vec2 uv, float iTime, float bass, float mid, float highMid, float treble) {
    if (u_bloom_intensity <= 0.01 || color.a >= 0.01) {
        return;
    }

    float aspect = u_item_width / u_item_height;
    vec2 centered = (uv - 0.5) * 2.0;
    centered.x *= aspect;
    float d = length(centered);
    float theta = atan(centered.y, centered.x);

    float innerRadius = u_inner_diameter / 2.0;
    float baseRadius = 0.35;
    float glowAmount = 0.0;
    vec3 glowColor = vec3(0.0);

    if (hasRings()) {
        float outerRad = innerRadius + bass * 0.05;
        float ringDist = abs(d - outerRad);
        float ringGlow = exp(-ringDist * 8.0 / u_bloom_intensity) * (1.0 + bass * 0.5);
        glowColor += u_primary_color.rgb * ringGlow;
        glowAmount = max(glowAmount, ringGlow);

        float accentRad = innerRadius * 0.92;
        float accentDist = abs(d - accentRad);
        float accentGlow = exp(-accentDist * 10.0 / u_bloom_intensity) * (0.7 + bass * 0.3);
        glowColor += mix(u_secondary_color.rgb, u_primary_color.rgb, 0.5) * accentGlow;
        glowAmount = max(glowAmount, accentGlow);
    }

    if ((hasBars() || hasWave()) && d > innerRadius * 0.8) {
        float adjustedTheta = theta + PI + iTime * u_rotation_speed * 0.2;
        float circlePos = mod(adjustedTheta, TWOPI) / TWOPI;
        float mirroredPos = circlePos < 0.5 ? circlePos * 2.0 : (1.0 - circlePos) * 2.0;
        float v = smoothAudio(mirroredPos);

        if (hasWave()) {
            float smoothedAudio = catmullAudio(mirroredPos);
            float waveRadius = min(baseRadius + smoothedAudio * 0.5, MAX_VISUAL_RADIUS);
            float distToWave = abs(d - waveRadius);
            float waveGlow = exp(-distToWave * 8.0 / u_bloom_intensity) * smoothedAudio * 2.5;
            glowColor += mix(u_primary_color.rgb, u_secondary_color.rgb, smoothedAudio) * waveGlow;
            glowAmount = max(glowAmount, waveGlow);
        }

        if (hasBars()) {
            float section = TWOPI / (NBARS * 2.0);
            float m = mod(adjustedTheta, section);
            float center = section / 2.0;
            float barAngleDist = min(abs(m - center), section - abs(m - center));
            float barEnd = min(baseRadius + v * 0.5, MAX_VISUAL_RADIUS);
            float radialDist = d < innerRadius ? innerRadius - d : (d > barEnd ? d - barEnd : 0.0);
            float totalDist = length(vec2(barAngleDist * d, radialDist));
            float barGlow = exp(-totalDist * 15.0 / u_bloom_intensity) * v * 2.0;
            float heightFactor = clamp((d - innerRadius) / max(barEnd - innerRadius, 0.001), 0.0, 1.0);
            glowColor += mix(u_primary_color.rgb, u_secondary_color.rgb, heightFactor) * barGlow;
            glowAmount = max(glowAmount, barGlow);
        }
    }

    float bloomMult = u_bloom_intensity * (1.0 + bass * 0.5);
    color.rgb = min(glowColor * bloomMult, vec3(1.5));
    color.a = min(glowAmount * bloomMult * 0.6 * max(u_primary_color.a, u_secondary_color.a), 0.8);
}

void main() {
    vec2 uv = v_texcoord;
    float iTime = sin(u_time * TWOPI / 3600.0) * 1800.0 + 1800.0;

    float bass = getBass();
    float mid = getMid();
    float highMid = getHighMid();
    float treble = getTreble();

    vec4 color;
    if (hasWave() && !hasBars()) {
        color = computePolarWave(uv, iTime, bass, mid, highMid, treble);
    } else if (hasBars() && !hasWave()) {
        color = computeBars(uv, iTime, bass, mid, highMid, treble);
    } else if (hasWave() && hasBars()) {
        color = max(computeBars(uv, iTime, bass, mid, highMid, treble), computePolarWave(uv, iTime, bass, mid, highMid, treble));
    } else {
        color = computeBars(uv, iTime, bass, mid, highMid, treble);
    }

    addBloom(color, uv, iTime, bass, mid, highMid, treble);

    vec2 fromCenter = (v_texcoord - 0.5) * 2.0;
    float edgeProximity = max(abs(fromCenter.x), abs(fromCenter.y));
    float fadeStart = MAX_VISUAL_RADIUS;
    color *= 1.0 - smoothstep(fadeStart, 1.0, edgeProximity);

    vec2 pixelPos = v_texcoord * vec2(u_item_width, u_item_height);
    vec2 centerPos = pixelPos - vec2(u_item_width, u_item_height) * 0.5;
    vec2 halfSize = vec2(u_item_width, u_item_height) * 0.5;
    float dist = roundedBoxSDF(centerPos, halfSize, u_corner_radius);
    float cornerMask = 1.0 - smoothstep(-1.0, 0.0, dist);

    float finalAlpha = color.a * cornerMask;
    gl_FragColor = vec4(color.rgb * finalAlpha, finalAlpha);
}
)";

  float modeValue(FancyAudioVisualizerMode mode) noexcept {
    switch (mode) {
    case FancyAudioVisualizerMode::Bars:
      return 0.0f;
    case FancyAudioVisualizerMode::Wave:
      return 1.0f;
    case FancyAudioVisualizerMode::Rings:
      return 2.0f;
    case FancyAudioVisualizerMode::BarsRings:
      return 3.0f;
    case FancyAudioVisualizerMode::WaveRings:
      return 4.0f;
    case FancyAudioVisualizerMode::All:
      return 5.0f;
    }
    return 3.0f;
  }

} // namespace

void FancyAudioVisualizerProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShader, kFragmentShader);
  const auto id = m_program.id();

  m_positionLoc = glGetAttribLocation(id, "a_position");
  m_surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  m_quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  m_transformLoc = glGetUniformLocation(id, "u_transform");
  m_audioSourceLoc = glGetUniformLocation(id, "u_audio_source");
  m_timeLoc = glGetUniformLocation(id, "u_time");
  m_itemWidthLoc = glGetUniformLocation(id, "u_item_width");
  m_itemHeightLoc = glGetUniformLocation(id, "u_item_height");
  m_primaryColorLoc = glGetUniformLocation(id, "u_primary_color");
  m_secondaryColorLoc = glGetUniformLocation(id, "u_secondary_color");
  m_sensitivityLoc = glGetUniformLocation(id, "u_sensitivity");
  m_rotationSpeedLoc = glGetUniformLocation(id, "u_rotation_speed");
  m_barWidthLoc = glGetUniformLocation(id, "u_bar_width");
  m_ringOpacityLoc = glGetUniformLocation(id, "u_ring_opacity");
  m_cornerRadiusLoc = glGetUniformLocation(id, "u_corner_radius");
  m_bloomIntensityLoc = glGetUniformLocation(id, "u_bloom_intensity");
  m_modeLoc = glGetUniformLocation(id, "u_mode");
  m_waveThicknessLoc = glGetUniformLocation(id, "u_wave_thickness");
  m_innerDiameterLoc = glGetUniformLocation(id, "u_inner_diameter");

  if (m_positionLoc < 0 || m_surfaceSizeLoc < 0 || m_quadSizeLoc < 0 || m_transformLoc < 0 || m_audioSourceLoc < 0) {
    throw std::runtime_error("failed to query fancy audio visualizer shader locations");
  }
}

void FancyAudioVisualizerProgram::destroy() {
  m_program.destroy();
  m_positionLoc = -1;
  m_surfaceSizeLoc = -1;
  m_quadSizeLoc = -1;
  m_transformLoc = -1;
  m_audioSourceLoc = -1;
  m_timeLoc = -1;
  m_itemWidthLoc = -1;
  m_itemHeightLoc = -1;
  m_primaryColorLoc = -1;
  m_secondaryColorLoc = -1;
  m_sensitivityLoc = -1;
  m_rotationSpeedLoc = -1;
  m_barWidthLoc = -1;
  m_ringOpacityLoc = -1;
  m_cornerRadiusLoc = -1;
  m_bloomIntensityLoc = -1;
  m_modeLoc = -1;
  m_waveThicknessLoc = -1;
  m_innerDiameterLoc = -1;
}

void FancyAudioVisualizerProgram::draw(
    TextureId audioTexture, float surfaceWidth, float surfaceHeight, float width, float height,
    const FancyAudioVisualizerStyle& style, const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0f || height <= 0.0f || audioTexture == 0) {
    return;
  }

  static constexpr std::array<GLfloat, 12> kQuad = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLoc, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLoc, width, height);
  glUniformMatrix3fv(m_transformLoc, 1, GL_FALSE, transform.m.data());
  glUniform1f(m_timeLoc, style.time);
  glUniform1f(m_itemWidthLoc, width);
  glUniform1f(m_itemHeightLoc, height);
  glUniform4f(
      m_primaryColorLoc, style.primaryColor.r, style.primaryColor.g, style.primaryColor.b, style.primaryColor.a
  );
  glUniform4f(
      m_secondaryColorLoc, style.secondaryColor.r, style.secondaryColor.g, style.secondaryColor.b,
      style.secondaryColor.a
  );
  glUniform1f(m_sensitivityLoc, style.sensitivity);
  glUniform1f(m_rotationSpeedLoc, style.rotationSpeed);
  glUniform1f(m_barWidthLoc, style.barWidth);
  glUniform1f(m_ringOpacityLoc, style.ringOpacity);
  glUniform1f(m_cornerRadiusLoc, style.cornerRadius);
  glUniform1f(m_bloomIntensityLoc, style.bloomIntensity);
  glUniform1f(m_modeLoc, modeValue(style.mode));
  glUniform1f(m_waveThicknessLoc, style.waveThickness);
  glUniform1f(m_innerDiameterLoc, style.innerDiameter);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(audioTexture.value()));
  glUniform1i(m_audioSourceLoc, 0);

  const auto posAttr = static_cast<GLuint>(m_positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
