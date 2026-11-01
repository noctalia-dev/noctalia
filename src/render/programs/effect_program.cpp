#include "render/programs/effect_program.h"

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
// Theme accent (ColorRole::Primary) - only the Fog shader reads this, to mix a touch of
// the user's actual theme color into its mist. Defaults to opaque black for callers (e.g.
// the standalone lab demo) that never set it; the Fog shader treats near-black as "no
// accent configured" and skips the tint rather than darkening the mist.
uniform vec4 u_accent_color;
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

// Same drifting cloud/fog backdrop as the Rain effect's rainFogColor() (see that
// definition's comment), reusing this shader's own efx_noise instead of duplicating a
// second near-identical hash/noise pair - every effect in this file already keeps its own
// copy of those rather than sharing one through kCommonFragment.
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
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    vec3 secondary = particleColor(vec3(0.314, 0.353, 0.416), vec3(0.2, 0.24, 0.3));
    vec3 tertiary = particleColor(vec3(0.404, 0.553, 0.498), vec3(0.2, 0.32, 0.28));
    vec3 primary = particleColor(vec3(0.561, 0.459, 0.259), vec3(0.42, 0.33, 0.16));
    vec3 accent = particleColor(vec3(0.275, 0.314, 0.22), vec3(0.19, 0.22, 0.14));

    float n1 = sunFogFbm(uv * 2.2, iTime);
    float n2 = sunFogFbm(uv * 3.5 + 10.0, iTime * 1.3);
    float n3 = sunFogFbm(uv * 2.8 - 5.0, iTime * 0.8);

    vec3 col = base;
    col = mix(col, secondary, smoothstep(0.35, 0.68, n1) * 0.48);
    col = mix(col, tertiary, smoothstep(0.52, 0.84, n2) * 0.33);
    col = mix(col, primary, pow(clamp(n1 * n2, 0.0, 1.0), 1.3) * 0.45);
    col = mix(col, accent, smoothstep(0.5, 0.8, n3) * 0.4);
    return col;
}

float sunCore(vec2 uv, vec2 sunPos, float iTime) {
    float dist = length(uv - sunPos);
    float mainFlare = exp(-dist * 15.0) * 2.0;
    float pulse = sin(iTime) * 0.1 + 0.9;
    return mainFlare * pulse;
}

// Hollow coating rings close around the disc, plus a true off-axis ghost chain along the
// sun -> screen-center line (fixed screen-space positions, unlike the old "flares" loop
// here, which multiplied toSun = uv - sunPos back into flarePos and so cancelled uv out
// to length(toSun)*|1 - i*0.3| - just more concentric rings around the sun itself, not
// actual secondary reflections on the opposite side of frame center like a real lens).
// Returns a tinted vec3 directly (each ring/ghost gets its own hue, like anti-reflective
// coating fringing) instead of a scalar multiplied by one flat sunColor.
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
    vec4 col = vec4(sunFogColor(uvAspect, iTime), 1.0);

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

    // Additive, not alpha-blended like the terms above - these are thin bright ring
    // outlines (mostly zero coverage), so a blend would barely show; additive lets each
    // ring's own tint punch through regardless of what's already composited underneath.
    vec2 screenCenter = vec2(0.5 * aspect, 0.5);
    resultRGB += sunLensFlares(uvAspect, sunPosAspect, screenCenter, iTime);

    resultRGB = mix(resultRGB, resultRGB * vec3(1.08, 1.04, 0.98), 0.15);

    float mask = cornerMask();
    float finalAlpha = resultAlpha * mask * u_bg_color.a;
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
    vec3 snowColor = particleColor(vec3(1.0), vec3(0.45, 0.55, 0.65));
    vec3 blended = mix(u_bg_color.rgb, snowColor, snowAlpha);

    float mask = cornerMask();
    float finalAlpha = mask * u_bg_color.a;
    gl_FragColor = vec4(blended * finalAlpha, finalAlpha);
}
)";

  // --- Rain effect v2 (sharper-edged streaks, random per-streak tilt, fewer/clearer
  // layers instead of 3 overlapping Gaussian-blurred ones - see
  // openspec/changes/weather-rain-effect-v2 for the prototyping history) ---
  // kMaxDropsGlsl must match render_styles.h's kMaxRainDrops (a literal here, since GLSL
  // ES 1.00 array sizes must be compile-time constants, not an includable C++ constant).
  constexpr char kRainFragment[] = R"(
uniform vec4 u_drops[24];
uniform int u_dropCount;

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

// Drifting fog/bokeh backdrop wash, replacing the flat u_bg_color base - an inline
// analytic approximation of the lab prototype's dedicated multi-pass fog FBO
// (openspec/changes/weather-rain-effect-v2/lab's FOG_BG_FRAGMENT: low-res render +
// bilinear-upscale blur). A single-pass per-pixel FBM is affordable here since this is a
// small Control Center card, not a full-screen effect, so the separate render target
// that approach exists to amortize isn't worth the added FBO/blur-pass plumbing.
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

// Tinted with the same four-hue spread as the lab version (gold/slate-blue/teal/olive),
// mixed via particleColor() against u_bg_color's luminance instead of hardcoded wallust
// colors, so it reads correctly against this app's actual light/dark theme.
vec3 rainFogColor(vec2 uv, float iTime) {
    // Capped at u_bg_color's own brightness (no *1.2 ceiling) and the four hue washes
    // toned down ~40% below - the first pass matched the lab's saturation, but the lab's
    // version sits behind a separate dark text panel; here the fog is the only backdrop
    // behind the card's temperature/label text, so it has to stay dark/muted enough for
    // that text to keep reading over it.
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    // particleColor()'s 2nd arg must be the DARKER/muted tint (used on light backgrounds,
    // per its own doc comment) - every other call site in this file follows
    // dark-bg-param=bright, light-bg-param=darker (sunColor, snowColor, starColor*,
    // hazeColor, rainTint above). These four were accidentally the other way around
    // (lighter pastel for the light-bg param), which washes the fog out to near-nothing
    // against an already-light Surface instead of giving it contrast.
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

// Good-quality 2D -> 4D hash (Dave Hoskins' public-domain hash-without-sine family).
// Returns four well-decorrelated values from a single seed, used instead of repeatedly
// calling a scalar hash with small constants added to the same cell id - those correlate
// and show up as visible patterning.
vec4 rain_hash42(vec2 p) {
    vec4 p4 = fract(vec4(p.x, p.y, p.x, p.y) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
    p4 += dot(p4, p4.wzxy + 33.33);
    return fract((p4.xxyz + p4.yzzw) * p4.zywx);
}

// Each droplet contributes a small paraboloid "dome" of height (zero outside its radius)
// instead of a signed distance; domes are summed (not blended) into heightSum/gradSum so
// overlapping droplets merge into one smooth bump automatically.
void rain_addDome(vec2 uv, vec2 c, float r, inout float heightSum, inout vec2 gradSum) {
    vec2 duv = uv - c;
    float d2 = dot(duv, duv);
    float rr = r * r;
    if (d2 < rr) {
        heightSum += rr - d2;
        gradSum += -2.0 * duv;
    }
}

// Dense field of small static droplets stuck to the pane, scattered on a jittered grid
// with each one slowly fading in and out over time so the field renews itself instead of
// looking frozen in place. Pure per-pixel procedural decoration - not simulated state,
// so it costs nothing to update and needs no uniform of its own.
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

        // Per-streak size/speed/length variety, not just two fixed layer presets -
        // every drop gets its own width, fall speed, and streak length from its column hash.
        float width = widthBase * (0.7 + rain_hash(vec2(cId, seed + 13.0)) * widthVar);
        float speed = speedBase * (0.7 + rain_hash(vec2(cId, seed + 11.0)) * speedVar);
        float dropLen = dropLenBase * (0.7 + rain_hash(vec2(cId, seed + 17.0)) * 0.6);

        // Each stream gets its own tilt, not one shared global slant - some streaks lean
        // left, some right, some nearly straight down, like gusty wind hitting different
        // streaks differently instead of a uniform wind direction.
        float tiltJitter = (rain_hash(vec2(cId, seed + 29.0)) - 0.5) * 1.8;
        px += uv.y * tiltJitter;

        // Wider, softer falloff from the centerline (smaller exponent) - reads as a
        // blurry smear instead of a crisp line, no flat plateau either way.
        float edge = exp(-1.6 * (px * px) / (width * width));

        float y = uv.y - iTime * speed;
        float row = y * rows;
        float phase = rain_hash(vec2(cId, seed + 3.0));
        float dropY = fract(row + phase);

        float head = smoothstep(0.0, 0.04, dropY);
        float tail = smoothstep(dropLen, dropLen - 0.15, dropY);
        float yFade = head * tail;

        // uv.y grows downward, and increasing dropY tracks increasing uv.y here, so the
        // drop's leading (falling) edge is at the HIGH end of dropY, not dropY=0 - bright
        // there, fading toward dropY=0 (where the drop "already was", trailing above it).
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

    // Glass droplets: summed parabolic "domes" (heights simply add, so overlapping drops
    // merge into one smooth bump with no per-pair blend-radius tuning) - the same
    // height-field + lighting math validated in the lab's Glass/Flow shader-only
    // iteration. A dense procedural static field gives the pane its frosted texture (pure
    // per-pixel decoration, no state to simulate); the real simulated drops (u_drops) are
    // the bigger falling/merging ones with actual physics driving their position.
    float heightSum = 0.0;
    vec2 gradSum = vec2(0.0);
    rain_staticField(uvA, iTime, heightSum, gradSum);
    // Each real drop draws as a short trailing chain of shrinking domes (not a single
    // circle) - same technique as the lab's runnerDrop(), now lengthed by the drop's own
    // simulated momentum (u_drops[i].w) instead of a hashed fake speed.
    for (int i = 0; i < 24; i++) {
        if (i < u_dropCount) {
            vec4 drop = u_drops[i];
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

    // Epsilon lowered from the first pass's 0.00012 (~r=0.011) to ~r=0.006, so the
    // smaller static-field droplets clear the threshold too instead of only the bigger
    // simulated ones - matching the lab's densely-covered "wet pane" read.
    float inside = smoothstep(0.0, 0.00004, heightSum);

    // Surface normal from the height field's gradient - clamped so dense overlapping
    // areas (where several domes' gradients stack) don't blow out into noise.
    vec2 slope = clamp(gradSum * 16.0, vec2(-2.5), vec2(2.5));
    vec3 normal = normalize(vec3(slope, 1.0));
    vec3 lightDir = normalize(vec3(-0.4, -0.6, 0.65));

    // Refraction: bend the sampled rain-streak AND fog-backdrop position by the surface
    // tilt - this is what makes a droplet read as a lens bulging the scene behind it
    // (the lab's real-texture refraction), not just a flat tinted circle. Fog is pulled a
    // touch harder than the streaks (1.3x vs 1.0x) since it is the richer, more legible
    // backdrop - the same relative weighting the lab's two-texture (sharp/blurred) sample
    // achieved by using different source resolutions.
    vec2 uvOffset = normal.xy * 0.4 * inside;
    vec2 streakUvA = uvA - uvOffset;
    vec2 fogUvA = uvA - uvOffset * 1.3;

    // Single cheap SMOOTH noise octave, much lower weight than v1 - atmosphere, not mud.
    float haze = rain_noise(uv * 2.5 + vec2(iTime * 0.02, iTime * 0.015));
    haze = smoothstep(0.3, 0.7, haze) * 0.08;

    // 2 layers with a clear foreground/background split instead of 3 similar ones:
    // foreground is fewer, wider, faster, brighter; background is more, thinner, slower, dimmer.
    float tilt = 0.35;
    //                          uv         cols  rows  speed var  dropLen tilt        width var   time   seed
    float fg = rainLayer(streakUvA, 12.0, 2.2, 7.0, 0.7, 0.45, tilt,       0.045, 0.7, iTime, 0.0);
    float bg = rainLayer(streakUvA, 22.0, 1.6, 3.8, 0.8, 0.55, tilt * 0.6, 0.022, 0.7, iTime, 100.0);

    float rain = clamp(fg + bg * 0.4, 0.0, 1.0);

    vec3 hazeColor = particleColor(vec3(0.82, 0.85, 0.9), vec3(0.55, 0.6, 0.68));
    vec3 col = mix(rainFogColor(fogUvA, iTime), hazeColor, haze);

    // Neutral grey, not blue - a noticeable blue cast (the old vec3(0.55, 0.65, 0.8) had
    // a 0.25 R-to-B gap) read fine over a flat dark background, but now stacks with the
    // fog's own blue/teal wash and the streaks cover enough of the card at this density
    // to dominate the whole card's color instead of reading as individual water lines.
    // Mix weight also dropped (0.75 -> 0.4) so the fog shows through the streaks instead
    // of being painted over by them - more "wet glass over a backdrop", less "blue film".
    vec3 rainTint = particleColor(vec3(0.68, 0.7, 0.74), vec3(0.42, 0.46, 0.5));
    vec3 resultRGB = mix(col, rainTint, rain * 0.4);

    // Water-surface shading, all derived from the one continuous normal field: a soft
    // specular highlight where the surface faces the light, and a grazing-angle edge
    // darkening (fresnel-like falloff from normal.z) instead of a hard uniform dark ring
    // around every circle - this is what keeps merged drops reading as one smooth shape.
    // Exponent raised (24 -> 40) and intensity cut by more than half (1.1 -> 0.45) - the
    // first pass's highlight blew out into a flat white wash across most of each droplet
    // instead of a small bright point, since this card never has a strong single light
    // source to justify a wide, intense specular lobe.
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

// --- Fog: soft drifting mist wash (Vanta.js Fog reference look) ---
// Vanta.js's Fog demo recursively self-warps FBM (f(p+fbm(p+fbm(p)))) at full-page hero
// scale, where there's enough screen real estate for the warped folds to read as
// atmospheric depth. That exact recursive-warp technique was already tried for this
// card-sized effect and rejected - shrunk down to Control Center size it just read as a
// shapeless colored smudge, not mist (see cloudIconDensity()'s comment above for the full
// technique history). The plain multi-octave FBM + layered color blend already proven as
// the drifting backdrop behind Rain/Sun/Snow/Stars (rainFogFbm/rainFogColor et al. in
// their own shader sources) gets the same soft, slowly-drifting quality WITHOUT the
// recursive self-warp, and already reads correctly at this size - so Fog reuses that same
// machinery as its own primary visual (not just a backdrop behind a foreground shape)
// instead of the icon-lobe approach Cloud uses, with a denser/cooler mist palette
// appropriate to being the main subject.
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

    // The previous attempt at contrast linearly re-centered/stretched each fbm octave
    // (clamp((n-0.5)*1.9+0.5, 0, 1)) BEFORE thresholding it - since fbm output is already
    // smooth and low-frequency at this scale, stretching it that hard just clipped most of
    // the card to one flat plateau (either fully "deep" or fully "mist") instead of adding
    // texture. The actual fix is higher-frequency noise (more, smaller patches = visible
    // structure across the whole card) plus full-strength (not partial) layer blending, not
    // a post-hoc stretch on top of coarse noise.
    bool hasAccent = length(u_accent_color.rgb) > 0.05;
    // Demo/no-theme fallback keeps the original neutral-grey mist; real app always has an
    // accent (Material3 Primary is never unset), so this only matters for the lab tool.
    vec3 accentTint = hasAccent ? u_accent_color.rgb : vec3(0.65, 0.68, 0.74);

    // Accent baked directly into both tonal extremes (not a separate overlay layer riding
    // a noise mask) so the theme color reads consistently through the whole wash instead
    // of only flashing in whichever patches happen to cross a threshold.
    vec3 mist = mix(particleColor(vec3(0.97, 0.98, 1.0), vec3(0.82, 0.85, 0.9)), accentTint, 0.3);
    vec3 deep = mix(particleColor(vec3(0.3, 0.34, 0.44), vec3(0.18, 0.22, 0.3)), accentTint, 0.35);

    // Higher frequency than the first pass (1.5/2.3 -> 2.6/3.8) so patches are smaller and
    // more numerous - that's what actually reads as "textured fog" instead of one or two
    // blobs filling half the card.
    float n1 = fogWashFbm(uv * 2.6, iTime);
    float n2 = fogWashFbm(uv * 3.8 + 10.0, iTime * 1.2);
    float n3 = fogWashFbm(uv * 1.6 - 5.0, iTime * 0.6);

    vec3 col = base;
    col = mix(col, deep, smoothstep(0.35, 0.6, n2));
    col = mix(col, mist, smoothstep(0.4, 0.65, n1));
    col = mix(col, mix(accentTint, vec3(1.0), 0.5), pow(clamp(n3, 0.0, 1.0), 2.2) * 0.25);
    return col;
}

// --- Cloud: same full-opacity wash technique as Fog, grey + theme accent palette ---
// The hand-drawn icon-lobe silhouette read as flat and cartoonish. Sun's warm four-hue
// backdrop read as a muddy olive swamp without Sun's disc to sell it as "light". A
// variable-alpha "coverage" cutout (sky visible through gaps between cloud puffs) was
// tried next to visually distinguish Cloud from Fog's wall-to-wall wash - but a hard alpha
// edge over this fbm field only antialiases cleanly at the full demo-grid tile's
// resolution; shrunk down to the actual small Control Center card, the edge spans too few
// physical pixels and shows every jagged step of the underlying value-noise grid. Back to
// full-opacity compositing (same as Fog, which never had this problem) with the accent
// tint baked in like Fog's mist/deep tones, grey instead of Fog's palette, and distinct
// noise frequencies/offsets from Fog's 2.6/3.8 at offset 10.0 so the two don't trace the
// same patch layout.
//
// Separate from fogWashFbm: that function drifts each of its 5 octaves at a DIFFERENT
// speed (iTime * 0.025 * fi, fi = 1..5) - fine for Fog's slow 0.02 outer time scale, but at
// Cloud's higher drift and different frequency the fast-moving high-frequency octaves
// visibly "boil"/flicker against the slower low-frequency ones in motion (invisible in a
// still screenshot, obvious live). This copy uses only 3 octaves, all drifting at the same
// speed, to remove that per-octave relative motion entirely.
float cloudWashFbm(vec2 p, float iTime) {
    float v = 0.0;
    float amp = 0.5;
    vec2 drift = vec2(iTime * 0.02, -iTime * 0.013);
    // Going back to 5 octaves (all sharing one fixed drift vector) brought the flicker
    // right back, so it wasn't the per-octave drift speed after all - it's that the
    // FINEST octave's spatial frequency aliases at this card's small render resolution no
    // matter how it's time-shifted, any animation of it reads as noisy flicker. The first
    // 3 octaves (same frequency range as the working no-flicker version) keep drifting
    // normally for slow cloud motion; a 4th, finer octave is added WITHOUT the `drift`
    // offset - same spatial detail/roughness, but static over time, so it can't alias
    // into visible motion.
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

    bool hasAccent = length(u_accent_color.rgb) > 0.05;
    vec3 accentTint = hasAccent ? u_accent_color.rgb : vec3(0.6, 0.63, 0.68);

    vec3 mist = mix(particleColor(vec3(0.94, 0.95, 0.97), vec3(0.85, 0.87, 0.91)), accentTint, 0.22);
    vec3 deep = mix(particleColor(vec3(0.4, 0.43, 0.5), vec3(0.28, 0.31, 0.38)), accentTint, 0.3);

    float n1 = cloudWashFbm(uv * 1.9 + vec2(842.0, -266.0), iTime);
    float n2 = cloudWashFbm(uv * 3.1 + vec2(-519.0, 333.0), iTime * 1.3);

    vec3 col = base;
    col = mix(col, deep, smoothstep(0.35, 0.6, n2));
    col = mix(col, mist, smoothstep(0.4, 0.65, n1));
    // Rounder, more clumped highlight where BOTH noise fields are high at once (same
    // product trick Sun's backdrop uses for its "primary" layer) - sequential mixing alone
    // just gradates smoothly from deep to mist, which is why it read as a soft wash rather
    // than distinct puffy clouds; an AND-like product carves out localized rounder clumps
    // instead, which is the actual shape cue "looks cloudy".
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
    float iTime = u_time * 0.12;
    vec4 col = vec4(u_bg_color.rgb, 1.0);

    float aspect = u_item_width / u_item_height;
    vec2 uvAspect = vec2(uv.x * aspect, uv.y);

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
  [[nodiscard]] std::optional<std::size_t> programIndex(EffectType type) {
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
  pd.dropsLoc = glGetUniformLocation(id, "u_drops");
  pd.dropCountLoc = glGetUniformLocation(id, "u_dropCount");

  if (pd.positionLoc < 0 || pd.surfaceSizeLoc < 0 || pd.transformLoc < 0) {
    throw std::runtime_error("failed to query effect shader locations");
  }
}

void EffectProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const EffectStyle& style, const Mat3& transform
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
    glUniform4f(
        pd.accentColorLoc, style.accentColor.r, style.accentColor.g, style.accentColor.b, style.accentColor.a
    );
  }
  if (pd.radiusLoc >= 0) {
    glUniform1f(pd.radiusLoc, style.radius);
  }
  if (pd.alternativeLoc >= 0) {
    glUniform1f(pd.alternativeLoc, isFog ? 1.0F : 0.0F);
  }
  if (pd.dropsLoc >= 0) {
    static_assert(kMaxRainDrops <= 24, "kRainFragment's u_drops array is sized for 24 drops");
    std::array<float, kMaxRainDrops * 4> dropData{};
    for (int i = 0; i < style.dropCount; ++i) {
      const auto& drop = style.drops[static_cast<std::size_t>(i)];
      dropData[static_cast<std::size_t>(i) * 4 + 0] = drop.x;
      dropData[static_cast<std::size_t>(i) * 4 + 1] = drop.y;
      dropData[static_cast<std::size_t>(i) * 4 + 2] = drop.radius;
      dropData[static_cast<std::size_t>(i) * 4 + 3] = drop.depth;
    }
    glUniform4fv(pd.dropsLoc, kMaxRainDrops, dropData.data());
  }
  if (pd.dropCountLoc >= 0) {
    glUniform1i(pd.dropCountLoc, style.dropCount);
  }

  auto posAttr = static_cast<GLuint>(pd.positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
