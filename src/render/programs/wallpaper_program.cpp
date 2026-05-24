#include "render/programs/wallpaper_program.h"

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
    v_texcoord = a_position;
    vec3 pixel = u_transform * vec3(a_position * u_quad_size, 1.0);
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  // Common GLSL functions shared by all transition fragment shaders.
  // Included at the top of each fragment source via string concatenation.
  constexpr char kCommonFunctions[] = R"(
precision highp float;

uniform sampler2D u_source1;
uniform sampler2D u_source2;
uniform float u_sourceKind1;
uniform float u_sourceKind2;
uniform vec4 u_sourceColor1;
uniform vec4 u_sourceColor2;
uniform float u_progress;
uniform float u_fillMode;
uniform float u_imageWidth1;
uniform float u_imageHeight1;
uniform float u_imageWidth2;
uniform float u_imageHeight2;
uniform float u_screenWidth;
uniform float u_screenHeight;
uniform vec4 u_fillColor;

varying vec2 v_texcoord;

vec2 calculateUV(vec2 uv, float imgWidth, float imgHeight) {
    vec2 transformedUV = uv;

    if (u_fillMode < 0.5) {
        // Center
        vec2 screenPixel = uv * vec2(u_screenWidth, u_screenHeight);
        vec2 imageOffset = (vec2(u_screenWidth, u_screenHeight) - vec2(imgWidth, imgHeight)) * 0.5;
        vec2 imagePixel = screenPixel - imageOffset;
        transformedUV = imagePixel / vec2(imgWidth, imgHeight);
    }
    else if (u_fillMode < 1.5) {
        // Crop (fill/cover)
        float scale = max(u_screenWidth / imgWidth, u_screenHeight / imgHeight);
        vec2 scaledImageSize = vec2(imgWidth, imgHeight) * scale;
        vec2 offset = (scaledImageSize - vec2(u_screenWidth, u_screenHeight)) / scaledImageSize;
        transformedUV = uv * (vec2(1.0) - offset) + offset * 0.5;
    }
    else if (u_fillMode < 2.5) {
        // Fit (contain)
        float scale = min(u_screenWidth / imgWidth, u_screenHeight / imgHeight);
        vec2 scaledImageSize = vec2(imgWidth, imgHeight) * scale;
        vec2 offset = (vec2(u_screenWidth, u_screenHeight) - scaledImageSize) * 0.5;
        vec2 screenPixel = uv * vec2(u_screenWidth, u_screenHeight);
        vec2 imagePixel = (screenPixel - offset) / scale;
        transformedUV = imagePixel / vec2(imgWidth, imgHeight);
    }
    else if (u_fillMode < 3.5) {
        // Stretch - no transform
    }
    else {
        // Repeat (tile)
        vec2 screenPixel = uv * vec2(u_screenWidth, u_screenHeight);
        transformedUV = screenPixel / vec2(imgWidth, imgHeight);
    }

    return transformedUV;
}

vec4 sampleWithFillMode(sampler2D tex, vec2 uv, float imgWidth, float imgHeight) {
    vec2 transformedUV = calculateUV(uv, imgWidth, imgHeight);

    if (u_fillMode > 3.5) {
        return texture2D(tex, fract(transformedUV));
    }

    if (transformedUV.x < 0.0 || transformedUV.x > 1.0 ||
        transformedUV.y < 0.0 || transformedUV.y > 1.0) {
        return u_fillColor;
    }

    return texture2D(tex, transformedUV);
}

vec4 sampleSource(sampler2D tex, vec2 uv, float imgWidth, float imgHeight, float sourceKind, vec4 sourceColor) {
    if (sourceKind > 0.5) {
        return sourceColor;
    }
    return sampleWithFillMode(tex, uv, imgWidth, imgHeight);
}
)";

  constexpr char kFadeFragment[] = R"(
void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2);
    gl_FragColor = mix(color1, color2, u_progress);
}
)";

  constexpr char kWipeFragment[] = R"(
uniform float u_direction;
uniform float u_smoothness;

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2);

    float mappedSmoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);
    float extendedProgress = u_progress * (1.0 + 2.0 * mappedSmoothness) - mappedSmoothness;
    float edge = 0.0;
    float factor = 0.0;

    if (u_direction < 0.5) {
        edge = 1.0 - extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.x);
        gl_FragColor = mix(color1, color2, factor);
    }
    else if (u_direction < 1.5) {
        edge = extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.x);
        gl_FragColor = mix(color2, color1, factor);
    }
    else if (u_direction < 2.5) {
        edge = 1.0 - extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.y);
        gl_FragColor = mix(color1, color2, factor);
    }
    else {
        edge = extendedProgress;
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, uv.y);
        gl_FragColor = mix(color2, color1, factor);
    }
}
)";

  constexpr char kDiscFragment[] = R"(
uniform float u_smoothness;
uniform float u_centerX;
uniform float u_centerY;
uniform float u_aspectRatio;

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2);

    float mappedSmoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);

    vec2 center = vec2(u_centerX, u_centerY);
    vec2 aspectUV = vec2(uv.x * u_aspectRatio, uv.y);
    vec2 aspectCenter = vec2(center.x * u_aspectRatio, center.y);
    float dist = distance(aspectUV, aspectCenter);

    // Calculate max distance to corners
    float maxDist = 0.0;
    maxDist = max(maxDist, distance(aspectCenter, vec2(0.0, 0.0)));
    maxDist = max(maxDist, distance(aspectCenter, vec2(u_aspectRatio, 0.0)));
    maxDist = max(maxDist, distance(aspectCenter, vec2(0.0, 1.0)));
    maxDist = max(maxDist, distance(aspectCenter, vec2(u_aspectRatio, 1.0)));

    float radius = u_progress * (maxDist + 2.0 * mappedSmoothness) - mappedSmoothness;
    float factor = smoothstep(radius - mappedSmoothness, radius + mappedSmoothness, dist);
    gl_FragColor = mix(color2, color1, factor);
}
)";

  constexpr char kStripesFragment[] = R"(
uniform float u_smoothness;
uniform float u_aspectRatio;
uniform float u_stripeCount;
uniform float u_angle;

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2);

    float mappedSmoothness = mix(0.001, 0.3, u_smoothness * u_smoothness);

    float rad = radians(u_angle);
    float cosA = cos(rad);
    float sinA = sin(rad);
    vec2 aspectUV = vec2(uv.x * u_aspectRatio, uv.y);

    float stripeCoord = aspectUV.x * cosA + aspectUV.y * sinA;
    float perpCoord = -aspectUV.x * sinA + aspectUV.y * cosA;

    float totalStripes = u_stripeCount;
    float maxStripeCoord = u_aspectRatio * abs(cosA) + abs(sinA);
    float stripeWidth = maxStripeCoord / totalStripes;

    float stripePos = stripeCoord / stripeWidth;
    float stripeIndex = floor(stripePos);
    float isOdd = mod(stripeIndex, 2.0);

    float maxPerp = u_aspectRatio * abs(sinA) + abs(cosA);
    float normalizedPerp = perpCoord / maxPerp;

    float delay = abs(normalizedPerp) * 0.3;
    float localProgress = clamp((u_progress - delay) / (1.0 - delay), 0.0, 1.0);

    float edge;
    float localFrac = fract(stripePos);
    float factor;
    if (isOdd > 0.5) {
        edge = mix(1.0 + mappedSmoothness, -mappedSmoothness, localProgress);
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, localFrac);
        gl_FragColor = mix(color1, color2, factor);
    } else {
        edge = mix(-mappedSmoothness, 1.0 + mappedSmoothness, localProgress);
        factor = smoothstep(edge - mappedSmoothness, edge + mappedSmoothness, localFrac);
        gl_FragColor = mix(color2, color1, factor);
    }
}
)";

  constexpr char kZoomFragment[] = R"(
void main() {
    vec2 uv = v_texcoord;

    float zoom = 0.15;

    // Old image zooms in (grows toward viewer) while fading out
    float scale1 = 1.0 + zoom * u_progress;
    vec2 uv1 = (uv - 0.5) / scale1 + 0.5;

    // New image arrives slightly zoomed in, returns to normal scale while fading in
    float scale2 = 1.0 + zoom * (1.0 - u_progress);
    vec2 uv2 = (uv - 0.5) / scale2 + 0.5;

    vec4 color1 = sampleSource(u_source1, uv1, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1);
    vec4 color2 = sampleSource(u_source2, uv2, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2);

    gl_FragColor = mix(color1, color2, u_progress);
}
)";

  constexpr char kHoneycombFragment[] = R"(
uniform float u_cellSize;
uniform float u_centerX;
uniform float u_centerY;
uniform float u_aspectRatio;
uniform float u_smoothness;

vec2 hexRound(float q, float r) {
    float x = q;
    float z = r;
    float y = -x - z;

    float rx = floor(x + 0.5);
    float ry = floor(y + 0.5);
    float rz = floor(z + 0.5);

    float dx = abs(rx - x);
    float dy = abs(ry - y);
    float dz = abs(rz - z);

    if (dx > dy && dx > dz) {
        rx = -ry - rz;
    } else if (dy > dz) {
        ry = -rx - rz;
    } else {
        rz = -rx - ry;
    }

    return vec2(rx, rz);
}

void main() {
    vec2 uv = v_texcoord;
    vec4 color1 = sampleSource(u_source1, uv, u_imageWidth1, u_imageHeight1, u_sourceKind1, u_sourceColor1);
    vec4 color2 = sampleSource(u_source2, uv, u_imageWidth2, u_imageHeight2, u_sourceKind2, u_sourceColor2);

    float mappedSmoothness = mix(0.001, 0.5, u_smoothness * u_smoothness);

    float size = u_cellSize;
    vec2 aspectUV = vec2(uv.x * u_aspectRatio, uv.y);

    float q = (aspectUV.x * (2.0 / 3.0)) / size;
    float r = ((-aspectUV.x / 3.0) + (sqrt(3.0) / 3.0) * aspectUV.y) / size;

    vec2 hexCoord = hexRound(q, r);

    // Convert back to screen space for cell center
    float cellCenterX = (hexCoord.x * 1.5) * size;
    float cellCenterY = (hexCoord.x * sqrt(3.0) / 2.0 + hexCoord.y * sqrt(3.0)) * size;
    vec2 cellCenter = vec2(cellCenterX, cellCenterY);

    vec2 waveOrigin = vec2(u_centerX * u_aspectRatio, u_centerY);
    float dist = distance(cellCenter, waveOrigin);

    float maxDist = 0.0;
    maxDist = max(maxDist, distance(waveOrigin, vec2(0.0, 0.0)));
    maxDist = max(maxDist, distance(waveOrigin, vec2(u_aspectRatio, 0.0)));
    maxDist = max(maxDist, distance(waveOrigin, vec2(0.0, 1.0)));
    maxDist = max(maxDist, distance(waveOrigin, vec2(u_aspectRatio, 1.0)));

    float radius = u_progress * (maxDist + 2.0 * mappedSmoothness) - mappedSmoothness;
    float factor = smoothstep(radius - mappedSmoothness, radius + mappedSmoothness, dist);
    gl_FragColor = mix(color2, color1, factor);
}
)";

} // namespace

void WallpaperProgram::ensureInitialized() {
  if (m_programs[0].program.isValid()) {
    return;
  }

  initProgram(static_cast<std::size_t>(WallpaperTransition::Fade), kFadeFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Wipe), kWipeFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Disc), kDiscFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Stripes), kStripesFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Zoom), kZoomFragment);
  initProgram(static_cast<std::size_t>(WallpaperTransition::Honeycomb), kHoneycombFragment);
}

void WallpaperProgram::destroy() {
  for (auto& pd : m_programs) {
    pd.program.destroy();
  }
}

void WallpaperProgram::initProgram(std::size_t index, const char* fragSource) {
  std::string fullFrag = std::string(kCommonFunctions) + fragSource;

  auto& pd = m_programs[index];
  pd.program.create(kVertexShader, fullFrag.c_str());

  auto id = pd.program.id();
  pd.positionLoc = glGetAttribLocation(id, "a_position");
  pd.surfaceSizeLoc = glGetUniformLocation(id, "u_surface_size");
  pd.quadSizeLoc = glGetUniformLocation(id, "u_quad_size");
  pd.transformLoc = glGetUniformLocation(id, "u_transform");
  pd.source1Loc = glGetUniformLocation(id, "u_source1");
  pd.source2Loc = glGetUniformLocation(id, "u_source2");
  pd.sourceKind1Loc = glGetUniformLocation(id, "u_sourceKind1");
  pd.sourceKind2Loc = glGetUniformLocation(id, "u_sourceKind2");
  pd.sourceColor1Loc = glGetUniformLocation(id, "u_sourceColor1");
  pd.sourceColor2Loc = glGetUniformLocation(id, "u_sourceColor2");
  pd.progressLoc = glGetUniformLocation(id, "u_progress");
  pd.fillModeLoc = glGetUniformLocation(id, "u_fillMode");
  pd.imageWidth1Loc = glGetUniformLocation(id, "u_imageWidth1");
  pd.imageHeight1Loc = glGetUniformLocation(id, "u_imageHeight1");
  pd.imageWidth2Loc = glGetUniformLocation(id, "u_imageWidth2");
  pd.imageHeight2Loc = glGetUniformLocation(id, "u_imageHeight2");
  pd.screenWidthLoc = glGetUniformLocation(id, "u_screenWidth");
  pd.screenHeightLoc = glGetUniformLocation(id, "u_screenHeight");
  pd.fillColorLoc = glGetUniformLocation(id, "u_fillColor");

  // Per-transition (may be -1 if not used by this shader)
  pd.directionLoc = glGetUniformLocation(id, "u_direction");
  pd.smoothnessLoc = glGetUniformLocation(id, "u_smoothness");
  pd.centerXLoc = glGetUniformLocation(id, "u_centerX");
  pd.centerYLoc = glGetUniformLocation(id, "u_centerY");
  pd.aspectRatioLoc = glGetUniformLocation(id, "u_aspectRatio");
  pd.stripeCountLoc = glGetUniformLocation(id, "u_stripeCount");
  pd.angleLoc = glGetUniformLocation(id, "u_angle");
  pd.maxBlockSizeLoc = glGetUniformLocation(id, "u_maxBlockSize");
  pd.cellSizeLoc = glGetUniformLocation(id, "u_cellSize");

  if (pd.positionLoc < 0 || pd.surfaceSizeLoc < 0 || pd.quadSizeLoc < 0 || pd.transformLoc < 0 || pd.source1Loc < 0 ||
      pd.progressLoc < 0) {
    throw std::runtime_error("failed to query wallpaper shader locations");
  }
}

void WallpaperProgram::draw(
    WallpaperTransition type, WallpaperSourceKind sourceKind1, TextureId texture1, const Color& sourceColor1,
    WallpaperSourceKind sourceKind2, TextureId texture2, const Color& sourceColor2, float surfaceWidth,
    float surfaceHeight, float quadWidth, float quadHeight, float imageWidth1, float imageHeight1, float imageWidth2,
    float imageHeight2, float progress, float fillMode, const TransitionParams& params, const Color& fillColor,
    const Mat3& transform
) const {
  auto idx = static_cast<std::size_t>(type);
  if (idx >= kTransitionCount || !m_programs[idx].program.isValid() || quadWidth <= 0.0f || quadHeight <= 0.0f) {
    return;
  }
  if (sourceKind1 == WallpaperSourceKind::Image && texture1 == 0) {
    return;
  }

  const auto& pd = m_programs[idx];

  static constexpr float kQuad[] = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  glUseProgram(pd.program.id());
  glUniform2f(pd.surfaceSizeLoc, surfaceWidth, surfaceHeight);
  glUniform2f(pd.quadSizeLoc, quadWidth, quadHeight);
  glUniformMatrix3fv(pd.transformLoc, 1, GL_FALSE, transform.m.data());

  // Textures
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture1.value()));
  glUniform1i(pd.source1Loc, 0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2.value()));
  if (pd.source2Loc >= 0) {
    glUniform1i(pd.source2Loc, 1);
  }

  // Common uniforms
  if (pd.sourceKind1Loc >= 0)
    glUniform1f(pd.sourceKind1Loc, sourceKind1 == WallpaperSourceKind::Color ? 1.0f : 0.0f);
  if (pd.sourceKind2Loc >= 0)
    glUniform1f(pd.sourceKind2Loc, sourceKind2 == WallpaperSourceKind::Color ? 1.0f : 0.0f);
  if (pd.sourceColor1Loc >= 0)
    glUniform4f(pd.sourceColor1Loc, sourceColor1.r, sourceColor1.g, sourceColor1.b, sourceColor1.a);
  if (pd.sourceColor2Loc >= 0)
    glUniform4f(pd.sourceColor2Loc, sourceColor2.r, sourceColor2.g, sourceColor2.b, sourceColor2.a);
  glUniform1f(pd.progressLoc, progress);
  if (pd.fillModeLoc >= 0)
    glUniform1f(pd.fillModeLoc, fillMode);
  if (pd.imageWidth1Loc >= 0)
    glUniform1f(pd.imageWidth1Loc, imageWidth1);
  if (pd.imageHeight1Loc >= 0)
    glUniform1f(pd.imageHeight1Loc, imageHeight1);
  if (pd.imageWidth2Loc >= 0)
    glUniform1f(pd.imageWidth2Loc, imageWidth2);
  if (pd.imageHeight2Loc >= 0)
    glUniform1f(pd.imageHeight2Loc, imageHeight2);
  if (pd.screenWidthLoc >= 0)
    glUniform1f(pd.screenWidthLoc, quadWidth);
  if (pd.screenHeightLoc >= 0)
    glUniform1f(pd.screenHeightLoc, quadHeight);
  if (pd.fillColorLoc >= 0)
    glUniform4f(pd.fillColorLoc, fillColor.r, fillColor.g, fillColor.b, fillColor.a);

  // Per-transition uniforms
  if (pd.directionLoc >= 0)
    glUniform1f(pd.directionLoc, params.direction);
  if (pd.smoothnessLoc >= 0)
    glUniform1f(pd.smoothnessLoc, params.smoothness);
  if (pd.centerXLoc >= 0)
    glUniform1f(pd.centerXLoc, params.centerX);
  if (pd.centerYLoc >= 0)
    glUniform1f(pd.centerYLoc, params.centerY);
  if (pd.aspectRatioLoc >= 0)
    glUniform1f(pd.aspectRatioLoc, params.aspectRatio);
  if (pd.stripeCountLoc >= 0)
    glUniform1f(pd.stripeCountLoc, params.stripeCount);
  if (pd.angleLoc >= 0)
    glUniform1f(pd.angleLoc, params.angle);
  if (pd.maxBlockSizeLoc >= 0)
    glUniform1f(pd.maxBlockSizeLoc, params.maxBlockSize);
  if (pd.cellSizeLoc >= 0)
    glUniform1f(pd.cellSizeLoc, params.cellSize);

  // Draw fullscreen quad
  auto posAttr = static_cast<GLuint>(pd.positionLoc);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);

  glActiveTexture(GL_TEXTURE0);
}
