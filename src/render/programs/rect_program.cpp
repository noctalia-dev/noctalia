#include "render/programs/rect_program.h"

#include "render/core/render_styles.h"

#include <array>
#include <stdexcept>

namespace {

  constexpr char kVertexShaderSource[] = R"(
precision highp float;

attribute vec2 a_position;
uniform vec2 u_surface_size;
uniform vec2 u_quad_size;
uniform vec2 u_rect_origin;
uniform vec2 u_rect_size;
uniform mat3 u_transform;
varying vec2 v_pixel;

vec2 to_ndc(vec2 pixel_pos) {
    vec2 normalized = pixel_pos / u_surface_size;
    return vec2(normalized.x * 2.0 - 1.0, 1.0 - normalized.y * 2.0);
}

void main() {
    vec2 local = a_position * u_quad_size;
    vec3 pixel = u_transform * vec3(local, 1.0);
    v_pixel = local - u_rect_origin;
    gl_Position = vec4(to_ndc(pixel.xy), 0.0, 1.0);
}
)";

  constexpr char kFragmentShaderSource[] = R"(
precision highp float;

uniform vec2 u_rect_size;
uniform vec4 u_color;
uniform vec4 u_border_color;
uniform int u_fill_mode;
uniform vec2 u_gradient_direction;
uniform vec4 u_gradient_stops;
uniform vec4 u_gradient_color0;
uniform vec4 u_gradient_color1;
uniform vec4 u_gradient_color2;
uniform vec4 u_gradient_color3;
uniform vec4 u_corner_shapes; // tl, tr, br, bl: 0 = convex, 1 = concave
uniform vec4 u_logical_inset; // left, top, right, bottom
uniform vec4 u_radii;  // tl, tr, br, bl
uniform float u_softness;
uniform int u_no_aa;
uniform int u_invert_fill;
uniform float u_border_width;
uniform int u_outer_shadow;
uniform vec2 u_shadow_cutout_offset;
uniform int u_shadow_exclusion;
uniform vec2 u_shadow_exclusion_offset;
uniform vec2 u_shadow_exclusion_size;
uniform vec4 u_shadow_exclusion_corner_shapes;
uniform vec4 u_shadow_exclusion_logical_inset;
uniform vec4 u_shadow_exclusion_radii;
varying vec2 v_pixel;

// Returns (signed distance, is_corner). is_corner = 1.0 when the fragment lies in
// the curved-corner quadrant of the active radius (both q components positive).
vec2 rounded_rect_distance_with_corner(vec2 point, vec2 size, vec4 radii) {
    vec2 half_size = size * 0.5;
    vec2 centered = point - half_size;
    float r = centered.x < 0.0
        ? (centered.y < 0.0 ? radii.x : radii.w)
        : (centered.y < 0.0 ? radii.y : radii.z);
    vec2 q = abs(centered) - (half_size - vec2(r));
    float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float is_corner = (r > 0.0 && q.x > 0.0 && q.y > 0.0) ? 1.0 : 0.0;
    return vec2(distance, is_corner);
}

float rounded_rect_distance(vec2 point, vec2 size, vec4 radii) {
    return rounded_rect_distance_with_corner(point, size, radii).x;
}

float circle_extent(float radius, float delta) {
    return sqrt(max(0.0, radius * radius - delta * delta));
}

// Returns (signed distance, is_corner). is_corner = 1.0 when the fragment is
// inside any active corner's r×r axis-aligned band (curved region — concave or
// convex). Fragments in straight-edge bands return 0.0.
vec2 shape_distance_with_corner(vec2 point, vec2 size, vec4 radii, vec4 corner_shapes, vec4 logical_inset) {
    vec4 safe_inset = max(logical_inset, vec4(0.0));
    vec2 body_min = min(safe_inset.xy, size);
    vec2 body_max = max(body_min, size - safe_inset.zw);
    vec2 body_size = max(body_max - body_min, vec2(0.0));
    float max_radius = max(min(body_size.x, body_size.y) * 0.5, 0.0);
    vec4 r = clamp(radii, vec4(0.0), vec4(max_radius));

    bool tl_concave = corner_shapes.x > 0.5;
    bool tr_concave = corner_shapes.y > 0.5;
    bool br_concave = corner_shapes.z > 0.5;
    bool bl_concave = corner_shapes.w > 0.5;
    bool any_concave = tl_concave || tr_concave || br_concave || bl_concave;

    if (!any_concave) {
        return rounded_rect_distance_with_corner(point - body_min, body_size, r);
    }

    bool in_corner_box = false;
    if (r.x > 0.0 && point.x < body_min.x + r.x && point.y < body_min.y + r.x) in_corner_box = true;
    if (r.y > 0.0 && point.x > body_max.x - r.y && point.y < body_min.y + r.y) in_corner_box = true;
    if (r.z > 0.0 && point.x > body_max.x - r.z && point.y > body_max.y - r.z) in_corner_box = true;
    if (r.w > 0.0 && point.x < body_min.x + r.w && point.y > body_max.y - r.w) in_corner_box = true;

    float x = point.x;
    float y = point.y;
    float left = body_min.x;
    float right = body_max.x;
    float top = body_min.y;
    float bottom = body_max.y;

    float radius = r.x;
    if (radius > 0.0 && y < body_min.y + radius) {
        float sample_y = clamp(y, body_min.y, body_min.y + radius);
        float dy = sample_y - (body_min.y + radius);
        float extent = circle_extent(radius, dy);
        if (tl_concave) {
            left = min(left, body_min.x - radius + extent);
        } else {
            left = max(left, body_min.x + radius - extent);
        }
    }
    if (radius > 0.0 && x < body_min.x + radius) {
        float sample_x = clamp(x, body_min.x, body_min.x + radius);
        float dx = sample_x - (body_min.x + radius);
        float extent = circle_extent(radius, dx);
        if (tl_concave) {
            top = min(top, body_min.y - radius + extent);
        } else {
            top = max(top, body_min.y + radius - extent);
        }
    }

    radius = r.y;
    if (radius > 0.0 && y < body_min.y + radius) {
        float sample_y = clamp(y, body_min.y, body_min.y + radius);
        float dy = sample_y - (body_min.y + radius);
        float extent = circle_extent(radius, dy);
        if (tr_concave) {
            right = max(right, body_max.x + radius - extent);
        } else {
            right = min(right, body_max.x - radius + extent);
        }
    }
    if (radius > 0.0 && x > body_max.x - radius) {
        float sample_x = clamp(x, body_max.x - radius, body_max.x);
        float dx = sample_x - (body_max.x - radius);
        float extent = circle_extent(radius, dx);
        if (tr_concave) {
            top = min(top, body_min.y - radius + extent);
        } else {
            top = max(top, body_min.y + radius - extent);
        }
    }

    radius = r.z;
    if (radius > 0.0 && y > body_max.y - radius) {
        float sample_y = clamp(y, body_max.y - radius, body_max.y);
        float dy = sample_y - (body_max.y - radius);
        float extent = circle_extent(radius, dy);
        if (br_concave) {
            right = max(right, body_max.x + radius - extent);
        } else {
            right = min(right, body_max.x - radius + extent);
        }
    }
    if (radius > 0.0 && x > body_max.x - radius) {
        float sample_x = clamp(x, body_max.x - radius, body_max.x);
        float dx = sample_x - (body_max.x - radius);
        float extent = circle_extent(radius, dx);
        if (br_concave) {
            bottom = max(bottom, body_max.y + radius - extent);
        } else {
            bottom = min(bottom, body_max.y - radius + extent);
        }
    }

    radius = r.w;
    if (radius > 0.0 && y > body_max.y - radius) {
        float sample_y = clamp(y, body_max.y - radius, body_max.y);
        float dy = sample_y - (body_max.y - radius);
        float extent = circle_extent(radius, dy);
        if (bl_concave) {
            left = min(left, body_min.x - radius + extent);
        } else {
            left = max(left, body_min.x + radius - extent);
        }
    }
    if (radius > 0.0 && x < body_min.x + radius) {
        float sample_x = clamp(x, body_min.x, body_min.x + radius);
        float dx = sample_x - (body_min.x + radius);
        float extent = circle_extent(radius, dx);
        if (bl_concave) {
            bottom = max(bottom, body_max.y + radius - extent);
        } else {
            bottom = min(bottom, body_max.y - radius + extent);
        }
    }

    float boundary_distance = max(max(left - x, x - right), max(top - y, y - bottom));
    float visual_clip = max(max(-point.x, point.x - size.x), max(-point.y, point.y - size.y));
    // Curve AA only when the curved boundary actually dominates. Where visual_clip wins
    // (e.g. concave wing along the visual-rect top edge) the boundary is axis-aligned —
    // snap with the straight-edge window instead.
    float is_corner = (in_corner_box && boundary_distance > visual_clip) ? 1.0 : 0.0;
    return vec2(max(boundary_distance, visual_clip), is_corner);
}

float shape_distance(vec2 point, vec2 size, vec4 radii, vec4 corner_shapes, vec4 logical_inset) {
    return shape_distance_with_corner(point, size, radii, corner_shapes, logical_inset).x;
}

float shadow_shape_distance(vec2 point, vec2 size, vec4 radii, vec4 corner_shapes, vec4 logical_inset) {
    float distance = shape_distance(point, size, radii, corner_shapes, logical_inset);

    vec4 safe_inset = max(logical_inset, vec4(0.0));
    vec2 body_min = min(safe_inset.xy, size);
    vec2 body_max = max(body_min, size - safe_inset.zw);
    vec2 body_size = max(body_max - body_min, vec2(0.0));
    float max_radius = max(min(body_size.x, body_size.y) * 0.5, 0.0);
    vec4 r = clamp(radii, vec4(0.0), vec4(max_radius));

    bool tl_concave = corner_shapes.x > 0.5;
    bool tr_concave = corner_shapes.y > 0.5;
    bool br_concave = corner_shapes.z > 0.5;
    bool bl_concave = corner_shapes.w > 0.5;
    bool any_concave = tl_concave || tr_concave || br_concave || bl_concave;
    if (!any_concave) {
        return distance;
    }

    float x = point.x;
    float y = point.y;
    float left_distance = body_min.x - x;
    float right_distance = x - body_max.x;
    float top_distance = body_min.y - y;
    float bottom_distance = y - body_max.y;

    float radius = r.x;
    if (!tl_concave && radius > 0.0 && x < body_min.x + radius && y < body_min.y + radius) {
        float corner_distance = length(point - vec2(body_min.x + radius, body_min.y + radius)) - radius;
        distance = max(max(right_distance, bottom_distance), corner_distance);
    }

    radius = r.y;
    if (!tr_concave && radius > 0.0 && x > body_max.x - radius && y < body_min.y + radius) {
        float corner_distance = length(point - vec2(body_max.x - radius, body_min.y + radius)) - radius;
        distance = max(max(left_distance, bottom_distance), corner_distance);
    }

    radius = r.z;
    if (!br_concave && radius > 0.0 && x > body_max.x - radius && y > body_max.y - radius) {
        float corner_distance = length(point - vec2(body_max.x - radius, body_max.y - radius)) - radius;
        distance = max(max(left_distance, top_distance), corner_distance);
    }

    radius = r.w;
    if (!bl_concave && radius > 0.0 && x < body_min.x + radius && y > body_max.y - radius) {
        float corner_distance = length(point - vec2(body_min.x + radius, body_max.y - radius)) - radius;
        distance = max(max(right_distance, top_distance), corner_distance);
    }

    float visual_clip = max(max(-point.x, point.x - size.x), max(-point.y, point.y - size.y));
    return max(distance, visual_clip);
}

// Pixel-grid-snap window for axis-aligned edges: half-coverage falls exactly on
// the boundary so an integer-aligned edge produces 100% on the inside pixel and
// 0% on the outside pixel, with no semi-transparent leakage.
float coverage_for(vec2 distance_with_corner, float aa_curve) {
    if (u_no_aa == 1) {
        return 1.0 - step(0.0, distance_with_corner.x);
    }
    float lo = mix(-0.5, -aa_curve, distance_with_corner.y);
    float hi = mix( 0.5,  aa_curve, distance_with_corner.y);
    return 1.0 - smoothstep(lo, hi, distance_with_corner.x);
}

float gradient_segment_t(float position, float start, float end) {
    return clamp((position - start) / max(end - start, 0.0001), 0.0, 1.0);
}

vec4 gradient_fill(float position) {
    vec4 stops = clamp(u_gradient_stops, vec4(0.0), vec4(1.0));
    stops.y = max(stops.y, stops.x);
    stops.z = max(stops.z, stops.y);
    stops.w = max(stops.w, stops.z);

    vec4 c0 = u_gradient_color0;
    vec4 c1 = u_gradient_color1;
    vec4 c2 = u_gradient_color2;
    vec4 c3 = u_gradient_color3;

    if (position <= stops.y) {
        return mix(c0, c1, gradient_segment_t(position, stops.x, stops.y));
    }
    if (position <= stops.z) {
        return mix(c1, c2, gradient_segment_t(position, stops.y, stops.z));
    }
    return mix(c2, c3, gradient_segment_t(position, stops.z, stops.w));
}

void main() {
    float aa = max(u_softness, 0.85);
    vec2 local_point = v_pixel;
    vec2 uv = clamp(local_point / u_rect_size, vec2(0.0), vec2(1.0));

    vec2 outer = shape_distance_with_corner(local_point, u_rect_size, u_radii, u_corner_shapes, u_logical_inset);
    float outer_distance = outer.x;
    float outer_coverage = coverage_for(outer, aa);
    if (u_invert_fill == 1) outer_coverage = 1.0 - outer_coverage;

    if (u_outer_shadow == 1) {
        float cutout_aa = 0.85;
        float shadow_distance = shadow_shape_distance(local_point, u_rect_size, u_radii, u_corner_shapes, u_logical_inset);
        float shadow_outer_coverage = 1.0 - smoothstep(-aa, aa, shadow_distance);
        float cutout_distance = shape_distance(local_point + u_shadow_cutout_offset, u_rect_size, u_radii, u_corner_shapes, u_logical_inset);
        float cutout_mask = 1.0 - smoothstep(-cutout_aa, cutout_aa, cutout_distance);
        float shadow_coverage = shadow_outer_coverage * (1.0 - cutout_mask);
        if (u_shadow_exclusion == 1 && u_shadow_exclusion_size.x > 0.0 && u_shadow_exclusion_size.y > 0.0) {
            float exclusion_distance = shape_distance(local_point + u_shadow_exclusion_offset, u_shadow_exclusion_size, u_shadow_exclusion_radii, u_shadow_exclusion_corner_shapes, u_shadow_exclusion_logical_inset);
            float exclusion_mask = 1.0 - smoothstep(-cutout_aa, cutout_aa, exclusion_distance);
            shadow_coverage *= 1.0 - exclusion_mask;
        }
        float out_alpha = u_color.a * shadow_coverage;
        if (out_alpha <= 0.0) {
            discard;
        }
        gl_FragColor = vec4(u_color.rgb * out_alpha, out_alpha);
        return;
    }

    float gradient_pos = clamp(dot(uv, u_gradient_direction), 0.0, 1.0);
    vec4 fill_base;
    if (u_fill_mode == 0) {
        fill_base = vec4(0.0);
    } else if (u_fill_mode == 1) {
        fill_base = u_color;
    } else {
        fill_base = gradient_fill(gradient_pos);
    }

    if (u_border_width <= 0.0 || u_border_color.a <= 0.0) {
        float out_alpha = fill_base.a * outer_coverage;
        if (out_alpha <= 0.0) {
            discard;
        }
        gl_FragColor = vec4(fill_base.rgb * out_alpha, out_alpha);
        return;
    }

    bool any_concave = u_corner_shapes.x > 0.5 || u_corner_shapes.y > 0.5 || u_corner_shapes.z > 0.5 || u_corner_shapes.w > 0.5;
    vec2 inner;
    if (any_concave) {
        inner = vec2(outer_distance + u_border_width, outer.y);
    } else {
        vec4 inner_radii = max(u_radii - vec4(u_border_width), vec4(0.0));
        vec2 inner_size = max(u_rect_size - vec2(u_border_width * 2.0), vec2(0.0));
        vec2 inner_point = local_point - vec2(u_border_width);
        vec4 inner_inset = max(u_logical_inset - vec4(u_border_width), vec4(0.0));
        inner = shape_distance_with_corner(inner_point, inner_size, inner_radii, u_corner_shapes, inner_inset);
    }
    float inner_coverage = coverage_for(inner, aa);

    if (fill_base.a <= 0.0) {
        float ring_coverage = outer_coverage * (1.0 - inner_coverage);
        float out_alpha = u_border_color.a * ring_coverage;
        if (out_alpha <= 0.0) {
            discard;
        }
        gl_FragColor = vec4(u_border_color.rgb * out_alpha, out_alpha);
        return;
    }

    // Fill and border occupy disjoint regions: the fill lives where
    // inner_coverage == 1, the border ring lives where inner_coverage == 0.
    // Mix between them so a translucent fill never sits on top of a
    // full-area border backplane (which would mask its opacity).
    vec3 border_pm = u_border_color.rgb * u_border_color.a;
    vec3 fill_pm = fill_base.rgb * fill_base.a;

    vec3 interior_rgb = mix(border_pm, fill_pm, inner_coverage);
    float interior_a = mix(u_border_color.a, fill_base.a, inner_coverage);

    // Apply outer shape mask
    float out_alpha = interior_a * outer_coverage;
    if (out_alpha <= 0.0) {
        discard;
    }

    // Output premultiplied alpha
    gl_FragColor = vec4(interior_rgb * outer_coverage, out_alpha);
}
)";

} // namespace

void RectProgram::ensureInitialized() {
  if (m_program.isValid()) {
    return;
  }

  m_program.create(kVertexShaderSource, kFragmentShaderSource);
  m_positionLocation = glGetAttribLocation(m_program.id(), "a_position");
  m_surfaceSizeLocation = glGetUniformLocation(m_program.id(), "u_surface_size");
  m_quadSizeLocation = glGetUniformLocation(m_program.id(), "u_quad_size");
  m_rectOriginLocation = glGetUniformLocation(m_program.id(), "u_rect_origin");
  m_rectSizeLocation = glGetUniformLocation(m_program.id(), "u_rect_size");
  m_colorLocation = glGetUniformLocation(m_program.id(), "u_color");
  m_borderColorLocation = glGetUniformLocation(m_program.id(), "u_border_color");
  m_fillModeLocation = glGetUniformLocation(m_program.id(), "u_fill_mode");
  m_gradientDirectionLocation = glGetUniformLocation(m_program.id(), "u_gradient_direction");
  m_gradientStopsLocation = glGetUniformLocation(m_program.id(), "u_gradient_stops");
  m_gradientColor0Location = glGetUniformLocation(m_program.id(), "u_gradient_color0");
  m_gradientColor1Location = glGetUniformLocation(m_program.id(), "u_gradient_color1");
  m_gradientColor2Location = glGetUniformLocation(m_program.id(), "u_gradient_color2");
  m_gradientColor3Location = glGetUniformLocation(m_program.id(), "u_gradient_color3");
  m_cornerShapesLocation = glGetUniformLocation(m_program.id(), "u_corner_shapes");
  m_logicalInsetLocation = glGetUniformLocation(m_program.id(), "u_logical_inset");
  m_radiiLocation = glGetUniformLocation(m_program.id(), "u_radii");
  m_softnessLocation = glGetUniformLocation(m_program.id(), "u_softness");
  m_noAaLocation = glGetUniformLocation(m_program.id(), "u_no_aa");
  m_invertFillLocation = glGetUniformLocation(m_program.id(), "u_invert_fill");
  m_borderWidthLocation = glGetUniformLocation(m_program.id(), "u_border_width");
  m_outerShadowLocation = glGetUniformLocation(m_program.id(), "u_outer_shadow");
  m_shadowCutoutOffsetLocation = glGetUniformLocation(m_program.id(), "u_shadow_cutout_offset");
  m_shadowExclusionLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion");
  m_shadowExclusionOffsetLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_offset");
  m_shadowExclusionSizeLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_size");
  m_shadowExclusionCornerShapesLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_corner_shapes");
  m_shadowExclusionLogicalInsetLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_logical_inset");
  m_shadowExclusionRadiiLocation = glGetUniformLocation(m_program.id(), "u_shadow_exclusion_radii");
  m_transformLocation = glGetUniformLocation(m_program.id(), "u_transform");

  if (m_positionLocation < 0
      || m_surfaceSizeLocation < 0
      || m_quadSizeLocation < 0
      || m_rectOriginLocation < 0
      || m_rectSizeLocation < 0
      || m_colorLocation < 0
      || m_borderColorLocation < 0
      || m_fillModeLocation < 0
      || m_gradientDirectionLocation < 0
      || m_radiiLocation < 0
      || m_softnessLocation < 0
      || m_gradientStopsLocation < 0
      || m_gradientColor0Location < 0
      || m_gradientColor1Location < 0
      || m_gradientColor2Location < 0
      || m_gradientColor3Location < 0
      || m_invertFillLocation < 0
      || m_noAaLocation < 0
      || m_cornerShapesLocation < 0
      || m_logicalInsetLocation < 0
      || m_borderWidthLocation < 0
      || m_outerShadowLocation < 0
      || m_shadowCutoutOffsetLocation < 0
      || m_shadowExclusionLocation < 0
      || m_shadowExclusionOffsetLocation < 0
      || m_shadowExclusionSizeLocation < 0
      || m_shadowExclusionCornerShapesLocation < 0
      || m_shadowExclusionLogicalInsetLocation < 0
      || m_shadowExclusionRadiiLocation < 0
      || m_transformLocation < 0) {
    throw std::runtime_error("failed to query rounded-rect shader locations");
  }
}

void RectProgram::destroy() {
  m_program.destroy();
  m_positionLocation = -1;
  m_surfaceSizeLocation = -1;
  m_quadSizeLocation = -1;
  m_rectOriginLocation = -1;
  m_rectSizeLocation = -1;
  m_colorLocation = -1;
  m_borderColorLocation = -1;
  m_fillModeLocation = -1;
  m_gradientDirectionLocation = -1;
  m_gradientStopsLocation = -1;
  m_gradientColor0Location = -1;
  m_gradientColor1Location = -1;
  m_gradientColor2Location = -1;
  m_gradientColor3Location = -1;
  m_cornerShapesLocation = -1;
  m_logicalInsetLocation = -1;
  m_radiiLocation = -1;
  m_softnessLocation = -1;
  m_noAaLocation = -1;
  m_invertFillLocation = -1;
  m_borderWidthLocation = -1;
  m_outerShadowLocation = -1;
  m_shadowCutoutOffsetLocation = -1;
  m_shadowExclusionLocation = -1;
  m_shadowExclusionOffsetLocation = -1;
  m_shadowExclusionSizeLocation = -1;
  m_shadowExclusionCornerShapesLocation = -1;
  m_shadowExclusionLogicalInsetLocation = -1;
  m_shadowExclusionRadiiLocation = -1;
  m_transformLocation = -1;
}

void RectProgram::abandon() noexcept { m_program.abandon(); }

void RectProgram::draw(
    float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
    const Mat3& transform
) const {
  if (!m_program.isValid() || width <= 0.0f || height <= 0.0f) {
    return;
  }

  const std::array<GLfloat, 12> vertices = {
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
  };

  const float padding = std::max(style.borderWidth + style.softness + 2.0f, 2.0f);
  const float quadWidth = width + padding * 2.0f;
  const float quadHeight = height + padding * 2.0f;
  const float rectOrigin = padding;
  const Mat3 quadTransform = transform * Mat3::translation(-padding, -padding);

  glUseProgram(m_program.id());
  glUniform2f(m_surfaceSizeLocation, surfaceWidth, surfaceHeight);
  glUniform2f(m_quadSizeLocation, quadWidth, quadHeight);
  glUniform2f(m_rectOriginLocation, rectOrigin, rectOrigin);
  glUniform2f(m_rectSizeLocation, width, height);
  glUniform4f(m_colorLocation, style.fill.r, style.fill.g, style.fill.b, style.fill.a);
  glUniform4f(m_borderColorLocation, style.border.r, style.border.g, style.border.b, style.border.a);
  int fillMode = 0;
  if (style.fillMode == FillMode::Solid) {
    fillMode = 1;
  } else if (style.fillMode == FillMode::LinearGradient) {
    fillMode = 2;
  }
  glUniform1i(m_fillModeLocation, fillMode);
  glUniform2f(
      m_gradientDirectionLocation, style.gradientDirection == GradientDirection::Horizontal ? 1.0f : 0.0f,
      style.gradientDirection == GradientDirection::Vertical ? 1.0f : 0.0f
  );
  const auto& stop0 = style.gradientStops[0];
  const auto& stop1 = style.gradientStops[1];
  const auto& stop2 = style.gradientStops[2];
  const auto& stop3 = style.gradientStops[3];
  glUniform4f(m_gradientStopsLocation, stop0.position, stop1.position, stop2.position, stop3.position);
  glUniform4f(m_gradientColor0Location, stop0.color.r, stop0.color.g, stop0.color.b, stop0.color.a);
  glUniform4f(m_gradientColor1Location, stop1.color.r, stop1.color.g, stop1.color.b, stop1.color.a);
  glUniform4f(m_gradientColor2Location, stop2.color.r, stop2.color.g, stop2.color.b, stop2.color.a);
  glUniform4f(m_gradientColor3Location, stop3.color.r, stop3.color.g, stop3.color.b, stop3.color.a);
  const auto cornerShapeValue = [](CornerShape shape) { return shape == CornerShape::Concave ? 1.0f : 0.0f; };
  glUniform4f(
      m_cornerShapesLocation, cornerShapeValue(style.corners.tl), cornerShapeValue(style.corners.tr),
      cornerShapeValue(style.corners.br), cornerShapeValue(style.corners.bl)
  );
  glUniform4f(
      m_logicalInsetLocation, style.logicalInset.left, style.logicalInset.top, style.logicalInset.right,
      style.logicalInset.bottom
  );
  glUniform4f(m_radiiLocation, style.radius.tl, style.radius.tr, style.radius.br, style.radius.bl);
  glUniform1f(m_softnessLocation, style.softness);
  glUniform1i(m_noAaLocation, style.noAa ? 1 : 0);
  glUniform1i(m_invertFillLocation, style.invertFill ? 1 : 0);
  glUniform1f(m_borderWidthLocation, style.borderWidth);
  glUniform1i(m_outerShadowLocation, style.outerShadow ? 1 : 0);
  glUniform2f(m_shadowCutoutOffsetLocation, style.shadowCutoutOffsetX, style.shadowCutoutOffsetY);
  glUniform1i(m_shadowExclusionLocation, style.shadowExclusion ? 1 : 0);
  glUniform2f(m_shadowExclusionOffsetLocation, style.shadowExclusionOffsetX, style.shadowExclusionOffsetY);
  glUniform2f(m_shadowExclusionSizeLocation, style.shadowExclusionWidth, style.shadowExclusionHeight);
  glUniform4f(
      m_shadowExclusionCornerShapesLocation, cornerShapeValue(style.shadowExclusionCorners.tl),
      cornerShapeValue(style.shadowExclusionCorners.tr), cornerShapeValue(style.shadowExclusionCorners.br),
      cornerShapeValue(style.shadowExclusionCorners.bl)
  );
  glUniform4f(
      m_shadowExclusionLogicalInsetLocation, style.shadowExclusionLogicalInset.left,
      style.shadowExclusionLogicalInset.top, style.shadowExclusionLogicalInset.right,
      style.shadowExclusionLogicalInset.bottom
  );
  glUniform4f(
      m_shadowExclusionRadiiLocation, style.shadowExclusionRadius.tl, style.shadowExclusionRadius.tr,
      style.shadowExclusionRadius.br, style.shadowExclusionRadius.bl
  );
  glUniformMatrix3fv(m_transformLocation, 1, GL_FALSE, quadTransform.m.data());
  const auto posAttr = static_cast<GLuint>(m_positionLocation);
  glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
  glEnableVertexAttribArray(posAttr);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glDisableVertexAttribArray(posAttr);
}
