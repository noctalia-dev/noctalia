#include "shell/render/programs/effect_program.h"

#include "render/core/texture_handle.h"

#include <array>
#include <optional>
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
// Theme accent (ColorRole::Primary), read by the Fog and Cloud shaders.
uniform vec4 u_accent_color;
uniform float u_radius;
uniform float u_alternative;
varying vec2 v_uv;
varying vec2 v_pixel;

// Rain and Snow particles: EffectNode packs two RGBA8 texels per particle, (x, y) then
// (radius, weight), each value 16-bit as hi/lo bytes. Width and ranges match EffectNode's
// kParticleTextureWidth, kParticleYMin/kParticleYMax, and kParticleRadiusMax.
uniform sampler2D u_particles;
uniform int u_particle_count;

vec2 particleUnpack(vec4 texel) {
    vec4 b = floor(texel * 255.0 + 0.5);
    return vec2(b.x * 256.0 + b.y, b.z * 256.0 + b.w) / 65535.0;
}

vec4 particleAt(int i) {
    float u = (float(i) * 2.0 + 0.5) / 120.0;
    vec2 xy = particleUnpack(texture2D(u_particles, vec2(u, 0.5)));
    vec2 rw = particleUnpack(texture2D(u_particles, vec2(u + 1.0 / 120.0, 0.5)));
    return vec4(xy.x, mix(-0.25, 1.25, xy.y), rw.x * 0.0625, rw.y);
}

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

// Blend toward dark tints on light backgrounds (u_bg_color luminance).
vec3 particleColor(vec3 darkModeColor, vec3 lightModeColor) {
    float luma = dot(u_bg_color.rgb, vec3(0.2126, 0.7152, 0.0722));
    return mix(darkModeColor, lightModeColor, smoothstep(0.45, 0.75, luma));
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
    // Rays sway slowly and each one breathes, so the sun never sits completely still.
    float rays = sin(angle * rayCount + sin(iTime * 5.0) * 1.2) * 0.5 + 0.5;
    rays = pow(rays, 3.0) * (0.6 + 0.4 * sin(iTime * 8.0 + angle * 3.0));
    float falloff = 1.0 - smoothstep(0.0, 1.2, dist);
    return rays * falloff * 0.15;
}

float atmosphericShimmer(vec2 uv, float iTime) {
    float n1 = efx_noise(uv * 5.0 + vec2(iTime * 0.1, iTime * 0.05));
    float n2 = efx_noise(uv * 8.0 - vec2(iTime * 0.08, iTime * 0.12));
    float n3 = efx_noise(uv * 12.0 + vec2(iTime * 0.15, -iTime * 0.1));
    return (n1 * 0.5 + n2 * 0.3 + n3 * 0.2) * 0.15;
}

// Sparse drifting wisps, same FBM as Rain's rainFogColor() but with high thresholds and low
// weights so the sky stays mostly clear and the wisps only give the rays something to catch.
float sunFogFbm(vec2 p, float iTime) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        float fi = float(i + 1);
        v += amp * efx_noise(p + vec2(iTime * 0.03 * fi, -iTime * 0.02 * fi));
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

vec3 sunFogColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.9, u_bg_color.rgb, uv.y);

    vec3 wisp = particleColor(vec3(0.36, 0.38, 0.44), vec3(0.78, 0.78, 0.78));
    vec3 sunlit = particleColor(vec3(0.56, 0.46, 0.26), vec3(0.93, 0.8, 0.55));

    float n1 = sunFogFbm(uv * 2.2, iTime);
    float n2 = sunFogFbm(uv * 3.5 + 10.0, iTime * 1.3);

    vec3 col = base;
    col = mix(col, wisp, smoothstep(0.5, 0.75, n1) * 0.5);
    col = mix(col, sunlit, pow(clamp(n1 * n2, 0.0, 1.0), 1.4) * 0.35);
    return col;
}

float sunCore(vec2 uv, vec2 sunPos, float iTime) {
    float dist = length(uv - sunPos);
    float mainFlare = exp(-dist * 15.0) * 2.0;
    float pulse = sin(iTime * 4.0) * 0.12 + 0.88;
    return mainFlare * pulse;
}

// Hollow tinted rings around the disc plus a ghost chain along the sun-to-center axis.
vec3 sunLensFlares(vec2 uv, vec2 sunPos, vec2 screenCenter, float iTime) {
    vec3 result = vec3(0.0);
    float dist = length(uv - sunPos);

    for (int i = 1; i <= 3; i++) {
        float radius = 0.025 + float(i) * 0.018;
        float thickness = 0.006;
        float ring = 1.0 - smoothstep(0.0, thickness, abs(dist - radius));
        vec3 tint = 0.5 + 0.5 * cos(vec3(0.0, 2.1, 4.2) + float(i) * 1.1);
        result += tint * ring * (0.22 / float(i));
    }

    vec2 axis = screenCenter - sunPos;
    for (int i = 0; i < 4; i++) {
        float t = 0.4 + float(i) * 0.35;
        vec2 ghostPos = sunPos + axis * t;
        float gDist = length(uv - ghostPos);
        float gRadius = 0.012 + float(i) * 0.01;
        float gThickness = gRadius * 0.5;
        float ghost = 1.0 - smoothstep(0.0, gThickness, abs(gDist - gRadius));
        vec3 tint = 0.5 + 0.5 * cos(vec3(1.0, 3.0, 5.0) + float(i) * 1.7);
        float fade = 1.0 / (1.2 + float(i) * 0.8);
        result += tint * ghost * fade * 0.12;
    }

    return result;
}

void main() {
    vec2 uv = v_uv;
    float iTime = u_time * 0.08;
    vec2 sunPos = vec2(0.85, 0.2);
    float aspect = u_item_width / u_item_height;
    vec2 uvAspect = vec2(uv.x * aspect, uv.y);
    vec2 sunPosAspect = vec2(sunPos.x * aspect, sunPos.y);
    // Wisps drift about a card width per minute: slow, but visibly moving.
    vec4 col = vec4(sunFogColor(uvAspect, iTime * 10.0), 1.0);

    float rays = sunRays(uvAspect, sunPosAspect, iTime);
    float shimmerEffect = atmosphericShimmer(uv, iTime);
    float flare = sunCore(uvAspect, sunPosAspect, iTime);

    vec3 sunColor = particleColor(vec3(1.0, 0.95, 0.7), vec3(0.85, 0.55, 0.05));
    vec3 shimmerColor = particleColor(vec3(1.0, 0.98, 0.85), vec3(0.78, 0.62, 0.3));

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

    // Additive: the rings are thin, mostly zero-coverage outlines that a blend would wash out.
    vec2 screenCenter = vec2(0.5 * aspect, 0.5);
    resultRGB += sunLensFlares(uvAspect, sunPosAspect, screenCenter, iTime);

    resultRGB = mix(resultRGB, resultRGB * vec3(1.08, 1.04, 0.98), 0.15);

    float mask = cornerMask();
    float finalAlpha = resultAlpha * mask * u_bg_color.a;
    gl_FragColor = vec4(resultRGB * (finalAlpha / max(resultAlpha, 0.001)), finalAlpha);
}
)";

  // --- Snow effect ---
  // The flake loop bound must match kMaxSnowFlakes.
  constexpr char kSnowFragment[] = R"(
float snow_hash(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

float snow_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = snow_hash(i);
    float b = snow_hash(i + vec2(1.0, 0.0));
    float c = snow_hash(i + vec2(0.0, 1.0));
    float d = snow_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Drifting cloud backdrop, same technique as Rain's rainFogColor().
float snowFogFbm(vec2 p, float iTime) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        float fi = float(i + 1);
        v += amp * snow_noise(p + vec2(iTime * 0.03 * fi, -iTime * 0.02 * fi));
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

vec3 snowFogColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    // Cold palette: icy blue, pale cyan, lavender-grey, slate.
    vec3 secondary = particleColor(vec3(0.247, 0.361, 0.447), vec3(0.17, 0.26, 0.32));
    vec3 tertiary = particleColor(vec3(0.318, 0.482, 0.49), vec3(0.19, 0.3, 0.3));
    vec3 primary = particleColor(vec3(0.42, 0.38, 0.49), vec3(0.28, 0.25, 0.34));
    vec3 accent = particleColor(vec3(0.267, 0.31, 0.333), vec3(0.18, 0.21, 0.22));

    float n1 = snowFogFbm(uv * 2.2, iTime);
    float n2 = snowFogFbm(uv * 3.5 + 10.0, iTime * 1.3);
    float n3 = snowFogFbm(uv * 2.8 - 5.0, iTime * 0.8);

    // Lower weights than Rain/Sun keep the backdrop calm so the flakes stand out.
    vec3 col = base;
    col = mix(col, secondary, smoothstep(0.35, 0.68, n1) * 0.34);
    col = mix(col, tertiary, smoothstep(0.52, 0.84, n2) * 0.23);
    col = mix(col, primary, pow(clamp(n1 * n2, 0.0, 1.0), 1.3) * 0.32);
    col = mix(col, accent, smoothstep(0.5, 0.8, n3) * 0.28);
    return col;
}

void main() {
    float aspect = u_item_width / u_item_height;
    vec2 uvA = vec2(v_uv.x * aspect, v_uv.y);
    float iTime = u_time * 0.15;

    // One pixel of card height in uv units; the anti-alias band never shrinks below it.
    float pixelUv = 1.0 / max(u_item_height, 1.0);

    float snow = 0.0;
    for (int i = 0; i < 60; i++) {
        if (i < u_particle_count) {
            vec4 flake = particleAt(i);
            vec2 c = vec2(flake.x * aspect, flake.y);
            float r = max(flake.z, pixelUv * 1.5);
            float dist = length(uvA - c);
            // Blur band grows quadratically with size: big, close flakes go soft, small
            // distant ones stay crisp.
            float sizeT = clamp(r / 0.028, 0.0, 1.0);
            float aa = mix(pixelUv, r * 0.85, sizeT * sizeT);
            float coverage = smoothstep(r + aa, r - aa, dist);
            // Bigger (closer) flakes read slightly brighter than small (distant) ones.
            snow += coverage * mix(0.55, 1.0, sizeT);
        }
    }
    snow = clamp(snow, 0.0, 1.0);

    vec3 snowColor = particleColor(vec3(1.0), vec3(0.45, 0.55, 0.65));
    vec3 fogBackdrop = snowFogColor(uvA, iTime);
    vec3 blended = mix(fogBackdrop, snowColor, snow);

    // Pale drifting cloud bank along the top of the card, from a horizontally stretched
    // snowFogFbm with a higher threshold.
    float cloudBand = 1.0 - smoothstep(0.1, 0.22, v_uv.y);
    if (cloudBand > 0.0) {
        float cloudNoise = snowFogFbm(uvA * vec2(2.6, 7.0) + vec2(iTime * 0.012, 0.0), iTime * 0.5);
        float cloudShape = smoothstep(0.42, 0.72, cloudNoise) * cloudBand;
        vec3 cloudColor = particleColor(vec3(0.93, 0.94, 0.96), vec3(0.8, 0.82, 0.86));
        // Dense regions go fully opaque so the bank occludes flakes behind it.
        blended = mix(blended, cloudColor, clamp(cloudShape * 1.4, 0.0, 1.0));
    }

    float mask = cornerMask();
    float finalAlpha = mask * u_bg_color.a;
    gl_FragColor = vec4(blended * finalAlpha, finalAlpha);
}
)";

  // --- Rain effect (glass droplets over tilted streaks and a drifting fog backdrop) ---
  // The drop loop bound must match kMaxRainDrops.
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

// Drifting fog backdrop: per-pixel FBM, cheap enough at Control Center card size.
float rainFogFbm(vec2 p, float iTime) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        float fi = float(i + 1);
        v += amp * rain_noise(p + vec2(iTime * 0.03 * fi, -iTime * 0.02 * fi));
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

// Four-hue wash (slate-blue, teal, gold, olive) over a darkened u_bg_color, kept muted so
// the card's text stays readable.
vec3 rainFogColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    vec3 secondary = particleColor(vec3(0.314, 0.353, 0.416), vec3(0.2, 0.24, 0.3));
    vec3 tertiary = particleColor(vec3(0.404, 0.553, 0.498), vec3(0.2, 0.32, 0.28));
    vec3 primary = particleColor(vec3(0.561, 0.459, 0.259), vec3(0.42, 0.33, 0.16));
    vec3 accent = particleColor(vec3(0.275, 0.314, 0.22), vec3(0.19, 0.22, 0.14));

    float n1 = rainFogFbm(uv * 2.2, iTime);
    float n2 = rainFogFbm(uv * 3.5 + 10.0, iTime * 1.3);
    float n3 = rainFogFbm(uv * 2.8 - 5.0, iTime * 0.8);

    vec3 col = base;
    col = mix(col, secondary, smoothstep(0.35, 0.68, n1) * 0.48);
    col = mix(col, tertiary, smoothstep(0.52, 0.84, n2) * 0.33);
    col = mix(col, primary, pow(clamp(n1 * n2, 0.0, 1.0), 1.3) * 0.45);
    col = mix(col, accent, smoothstep(0.5, 0.8, n3) * 0.4);
    return col;
}

// 2D -> 4D hash (Dave Hoskins' hash-without-sine), four decorrelated values per cell.
vec4 rain_hash42(vec2 p) {
    vec4 p4 = fract(vec4(p.x, p.y, p.x, p.y) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
    p4 += dot(p4, p4.wzxy + 33.33);
    return fract((p4.xxyz + p4.yzzw) * p4.zywx);
}

// Paraboloid height dome, zero outside its radius. Domes are summed into heightSum/gradSum
// so overlapping droplets merge into one smooth bump.
void rain_addDome(vec2 uv, vec2 c, float r, inout float heightSum, inout vec2 gradSum) {
    vec2 duv = uv - c;
    float d2 = dot(duv, duv);
    float rr = r * r;
    if (d2 < rr) {
        heightSum += rr - d2;
        gradSum += -2.0 * duv;
    }
}

// Static droplets on a jittered grid, each fading in and out over time so the field renews.
void rain_staticField(vec2 uv, float iTime, inout float heightSum, inout vec2 gradSum) {
    float cellSize = 0.042;
    vec2 baseCell = floor(uv / cellSize);
    float density = rain_noise(uv * 3.2 + 11.0);

    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            vec2 cId = baseCell + vec2(float(dx), float(dy));
            vec4 h = rain_hash42(cId);
            float threshold = mix(0.35, 0.7, density);
            if (h.x < threshold) continue;

            vec2 jitter = h.yz;
            vec2 c = (cId + jitter) * cellSize;
            float sizeT = pow(h.w, 2.2);
            float r = mix(0.006, 0.024, sizeT);

            float phase = rain_hash42(cId * 1.7 + 9.3).x;
            float life = fract(iTime * 0.045 + phase);
            float fade = smoothstep(0.0, 0.15, life) * smoothstep(1.0, 0.82, life);
            r *= fade;
            if (r < 0.0015) continue;

            rain_addDome(uv, c, r, heightSum, gradSum);
        }
    }
}

float rainLayer(
    vec2 uv, float columns, float rows, float speedBase, float speedVar, float dropLenBase,
    float tilt, float widthBase, float widthVar, float iTime, float seed
) {
    float x = uv.x + uv.y * tilt;
    float col = x * columns;
    float colId = floor(col);
    float colFrac = fract(col);

    float streaks = 0.0;

    for (int dc = -1; dc <= 1; dc++) {
        float cId = colId + float(dc);

        float rnd = rain_hash(vec2(cId, seed));
        if (rnd < 0.35) continue;

        float xPos = rain_hash(vec2(cId * 1.73, seed + 7.0)) * 0.6 + 0.2;
        float px = colFrac - (float(dc) + xPos);

        // Per-streak width, fall speed, and length from the column hash.
        float width = widthBase * (0.7 + rain_hash(vec2(cId, seed + 13.0)) * widthVar);
        float speed = speedBase * (0.7 + rain_hash(vec2(cId, seed + 11.0)) * speedVar);
        float dropLen = dropLenBase * (0.7 + rain_hash(vec2(cId, seed + 17.0)) * 0.6);

        // Per-streak tilt jitter, so streaks lean in different directions.
        float tiltJitter = (rain_hash(vec2(cId, seed + 29.0)) - 0.5) * 1.8;
        px += uv.y * tiltJitter;

        // Soft Gaussian falloff from the streak centerline.
        float edge = exp(-1.6 * (px * px) / (width * width));

        float y = uv.y - iTime * speed;
        float row = y * rows;
        float phase = rain_hash(vec2(cId, seed + 3.0));
        float dropY = fract(row + phase);

        float head = smoothstep(0.0, 0.04, dropY);
        float tail = smoothstep(dropLen, dropLen - 0.15, dropY);
        float yFade = head * tail;

        // uv.y grows downward, so the falling head is at high dropY: brightest there,
        // fading toward the trail.
        float brightFactor = mix(0.22, 1.0, clamp(dropY / dropLen, 0.0, 1.0));

        streaks += edge * yFade * brightFactor;
    }

    return streaks;
}

void main() {
    vec2 uv = v_uv;
    float iTime = u_time * 0.12;
    float aspect = u_item_width / u_item_height;
    vec2 uvA = vec2(uv.x * aspect, uv.y);

    // Glass droplets as a summed height field: the procedural static field plus the
    // simulated drops from u_particles.
    float heightSum = 0.0;
    vec2 gradSum = vec2(0.0);
    rain_staticField(uvA, iTime, heightSum, gradSum);
    // Each simulated drop is a trail of shrinking domes, lengthened by its momentum (w).
    for (int i = 0; i < 24; i++) {
        if (i < u_particle_count) {
            vec4 drop = particleAt(i);
            vec2 c = vec2(drop.x * aspect, drop.y);
            float rCur = drop.z;
            float tailStep = drop.z * (0.65 + drop.w * 0.6);
            for (int k = 0; k < 5; k++) {
                vec2 duv = uvA - c;
                float r2 = rCur * rCur;
                float d2 = dot(duv, duv);
                if (d2 < r2) {
                    heightSum += r2 - d2;
                    gradSum += -2.0 * duv;
                }
                c -= vec2(0.0, tailStep);
                rCur *= 0.65;
            }
        }
    }

    // Low threshold so the smallest static droplets still register.
    float inside = smoothstep(0.0, 0.00004, heightSum);

    // Surface normal from the height gradient, clamped so stacked domes don't blow out.
    vec2 slope = clamp(gradSum * 16.0, vec2(-2.5), vec2(2.5));
    vec3 normal = normalize(vec3(slope, 1.0));
    vec3 lightDir = normalize(vec3(-0.4, -0.6, 0.65));

    // Refraction: offset the streak and fog lookups by the surface tilt, fog slightly more.
    vec2 uvOffset = normal.xy * 0.4 * inside;
    vec2 streakUvA = uvA - uvOffset;
    vec2 fogUvA = uvA - uvOffset * 1.3;

    // Single smooth noise octave of faint haze.
    float haze = rain_noise(uv * 2.5 + vec2(iTime * 0.02, iTime * 0.015));
    haze = smoothstep(0.3, 0.7, haze) * 0.08;

    // Foreground layer: fewer, wider, faster, brighter. Background: more, thinner, slower, dimmer.
    float tilt = 0.35;
    //                          uv         cols  rows  speed var  dropLen tilt        width var   time   seed
    float fg = rainLayer(streakUvA, 12.0, 2.2, 7.0, 0.7, 0.45, tilt,       0.045, 0.7, iTime, 0.0);
    float bg = rainLayer(streakUvA, 22.0, 1.6, 3.8, 0.8, 0.55, tilt * 0.6, 0.022, 0.7, iTime, 100.0);

    float rain = clamp(fg + bg * 0.4, 0.0, 1.0);

    vec3 hazeColor = particleColor(vec3(0.82, 0.85, 0.9), vec3(0.55, 0.6, 0.68));
    vec3 col = mix(rainFogColor(fogUvA, iTime), hazeColor, haze);

    // Neutral grey streak tint at a low mix weight so the fog shows through.
    vec3 rainTint = particleColor(vec3(0.68, 0.7, 0.74), vec3(0.42, 0.46, 0.5));
    vec3 resultRGB = mix(col, rainTint, rain * 0.4);

    // Water-surface shading from the normal field: a tight specular highlight plus
    // grazing-angle edge darkening, so merged drops read as one smooth shape.
    float spec = pow(max(dot(normal, lightDir), 0.0), 40.0);
    resultRGB += vec3(0.95, 0.98, 1.0) * spec * 0.45 * inside;

    float edgeDark = pow(clamp(1.0 - normal.z, 0.0, 1.0), 2.2) * inside;
    resultRGB -= vec3(0.05, 0.06, 0.07) * edgeDark;

    resultRGB = mix(resultRGB, resultRGB * 1.08 + vec3(0.015, 0.018, 0.022), inside * 0.4);

    float mask = cornerMask();
    float finalAlpha = mask * u_bg_color.a;
    gl_FragColor = vec4(resultRGB * finalAlpha, finalAlpha);
}
)";

  // --- Cloud / Fog effect ---
  constexpr char kCloudFragment[] = R"(
float cloud_hash(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

// --- Fog: dense drifting mist wash ---
float fogNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = cloud_hash(i);
    float b = cloud_hash(i + vec2(1.0, 0.0));
    float c = cloud_hash(i + vec2(0.0, 1.0));
    float d = cloud_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fogWashFbm(vec2 p, float iTime) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++) {
        float fi = float(i + 1);
        v += amp * fogNoise(p + vec2(iTime * 0.025 * fi, -iTime * 0.016 * fi));
        p *= 2.0;
        amp *= 0.55;
    }
    return v;
}

vec3 fogWashColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.72, u_bg_color.rgb * 1.0, uv.y);

    vec3 accentTint = u_accent_color.rgb;

    // Accent mixed into both tonal extremes so the theme color carries through the whole wash.
    vec3 mist = mix(particleColor(vec3(0.97, 0.98, 1.0), vec3(0.82, 0.85, 0.9)), accentTint, 0.3);
    vec3 deep = mix(particleColor(vec3(0.3, 0.34, 0.44), vec3(0.18, 0.22, 0.3)), accentTint, 0.35);

    float n1 = fogWashFbm(uv * 2.6, iTime);
    float n2 = fogWashFbm(uv * 3.8 + 10.0, iTime * 1.2);
    float n3 = fogWashFbm(uv * 1.6 - 5.0, iTime * 0.6);

    // Low thresholds push most of the card into deep or mist tones for a dense look.
    vec3 col = base;
    col = mix(col, deep, smoothstep(0.25, 0.5, n2));
    col = mix(col, mist, smoothstep(0.3, 0.55, n1));
    col = mix(col, mix(accentTint, vec3(1.0), 0.5), pow(clamp(n3, 0.0, 1.0), 2.2) * 0.25);
    return col;
}

// --- Cloud: textured grey sky wash tinted with the theme accent ---
// Three drifting octaves plus a fourth static one: animating the finest octave aliases into
// flicker at card resolution.
float cloudWashFbm(vec2 p, float iTime) {
    float v = 0.0;
    float amp = 0.5;
    vec2 drift = vec2(iTime * 0.02, -iTime * 0.013);
    for (int i = 0; i < 3; i++) {
        v += amp * fogNoise(p + drift);
        p *= 2.0;
        amp *= 0.55;
    }
    v += amp * fogNoise(p);
    return v;
}

vec3 cloudSkyColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.76, u_bg_color.rgb * 0.98, uv.y);

    vec3 accentTint = u_accent_color.rgb;

    vec3 mist = mix(particleColor(vec3(0.94, 0.95, 0.97), vec3(0.85, 0.87, 0.91)), accentTint, 0.22);
    vec3 deep = mix(particleColor(vec3(0.4, 0.43, 0.5), vec3(0.28, 0.31, 0.38)), accentTint, 0.3);

    float n1 = cloudWashFbm(uv * 1.9 + vec2(842.0, -266.0), iTime);
    float n2 = cloudWashFbm(uv * 3.1 + vec2(-519.0, 333.0), iTime * 1.3);

    vec3 col = base;
    col = mix(col, deep, smoothstep(0.35, 0.6, n2));
    col = mix(col, mist, smoothstep(0.4, 0.65, n1));
    // Product of both fields carves out rounder, clumped highlights that read as cloud puffs.
    col = mix(col, mist, pow(clamp(n1 * n2, 0.0, 1.0), 1.3) * 0.5);
    return col;
}

void main() {
    vec2 uv = v_uv;
    float aspect = u_item_width / u_item_height;
    vec2 uvA = vec2(uv.x * aspect, uv.y);
    float mask = cornerMask();
    float finalAlpha = u_bg_color.a * mask;

    if (u_alternative > 0.5) {
        float iTime = u_time * 0.02;
        vec3 fogCol = fogWashColor(uvA, iTime);
        gl_FragColor = vec4(fogCol * finalAlpha, finalAlpha);
        return;
    }

    float iTime = u_time * 0.025;
    vec3 cloudCol = cloudSkyColor(uvA, iTime);
    gl_FragColor = vec4(cloudCol * finalAlpha, finalAlpha);
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

float star_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = star_hash(i);
    float b = star_hash(i + vec2(1.0, 0.0));
    float c = star_hash(i + vec2(0.0, 1.0));
    float d = star_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Drifting night-sky nebula backdrop: the same FBM wash with an indigo/violet/teal palette.
float starFogFbm(vec2 p, float iTime) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        float fi = float(i + 1);
        v += amp * star_noise(p + vec2(iTime * 0.02 * fi, -iTime * 0.015 * fi));
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

vec3 starNebulaColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    vec3 indigo = particleColor(vec3(0.192, 0.165, 0.329), vec3(0.14, 0.12, 0.22));
    vec3 violet = particleColor(vec3(0.329, 0.204, 0.388), vec3(0.22, 0.14, 0.26));
    vec3 teal = particleColor(vec3(0.165, 0.302, 0.376), vec3(0.1, 0.2, 0.24));
    vec3 roseDust = particleColor(vec3(0.388, 0.247, 0.302), vec3(0.24, 0.16, 0.19));

    float n1 = starFogFbm(uv * 2.0, iTime);
    float n2 = starFogFbm(uv * 3.2 + 10.0, iTime * 1.2);
    float n3 = starFogFbm(uv * 2.6 - 5.0, iTime * 0.8);

    vec3 col = base;
    col = mix(col, indigo, smoothstep(0.35, 0.68, n1) * 0.55);
    col = mix(col, violet, smoothstep(0.52, 0.84, n2) * 0.4);
    col = mix(col, teal, pow(clamp(n1 * n2, 0.0, 1.0), 1.3) * 0.35);
    col = mix(col, roseDust, smoothstep(0.5, 0.8, n3) * 0.3);
    return col;
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
            if (starChance > 0.7) {
                vec2 toStar = (offset + starPos - gridPos);
                float dist = length(toStar) * density;
                float starSize = 2.4;
                float brightness = star_hash(cellID + vec2(23.456, 78.901)) * 0.6 + 0.4;

                // Two per-star frequencies mixed by a per-star weight, plus a per-star
                // sharpness exponent: some stars glow slowly, others flicker sharply.
                float twinkleSpeed1 = star_hash(cellID + vec2(34.567, 89.012)) * 3.0 + 2.0;
                float twinkleSpeed2 = star_hash(cellID + vec2(45.678, 90.123)) * 7.0 + 4.0;
                float twinkleMix = star_hash(cellID + vec2(56.789, 11.234));
                float sharpness = star_hash(cellID + vec2(67.891, 22.345)) * 3.5 + 1.5;
                float phase1 = iTime * twinkleSpeed1 + star_hash(cellID) * 6.28;
                float phase2 = iTime * twinkleSpeed2 + star_hash(cellID + vec2(1.0, 1.0)) * 6.28;
                float wave = mix(sin(phase1), sin(phase2), twinkleMix) * 0.5 + 0.5;
                float twinkle = pow(wave, sharpness);

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
    float iTime = u_time * 0.12;
    float aspect = u_item_width / u_item_height;
    vec2 uvAspect = vec2(uv.x * aspect, uv.y);
    vec4 col = vec4(starNebulaColor(uvAspect, iTime * 0.6), 1.0);

    float stars1 = stars(uvAspect, 40.0, iTime);
    float stars2 = stars(uvAspect + vec2(0.5, 0.3), 25.0, iTime * 1.3);
    float stars3 = stars(uvAspect + vec2(0.25, 0.7), 15.0, iTime * 0.9);

    vec3 starColor1 = particleColor(vec3(0.85, 0.9, 1.0), vec3(0.25, 0.3, 0.45));
    vec3 starColor2 = particleColor(vec3(0.95, 0.97, 1.0), vec3(0.3, 0.35, 0.5));
    vec3 starColor3 = particleColor(vec3(1.0, 0.98, 0.95), vec3(0.35, 0.35, 0.45));

    vec3 starsRGB = starColor1 * stars1 * 0.6 +
                    starColor2 * stars2 * 0.8 +
                    starColor3 * stars3 * 1.0;

    float starsAlpha = clamp(stars1 * 0.6 + stars2 * 0.8 + stars3, 0.0, 1.0);

    vec3 resultRGB = starsRGB * starsAlpha + col.rgb * (1.0 - starsAlpha);
    float resultAlpha = starsAlpha + col.a * (1.0 - starsAlpha);

    float mask = cornerMask();
    float finalAlpha = resultAlpha * mask * u_bg_color.a;
    gl_FragColor = vec4(resultRGB * (finalAlpha / max(resultAlpha, 0.001)), finalAlpha);
}
)";

  // Slot in m_programs. Fog has no slot of its own; draw() renders it with the Cloud program.
  std::optional<std::size_t> programIndex(EffectType type) {
    switch (type) {
    case EffectType::Sun:
      return 0;
    case EffectType::Snow:
      return 1;
    case EffectType::Rain:
      return 2;
    case EffectType::Cloud:
      return 3;
    case EffectType::Stars:
      return 4;
    case EffectType::None:
    case EffectType::Fog:
      return std::nullopt;
    }
    return std::nullopt;
  }

} // namespace

void EffectProgram::ensureInitialized() {
  if (m_programs[0].program.isValid()) {
    return;
  }

  initProgram(EffectType::Sun, kSunFragment);
  initProgram(EffectType::Snow, kSnowFragment);
  initProgram(EffectType::Rain, kRainFragment);
  initProgram(EffectType::Cloud, kCloudFragment);
  initProgram(EffectType::Stars, kStarsFragment);
}

void EffectProgram::destroy() {
  for (auto& pd : m_programs) {
    pd.program.destroy();
  }
}

void EffectProgram::abandon() noexcept {
  for (auto& pd : m_programs) {
    pd.program.abandon();
  }
}

void EffectProgram::initProgram(EffectType type, const char* fragSource) {
  std::string fullFrag = std::string(kCommonFragment) + fragSource;

  auto& pd = m_programs[programIndex(type).value()];
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
  pd.accentColorLoc = glGetUniformLocation(id, "u_accent_color");
  pd.radiusLoc = glGetUniformLocation(id, "u_radius");
  pd.alternativeLoc = glGetUniformLocation(id, "u_alternative");
  pd.particlesLoc = glGetUniformLocation(id, "u_particles");
  pd.particleCountLoc = glGetUniformLocation(id, "u_particle_count");

  if (pd.positionLoc < 0 || pd.surfaceSizeLoc < 0 || pd.transformLoc < 0) {
    throw std::runtime_error("failed to query effect shader locations");
  }
}

void EffectProgram::draw(
    TextureId particles, float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style,
    const Mat3& transform
) const {
  if (style.type == EffectType::None || width <= 0.0F || height <= 0.0F) {
    return;
  }

  // Fog reuses the Cloud shader with alternative=1
  const bool isFog = style.type == EffectType::Fog;
  const auto effectType = isFog ? EffectType::Cloud : style.type;
  const auto idx = programIndex(effectType);
  if (!idx.has_value() || !m_programs[*idx].program.isValid()) {
    return;
  }

  const auto& pd = m_programs[*idx];

  static constexpr float kQuad[] = {
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F,
  };

  glUseProgram(pd.program.id());

  glUniform2f(pd.surfaceSizeLoc, surfaceWidth, surfaceHeight);
  glUniform2f(pd.quadSizeLoc, width, height);
  glUniform2f(pd.rectOriginLoc, 0.0F, 0.0F);
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
  if (pd.accentColorLoc >= 0) {
    glUniform4f(pd.accentColorLoc, style.accentColor.r, style.accentColor.g, style.accentColor.b, style.accentColor.a);
  }
  if (pd.radiusLoc >= 0) {
    glUniform1f(pd.radiusLoc, style.radius);
  }
  if (pd.alternativeLoc >= 0) {
    glUniform1f(pd.alternativeLoc, isFog ? 1.0F : 0.0F);
  }
  if (pd.particlesLoc >= 0) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(particles.value()));
    glUniform1i(pd.particlesLoc, 0);
  }
  if (pd.particleCountLoc >= 0) {
    glUniform1i(pd.particleCountLoc, particles.valid() ? style.particleCount : 0);
  }

  auto posAttr = static_cast<GLuint>(pd.positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
