#pragma once

namespace weather_effect_shader {

  // Sun disc, rays, and lens flares over sparse drifting wisps.
  inline constexpr char kSunFragment[] = R"(
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
    float n1 = valueNoise(uv * 5.0 + vec2(iTime * 0.1, iTime * 0.05));
    float n2 = valueNoise(uv * 8.0 - vec2(iTime * 0.08, iTime * 0.12));
    float n3 = valueNoise(uv * 12.0 + vec2(iTime * 0.15, -iTime * 0.1));
    return (n1 * 0.5 + n2 * 0.3 + n3 * 0.2) * 0.15;
}

// Sparse drifting wisps: high thresholds and low weights keep the sky mostly clear, so the
// wisps only give the rays something to catch.
vec3 sunFogColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.9, u_bg_color.rgb, uv.y);

    vec3 wisp = particleColor(vec3(0.36, 0.38, 0.44), vec3(0.78, 0.78, 0.78));
    vec3 sunlit = particleColor(vec3(0.56, 0.46, 0.26), vec3(0.93, 0.8, 0.55));

    float n1 = churningFbm(uv * 2.2, vec2(0.03, -0.02) * iTime);
    float n2 = churningFbm(uv * 3.5 + 10.0, vec2(0.03, -0.02) * (iTime * 1.3));

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

} // namespace weather_effect_shader
