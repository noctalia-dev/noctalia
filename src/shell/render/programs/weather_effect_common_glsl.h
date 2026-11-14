#pragma once

namespace weather_effect_shader {

  // Shared by every weather effect program.
  inline constexpr char kVertexShader[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform mat3 u_transform;
varying vec2 v_uv;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_uv = a_position;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  // Prepended to every effect fragment shader: uniforms, particle unpacking, corner mask,
  // theme-aware tint, and the shared noise (hash12, valueNoise, churningFbm, billowFbm).
  inline constexpr char kCommonFragment[] = R"(
precision highp float;

uniform float u_time;
uniform float u_item_width;
uniform float u_item_height;
uniform vec4 u_bg_color;
// Theme accent (ColorRole::Primary), read by the Fog and Cloud shaders.
uniform vec4 u_accent_color;
uniform float u_radius;
uniform float u_alternative;
varying vec2 v_uv;

// Rain and Snow particles: WeatherEffectNode packs two RGBA8 texels per particle, (x, y) then
// (radius, weight), each value 16-bit as hi/lo bytes. Width and ranges match WeatherEffectNode's
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

float hash12(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}

// Smooth value noise in [0, 1].
float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Four octaves where octave k is offset by k * drift, so finer detail moves faster and the
// wash churns as it drifts. Backdrop of the Sun, Snow, Rain, and Stars effects.
float churningFbm(vec2 p, vec2 drift) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        v += amp * valueNoise(p + drift * float(i + 1));
        p *= 2.0;
        amp *= 0.5;
    }
    return v;
}

// Up to five octaves (`octaves`) that drift as one sheet, each rotated off the previous grid
// to hide the value-noise lattice, after a cheap two-lookup warp that bends shapes into
// billows. Body of the Cloud and Fog effects.
float billowFbm(vec2 p, vec2 drift, int octaves) {
    p += drift;
    vec2 warp = vec2(valueNoise(p * 0.6), valueNoise(p * 0.6 + vec2(5.2, 1.3)));
    p += (warp - 0.5) * 0.9;

    const mat2 octave = mat2(1.6, 1.2, -1.2, 1.6);
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++) {
        if (i >= octaves) {
            break;
        }
        v += amp * valueNoise(p);
        p = octave * p;
        amp *= 0.55;
    }
    return v;
}
)";

} // namespace weather_effect_shader
