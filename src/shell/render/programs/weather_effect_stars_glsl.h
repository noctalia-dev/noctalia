#pragma once

namespace weather_effect_shader {

  // Twinkling starfield over a drifting nebula.
  inline constexpr char kStarsFragment[] = R"(
vec2 star_hash2(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(vec2(p.x * p.y, p.y * p.x));
}

// Drifting night-sky nebula backdrop: the churning wash with an indigo/violet/teal palette.
vec3 starNebulaColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    vec3 indigo = particleColor(vec3(0.192, 0.165, 0.329), vec3(0.14, 0.12, 0.22));
    vec3 violet = particleColor(vec3(0.329, 0.204, 0.388), vec3(0.22, 0.14, 0.26));
    vec3 teal = particleColor(vec3(0.165, 0.302, 0.376), vec3(0.1, 0.2, 0.24));
    vec3 roseDust = particleColor(vec3(0.388, 0.247, 0.302), vec3(0.24, 0.16, 0.19));

    float n1 = churningFbm(uv * 2.0, vec2(0.02, -0.015) * iTime);
    float n2 = churningFbm(uv * 3.2 + 10.0, vec2(0.02, -0.015) * (iTime * 1.2));
    float n3 = churningFbm(uv * 2.6 - 5.0, vec2(0.02, -0.015) * (iTime * 0.8));

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

            float starChance = hash12(cellID + vec2(12.345, 67.890));
            if (starChance > 0.7) {
                vec2 toStar = (offset + starPos - gridPos);
                float dist = length(toStar) * density;
                float starSize = 2.4;
                float brightness = hash12(cellID + vec2(23.456, 78.901)) * 0.6 + 0.4;

                // Two per-star frequencies mixed by a per-star weight, plus a per-star
                // sharpness exponent: some stars glow slowly, others flicker sharply.
                float twinkleSpeed1 = hash12(cellID + vec2(34.567, 89.012)) * 3.0 + 2.0;
                float twinkleSpeed2 = hash12(cellID + vec2(45.678, 90.123)) * 7.0 + 4.0;
                float twinkleMix = hash12(cellID + vec2(56.789, 11.234));
                float sharpness = hash12(cellID + vec2(67.891, 22.345)) * 3.5 + 1.5;
                float phase1 = iTime * twinkleSpeed1 + hash12(cellID) * 6.28;
                float phase2 = iTime * twinkleSpeed2 + hash12(cellID + vec2(1.0, 1.0)) * 6.28;
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

} // namespace weather_effect_shader
