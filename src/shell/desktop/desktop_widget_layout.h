#pragma once

#include "shell/desktop/desktop_widgets_controller.h"
#include "shell/desktop/widget_transform.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <string>

namespace desktop_widgets {

  inline float widgetContentScale(float baseUiScale, const DesktopWidgetState& state) {
    return std::max(0.01f, baseUiScale) * std::max(0.01f, state.scale);
  }

  inline std::string outputKey(const WaylandOutput& output) {
    if (!output.connectorName.empty()) {
      return output.connectorName;
    }
    return std::to_string(output.name);
  }

  inline const WaylandOutput* findOutputByKey(const WaylandConnection& wayland, const std::string& key) {
    if (key.empty()) {
      return nullptr;
    }
    for (const auto& output : wayland.outputs()) {
      if (!output.done || output.output == nullptr) {
        continue;
      }
      if (outputKey(output) == key) {
        return &output;
      }
    }
    return nullptr;
  }

  inline const WaylandOutput*
  resolveEffectiveOutput(const WaylandConnection& wayland, const std::string& requestedOutput) {
    const auto& outputs = wayland.outputs();
    const WaylandOutput* primary = nullptr;
    for (const auto& output : outputs) {
      if (!output.done || output.output == nullptr) {
        continue;
      }
      if (primary == nullptr) {
        primary = &output;
      }
      if (!requestedOutput.empty() && outputKey(output) == requestedOutput) {
        return &output;
      }
    }
    return primary;
  }

  inline float outputLogicalWidth(const WaylandOutput& output) {
    if (output.logicalWidth > 0) {
      return static_cast<float>(output.logicalWidth);
    }
    return static_cast<float>(std::max(1, output.width / std::max(1, output.scale)));
  }

  inline float outputLogicalHeight(const WaylandOutput& output) {
    if (output.logicalHeight > 0) {
      return static_cast<float>(output.logicalHeight);
    }
    return static_cast<float>(std::max(1, output.height / std::max(1, output.scale)));
  }

  // Single source of truth for desktop widget coordinate clamping. Edit mode, default mode, and
  // snapshot normalization all route through here so the visibility rule stays identical. Resolves
  // the widget's effective output, then constrains state.cx/cy so that at least
  // kDesktopWidgetMinVisibleFraction of the widget's rotated AABB remains on screen. The caller
  // passes the widget's current content-scaled intrinsic size, so state.scale is already reflected
  // in intrinsicWidth/intrinsicHeight and must not be applied again as a transform multiplier here.
  inline const WaylandOutput* clampStateToOutput(
      const WaylandConnection& wayland, DesktopWidgetState& state, float intrinsicWidth, float intrinsicHeight
  ) {
    const WaylandOutput* output = resolveEffectiveOutput(wayland, state.outputName);
    if (output == nullptr) {
      return nullptr;
    }
    const WidgetTransformClampResult clamped = clampWidgetCenterToOutput(
        state.cx, state.cy, intrinsicWidth, intrinsicHeight, 1.0f, state.rotationRad, outputLogicalWidth(*output),
        outputLogicalHeight(*output), kDesktopWidgetMinVisibleFraction
    );
    state.cx = clamped.cx;
    state.cy = clamped.cy;
    return output;
  }

} // namespace desktop_widgets
