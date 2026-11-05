#pragma once

namespace weather_effect_shader {

  // Simulated flakes under a drifting cloud bank, over a cold backdrop.
  // The flake loop bound must match kMaxSnowFlakes.
  inline constexpr char kSnowFragment[] = R"(
vec3 snowFogColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    // Cold palette: icy blue, pale cyan, lavender-grey, slate.
    vec3 secondary = particleColor(vec3(0.247, 0.361, 0.447), vec3(0.17, 0.26, 0.32));
    vec3 tertiary = particleColor(vec3(0.318, 0.482, 0.49), vec3(0.19, 0.3, 0.3));
    vec3 primary = particleColor(vec3(0.42, 0.38, 0.49), vec3(0.28, 0.25, 0.34));
    vec3 accent = particleColor(vec3(0.267, 0.31, 0.333), vec3(0.18, 0.21, 0.22));

    float n1 = churningFbm(uv * 2.2, vec2(0.03, -0.02) * iTime);
    float n2 = churningFbm(uv * 3.5 + 10.0, vec2(0.03, -0.02) * (iTime * 1.3));
    float n3 = churningFbm(uv * 2.8 - 5.0, vec2(0.03, -0.02) * (iTime * 0.8));

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

    // Pale cloud bank along the top of the card, from a horizontally stretched churningFbm.
    // Its lower edge wanders with a slow noise and fades over a wide band, so there is no
    // straight seam; the bank drifts sideways and churns slowly.
    float bandEdge = mix(0.22, 0.4, valueNoise(vec2(uvA.x * 1.5 + iTime * 0.05, iTime * 0.1)));
    float cloudBand = 1.0 - smoothstep(0.0, bandEdge, v_uv.y);
    cloudBand *= cloudBand;
    if (cloudBand > 0.0) {
        float cloudNoise = churningFbm(uvA * vec2(2.6, 6.0) + vec2(iTime * 0.08, 0.0), vec2(0.03, -0.02) * iTime);
        float cloudShape = smoothstep(0.25, 0.85, cloudNoise) * cloudBand;
        vec3 cloudColor = particleColor(vec3(0.93, 0.94, 0.96), vec3(0.8, 0.82, 0.86));
        // Dense regions near the top go nearly opaque so the bank occludes flakes behind it.
        blended = mix(blended, cloudColor, clamp(cloudShape * 1.6, 0.0, 0.92));
    }

    float mask = cornerMask();
    float finalAlpha = mask * u_bg_color.a;
    gl_FragColor = vec4(blended * finalAlpha, finalAlpha);
}
)";

} // namespace weather_effect_shader
