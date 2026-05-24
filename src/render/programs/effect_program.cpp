#include "render/programs/effect_program.h"

#include <array>
#include <stdexcept>
#include <string>

namespace {

  constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform vec2 u_rect_origin;
uniform mat3 u_transform;
varying vec2 v_uv;
varying vec2 v_pixel;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_pixel = local - u_rect_origin;
    v_uv = a_position;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kCommonFragment[] = R"(
precision highp float;

uniform vec2 u_rect_size;
uniform float u_time;
uniform float u_item_width;
uniform float u_item_height;
uniform vec4 u_bg_color;
uniform float u_radius;
uniform float u_alternative;
varying vec2 v_uv;
varying vec2 v_pixel;

float roundedBoxSDF(vec2 center, vec2 size, float radius) {
    vec2 q = abs(center) - size + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float cornerMask() {
    vec2 pixelPos = v_uv * vec2(u_item_width, u_item_height);
    vec2 center = pixelPos - vec2(u_item_width, u_item_height) * 0.5;
    vec2 halfSize = vec2(u_item_width, u_item_height) * 0.5;
    float dist = roundedBoxSDF(center, halfSize, u_radius);
    return 1.0 - smoothstep(-1.0, 0.0, dist);
}
)";

  // --- Sun effect ---
  constexpr char kSunFragment[] = R"(
float efx_hash(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float efx_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = efx_hash(i);
    float b = efx_hash(i + vec2(1.0, 0.0));
    float c = efx_hash(i + vec2(0.0, 1.0));
    float d = efx_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float sunRays(vec2 uv, vec2 sunPos, float iTime) {
    vec2 toSun = uv - sunPos;
    float angle = atan(toSun.y, toSun.x);
    float dist = length(toSun);
    float rayCount = 7.0;
    float rays = sin(angle * rayCount + sin(iTime * 0.25)) * 0.5 + 0.5;
    rays = pow(rays, 3.0);
    float falloff = 1.0 - smoothstep(0.0, 1.2, dist);
    return rays * falloff * 0.15;
}

float atmosphericShimmer(vec2 uv, float iTime) {
    float n1 = efx_noise(uv * 5.0 + vec2(iTime * 0.1, iTime * 0.05));
    float n2 = efx_noise(uv * 8.0 - vec2(iTime * 0.08, iTime * 0.12));
    float n3 = efx_noise(uv * 12.0 + vec2(iTime * 0.15, -iTime * 0.1));
    return (n1 * 0.5 + n2 * 0.3 + n3 * 0.2) * 0.15;
}

float sunCore(vec2 uv, vec2 sunPos, float iTime) {
    vec2 toSun = uv - sunPos;
    float dist = length(toSun);
    float mainFlare = exp(-dist * 15.0) * 2.0;
    float flares = 0.0;
    for (int i = 1; i <= 3; i++) {
        vec2 flarePos = sunPos + toSun * float(i) * 0.3;
        float flareDist = length(uv - flarePos);
        float flareSize = 0.02 + float(i) * 0.01;
        flares += smoothstep(flareSize * 2.0, flareSize * 0.5, flareDist) * (0.3 / float(i));
    }
    float pulse = sin(iTime) * 0.1 + 0.9;
    return (mainFlare + flares) * pulse;
}

void main() {
    vec2 uv = v_uv;
    float iTime = u_time * 0.08;
    vec4 col = vec4(u_bg_color.rgb, 1.0);
    vec2 sunPos = vec2(0.85, 0.2);
    float aspect = u_item_width / u_item_height;
    vec2 uvAspect = vec2(uv.x * aspect, uv.y);
    vec2 sunPosAspect = vec2(sunPos.x * aspect, sunPos.y);

    float rays = sunRays(uvAspect, sunPosAspect, iTime);
    float shimmerEffect = atmosphericShimmer(uv, iTime);
    float flare = sunCore(uvAspect, sunPosAspect, iTime);

    vec3 sunColor = vec3(1.0, 0.95, 0.7);
    vec3 shimmerColor = vec3(1.0, 0.98, 0.85);

    vec3 resultRGB = col.rgb;
    float resultAlpha = col.a;

    vec3 raysContribution = sunColor * rays;
    float raysAlpha = rays * 0.4;
    resultRGB = raysContribution + resultRGB * (1.0 - raysAlpha);
    resultAlpha = raysAlpha + resultAlpha * (1.0 - raysAlpha);

    vec3 shimmerContribution = shimmerColor * shimmerEffect;
    float shimmerAlpha = shimmerEffect * 0.1;
    resultRGB = shimmerContribution + resultRGB * (1.0 - shimmerAlpha);
    resultAlpha = shimmerAlpha + resultAlpha * (1.0 - shimmerAlpha);

    vec3 flareContribution = sunColor * flare;
    float flareAlpha = clamp(flare, 0.0, 1.0) * 0.6;
    resultRGB = flareContribution + resultRGB * (1.0 - flareAlpha);
    resultAlpha = flareAlpha + resultAlpha * (1.0 - flareAlpha);

    resultRGB = mix(resultRGB, resultRGB * vec3(1.08, 1.04, 0.98), 0.15);

    float mask = cornerMask();
    float finalAlpha = resultAlpha * mask;
    gl_FragColor = vec4(resultRGB * (finalAlpha / max(resultAlpha, 0.001)), finalAlpha);
}
)";

  // --- Snow effect ---
  constexpr char kSnowFragment[] = R"(
void main() {
    float aspect = u_item_width / u_item_height;
    vec2 uv = v_uv;
    uv.x *= aspect;
    uv.y = 1.0 - uv.y;

    float iTime = u_time * 0.15;
    float snow = 0.0;

    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 12; i++) {
            float cellSize = 2.0 + (float(i) * 3.0);
            float downSpeed = 0.3 + (sin(iTime * 0.4 + float(k + i * 20)) + 1.0) * 0.00008;

            vec2 uvAnim = uv + vec2(
                0.01 * sin((iTime + float(k * 6185)) * 0.6 + float(i)) * (5.0 / float(i + 1)),
                downSpeed * (iTime + float(k * 1352)) * (1.0 / float(i + 1))
            );

            vec2 uvStep = (ceil((uvAnim) * cellSize - vec2(0.5, 0.5)) / cellSize);
            float x = fract(sin(dot(uvStep.xy, vec2(12.9898 + float(k) * 12.0, 78.233 + float(k) * 315.156))) * 43758.5453 + float(k) * 12.0) - 0.5;
            float y = fract(sin(dot(uvStep.xy, vec2(62.2364 + float(k) * 23.0, 94.674 + float(k) * 95.0))) * 62159.8432 + float(k) * 12.0) - 0.5;

            float randomMagnitude1 = sin(iTime * 2.5) * 0.7 / cellSize;
            float randomMagnitude2 = cos(iTime * 1.65) * 0.7 / cellSize;

            float d = 5.0 * distance((uvStep.xy + vec2(x * sin(y), y) * randomMagnitude1 + vec2(y, x) * randomMagnitude2), uvAnim.xy);

            float omiVal = fract(sin(dot(uvStep.xy, vec2(32.4691, 94.615))) * 31572.1684);
            if (omiVal < 0.03) {
                float newd = (x + 1.0) * 0.4 * clamp(1.9 - d * (15.0 + (x * 6.3)) * (cellSize / 1.4), 0.0, 1.0);
                snow += newd;
            }
        }
    }

    float snowAlpha = clamp(snow * 2.0, 0.0, 1.0);
    vec3 snowColor = vec3(1.0);
    vec3 blended = mix(u_bg_color.rgb, snowColor, snowAlpha);

    float mask = cornerMask();
    float finalAlpha = mask;
    gl_FragColor = vec4(blended * finalAlpha, finalAlpha);
}
)";

  // --- Rain effect (atmospheric haze + discrete falling drops) ---
  constexpr char kRainFragment[] = R"(
float rain_hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

float rain_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = rain_hash(i);
    float b = rain_hash(i + vec2(1.0, 0.0));
    float c = rain_hash(i + vec2(0.0, 1.0));
    float d = rain_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float rainLayer(vec2 uv, float columns, float rows, float speed, float dropLen, float tilt, float iTime, float seed) {
    float x = uv.x + uv.y * tilt;
    float y = uv.y - iTime * speed;

    float col = x * columns;
    float colId = floor(col);
    float colFrac = fract(col);

    float row = y * rows;

    float streaks = 0.0;

    for (int dc = -1; dc <= 1; dc++) {
        float cId = colId + float(dc);

        float rnd = rain_hash(vec2(cId, seed));
        if (rnd < 0.55) continue;

        float xPos = rain_hash(vec2(cId * 1.73, seed + 7.0)) * 0.4 + 0.3;
        float px = colFrac - (float(dc) + xPos);

        float width = 0.02 + rnd * 0.015;
        float xFade = exp(-0.5 * (px * px) / (width * width));

        // Per-column phase offset so drops don't align across columns
        float phase = rain_hash(vec2(cId, seed + 3.0));
        float dropY = fract(row + phase);

        // Drop: visible from 0 to dropLen, gap from dropLen to 1
        float head = smoothstep(0.0, 0.06, dropY);
        float tail = smoothstep(dropLen, dropLen - 0.1, dropY);
        float yFade = head * tail;

        // Brightness tapers toward tail (brighter at head)
        float taper = 1.0 - dropY / dropLen;
        taper = clamp(taper, 0.0, 1.0);

        float brightness = (0.4 + rain_hash(vec2(cId, seed + 5.0)) * 0.4) * (0.5 + taper * 0.5);
        streaks += xFade * yFade * brightness;
    }

    return streaks;
}

void main() {
    vec2 uv = v_uv;
    float iTime = u_time * 0.12;
    float aspect = u_item_width / u_item_height;
    vec2 uvA = vec2(uv.x * aspect, uv.y);

    // Atmospheric haze
    float n1 = rain_noise(uv * 2.0 + vec2(iTime * 0.02, iTime * 0.015));
    float n2 = rain_noise(uv * 4.0 - vec2(iTime * 0.03, -iTime * 0.01));
    float n3 = rain_noise(uv * 8.0 + vec2(-iTime * 0.01, iTime * 0.025));
    float haze = n1 * 0.5 + n2 * 0.3 + n3 * 0.2;
    haze = smoothstep(0.25, 0.7, haze) * 0.25;

    // 3 rain layers: columns, rows (drops per unit height), speed, dropLen (0-1 fraction visible), tilt
    float tilt = 0.07;
    float r1 = rainLayer(uvA, 40.0, 3.5, 2.4, 0.35, tilt,          iTime, 0.0);
    float r2 = rainLayer(uvA, 25.0, 2.5, 1.7, 0.40, tilt * 0.7,    iTime, 100.0);
    float r3 = rainLayer(uvA, 14.0, 1.8, 1.1, 0.50, tilt * 0.4,    iTime, 200.0);

    float rain = r1 * 0.3 + r2 * 0.4 + r3 * 0.55;
    rain = clamp(rain, 0.0, 1.0);

    // Compose: background → haze → rain
    vec3 hazeColor = vec3(0.82, 0.85, 0.9);
    vec3 col = mix(u_bg_color.rgb, hazeColor, haze);

    vec3 rainTint = vec3(0.88, 0.91, 1.0);
    vec3 resultRGB = mix(col, rainTint, rain * 0.45);

    float mask = cornerMask();
    gl_FragColor = vec4(resultRGB * mask, mask);
}
)";

  // --- Cloud / Fog effect ---
  constexpr char kCloudFragment[] = R"(
float cloud_hash(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float cloud_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = cloud_hash(i);
    float b = cloud_hash(i + vec2(1.0, 0.0));
    float c = cloud_hash(i + vec2(0.0, 1.0));
    float d = cloud_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float turbulence(vec2 p, float iTime) {
    float t = 0.0;
    float scale = 1.0;
    for (int i = 0; i < 5; i++) {
        t += abs(cloud_noise(p * scale + iTime * 0.1 * scale)) / scale;
        scale *= 2.0;
    }
    return t;
}

void main() {
    vec2 uv = v_uv;
    vec4 col = vec4(u_bg_color.rgb, 1.0);

    float timeSpeed, layerScale1, layerScale2, layerScale3;
    float flowSpeed1, flowSpeed2;
    float densityMin, densityMax;
    float baseOpacity;
    float pulseAmount;

    if (u_alternative > 0.5) {
        timeSpeed = 0.03;
        layerScale1 = 1.0;
        layerScale2 = 2.5;
        layerScale3 = 2.0;
        flowSpeed1 = 0.0;
        flowSpeed2 = 0.02;
        densityMin = 0.1;
        densityMax = 0.9;
        baseOpacity = 0.75;
        pulseAmount = 0.05;
    } else {
        timeSpeed = 0.08;
        layerScale1 = 2.0;
        layerScale2 = 4.0;
        layerScale3 = 6.0;
        flowSpeed1 = 0.03;
        flowSpeed2 = 0.04;
        densityMin = 0.35;
        densityMax = 0.75;
        baseOpacity = 0.4;
        pulseAmount = 0.15;
    }

    float iTime = u_time * timeSpeed;

    vec2 flow1 = vec2(iTime * flowSpeed1, iTime * flowSpeed1 * 0.7);
    vec2 flow2 = vec2(-iTime * flowSpeed2, iTime * flowSpeed2 * 0.8);

    float fog1 = cloud_noise(uv * layerScale1 + flow1);
    float fog2 = cloud_noise(uv * layerScale2 + flow2);
    float fog3 = turbulence(uv * layerScale3, iTime);

    float fogPattern = fog1 * 0.5 + fog2 * 0.3 + fog3 * 0.2;
    float fogDensity = smoothstep(densityMin, densityMax, fogPattern);

    float pulse = sin(iTime * 0.4) * pulseAmount + (1.0 - pulseAmount);
    fogDensity *= pulse;

    vec3 hazeColor = vec3(0.88, 0.90, 0.93);
    float hazeOpacity = fogDensity * baseOpacity;
    vec3 fogContribution = hazeColor * hazeOpacity;
    float fogAlpha = hazeOpacity;

    vec3 resultRGB = fogContribution + col.rgb * (1.0 - fogAlpha);
    float resultAlpha = fogAlpha + col.a * (1.0 - fogAlpha);

    float mask = cornerMask();
    float finalAlpha = resultAlpha * mask;
    gl_FragColor = vec4(resultRGB * (finalAlpha / max(resultAlpha, 0.001)), finalAlpha);
}
)";

  // --- Stars effect ---
  constexpr char kStarsFragment[] = R"(
float star_hash(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

vec2 star_hash2(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(vec2(p.x * p.y, p.y * p.x));
}

float stars(vec2 uv, float density, float iTime) {
    vec2 gridUV = uv * density;
    vec2 gridID = floor(gridUV);
    vec2 gridPos = fract(gridUV);

    float starField = 0.0;

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 offset = vec2(float(x), float(y));
            vec2 cellID = gridID + offset;

            vec2 starPos = star_hash2(cellID);

            float starChance = star_hash(cellID + vec2(12.345, 67.890));
            if (starChance > 0.85) {
                vec2 toStar = (offset + starPos - gridPos);
                float dist = length(toStar) * density;
                float starSize = 1.5;
                float brightness = star_hash(cellID + vec2(23.456, 78.901)) * 0.6 + 0.4;

                float twinkleSpeed = star_hash(cellID + vec2(34.567, 89.012)) * 3.0 + 2.0;
                float twinklePhase = iTime * twinkleSpeed + star_hash(cellID) * 6.28;
                float twinkle = pow(sin(twinklePhase) * 0.5 + 0.5, 3.0);

                float star = 0.0;
                if (dist < starSize) {
                    star = 1.0 * brightness * (0.3 + twinkle * 0.7);

                    if (brightness > 0.7) {
                        float crossGlow = max(
                            exp(-abs(toStar.x) * density * 5.0),
                            exp(-abs(toStar.y) * density * 5.0)
                        ) * 0.3 * twinkle;
                        star += crossGlow;
                    }
                }

                starField += star;
            }
        }
    }

    return starField;
}

void main() {
    vec2 uv = v_uv;
    float iTime = u_time * 0.01;
    vec4 col = vec4(u_bg_color.rgb, 1.0);

    float aspect = u_item_width / u_item_height;
    vec2 uvAspect = vec2(uv.x * aspect, uv.y);

    float stars1 = stars(uvAspect, 40.0, iTime);
    float stars2 = stars(uvAspect + vec2(0.5, 0.3), 25.0, iTime * 1.3);
    float stars3 = stars(uvAspect + vec2(0.25, 0.7), 15.0, iTime * 0.9);

    vec3 starColor1 = vec3(0.85, 0.9, 1.0);
    vec3 starColor2 = vec3(0.95, 0.97, 1.0);
    vec3 starColor3 = vec3(1.0, 0.98, 0.95);

    vec3 starsRGB = starColor1 * stars1 * 0.6 +
                    starColor2 * stars2 * 0.8 +
                    starColor3 * stars3 * 1.0;

    float starsAlpha = clamp(stars1 * 0.6 + stars2 * 0.8 + stars3, 0.0, 1.0);

    vec3 resultRGB = starsRGB * starsAlpha + col.rgb * (1.0 - starsAlpha);
    float resultAlpha = starsAlpha + col.a * (1.0 - starsAlpha);

    float mask = cornerMask();
    float finalAlpha = resultAlpha * mask;
    gl_FragColor = vec4(resultRGB * (finalAlpha / max(resultAlpha, 0.001)), finalAlpha);
}
)";

} // namespace

void EffectProgram::ensureInitialized() {
  if (m_programs[0].program.isValid()) {
    return;
  }

  initProgram(0, kSunFragment);
  initProgram(1, kSnowFragment);
  initProgram(2, kRainFragment);
  initProgram(3, kCloudFragment);
  initProgram(4, kStarsFragment);
}

void EffectProgram::destroy() {
  for (auto& pd : m_programs) {
    pd.program.destroy();
  }
}

void EffectProgram::initProgram(std::size_t index, const char* fragSource) {
  std::string fullFrag = std::string(kCommonFragment) + fragSource;

  auto& pd = m_programs[index];
  pd.program.create(kVertexShader, fullFrag.c_str());

  auto id = pd.program.id();
  pd.positionLoc = glGetAttribLocation(id, "a_position");
  pd.surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  pd.quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  pd.rectOriginLoc = glGetUniformLocation(id, "u_rect_origin");
  pd.rectSizeLoc = glGetUniformLocation(id, "u_rect_size");
  pd.transformLoc = glGetUniformLocation(id, "u_transform");
  pd.timeLoc = glGetUniformLocation(id, "u_time");
  pd.itemWidthLoc = glGetUniformLocation(id, "u_item_width");
  pd.itemHeightLoc = glGetUniformLocation(id, "u_item_height");
  pd.bgColorLoc = glGetUniformLocation(id, "u_bg_color");
  pd.radiusLoc = glGetUniformLocation(id, "u_radius");
  pd.alternativeLoc = glGetUniformLocation(id, "u_alternative");

  if (pd.positionLoc < 0 || pd.surfaceSizeLoc < 0 || pd.transformLoc < 0) {
    throw std::runtime_error("failed to query effect shader locations");
  }
}

void EffectProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style, const Mat3& transform
) const {
  if (style.type == EffectType::None || width <= 0.0f || height <= 0.0f) {
    return;
  }

  // Fog reuses the Cloud shader with alternative=1
  const bool isFog = style.type == EffectType::Fog;
  const auto effectType = isFog ? EffectType::Cloud : style.type;
  auto idx = static_cast<std::size_t>(effectType) - 1;
  if (idx >= kEffectCount || !m_programs[idx].program.isValid()) {
    return;
  }

  const auto& pd = m_programs[idx];

  static constexpr float kQuad[] = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(pd.program.id());

  glUniform2f(pd.surfaceSizeLoc, surfaceWidth, surfaceHeight);
  glUniform2f(pd.quadSizeLoc, width, height);
  glUniform2f(pd.rectOriginLoc, 0.0f, 0.0f);
  if (pd.rectSizeLoc >= 0) {
    glUniform2f(pd.rectSizeLoc, width, height);
  }
  glUniformMatrix3fv(pd.transformLoc, 1, GL_FALSE, transform.m.data());

  if (pd.timeLoc >= 0) {
    glUniform1f(pd.timeLoc, style.time);
  }
  if (pd.itemWidthLoc >= 0) {
    glUniform1f(pd.itemWidthLoc, width);
  }
  if (pd.itemHeightLoc >= 0) {
    glUniform1f(pd.itemHeightLoc, height);
  }
  if (pd.bgColorLoc >= 0) {
    glUniform4f(pd.bgColorLoc, style.bgColor.r, style.bgColor.g, style.bgColor.b, style.bgColor.a);
  }
  if (pd.radiusLoc >= 0) {
    glUniform1f(pd.radiusLoc, style.radius);
  }
  if (pd.alternativeLoc >= 0) {
    glUniform1f(pd.alternativeLoc, isFog ? 1.0f : 0.0f);
  }

  auto posAttr = static_cast<GLuint>(pd.positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
