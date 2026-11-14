#pragma once

namespace weather_effect_shader {

  // Cloud, and Fog when u_alternative is 1.
  inline constexpr char kCloudFragment[] = R"(
// --- Fog: veil of drifting mist banks ---
vec3 fogWashColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.85, u_bg_color.rgb, uv.y);

    vec3 accentTint = u_accent_color.rgb;

    // Accent mixed into both tonal extremes so the theme color carries through the whole wash.
    vec3 mist = mix(particleColor(vec3(0.78, 0.8, 0.84), vec3(0.9, 0.91, 0.93)), accentTint, 0.25);
    vec3 deep = mix(particleColor(vec3(0.3, 0.34, 0.44), vec3(0.44, 0.48, 0.56)), accentTint, 0.3);

    // Horizontally stretched noise reads as drifting banks of mist rather than cloud puffs;
    // the second bank layer drifts faster.
    float n1 = billowFbm(uv * vec2(1.5, 2.8), vec2(2.5, -0.3) * iTime, 5);
    float n2 = billowFbm(uv * vec2(2.0, 3.8) + 10.0, vec2(2.5, -0.3) * (iTime * 1.2), 5);

    // A veil that thickens toward the bottom, then soft banks of mist on top of it.
    vec3 col = mix(base, mist, mix(0.15, 0.45, uv.y));
    col = mix(col, deep, smoothstep(0.3, 0.75, n2) * 0.6);
    col = mix(col, mist, smoothstep(0.35, 0.75, n1) * 0.65);
    return col;
}

// --- Cloud: textured grey sky wash tinted with the theme accent ---
vec3 cloudSkyColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.76, u_bg_color.rgb * 0.98, uv.y);

    vec3 accentTint = u_accent_color.rgb;

    vec3 mist = mix(particleColor(vec3(0.84, 0.86, 0.9), vec3(0.88, 0.89, 0.92)), accentTint, 0.22);
    vec3 deep = mix(particleColor(vec3(0.3, 0.32, 0.38), vec3(0.48, 0.51, 0.58)), accentTint, 0.3);

    // The second field drifts faster for parallax.
    float n1 = billowFbm(uv * 1.9 + vec2(842.0, -266.0), vec2(1.0, -0.3) * iTime, 4);
    float n2 = billowFbm(uv * 3.1 + vec2(-519.0, 333.0), vec2(1.0, -0.3) * (iTime * 1.6), 4);

    // Partial weights and wide transitions keep the sky soft rather than high-contrast.
    vec3 col = base;
    col = mix(col, deep, smoothstep(0.3, 0.7, n2) * 0.8);
    col = mix(col, mist, smoothstep(0.35, 0.75, n1) * 0.75);
    // Product of both fields carves out rounder, clumped highlights that read as cloud puffs.
    col = mix(col, mist, pow(clamp(n1 * n2, 0.0, 1.0), 1.3) * 0.45);
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

} // namespace weather_effect_shader
