#pragma once

namespace weather_effect_shader {

  // Glass droplets over tilted streaks and a drifting fog backdrop.
  // The drop loop bound must match kMaxRainDrops.
  inline constexpr char kRainFragment[] = R"(
float rain_hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

// Four-hue wash (slate-blue, teal, gold, olive) over a darkened u_bg_color, kept muted so
// the card's text stays readable.
vec3 rainFogColor(vec2 uv, float iTime) {
    vec3 base = mix(u_bg_color.rgb * 0.7, u_bg_color.rgb * 0.95, uv.y);

    vec3 secondary = particleColor(vec3(0.314, 0.353, 0.416), vec3(0.2, 0.24, 0.3));
    vec3 tertiary = particleColor(vec3(0.404, 0.553, 0.498), vec3(0.2, 0.32, 0.28));
    vec3 primary = particleColor(vec3(0.561, 0.459, 0.259), vec3(0.42, 0.33, 0.16));
    vec3 accent = particleColor(vec3(0.275, 0.314, 0.22), vec3(0.19, 0.22, 0.14));

    float n1 = churningFbm(uv * 2.2, vec2(0.03, -0.02) * iTime);
    float n2 = churningFbm(uv * 3.5 + 10.0, vec2(0.03, -0.02) * (iTime * 1.3));
    float n3 = churningFbm(uv * 2.8 - 5.0, vec2(0.03, -0.02) * (iTime * 0.8));

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

// Droplets stuck to the pane on a jittered grid. Each lives 30-60 s (iTime is u_time * 0.12),
// fading in and out, and slides down 0.5-1.2 cells over its life; bigger ones slide further.
// Droplets only move down, so the search reaches two cells up; slides stay under 1.4 cells
// to remain inside it.
void rain_staticField(vec2 uv, float iTime, inout float heightSum, inout vec2 gradSum) {
    float cellSize = 0.042;
    vec2 baseCell = floor(uv / cellSize);

    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -2; dy <= 1; dy++) {
            vec2 cId = baseCell + vec2(float(dx), float(dy));
            vec4 h = rain_hash42(cId);
            // Per-cell density, so a sliding droplet is never culled partway across.
            float density = valueNoise(cId * cellSize * 3.2 + 11.0);
            float threshold = mix(0.35, 0.7, density);
            if (h.x < threshold) continue;

            vec2 jitter = h.yz;
            vec2 c = (cId + jitter) * cellSize;
            float sizeT = pow(h.w, 2.2);
            float r = mix(0.006, 0.024, sizeT);

            vec4 lifeHash = rain_hash42(cId * 1.7 + 9.3);
            float lifeRate = mix(1.0 / (60.0 * 0.12), 1.0 / (30.0 * 0.12), lifeHash.y);
            float life = fract(iTime * lifeRate + lifeHash.x);
            float fade = smoothstep(0.0, 0.15, life) * smoothstep(1.0, 0.82, life);
            c.y += life * mix(0.5, 1.2, sizeT) * cellSize;
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
    float haze = valueNoise(uv * 2.5 + vec2(iTime * 0.02, iTime * 0.015));
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

} // namespace weather_effect_shader
