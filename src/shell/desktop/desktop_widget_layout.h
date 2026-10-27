#pragma once

#include "config/config_types.h"
#include "shell/desktop/widget_transform.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace desktop_widgets {

  inline float widgetContentScale(float baseUiScale) { return std::max(0.01F, baseUiScale); }

  inline void widgetNodeScale(const DesktopWidgetState& state, float& outScaleX, float& outScaleY) {
    outScaleX = state.flipX ? -1.0F : 1.0F;
    outScaleY = state.flipY ? -1.0F : 1.0F;
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
      if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
        continue;
      }
      if (outputKey(output) == key) {
        return &output;
      }
    }
    return nullptr;
  }

  struct OutputSelection {
    const WaylandOutput* output = nullptr;
    bool ambiguous = false;
  };

  inline OutputSelection selectOutput(std::span<const WaylandOutput> outputs, const std::string& selector) {
    for (const auto& output : outputs) {
      if (output.done
          && output.output != nullptr
          && output.hasUsableGeometry()
          && (selector.empty() || outputKey(output) == selector)) {
        return {&output, false};
      }
    }

    const WaylandOutput* selected = nullptr;
    for (const auto& output : outputs) {
      if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
        continue;
      }
      if (outputMatchesSelector(selector, output)) {
        if (selected != nullptr) {
          return {nullptr, true};
        }
        selected = &output;
      }
    }
    return {selected, false};
  }

  inline const WaylandOutput*
  resolveEffectiveOutput(const WaylandConnection& wayland, const std::string& requestedOutput) {
    return selectOutput(wayland.outputs(), requestedOutput).output;
  }

  inline const WaylandOutput* resolveStateOutput(const WaylandConnection& wayland, const DesktopWidgetState& state) {
    return resolveEffectiveOutput(wayland, state.outputName);
  }

  inline float outputLogicalWidth(const WaylandOutput& output) {
    return static_cast<float>(std::max(1, output.effectiveLogicalWidth()));
  }

  inline float outputLogicalHeight(const WaylandOutput& output) {
    return static_cast<float>(std::max(1, output.effectiveLogicalHeight()));
  }

  class PlacementMapper {
  public:
    [[nodiscard]] bool
    remapForOutputChange(std::span<const WaylandOutput> outputs, std::vector<DesktopWidgetState>& widgets) const {
      bool changed = false;
      for (auto& widget : widgets) {
        const WaylandOutput* output = selectOutput(outputs, widget.outputName).output;
        if (output == nullptr) {
          continue;
        }

        const float width = outputLogicalWidth(*output);
        const float height = outputLogicalHeight(*output);
        if (widget.placementWidth <= 0.0F || widget.placementHeight <= 0.0F) {
          widget.placementWidth = width;
          widget.placementHeight = height;
          changed = true;
          continue;
        }

        if (widget.placementWidth == width && widget.placementHeight == height) {
          continue;
        }

        widget.cx *= width / widget.placementWidth;
        widget.cy *= height / widget.placementHeight;
        widget.placementWidth = width;
        widget.placementHeight = height;
        changed = true;
      }
      return changed;
    }

    void
    rebaseForCurrentOutputs(std::span<const WaylandOutput> outputs, std::vector<DesktopWidgetState>& widgets) const {
      for (auto& widget : widgets) {
        const WaylandOutput* output = selectOutput(outputs, widget.outputName).output;
        if (output == nullptr) {
          continue;
        }
        widget.placementWidth = outputLogicalWidth(*output);
        widget.placementHeight = outputLogicalHeight(*output);
      }
    }
  };

  // Single source of truth for desktop widget coordinate clamping. Edit mode, default mode, and
  // snapshot normalization all route through here so the visibility rule stays identical. Resolves
  // the widget's effective output, then constrains state.cx/cy so that at least
  // kDesktopWidgetMinVisibleFraction of the widget's rotated AABB remains on screen. The caller
  // passes the widget's current box size as intrinsicWidth/intrinsicHeight, so no extra scale
  // multiplier is applied here.
  inline const WaylandOutput* clampStateToOutput(
      std::span<const WaylandOutput> outputs, DesktopWidgetState& state, float intrinsicWidth, float intrinsicHeight
  ) {
    const WaylandOutput* output = selectOutput(outputs, state.outputName).output;
    if (output == nullptr) {
      return nullptr;
    }
    const WidgetTransformClampResult clamped = clampWidgetCenterToOutput(
        state.cx, state.cy, intrinsicWidth, intrinsicHeight, 1.0F, state.rotationRad, outputLogicalWidth(*output),
        outputLogicalHeight(*output), kDesktopWidgetMinVisibleFraction
    );
    state.cx = clamped.cx;
    state.cy = clamped.cy;
    return output;
  }

  inline const WaylandOutput* clampStateToOutput(
      const WaylandConnection& wayland, DesktopWidgetState& state, float intrinsicWidth, float intrinsicHeight
  ) {
    return clampStateToOutput(wayland.outputs(), state, intrinsicWidth, intrinsicHeight);
  }

} // namespace desktop_widgets
