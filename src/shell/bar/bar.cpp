#include "shell/bar/bar.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/tray/tray_service.h"
#include "dbus/upower/upower_service.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/bar/widget.h"
#include "shell/bar/widgets/scripted_widget.h"
#include "shell/panel/panel_manager.h"
#include "shell/surface_shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/gamma_service.h"
#include "system/system_monitor_service.h"
#include "system/weather_service.h"
#include "theme/theme_service.h"
#include "time/time_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <wayland-client-core.h>

namespace {

  constexpr float kCircularCapsuleNarrowWidthEpsilon = 1.0f;
  constexpr std::int32_t kAutoHideTriggerPx = 3;
  constexpr float kAutoHideSlideExtraPx = 4.0f;
  [[nodiscard]] int barAutoHideEdgeGutter(const BarConfig& cfg) noexcept {
    if (!cfg.autoHide || cfg.marginEdge <= 0) {
      return 0;
    }
    return cfg.marginEdge;
  }

  [[nodiscard]] std::int32_t
  reservedBarExclusiveZone(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
    const auto sb = shell::surface_shadow::bleed(barConfig.shadow, shadowConfig);
    const std::int32_t mEdge = barConfig.marginEdge;
    if (barConfig.position == "bottom") {
      return barConfig.thickness + std::min(mEdge, sb.down);
    }
    if (barConfig.position == "top") {
      return std::min(mEdge, sb.up) + barConfig.thickness;
    }
    if (barConfig.position == "right") {
      return barConfig.thickness + std::min(mEdge, sb.right);
    }
    if (barConfig.position == "left") {
      return std::min(mEdge, sb.left) + barConfig.thickness;
    }
    return barConfig.thickness;
  }

  [[nodiscard]] std::vector<InputRect>
  barAutoHideSurfaceInputRegion(const BarConfig& cfg, int surfW, int surfH, bool fullSurface) {
    if (surfW <= 0 || surfH <= 0) {
      return {};
    }
    if (fullSurface) {
      return {InputRect{0, 0, surfW, surfH}};
    }

    const int strip = std::min(kAutoHideTriggerPx, cfg.position == "left" || cfg.position == "right" ? surfW : surfH);
    if (cfg.position == "bottom") {
      return {InputRect{0, surfH - strip, surfW, strip}};
    }
    if (cfg.position == "left") {
      return {InputRect{0, 0, strip, surfH}};
    }
    if (cfg.position == "right") {
      return {InputRect{surfW - strip, 0, strip, surfH}};
    }
    return {InputRect{0, 0, surfW, strip}};
  }

  bool pointInsideNode(const Node* node, float sceneX, float sceneY) {
    if (node == nullptr) {
      return false;
    }
    float localX = 0.0f;
    float localY = 0.0f;
    if (!Node::mapFromScene(node, sceneX, sceneY, localX, localY)) {
      return false;
    }
    return localX >= 0.0f && localX < node->width() && localY >= 0.0f && localY < node->height();
  }

  HitTestOutset crossAxisOutsetToSlot(const Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return {};
    }

    float nodeX = 0.0f;
    float nodeY = 0.0f;
    float slotX = 0.0f;
    float slotY = 0.0f;
    Node::absolutePosition(node, nodeX, nodeY);
    Node::absolutePosition(slot, slotX, slotY);

    if (isVertical) {
      return {
          .left = std::max(0.0f, nodeX - slotX),
          .top = 0.0f,
          .right = std::max(0.0f, (slotX + slot->width()) - (nodeX + node->width())),
          .bottom = 0.0f,
      };
    }

    return {
        .left = 0.0f,
        .top = std::max(0.0f, nodeY - slotY),
        .right = 0.0f,
        .bottom = std::max(0.0f, (slotY + slot->height()) - (nodeY + node->height())),
    };
  }

  void applyBarWidgetHitTargets(Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return;
    }

    if (dynamic_cast<InputArea*>(node) != nullptr || node->clipChildren()) {
      node->setHitTestOutset(crossAxisOutsetToSlot(node, slot, isVertical));
    }

    for (const auto& child : node->children()) {
      applyBarWidgetHitTargets(child.get(), slot, isVertical);
    }
  }

  Widget* widgetAtPoint(const std::vector<std::unique_ptr<Widget>>& widgets, float sceneX, float sceneY) {
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
      auto* widget = it->get();
      if (widget == nullptr || widget->root() == nullptr || !widget->root()->visible()) {
        continue;
      }
      if (Node::hitTest(widget->root(), sceneX, sceneY) != nullptr || pointInsideNode(widget->root(), sceneX, sceneY)) {
        return widget;
      }
    }
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
      auto* widget = it->get();
      auto* root = widget != nullptr ? widget->root() : nullptr;
      auto* bounds = widget != nullptr ? widget->layoutBoundsNode() : nullptr;
      if (root == nullptr || bounds == nullptr || bounds == root || root->parent() != bounds || !bounds->visible()) {
        continue;
      }
      if (Node::hitTest(bounds, sceneX, sceneY) != nullptr || pointInsideNode(bounds, sceneX, sceneY)) {
        return widget;
      }
    }
    return nullptr;
  }

  Widget* widgetAtPoint(const BarInstance& instance, float sceneX, float sceneY) {
    if (auto* widget = widgetAtPoint(instance.endWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    if (auto* widget = widgetAtPoint(instance.centerWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    return widgetAtPoint(instance.startWidgets, sceneX, sceneY);
  }

  std::pair<float, float> surfaceOriginForOutputLocal(const BarInstance& instance, const WaylandOutput& outputInfo) {
    if (instance.surface == nullptr) {
      return {0.0f, 0.0f};
    }
    const auto* surface = instance.surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const float mTop = static_cast<float>(surface->marginTop());
    const float mRight = static_cast<float>(surface->marginRight());
    const float mBottom = static_cast<float>(surface->marginBottom());
    const float mLeft = static_cast<float>(surface->marginLeft());
    const float surfW = static_cast<float>(surface->width());
    const float surfH = static_cast<float>(surface->height());
    const float outputW = static_cast<float>(outputInfo.logicalWidth);
    const float outputH = static_cast<float>(outputInfo.logicalHeight);

    float x = 0.0f;
    float y = 0.0f;
    if (aLeft && aRight) {
      x = mLeft;
    } else if (aRight) {
      x = std::max(0.0f, outputW - mRight - surfW);
    } else {
      x = mLeft;
    }

    if (aTop && aBottom) {
      y = mTop;
    } else if (aBottom) {
      y = std::max(0.0f, outputH - mBottom - surfH);
    } else {
      y = mTop;
    }
    return {x, y};
  }

  std::uint32_t positionToAnchor(const std::string& position) {
    if (position == "bottom") {
      return LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right;
    }
    if (position == "left") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    }
    if (position == "right") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Right;
    }
    // Default: top
    return LayerShellAnchor::Top | LayerShellAnchor::Left | LayerShellAnchor::Right;
  }

  constexpr Logger kLog("bar");
  constexpr std::string_view kScriptedWidgetIpcUsage =
      "scripted-widget <widget-name> <target[:bar-name]> <event> [payload]";

  struct ScriptedWidgetIpcCounts {
    int matched = 0;
    int handled = 0;
    int missingHost = 0;
    int missingCallback = 0;
    int failed = 0;
  };

  struct ScriptedWidgetIpcTarget {
    std::string outputSelector;
    std::string barName;
    bool hasBarName = false;
    bool allOutputs = false;
  };

  struct ScriptedWidgetIpcCandidate {
    ScriptedWidget* widget = nullptr;
  };

  bool takeIpcWord(std::string_view& text, std::string& word) {
    text = StringUtils::trimLeftView(text);
    if (text.empty()) {
      return false;
    }
    std::size_t end = 0;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
      ++end;
    }
    word.assign(text.substr(0, end));
    text.remove_prefix(end);
    return true;
  }

  std::optional<ScriptedWidgetIpcTarget> parseScriptedWidgetIpcTarget(std::string_view raw) {
    ScriptedWidgetIpcTarget target;
    const std::size_t separator = raw.find(':');
    if (separator == std::string_view::npos) {
      target.outputSelector = std::string(raw);
    } else {
      target.outputSelector = std::string(raw.substr(0, separator));
      target.barName = std::string(raw.substr(separator + 1));
      target.hasBarName = true;
    }
    if (target.outputSelector.empty() || (target.hasBarName && target.barName.empty())) {
      return std::nullopt;
    }
    target.allOutputs = target.outputSelector == "all";
    return target;
  }

  void recordScriptedWidgetIpcResult(ScriptedWidgetIpcCounts& counts, ScriptedWidget::IpcDispatchResult result) {
    ++counts.matched;
    switch (result) {
    case ScriptedWidget::IpcDispatchResult::Handled:
      ++counts.handled;
      break;
    case ScriptedWidget::IpcDispatchResult::MissingHost:
      ++counts.missingHost;
      break;
    case ScriptedWidget::IpcDispatchResult::MissingCallback:
      ++counts.missingCallback;
      break;
    case ScriptedWidget::IpcDispatchResult::Failed:
      ++counts.failed;
      break;
    }
  }

  ColorSpec withOpacity(const ColorSpec& color, float opacity) {
    ColorSpec out = color;
    out.alpha = std::clamp(out.alpha * std::clamp(opacity, 0.0f, 1.0f), 0.0f, 1.0f);
    return out;
  }

  struct BarVisualGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
  };

  struct BarSurfaceSpec {
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
    std::int32_t exclusiveZone = 0;
  };

  [[nodiscard]] BarSurfaceSpec
  computeBarSurfaceSpec(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
    const bool vertical = (barConfig.position == "left" || barConfig.position == "right");
    const bool isBottom = barConfig.position == "bottom";
    const bool isRight = barConfig.position == "right";
    const std::int32_t mEnds = barConfig.marginEnds;
    const std::int32_t mEdge = barConfig.marginEdge;
    const auto sb = shell::surface_shadow::bleed(barConfig.shadow, shadowConfig);
    const int edgeGutter = barAutoHideEdgeGutter(barConfig);

    BarSurfaceSpec spec;
    if (!vertical) {
      spec.marginLeft = std::max(0, mEnds - sb.left);
      spec.marginRight = std::max(0, mEnds - sb.right);
      if (isBottom) {
        if (edgeGutter > 0) {
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.up + barConfig.thickness + sb.down + edgeGutter);
        } else {
          spec.marginBottom = std::max(0, mEdge - sb.down);
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.up + barConfig.thickness + std::min(mEdge, sb.down));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.down + barConfig.thickness + sb.up + edgeGutter);
        } else {
          spec.marginTop = std::max(0, mEdge - sb.up);
          spec.surfaceHeight = static_cast<std::uint32_t>(std::min(mEdge, sb.up) + barConfig.thickness + sb.down);
        }
      }
    } else {
      spec.marginTop = std::max(0, mEnds - sb.up);
      spec.marginBottom = std::max(0, mEnds - sb.down);
      if (isRight) {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.left + barConfig.thickness + sb.right + edgeGutter);
        } else {
          spec.marginRight = std::max(0, mEdge - sb.right);
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.left + barConfig.thickness + std::min(mEdge, sb.right));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.right + barConfig.thickness + sb.left + edgeGutter);
        } else {
          spec.marginLeft = std::max(0, mEdge - sb.left);
          spec.surfaceWidth = static_cast<std::uint32_t>(std::min(mEdge, sb.left) + barConfig.thickness + sb.right);
        }
      }
    }

    spec.exclusiveZone =
        (!barConfig.autoHide && barConfig.reserveSpace) ? reservedBarExclusiveZone(barConfig, shadowConfig) : 0;
    return spec;
  }

  // Returns true when two bar configs would produce an identical layer-shell
  // surface (same anchor, size, exclusive zone, namespace). When true, an
  // existing BarInstance can be retained on reload and only its widget tree
  // rebuilt — avoiding the screen-shift caused by destroying and recreating
  // the exclusive zone.
  bool barConfigSurfaceFieldsEqual(
      const BarConfig& a, const BarConfig& b, const ShellConfig::ShadowConfig& previousShadow,
      const ShellConfig::ShadowConfig& nextShadow
  ) {
    const bool sameShadowSurface =
        (!a.shadow && !b.shadow) || shell::surface_shadow::sameSurfaceMetrics(previousShadow, nextShadow);
    return a.name == b.name && a.position == b.position && a.enabled == b.enabled && a.autoHide == b.autoHide &&
           a.reserveSpace == b.reserveSpace && a.thickness == b.thickness && a.marginEnds == b.marginEnds &&
           a.marginEdge == b.marginEdge && a.shadow == b.shadow && sameShadowSurface &&
           a.monitorOverrides == b.monitorOverrides;
  }

  bool barSurfaceOrderRequiresRecreate(const std::vector<BarConfig>& previous, const std::vector<BarConfig>& next) {
    std::vector<std::string> preserved;
    preserved.reserve(previous.size());
    for (const auto& oldBar : previous) {
      const auto it =
          std::find_if(next.begin(), next.end(), [&](const BarConfig& newBar) { return newBar.name == oldBar.name; });
      if (it != next.end()) {
        preserved.push_back(oldBar.name);
      }
    }

    if (preserved.size() > next.size()) {
      return true;
    }
    for (std::size_t i = 0; i < preserved.size(); ++i) {
      if (next[i].name != preserved[i]) {
        return true;
      }
    }
    return false;
  }

  BarVisualGeometry computeBarVisualGeometry(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight
  ) {
    const float barThickness = static_cast<float>(cfg.thickness);
    const float marginEnds = static_cast<float>(cfg.marginEnds);
    const float marginEdge = static_cast<float>(cfg.marginEdge);
    const bool isBottom = cfg.position == "bottom";
    const bool isRight = cfg.position == "right";
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const auto sbi = shell::surface_shadow::bleed(cfg.shadow, shadow);
    const float bleedLeft = static_cast<float>(sbi.left);
    const float bleedRight = static_cast<float>(sbi.right);
    const float bleedUp = static_cast<float>(sbi.up);
    const float bleedDown = static_cast<float>(sbi.down);

    if (isVertical) {
      // Vertical bar: edge gap is left/right, ends inset is top/bottom.
      const float y = std::min(marginEnds, bleedUp);
      float x = isRight ? bleedLeft : std::min(marginEdge, bleedLeft);
      if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
        if (isRight) {
          x = surfaceWidth - static_cast<float>(gutter) - barThickness - bleedRight;
        } else {
          x = static_cast<float>(gutter) + bleedLeft;
        }
      }
      return {
          .x = x,
          .y = y,
          .width = barThickness,
          .height = surfaceHeight - y - std::min(marginEnds, bleedDown),
      };
    }

    // Horizontal bar: edge gap is top/bottom, ends inset is left/right.
    const float x = std::min(marginEnds, bleedLeft);
    float y = isBottom ? bleedUp : std::min(marginEdge, bleedUp);
    if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
      if (isBottom) {
        y = surfaceHeight - static_cast<float>(gutter) - barThickness - bleedDown;
      } else {
        y = static_cast<float>(gutter) + bleedUp;
      }
    }
    return {
        .x = x,
        .y = y,
        .width = surfaceWidth - x - std::min(marginEnds, bleedRight),
        .height = barThickness,
    };
  }

  [[nodiscard]] InputRect
  barContentInputRegion(const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, int surfW, int surfH) {
    const auto barVisual = computeBarVisualGeometry(cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH));
    return InputRect{
        static_cast<int>(barVisual.x), static_cast<int>(barVisual.y), static_cast<int>(barVisual.width),
        static_cast<int>(barVisual.height)
    };
  }

  std::pair<float, float> computeAutoHideHiddenDelta(
      bool isVertical, bool isBottom, bool isRight, float w, float h, float contentLeft, float contentTop,
      float contentRight, float contentBottom
  ) {
    const float k = kAutoHideSlideExtraPx;
    if (!isVertical) {
      if (isBottom) {
        return {0.0f, (h - contentTop) + k};
      }
      return {0.0f, -(contentBottom + k)};
    }
    if (isRight) {
      return {(w - contentLeft) + k, 0.0f};
    }
    return {-(contentRight + k), 0.0f};
  }

  void applyBarShadowStyle(
      BarInstance& instance, const ShellConfig::ShadowConfig& shadowConfig, float surfaceWidth, float surfaceHeight
  ) {
    if (instance.shadow == nullptr) {
      return;
    }

    const Radii barRadii{
        static_cast<float>(instance.barConfig.radiusTopLeft),
        static_cast<float>(instance.barConfig.radiusTopRight),
        static_cast<float>(instance.barConfig.radiusBottomRight),
        static_cast<float>(instance.barConfig.radiusBottomLeft),
    };
    const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight);
    const float barAreaW = barVisual.width;
    const float barAreaH = barVisual.height;
    const float bgOpacity = std::clamp(instance.barConfig.backgroundOpacity, 0.0f, 1.0f);
    const float shadowX = barVisual.x + static_cast<float>(shadowConfig.offsetX);
    const float shadowY = barVisual.y + static_cast<float>(shadowConfig.offsetY);
    RoundedRectStyle shadowStyle =
        shell::surface_shadow::style(shadowConfig, bgOpacity, shell::surface_shadow::Shape{.radius = barRadii});

    const bool panelShadowExclusion = instance.attachedPanelGeometry.has_value() &&
                                      instance.attachedPanelGeometry->width > 0.0f &&
                                      instance.attachedPanelGeometry->height > 0.0f;
    if (panelShadowExclusion) {
      const auto& attached = *instance.attachedPanelGeometry;
      const float convexRadius = std::max(0.0f, attached.cornerRadius);
      const float bulgeRadius = std::max(0.0f, attached.bulgeRadius);
      const std::string_view barPosition = instance.barConfig.position;
      const auto corners = attached_panel::cornerShapes(barPosition);
      const auto pickRadius = [&](CornerShape shape) {
        return shape == CornerShape::Concave ? bulgeRadius : convexRadius;
      };
      shadowStyle.shadowExclusion = true;
      shadowStyle.shadowExclusionOffsetX = shadowX - attached.x;
      shadowStyle.shadowExclusionOffsetY = shadowY - attached.y;
      shadowStyle.shadowExclusionWidth = attached.width;
      shadowStyle.shadowExclusionHeight = attached.height;
      shadowStyle.shadowExclusionCorners = corners;
      shadowStyle.shadowExclusionLogicalInset = attached_panel::logicalInset(barPosition, bulgeRadius);
      shadowStyle.shadowExclusionRadius =
          Radii{pickRadius(corners.tl), pickRadius(corners.tr), pickRadius(corners.br), pickRadius(corners.bl)};
    }

    auto configureShadow = [&](Box* node, float x, float y) {
      if (node == nullptr) {
        return;
      }
      node->setStyle(shadowStyle);
      node->setZIndex(-1);
      node->setPosition(x, y);
      node->setSize(barAreaW, barAreaH);
    };

    instance.shadow->setHitTestVisible(false);
    instance.shadow->setVisible(true);
    configureShadow(instance.shadow, shadowX, shadowY);

    if (instance.shadowLeftClip != nullptr) {
      instance.shadowLeftClip->setVisible(false);
    }
    if (instance.shadowRightClip != nullptr) {
      instance.shadowRightClip->setVisible(false);
    }
  }

  void layoutBarSections(
      BarInstance& instance, Renderer& renderer, float barAreaW, float barAreaH, float padding, bool isVertical
  ) {
    const float slotCross = isVertical ? barAreaW : barAreaH;

    auto layoutWidgets = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (widget->root() != nullptr) {
          widget->layout(renderer, barAreaW, barAreaH);
        }
      }
    };
    layoutWidgets(instance.startWidgets);
    layoutWidgets(instance.centerWidgets);
    layoutWidgets(instance.endWidgets);

    float cachedBodyExtent = -1.0f;
    float cachedBodyExtentScale = -1.0f;
    auto finalizeCapsules = [isVertical, slotCross, &renderer, &cachedBodyExtent,
                             &cachedBodyExtentScale](std::vector<BarCapsuleRun>& runs) {
      for (auto& run : runs) {
        Node* shell = run.shell;
        Box* bg = run.bg;
        Node* content = run.content;
        if (shell == nullptr || bg == nullptr || content == nullptr) {
          continue;
        }
        if (run.container != nullptr) {
          run.container->layout(renderer);
        }

        bool hasVisibleContent = false;
        bool hasVisibleInk = false;
        for (Widget* widget : run.widgets) {
          if (widget == nullptr || widget->root() == nullptr) {
            continue;
          }
          hasVisibleContent = hasVisibleContent || widget->root()->visible();
          hasVisibleInk = hasVisibleInk || widget->shouldShowBarCapsule();
        }

        shell->setVisible(hasVisibleContent);
        const float scale = run.contentScale;
        const float iw = content->width();
        const float ih = content->height();
        if (!hasVisibleInk) {
          shell->setSize(iw, ih);
          content->setPosition(0.0f, 0.0f);
          bg->setVisible(false);
          bg->setPosition(0.0f, 0.0f);
          bg->setSize(iw, ih);
          continue;
        }
        if (scale != cachedBodyExtentScale) {
          const auto refMetrics = renderer.measureFont(Style::fontSizeBody * scale);
          cachedBodyExtent = std::round(refMetrics.bottom - refMetrics.top);
          cachedBodyExtentScale = scale;
        }
        const float bodyExtent = cachedBodyExtent;
        const float iconExtent = std::max(bodyExtent, std::round(Style::barGlyphSize * scale));
        const float pad = run.spec.padding * scale;
        const float padMain = pad;
        const float padCross = std::min(pad, Style::spaceXs * scale);
        float capsuleCross = bodyExtent + 2.0f * padCross;
        if (isVertical) {
          capsuleCross = std::min(capsuleCross, slotCross);
        }
        float shellMain = (isVertical ? ih : iw) + 2.0f * padMain;
        float shellCross = capsuleCross;
        float shellW = isVertical ? shellCross : shellMain;
        float shellH = isVertical ? shellMain : shellCross;
        float contentX = std::round((shellW - iw) * 0.5f);
        float contentY = std::round((shellH - ih) * 0.5f);
        // Glyph-only widgets become a fixed circle based on the bar capsule
        // cross-size, not on the measured content width. Multi-line / wide
        // content (e.g. stacked vertical clock) must NOT be squared, or the
        // capsule collapses on the main axis.
        const float iconThreshold = iconExtent + (kCircularCapsuleNarrowWidthEpsilon * scale);
        const bool iconSized = run.allowCircularSizing && iw <= iconThreshold && ih <= iconThreshold;
        if (iconSized) {
          const float side = capsuleCross;
          shellW = side;
          shellH = side;
          contentX = std::round((shellW - iw) * 0.5f);
          contentY = std::round((shellH - ih) * 0.5f);
        }
        shell->setSize(shellW, shellH);
        bg->setVisible(true);
        bg->setPosition(0.0f, 0.0f);
        bg->setSize(shellW, shellH);
        content->setPosition(contentX, contentY);
        const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
        bg->setRadius(
            radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(shellW, shellH)
                                    : std::max(0.0f, std::min(shellW, shellH) * 0.5f)
        );
      }
    };
    finalizeCapsules(instance.startCapsuleRuns);
    finalizeCapsules(instance.centerCapsuleRuns);
    finalizeCapsules(instance.endCapsuleRuns);

    const float contentMainStart = padding;
    const float contentMainEnd = std::max(contentMainStart, (isVertical ? barAreaH : barAreaW) - padding);
    const float contentMainSpan = std::max(0.0f, contentMainEnd - contentMainStart);

    auto configureSlot = [&](Node* slot, float mainOffset, float mainSize) {
      slot->setClipChildren(true);
      if (isVertical) {
        slot->setPosition(0.0f, mainOffset);
        slot->setSize(slotCross, mainSize);
      } else {
        slot->setPosition(mainOffset, 0.0f);
        slot->setSize(mainSize, slotCross);
      }
    };

    auto configureSection = [&](Flex* section, FlexJustify justify) {
      section->setJustify(justify);
      section->layout(renderer);
    };

    configureSection(instance.startSection, FlexJustify::Start);
    configureSection(instance.centerSection, FlexJustify::Center);
    configureSection(instance.endSection, FlexJustify::End);

    // Anchor mode: if a center widget is flagged as the anchor, pin its center to the
    // bar midline so surrounding siblings growing/shrinking cannot drift it sideways.
    const Node* anchorNode = nullptr;
    for (const auto& widget : instance.centerWidgets) {
      if (widget != nullptr && widget->isAnchor() && widget->layoutBoundsNode() != nullptr) {
        anchorNode = widget->layoutBoundsNode();
        break;
      }
    }

    const float barMidline = contentMainStart + contentMainSpan * 0.5f;
    const float centerNaturalMain = isVertical ? instance.centerSection->height() : instance.centerSection->width();

    float centerSlotStart;
    float centerSlotMain;
    float centerSectionOffset; // offset of section origin within its slot along main axis
    if (anchorNode != nullptr) {
      const float anchorOffsetInSection = isVertical ? anchorNode->y() : anchorNode->x();
      const float anchorSpan = isVertical ? anchorNode->height() : anchorNode->width();
      const float anchorCenterInSection = anchorOffsetInSection + anchorSpan * 0.5f;
      // Place the section so that the anchor's center sits at barMidline.
      float desiredSectionStart = barMidline - anchorCenterInSection;
      // Clamp so the section stays within the content area.
      const float maxStart = contentMainEnd - centerNaturalMain;
      desiredSectionStart = std::clamp(desiredSectionStart, contentMainStart, std::max(contentMainStart, maxStart));
      centerSlotStart = desiredSectionStart;
      centerSlotMain = std::min(centerNaturalMain, contentMainEnd - centerSlotStart);
      centerSectionOffset = 0.0f;
    } else {
      centerSlotMain = std::min(contentMainSpan, centerNaturalMain);
      centerSlotStart = contentMainStart + std::max(0.0f, (contentMainSpan - centerSlotMain) * 0.5f);
      centerSectionOffset = (centerSlotMain - centerNaturalMain) * 0.5f;
    }
    const float centerSlotEnd = centerSlotStart + centerSlotMain;
    const float startSlotMain = std::max(0.0f, centerSlotStart - contentMainStart);
    const float endSlotMain = std::max(0.0f, contentMainEnd - centerSlotEnd);

    configureSlot(instance.startSlot, contentMainStart, startSlotMain);
    configureSlot(instance.centerSlot, centerSlotStart, centerSlotMain);
    configureSlot(instance.endSlot, centerSlotEnd, endSlotMain);

    if (isVertical) {
      instance.startSection->setPosition((slotCross - instance.startSection->width()) * 0.5f, 0.0f);
      instance.centerSection->setPosition((slotCross - instance.centerSection->width()) * 0.5f, centerSectionOffset);
      instance.endSection->setPosition(
          (slotCross - instance.endSection->width()) * 0.5f, endSlotMain - instance.endSection->height()
      );
    } else {
      instance.startSection->setPosition(0.0f, (slotCross - instance.startSection->height()) * 0.5f);
      instance.centerSection->setPosition(centerSectionOffset, (slotCross - instance.centerSection->height()) * 0.5f);
      instance.endSection->setPosition(
          endSlotMain - instance.endSection->width(), (slotCross - instance.endSection->height()) * 0.5f
      );
    }

    applyBarWidgetHitTargets(instance.startSection, instance.startSlot, isVertical);
    applyBarWidgetHitTargets(instance.centerSection, instance.centerSlot, isVertical);
    applyBarWidgetHitTargets(instance.endSection, instance.endSlot, isVertical);
  }

  void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) {
    for (auto& widget : widgets) {
      if (widget != nullptr && widget->needsFrameTick()) {
        widget->onFrameTick(deltaMs);
      }
    }
  }

  bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::any_of(widgets.begin(), widgets.end(), [](const auto& widget) {
      return widget != nullptr && widget->needsFrameTick();
    });
  }

} // namespace

Bar::Bar() = default;

bool Bar::initialize(
    CompositorPlatform& platform, ConfigService* config, TimeService* timeService, NotificationManager* notifications,
    TrayService* tray, PipeWireService* audio, UPowerService* upower, SystemMonitorService* sysmon,
    PowerProfilesService* powerProfiles, INetworkService* network, IdleInhibitor* idleInhibitor, MprisService* mpris,
    PipeWireSpectrum* audioSpectrum, HttpClient* httpClient, WeatherService* weatherService,
    RenderContext* renderContext, GammaService* nightLight, noctalia::theme::ThemeService* themeService,
    BluetoothService* bluetooth, BrightnessService* brightness, LockKeysService* lockKeys, ClipboardService* clipboard,
    FileWatcher* fileWatcher
) {
  m_platform = &platform;
  m_config = config;
  m_notifications = notifications;
  m_tray = tray;
  m_audio = audio;
  m_upower = upower;
  m_sysmon = sysmon;
  m_powerProfiles = powerProfiles;
  m_network = network;
  m_idleInhibitor = idleInhibitor;
  m_mpris = mpris;
  m_audioSpectrum = audioSpectrum;
  m_httpClient = httpClient;
  m_weatherService = weatherService;
  m_renderContext = renderContext;
  m_nightLight = nightLight;
  m_themeService = themeService;
  m_bluetooth = bluetooth;
  m_brightness = brightness;
  m_lockKeys = lockKeys;
  m_clipboard = clipboard;
  m_fileWatcher = fileWatcher;

  m_widgetFactory = std::make_unique<WidgetFactory>(
      *m_platform, m_config->config(), m_notifications, m_tray, m_audio, m_upower, m_sysmon, m_powerProfiles, m_network,
      m_idleInhibitor, m_mpris, m_audioSpectrum, m_httpClient, m_weatherService, m_nightLight, m_themeService,
      m_bluetooth, m_brightness, m_lockKeys, m_clipboard, m_fileWatcher
  );

  if (timeService != nullptr) {
    timeService->setTickSecondCallback([this]() {
      for (auto& inst : m_instances) {
        if (inst->surface != nullptr) {
          inst->surface->requestUpdate();
        }
      }
    });
  }

  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_config->addReloadCallback([this]() {
    const auto& cfg = m_config->config();
    if (cfg.bars == m_lastBars && cfg.widgets == m_lastWidgets && cfg.shell.shadow == m_lastShadow) {
      return;
    }
    reload();
  });

  syncInstances();
  return true;
}

void Bar::onSecondTick() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
    }
  }
}

void Bar::reload() {
  kLog.info("reloading config");
  const auto previousBars = m_lastBars;
  const auto previousShadow = m_lastShadow;
  const bool recreateForOrder = barSurfaceOrderRequiresRecreate(previousBars, m_config->config().bars);
  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_widgetFactory = std::make_unique<WidgetFactory>(
      *m_platform, m_config->config(), m_notifications, m_tray, m_audio, m_upower, m_sysmon, m_powerProfiles, m_network,
      m_idleInhibitor, m_mpris, m_audioSpectrum, m_httpClient, m_weatherService, m_nightLight, m_themeService,
      m_bluetooth, m_brightness, m_lockKeys, m_clipboard, m_fileWatcher
  );

  if (recreateForOrder) {
    kLog.info("bar order changed; recreating layer-shell surfaces");
    closeAllInstances();
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying bar surfaces for order change: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
    syncInstances();
    return;
  }

  // Look up new bar configs by name.
  std::unordered_map<std::string, std::pair<const BarConfig*, std::size_t>> newBarsByName;
  newBarsByName.reserve(m_lastBars.size());
  for (std::size_t i = 0; i < m_lastBars.size(); ++i) {
    newBarsByName[m_lastBars[i].name] = {&m_lastBars[i], i};
  }

  // For each existing instance, decide whether to rebuild contents in place
  // (surface preserved → no exclusive-zone churn) or destroy (will be recreated
  // by syncInstances below).
  bool destroyedAny = false;
  std::erase_if(m_instances, [&](const std::unique_ptr<BarInstance>& instUp) {
    auto& inst = *instUp;
    auto it = newBarsByName.find(inst.barConfig.name);
    auto destroy = [&]() {
      if (inst.surface != nullptr) {
        m_surfaceMap.erase(inst.surface->wlSurface());
      }
      if (m_hoveredInstance == &inst) {
        m_hoveredInstance = nullptr;
      }
      destroyedAny = true;
      return true;
    };
    if (it == newBarsByName.end()) {
      return destroy();
    }

    const auto& outputs = m_platform->outputs();
    auto outIt =
        std::find_if(outputs.begin(), outputs.end(), [&inst](const auto& o) { return o.name == inst.outputName; });
    if (outIt == outputs.end()) {
      return destroy();
    }

    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return destroy();
    }
    if (!barConfigSurfaceFieldsEqual(inst.barConfig, resolved, previousShadow, m_lastShadow)) {
      return destroy();
    }

    inst.barIndex = it->second.second;
    rebuildInstanceContents(inst, resolved);
    return false;
  });

  if (destroyedAny) {
    // Drain pending Wayland events for the just-destroyed surfaces before
    // creating new ones. Without this, the roundtrip inside LayerSurface::initialize
    // reads stale closures for dead proxies, which libwayland drops without freeing.
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying stale bar surfaces: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
  }

  syncInstances();
}

void Bar::closeAllInstances() {
  m_surfaceMap.clear();
  m_hoveredInstance = nullptr;
  m_instances.clear();
}

void Bar::onOutputChange() { syncInstances(); }

void Bar::refresh() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
      if (inst->animations.hasActive() || instanceNeedsFrameTick(*inst)) {
        inst->surface->requestRedraw();
      }
    }
  }
}

void Bar::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Bar::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void Bar::setAutoHideSuppressionCallback(std::function<bool(const BarInstance&)> callback) {
  m_autoHideSuppressionCallback = std::move(callback);
}

void Bar::reevaluateAutoHide() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr || !instance->barConfig.autoHide || instance->pointerInside ||
        instance->attachedPopupCount > 0) {
      continue;
    }
    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
    if (suppressAutoHide || instance->hideOpacity <= 0.001f) {
      continue;
    }
    startHideFadeOut(*instance);
  }
}

void Bar::setOpenWidgetSettingsCallback(std::function<void(std::string, std::string)> callback) {
  m_openWidgetSettingsCallback = std::move(callback);
}

bool Bar::isRunning() const noexcept {
  return std::any_of(m_instances.begin(), m_instances.end(), [](const auto& inst) {
    return inst->surface && inst->surface->isRunning();
  });
}

bool Bar::instanceEffectivelyVisible(const BarInstance& instance) const noexcept {
  if (instance.barConfig.autoHide) {
    return instance.hideOpacity > 0.5f;
  }
  return instance.slideRoot == nullptr || instance.slideRoot->opacity() > 0.5f;
}

bool Bar::instanceAcceptsPointerInput(const BarInstance& instance) const noexcept {
  return instance.barConfig.autoHide || !instance.ipcLayoutReleased;
}

bool Bar::isVisible() const noexcept {
  return std::any_of(m_instances.begin(), m_instances.end(), [this](const auto& inst) {
    return instanceEffectivelyVisible(*inst);
  });
}

void Bar::clearInstancePointerState(BarInstance& instance) {
  instance.pointerInside = false;
  instance.inputDispatcher.pointerLeave();
  if (m_hoveredInstance == &instance) {
    m_hoveredInstance = nullptr;
  }
}

void Bar::setInstanceIpcVisible(BarInstance& instance, bool visible) {
  if (instance.surface == nullptr) {
    return;
  }
  if (instance.barConfig.autoHide) {
    if (visible) {
      revealAutoHideBar(instance);
    } else {
      startHideFadeOut(instance);
    }
    return;
  }
  if (instance.slideRoot == nullptr) {
    return;
  }
  // Non-autohide IPC: instant show/hide (no opacity fade — avoids sluggish hide and blur bleed-through).
  instance.animations.cancelForOwner(instance.slideRoot);
  instance.slideRoot->setOpacity(visible ? 1.0f : 0.0f);
  if (!visible) {
    clearInstancePointerState(instance);
  }
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::applyIpcVisibility(bool visible) {
  for (const auto& instance : m_instances) {
    if (instance == nullptr) {
      continue;
    }
    setInstanceIpcVisible(*instance, visible);
    syncBarSurfaceChrome(*instance);
  }
}

bool Bar::barContentVisuallyShown(const BarInstance& instance) const noexcept {
  constexpr float kShownThreshold = 0.02f;
  if (instance.barConfig.autoHide) {
    return instance.hideOpacity > kShownThreshold;
  }
  return instance.slideRoot == nullptr || instance.slideRoot->opacity() > kShownThreshold;
}

bool Bar::shouldReserveExclusiveZone(const BarInstance& instance) const noexcept {
  // v4 parity: auto-hide never reserves compositor space (overlay slide only).
  if (instance.barConfig.autoHide) {
    return false;
  }
  if (instance.ipcLayoutReleased) {
    return false;
  }
  return instance.barConfig.reserveSpace;
}

void Bar::syncBarExclusiveZone(BarInstance& instance) {
  if (instance.surface == nullptr || m_config == nullptr) {
    return;
  }
  const std::int32_t zone = shouldReserveExclusiveZone(instance)
                                ? reservedBarExclusiveZone(instance.barConfig, m_config->config().shell.shadow)
                                : 0;
  instance.surface->setExclusiveZone(zone);
}

void Bar::syncBarSurfaceChrome(BarInstance& instance) {
  syncBarExclusiveZone(instance);
  applyBarCompositorBlur(instance);
}

std::optional<LayerPopupParentContext> Bar::popupParentContextForSurface(wl_surface* surface) const noexcept {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr || instance->surface == nullptr) {
    return std::nullopt;
  }

  auto* layerSurface = instance->surface->layerSurface();
  const auto width = instance->surface->width();
  const auto height = instance->surface->height();
  if (layerSurface == nullptr || width == 0 || height == 0) {
    return std::nullopt;
  }

  return LayerPopupParentContext{
      .surface = instance->surface->wlSurface(),
      .layerSurface = layerSurface,
      .output = instance->output,
      .width = width,
      .height = height,
  };
}

std::optional<LayerPopupParentContext> Bar::preferredPopupParentContext(wl_output* output) const noexcept {
  BarInstance* instance = instanceForOutput(output);
  if (instance == nullptr && !m_instances.empty()) {
    instance = m_instances.front().get();
  }
  return instance != nullptr && instance->surface != nullptr
             ? popupParentContextForSurface(instance->surface->wlSurface())
             : std::nullopt;
}

std::vector<InputRect> Bar::surfaceRectsForOutput(wl_output* output) const {
  std::vector<InputRect> rects;
  if (m_platform == nullptr || output == nullptr) {
    return rects;
  }

  const WaylandOutput* wlOutput = m_platform->findOutputByWl(output);
  if (wlOutput == nullptr) {
    return rects;
  }
  // logicalWidth/Height become valid only after xdg_output.done; before that
  // we cannot accurately place a bottom/right anchored bar.
  if (wlOutput->logicalWidth <= 0 || wlOutput->logicalHeight <= 0) {
    return rects;
  }
  const std::int32_t outputW = wlOutput->logicalWidth;
  const std::int32_t outputH = wlOutput->logicalHeight;

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (!instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    const auto* surface = instance->surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const std::int32_t mTop = surface->marginTop();
    const std::int32_t mRight = surface->marginRight();
    const std::int32_t mBottom = surface->marginBottom();
    const std::int32_t mLeft = surface->marginLeft();
    // surface->width()/height() may be 0 before configure; fall back to BarConfig
    // thickness so we still publish a sensible exclusion for fresh surfaces.
    const std::int32_t surfW = static_cast<std::int32_t>(surface->width());
    const std::int32_t surfH = static_cast<std::int32_t>(surface->height());

    std::int32_t rectW = surfW;
    std::int32_t rectH = surfH;
    std::int32_t rectX = 0;
    std::int32_t rectY = 0;

    if (aLeft && aRight) {
      rectW = std::max(0, outputW - mLeft - mRight);
      rectX = mLeft;
    } else if (aRight) {
      rectX = std::max(0, outputW - mRight - rectW);
    } else {
      rectX = mLeft;
    }

    if (aTop && aBottom) {
      rectH = std::max(0, outputH - mTop - mBottom);
      rectY = mTop;
    } else if (aBottom) {
      rectY = std::max(0, outputH - mBottom - rectH);
    } else {
      rectY = mTop;
    }

    if (rectW > 0 && rectH > 0) {
      rects.push_back(InputRect{rectX, rectY, rectW, rectH});
    }
  }

  return rects;
}

std::vector<wl_surface*> Bar::allBarSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance != nullptr && instance->surface != nullptr && instanceAcceptsPointerInput(*instance)) {
      if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
        surfaces.push_back(s);
      }
    }
  }
  return surfaces;
}

void Bar::setAttachedPanelGeometry(
    wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry
) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr) {
    return;
  }

  instance->attachedPanelGeometry = geometry;
  if (instance->surface != nullptr && instance->surface->width() > 0 && instance->surface->height() > 0) {
    applyBarShadowStyle(
        *instance, m_config->config().shell.shadow, static_cast<float>(instance->surface->width()),
        static_cast<float>(instance->surface->height())
    );
    instance->surface->requestRedraw();
  }
}

void Bar::beginAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  ++instance->attachedPopupCount;
}

void Bar::endAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  if (instance->attachedPopupCount > 0) {
    --instance->attachedPopupCount;
  }
  if (m_platform != nullptr) {
    instance->pointerInside = (m_platform->lastPointerSurface() == surface);
  }
  if (!instance->pointerInside && m_hoveredInstance == instance) {
    m_hoveredInstance = nullptr;
  } else if (instance->pointerInside) {
    m_hoveredInstance = instance;
  }
  if (instance->attachedPopupCount > 0 || !instance->barConfig.autoHide || instance->pointerInside) {
    return;
  }
  const bool suppressAutoHide =
      (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
  if (!suppressAutoHide) {
    startHideFadeOut(*instance);
  }
}

void Bar::show() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::hide() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr && !instance->barConfig.autoHide) {
      // bar-hide IPC always frees layout on non-autohide bars (v4 isVisible=false), regardless of reserve_space.
      instance->ipcLayoutReleased = true;
    }
  }
  applyIpcVisibility(false);
}

void Bar::toggle() {
  const bool anyEffectivelyVisible = std::any_of(m_instances.begin(), m_instances.end(), [this](const auto& inst) {
    return inst != nullptr && instanceEffectivelyVisible(*inst);
  });

  if (anyEffectivelyVisible) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && !instance->barConfig.autoHide) {
        instance->ipcLayoutReleased = true;
      }
    }
    applyIpcVisibility(false);
    return;
  }

  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::syncInstances() {
  const auto& outputs = m_platform->outputs();
  const auto& bars = m_config->config().bars;

  // Remove instances for outputs that no longer exist
  std::erase_if(m_instances, [&outputs](const auto& inst) {
    bool found =
        std::any_of(outputs.begin(), outputs.end(), [&inst](const auto& out) { return out.name == inst->outputName; });
    if (!found) {
      kLog.info("removing instance for output {}", inst->outputName);
    }
    return !found;
  });

  // Create instances for each bar definition × each output
  for (std::size_t barIdx = 0; barIdx < bars.size(); ++barIdx) {
    for (const auto& output : outputs) {
      if (!output.done) {
        continue;
      }

      bool exists = std::any_of(m_instances.begin(), m_instances.end(), [&output, barIdx](const auto& inst) {
        return inst->outputName == output.name && inst->barIndex == barIdx;
      });
      if (!exists) {
        auto resolved = ConfigService::resolveForOutput(bars[barIdx], output);
        if (!resolved.enabled) {
          continue;
        }
        createInstance(output, barIdx, resolved);
      }
    }
  }
}

void Bar::createInstance(const WaylandOutput& output, std::size_t barIndex, const BarConfig& barConfig) {
  auto instance = std::make_unique<BarInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->barConfig = barConfig;
  instance->barIndex = barIndex;

  const auto anchor = positionToAnchor(barConfig.position);
  const auto surfaceSpec = computeBarSurfaceSpec(barConfig, m_config->config().shell.shadow);

  kLog.info(
      "creating #{} \"{}\" on {} ({}), thickness={} position={} reserve_space={} exclusive_zone={}", barIndex,
      barConfig.name, output.connectorName, output.description, barConfig.thickness, barConfig.position,
      barConfig.reserveSpace, surfaceSpec.exclusiveZone
  );

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-bar-" + barConfig.name,
      .layer = LayerShellLayer::Top,
      .anchor = anchor,
      .width = surfaceSpec.surfaceWidth,
      .height = surfaceSpec.surfaceHeight,
      .exclusiveZone = surfaceSpec.exclusiveZone,
      .marginTop = surfaceSpec.marginTop,
      .marginRight = surfaceSpec.marginRight,
      .marginBottom = surfaceSpec.marginBottom,
      .marginLeft = surfaceSpec.marginLeft,
      .defaultHeight = surfaceSpec.surfaceHeight,
  };

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  instance->surface->setRenderContext(m_renderContext);

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([this, inst](std::uint32_t width, std::uint32_t height) {
    buildScene(*inst, width, height);
  });
  instance->surface->setPrepareFrameCallback([this, inst](bool needsUpdate, bool needsLayout) {
    prepareFrame(*inst, needsUpdate, needsLayout);
  });
  instance->surface->setFrameTickCallback([inst](float deltaMs) {
    tickWidgets(inst->startWidgets, deltaMs);
    tickWidgets(inst->centerWidgets, deltaMs);
    tickWidgets(inst->endWidgets, deltaMs);
  });

  instance->surface->setAnimationManager(&instance->animations);
  populateWidgets(*instance);

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to initialize surface for output {}", output.name);
    return;
  }

  m_surfaceMap[instance->surface->wlSurface()] = instance.get();
  m_instances.push_back(std::move(instance));
}

void Bar::destroyInstance(std::uint32_t outputName) {
  std::erase_if(m_instances, [outputName](const auto& inst) { return inst->outputName == outputName; });
}

void Bar::populateWidgets(BarInstance& instance) {
  const auto& widgetConfigs = m_config->config().widgets;
  const FontWeight labelFontWeight = static_cast<FontWeight>(instance.barConfig.fontWeight);
  auto createWidgets = [&](const std::vector<std::string>& names, std::vector<std::unique_ptr<Widget>>& dest) {
    for (const auto& name : names) {
      auto widget = m_widgetFactory->create(
          name, instance.output, instance.barConfig.scale, instance.barConfig.position, instance.barConfig.name,
          static_cast<float>(instance.barConfig.widgetSpacing)
      );
      if (widget != nullptr) {
        widget->setConfigName(name);
        const WidgetConfig* wcPtr = nullptr;
        if (auto it = widgetConfigs.find(name); it != widgetConfigs.end()) {
          wcPtr = &it->second;
          widget->setAnchor(wcPtr->getBool("anchor", false));
        }
        widget->setBarCapsuleSpec(resolveWidgetBarCapsuleSpec(instance.barConfig, wcPtr));
        widget->setLabelFontWeight(labelFontWeight);
        if (wcPtr != nullptr && wcPtr->hasSetting("color")) {
          widget->setWidgetForeground(wcPtr->getOptionalColorSpec("color", "widget." + name + ".color"));
        } else if (instance.barConfig.widgetColor.has_value()) {
          widget->setWidgetForeground(*instance.barConfig.widgetColor);
        }
        dest.push_back(std::move(widget));
      }
    }
  };

  createWidgets(instance.barConfig.startWidgets, instance.startWidgets);
  createWidgets(instance.barConfig.centerWidgets, instance.centerWidgets);
  createWidgets(instance.barConfig.endWidgets, instance.endWidgets);

#ifndef NDEBUG
  // Prepend a red "debug" pill to the end section if running a debug build
  auto debugWidget = m_widgetFactory->create(
      "debug_indicator", instance.output, instance.barConfig.scale, instance.barConfig.position,
      instance.barConfig.name, static_cast<float>(instance.barConfig.widgetSpacing)
  );
  if (debugWidget != nullptr) {
    debugWidget->setConfigName("debug_indicator");
    debugWidget->setLabelFontWeight(labelFontWeight);
    debugWidget->create();
    instance.endWidgets.insert(instance.endWidgets.begin(), std::move(debugWidget));
  }
#endif
}

void Bar::attachWidgetsToSections(BarInstance& instance) {
  const bool isVertical = instance.barConfig.position == "left" || instance.barConfig.position == "right";
  const float widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);

  auto attach = [&](std::vector<std::unique_ptr<Widget>>& widgets, std::vector<BarCapsuleRun>& capsuleRuns,
                    Flex* section) {
    if (section == nullptr) {
      return;
    }

    for (auto& widget : widgets) {
      widget->setAnimationManager(&instance.animations);
      widget->setUpdateCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestUpdate();
        }
      });
      widget->setRedrawCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestRedraw();
        }
      });
      widget->setFrameTickRequestCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestFrameTick();
        }
      });
      if (auto* scripted = dynamic_cast<ScriptedWidget*>(widget.get()); scripted != nullptr) {
        scripted->setUpdateDeferralCallback([]() {
          auto* panel = PanelManager::current();
          return panel != nullptr && panel->isPanelTransitionActive();
        });
      }
      widget->setPanelToggleCallback([this, inst = &instance](
                                         std::string_view panelId, std::string_view context,
                                         std::optional<float> anchorSurfaceX, std::optional<float> anchorSurfaceY
                                     ) {
        float anchorX = inst->lastPointerSx;
        float anchorY = inst->lastPointerSy;
        if (anchorSurfaceX.has_value()) {
          anchorX = *anchorSurfaceX;
        }
        if (anchorSurfaceY.has_value()) {
          anchorY = *anchorSurfaceY;
        }
        if (m_platform != nullptr && inst->output != nullptr) {
          if (const auto* out = m_platform->findOutputByWl(inst->output);
              out != nullptr && out->logicalWidth > 0 && out->logicalHeight > 0) {
            const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*inst, *out);
            anchorX += surfaceX;
            anchorY += surfaceY;
          }
        }
        PanelManager::instance().togglePanel(
            std::string(panelId), PanelOpenRequest{
                                      .output = inst->output,
                                      .anchorX = anchorX,
                                      .anchorY = anchorY,
                                      .hasExplicitAnchor = anchorSurfaceX.has_value() || anchorSurfaceY.has_value(),
                                      .hasAnchorPosition = true,
                                      .context = context,
                                      .sourceBarName = inst->barConfig.name
                                  }
        );
      });
      widget->create();
    }

    capsuleRuns.clear();

    auto addPlainWidget = [&](Widget& widget) {
      widget.setBarCapsuleScene(nullptr, nullptr);
      auto* added = section->addChild(widget.releaseRoot());
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    auto addSingleCapsule = [&](Widget& widget) {
      const auto& cap = widget.barCapsuleSpec();
      auto shell = std::make_unique<Node>();
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget.contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = withOpacity(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, Style::borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));
      shellPtr->addChild(widget.releaseRoot());
      widget.setBarCapsuleScene(shellPtr, bgPtr);
      capsuleRuns.push_back(
          BarCapsuleRun{
              .shell = shellPtr,
              .bg = bgPtr,
              .container = nullptr,
              .content = widget.root(),
              .spec = cap,
              .contentScale = widget.contentScale(),
              .allowCircularSizing = true,
              .widgets = {&widget},
          }
      );
      auto* added = section->addChild(std::move(shell));
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    auto canJoinCapsuleGroup = [](const Widget& first, const Widget& next) {
      const auto& firstSpec = first.barCapsuleSpec();
      const auto& nextSpec = next.barCapsuleSpec();
      const bool sameCapsuleStyle = firstSpec.fill == nextSpec.fill && firstSpec.group == nextSpec.group &&
                                    firstSpec.border == nextSpec.border &&
                                    firstSpec.foreground == nextSpec.foreground &&
                                    firstSpec.radius == nextSpec.radius && firstSpec.opacity == nextSpec.opacity;
      return firstSpec.enabled && nextSpec.enabled && !first.isAnchor() && !next.isAnchor() &&
             !firstSpec.group.empty() && sameCapsuleStyle && first.contentScale() == next.contentScale();
    };

    std::size_t index = 0;
    while (index < widgets.size()) {
      auto& widget = widgets[index];
      if (widget->root() == nullptr) {
        ++index;
        continue;
      }

      const auto& cap = widget->barCapsuleSpec();
      if (!cap.enabled) {
        addPlainWidget(*widget);
        ++index;
        continue;
      }

      if (widget->isAnchor() || cap.group.empty()) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      std::size_t runEnd = index + 1;
      while (runEnd < widgets.size() && widgets[runEnd] != nullptr && widgets[runEnd]->root() != nullptr &&
             canJoinCapsuleGroup(*widget, *widgets[runEnd])) {
        ++runEnd;
      }

      if (runEnd - index < 2) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      auto shell = std::make_unique<Node>();
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget->contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = withOpacity(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, Style::borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));

      auto inner = ui::makeFlex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal, {
                                                                                .align = FlexAlign::Center,
                                                                                .gap = widgetSpacing,
                                                                            }
      );
      Flex* innerPtr = inner.get();
      shellPtr->addChild(std::move(inner));

      BarCapsuleRun run;
      run.shell = shellPtr;
      run.bg = bgPtr;
      run.container = innerPtr;
      run.content = innerPtr;
      run.spec = cap;
      run.contentScale = widget->contentScale();
      run.allowCircularSizing = false;

      for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
        auto& member = widgets[memberIndex];
        run.spec.padding = std::max(run.spec.padding, member->barCapsuleSpec().padding);
        member->setBarCapsuleScene(shellPtr, bgPtr);
        run.widgets.push_back(member.get());
        auto* added = innerPtr->addChild(member->releaseRoot());
        if (member->noGapAroundMe()) {
          innerPtr->setChildGapExcluded(added, true);
        }
      }

      capsuleRuns.push_back(std::move(run));
      section->addChild(std::move(shell));
      index = runEnd;
    }
  };

  attach(instance.startWidgets, instance.startCapsuleRuns, instance.startSection);
  attach(instance.centerWidgets, instance.centerCapsuleRuns, instance.centerSection);
  attach(instance.endWidgets, instance.endCapsuleRuns, instance.endSection);
}

void Bar::rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig) {
  // Drop any pointer hover/capture state pointing into the widgets we're about
  // to destroy. Hover will be re-acquired on the next pointer motion.
  instance.inputDispatcher.pointerLeave();

  instance.barConfig = newConfig;

  // Detach old widget root nodes from their sections and destroy the widgets.
  // Widgets release their root into the section on creation, so the section
  // owns those nodes — clearing the section frees the scene tree.
  auto clearChildren = [](Node* node) {
    if (node == nullptr) {
      return;
    }
    while (!node->children().empty()) {
      node->removeChild(node->children().back().get());
    }
  };
  clearChildren(instance.startSection);
  clearChildren(instance.centerSection);
  clearChildren(instance.endSection);
  instance.startWidgets.clear();
  instance.centerWidgets.clear();
  instance.endWidgets.clear();
  instance.startCapsuleRuns.clear();
  instance.centerCapsuleRuns.clear();
  instance.endCapsuleRuns.clear();

  // Refresh section-level layout knobs that may have changed (gap; direction
  // doesn't change because position is part of the surface-fields gate).
  const float widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  if (instance.startSection != nullptr) {
    instance.startSection->setGap(widgetSpacing);
  }
  if (instance.centerSection != nullptr) {
    instance.centerSection->setGap(widgetSpacing);
  }
  if (instance.endSection != nullptr) {
    instance.endSection->setGap(widgetSpacing);
  }

  populateWidgets(instance);
  attachWidgetsToSections(instance);

  applyBackgroundPalette(instance);
  syncBarSurfaceChrome(instance);

  if (instance.surface != nullptr) {
    // Re-run buildScene at the current surface size so radii / styling pick
    // up changes. The first-frame branch is skipped because sceneRoot is
    // already in place.
    const auto w = instance.surface->width();
    const auto h = instance.surface->height();
    if (w > 0 && h > 0) {
      buildScene(instance, w, h);
    }
    instance.surface->requestLayout();
  }
}

void Bar::tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) { ::tickWidgets(widgets, deltaMs); }

bool Bar::widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
  return ::widgetsNeedFrameTick(widgets);
}

bool Bar::instanceNeedsFrameTick(const BarInstance& instance) {
  return widgetsNeedFrameTick(instance.startWidgets) || widgetsNeedFrameTick(instance.centerWidgets) ||
         widgetsNeedFrameTick(instance.endWidgets);
}

void Bar::applyBackgroundPalette(BarInstance& instance) {
  if (instance.bg == nullptr) {
    return;
  }
  auto style = instance.bg->style();
  style.fill = colorForRole(ColorRole::Surface, instance.barConfig.backgroundOpacity);
  style.border = resolveColorSpec(instance.barConfig.border);
  style.borderWidth = instance.barConfig.borderWidth;
  instance.bg->setStyle(style);
}

void Bar::syncBarAutoHideInputRegion(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  if (!instanceAcceptsPointerInput(instance)) {
    instance.surface->setInputRegion({});
    return;
  }
  if (instance.barConfig.autoHide) {
    const bool fullSurface = instance.pointerInside || instance.attachedPopupCount > 0 || instance.hideOpacity > 0.5f;
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, fullSurface));
    return;
  }
  instance.surface->setInputRegion(
      {barContentInputRegion(instance.barConfig, m_config->config().shell.shadow, surfW, surfH)}
  );
}

void Bar::revealAutoHideBar(BarInstance& instance) {
  if (!instance.barConfig.autoHide || instance.surface == nullptr || instance.slideRoot == nullptr) {
    return;
  }

  instance.ipcLayoutReleased = false;
  instance.animations.cancelForOwner(instance.slideRoot);
  const float current = instance.hideOpacity;
  instance.animations.animate(
      current, 1.0f, Style::animNormal, Easing::EaseOutCubic, [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      }
  );
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::syncBarSlideLayerTransform(BarInstance& instance) const {
  if (instance.slideRoot == nullptr) {
    return;
  }
  if (instance.barConfig.autoHide) {
    const float t = 1.0f - instance.hideOpacity;
    instance.slideRoot->setPosition(instance.slideHiddenDx * t, instance.slideHiddenDy * t);
  } else {
    instance.slideRoot->setPosition(0.0f, 0.0f);
  }
}

void Bar::applyBarCompositorBlur(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  if (!barContentVisuallyShown(instance)) {
    instance.surface->clearBlurRegion();
    return;
  }

  if (instance.bg == nullptr) {
    return;
  }
  float absX = 0.0f;
  float absY = 0.0f;
  Node::absolutePosition(instance.bg, absX, absY);
  const int px = static_cast<int>(std::lround(absX));
  const int py = static_cast<int>(std::lround(absY));
  const int pw = static_cast<int>(std::lround(std::max(0.0f, instance.bg->width())));
  const int ph = static_cast<int>(std::lround(std::max(0.0f, instance.bg->height())));
  auto blurStrips = Surface::tessellateRoundedRect(
      px, py, pw, ph, static_cast<float>(instance.barConfig.radiusTopLeft),
      static_cast<float>(instance.barConfig.radiusTopRight), static_cast<float>(instance.barConfig.radiusBottomRight),
      static_cast<float>(instance.barConfig.radiusBottomLeft)
  );
  instance.surface->setBlurRegion(blurStrips);
}

void Bar::startHideFadeOut(BarInstance& instance) {
  const float current = instance.hideOpacity;
  instance.animations.animate(
      current, 0.0f, Style::animNormal, Easing::EaseInQuad,
      [this, inst = &instance](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this]() {
        if (inst->surface == nullptr) {
          return;
        }
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        inst->surface->requestRedraw();
      }
  );
  syncBarSurfaceChrome(instance);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::buildScene(BarInstance& instance, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("Bar::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }
  auto* renderer = m_renderContext;

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const float padding = static_cast<float>(instance.barConfig.padding);
  const float widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float shadowSize = shell::surface_shadow::enabled(instance.barConfig.shadow, shadowConfig)
                               ? static_cast<float>(shadowConfig.blur)
                               : 0.0f;
  const float shadowOffsetX = static_cast<float>(shadowConfig.offsetX);
  const float shadowOffsetY = static_cast<float>(shadowConfig.offsetY);
  const bool isBottom = instance.barConfig.position == "bottom";
  const bool isRight = instance.barConfig.position == "right";
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const Radii barRadii{
      static_cast<float>(instance.barConfig.radiusTopLeft),
      static_cast<float>(instance.barConfig.radiusTopRight),
      static_cast<float>(instance.barConfig.radiusBottomRight),
      static_cast<float>(instance.barConfig.radiusBottomLeft),
  };

  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h);
  const float barAreaX = barVisual.x;
  const float barAreaY = barVisual.y;
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = std::make_unique<Node>();
    instance.sceneRoot->setAnimationManager(&instance.animations);
    instance.sceneRoot->setSize(w, h);

    auto slide = std::make_unique<Node>();
    slide->setParticipatesInLayout(false);
    instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

    // Bar background
    instance.bg = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));

    // Shadow — bar shape copy rendered with large SDF softness to simulate a blurred drop shadow.
    if (shadowSize > 0.0f) {
      instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(
          ui::box({
              .configure = [](Box& shadow) { shadow.setHitTestVisible(false); },
          })
      ));

      auto leftClip = std::make_unique<Node>();
      leftClip->setClipChildren(true);
      leftClip->setZIndex(-1);
      instance.shadowLeftClip = instance.slideRoot->addChild(std::move(leftClip));
      instance.shadowLeft = static_cast<Box*>(instance.shadowLeftClip->addChild(ui::box()));

      auto rightClip = std::make_unique<Node>();
      rightClip->setClipChildren(true);
      rightClip->setZIndex(-1);
      instance.shadowRightClip = instance.slideRoot->addChild(std::move(rightClip));
      instance.shadowRight = static_cast<Box*>(instance.shadowRightClip->addChild(ui::box()));
    }
    // Note: shadow is inserted before bar sections so it renders below them (z=-1 is set below).

    auto contentClip = std::make_unique<Node>();
    contentClip->setClipChildren(true);
    instance.contentClip = instance.slideRoot->addChild(std::move(contentClip));

    auto makeSlot = [&instance]() {
      auto slot = std::make_unique<Node>();
      slot->setClipChildren(true);
      return instance.contentClip->addChild(std::move(slot));
    };
    instance.startSlot = makeSlot();
    instance.centerSlot = makeSlot();
    instance.endSlot = makeSlot();

    // Create section boxes
    auto makeSection = [widgetSpacing, isVertical]() {
      return ui::makeFlex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal, {
                                                                                .align = FlexAlign::Center,
                                                                                .gap = widgetSpacing,
                                                                            }
      );
    };

    instance.startSection = static_cast<Flex*>(instance.startSlot->addChild(makeSection()));
    instance.centerSection = static_cast<Flex*>(instance.centerSlot->addChild(makeSection()));
    instance.endSection = static_cast<Flex*>(instance.endSlot->addChild(makeSection()));

    attachWidgetsToSections(instance);

    // Wire up InputDispatcher for this instance
    instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
    instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    instance.inputDispatcher.setHoverChangeCallback([inst = &instance](InputArea* /*old*/, InputArea* next) {
      TooltipManager::instance().onHoverChange(next, inst->surface->layerSurface(), inst->output);
    });

    if (instance.barConfig.autoHide) {
      instance.slideRoot->setOpacity(1.0f);
      instance.hideOpacity = 0.0f;
    } else {
      instance.slideRoot->setOpacity(0.0f);
      instance.hideOpacity = 1.0f;
      instance.animations.animate(
          0.0f, 1.0f, Style::animSlow, Easing::EaseOutCubic,
          [slide = instance.slideRoot, inst = &instance, this](float v) {
            slide->setOpacity(v);
            syncBarSurfaceChrome(*inst);
          },
          {}, instance.slideRoot
      );
    }

    instance.surface->setSceneRoot(instance.sceneRoot.get());
  }

  // Update root size on reconfigure
  instance.sceneRoot->setSize(w, h);
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }

  // Background covers only the bar visual area (not the shadow extension).
  // Keep it exactly aligned with the shadow shape; the shadow shader now
  // draws only outside the rect, so any size mismatch is visible at corners.
  if (instance.bg != nullptr) {
    const RoundedRectStyle bgStyle{
        .fill = colorForRole(ColorRole::Surface, instance.barConfig.backgroundOpacity),
        .border = resolveColorSpec(instance.barConfig.border),
        .fillMode = FillMode::Solid,
        .radius = barRadii,
        .softness = 0.0f,
        .borderWidth = instance.barConfig.borderWidth,
    };
    instance.bg->setStyle(bgStyle);
    instance.bg->setPosition(barAreaX, barAreaY);
    instance.bg->setSize(barAreaW, barAreaH);
  }

  instance.paletteConn = paletteChanged().connect([inst = &instance] {
    applyBackgroundPalette(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  });
  if (instance.contentClip != nullptr) {
    instance.contentClip->setPosition(barAreaX, barAreaY);
    instance.contentClip->setSize(barAreaW, barAreaH);
  }

  applyBarShadowStyle(instance, shadowConfig, w, h);

  layoutBarSections(instance, *renderer, barAreaW, barAreaH, padding, isVertical);

  if (instance.barConfig.autoHide) {
    float contentLeft = barAreaX;
    float contentTop = barAreaY;
    float contentRight = barAreaX + barAreaW;
    float contentBottom = barAreaY + barAreaH;
    if (instance.shadow != nullptr) {
      const float sx = barAreaX + shadowOffsetX;
      const float sy = barAreaY + shadowOffsetY;
      contentLeft = std::min(contentLeft, sx);
      contentTop = std::min(contentTop, sy);
      contentRight = std::max(contentRight, sx + barAreaW);
      contentBottom = std::max(contentBottom, sy + barAreaH);
    }
    const auto hiddenDelta = computeAutoHideHiddenDelta(
        isVertical, isBottom, isRight, w, h, contentLeft, contentTop, contentRight, contentBottom
    );
    instance.slideHiddenDx = hiddenDelta.first;
    instance.slideHiddenDy = hiddenDelta.second;
  } else {
    instance.slideHiddenDx = 0.0f;
    instance.slideHiddenDy = 0.0f;
  }
  syncBarSlideLayerTransform(instance);

  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
}

void Bar::updateWidgets(BarInstance& instance) {
  if (m_renderContext == nullptr) {
    return;
  }
  auto* renderer = m_renderContext;

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const float padding = static_cast<float>(instance.barConfig.padding);
  const float barThickness = static_cast<float>(instance.barConfig.thickness);
  const float marginEnds = static_cast<float>(instance.barConfig.marginEnds);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto sbi = shell::surface_shadow::bleed(instance.barConfig.shadow, m_config->config().shell.shadow);
  const float bleedLeft = static_cast<float>(sbi.left);
  const float bleedRight = static_cast<float>(sbi.right);
  const float bleedUp = static_cast<float>(sbi.up);
  const float bleedDown = static_cast<float>(sbi.down);
  float barAreaW, barAreaH;
  if (isVertical) {
    const float barAreaY = std::min(marginEnds, bleedUp);
    barAreaW = barThickness;
    barAreaH = h - barAreaY - std::min(marginEnds, bleedDown);
  } else {
    const float barAreaX = std::min(marginEnds, bleedLeft);
    barAreaW = w - barAreaX - std::min(marginEnds, bleedRight);
    barAreaH = barThickness;
  }

  auto updateSection = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
    for (auto& widget : widgets) {
      if (widget->root() == nullptr) {
        continue;
      }
      widget->update(*renderer);
      widget->layout(*renderer, barAreaW, barAreaH);
    }
  };

  updateSection(instance.startWidgets);
  updateSection(instance.centerWidgets);
  updateSection(instance.endWidgets);
  layoutBarSections(instance, *renderer, barAreaW, barAreaH, padding, isVertical);
}

void Bar::prepareFrame(BarInstance& instance, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    updateWidgets(instance);
    return;
  }

  if (!needsLayout) {
    return;
  }

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const float padding = static_cast<float>(instance.barConfig.padding);
  const float barThickness = static_cast<float>(instance.barConfig.thickness);
  const float marginEnds = static_cast<float>(instance.barConfig.marginEnds);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto sbi = shell::surface_shadow::bleed(instance.barConfig.shadow, m_config->config().shell.shadow);
  const float bleedLeft = static_cast<float>(sbi.left);
  const float bleedRight = static_cast<float>(sbi.right);
  const float bleedUp = static_cast<float>(sbi.up);
  const float bleedDown = static_cast<float>(sbi.down);
  float barAreaW = 0.0f;
  float barAreaH = 0.0f;
  if (isVertical) {
    const float barAreaY = std::min(marginEnds, bleedUp);
    barAreaW = barThickness;
    barAreaH = h - barAreaY - std::min(marginEnds, bleedDown);
  } else {
    const float barAreaX = std::min(marginEnds, bleedLeft);
    barAreaW = w - barAreaX - std::min(marginEnds, bleedRight);
    barAreaH = barThickness;
  }

  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    for (auto& widget : instance.startWidgets) {
      widget->layout(*m_renderContext, barAreaW, barAreaH);
    }
    for (auto& widget : instance.centerWidgets) {
      widget->layout(*m_renderContext, barAreaW, barAreaH);
    }
    for (auto& widget : instance.endWidgets) {
      widget->layout(*m_renderContext, barAreaW, barAreaH);
    }
    layoutBarSections(instance, *m_renderContext, barAreaW, barAreaH, padding, isVertical);
  }
}

bool Bar::onPointerEvent(const PointerEvent& event) {
  bool consumed = false;
  BarInstance* targetInstance = nullptr;
  if (event.surface != nullptr) {
    targetInstance = instanceForSurface(event.surface);
  } else {
    targetInstance = m_hoveredInstance;
  }

  auto routeWidgetPopups = [&](BarInstance& instance) {
    auto routeGroup = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (widget != nullptr && widget->onPointerEvent(event)) {
          return true;
        }
      }
      return false;
    };
    return routeGroup(instance.startWidgets) || routeGroup(instance.centerWidgets) || routeGroup(instance.endWidgets);
  };
  if (targetInstance != nullptr) {
    if (!instanceAcceptsPointerInput(*targetInstance)) {
      clearInstancePointerState(*targetInstance);
      return false;
    }
    if (routeWidgetPopups(*targetInstance)) {
      return true;
    }
  } else {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && instanceAcceptsPointerInput(*instance) && routeWidgetPopups(*instance)) {
        return true;
      }
    }
  }

  if (targetInstance != nullptr && event.type == PointerEvent::Type::Button && event.button == BTN_MIDDLE &&
      event.state == 1 && m_config != nullptr && m_config->config().shell.middleClickOpensWidgetSettings) {
    auto* widget = widgetAtPoint(*targetInstance, static_cast<float>(event.sx), static_cast<float>(event.sy));
    if (widget != nullptr && !widget->reservesMiddleClick() && !widget->configName().empty() &&
        m_openWidgetSettingsCallback) {
      m_openWidgetSettingsCallback(targetInstance->barConfig.name, std::string(widget->configName()));
      return true;
    }
  }

  if (targetInstance != nullptr && targetInstance->attachedPopupCount > 0) {
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_hoveredInstance = targetInstance;
      targetInstance->pointerInside = true;
      break;
    case PointerEvent::Type::Leave:
      targetInstance->pointerInside = false;
      if (m_hoveredInstance == targetInstance) {
        m_hoveredInstance = nullptr;
      }
      break;
    case PointerEvent::Type::Motion:
    case PointerEvent::Type::Button:
    case PointerEvent::Type::Axis:
      if (event.type == PointerEvent::Type::Button && event.button == BTN_RIGHT && event.state == 1) {
        auto& panelManager = PanelManager::instance();
        if (panelManager.isOpenPanel("control-center")) {
          panelManager.closePanel();
        } else {
          float anchorX = static_cast<float>(event.sx);
          float anchorY = static_cast<float>(event.sy);
          if (m_platform != nullptr && targetInstance->output != nullptr) {
            if (const auto* out = m_platform->findOutputByWl(targetInstance->output);
                out != nullptr && out->logicalWidth > 0 && out->logicalHeight > 0) {
              const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*targetInstance, *out);
              anchorX += surfaceX;
              anchorY += surfaceY;
            }
          }
          panelManager.openPanel(
              "control-center", PanelOpenRequest{
                                    .output = targetInstance->output,
                                    .anchorX = anchorX,
                                    .anchorY = anchorY,
                                    .hasAnchorPosition = true,
                                    .context = "home",
                                    .sourceBarName = targetInstance->barConfig.name
                                }
          );
        }
        return true;
      }
      break;
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    auto it = m_surfaceMap.find(event.surface);
    if (it == m_surfaceMap.end()) {
      break;
    }
    m_hoveredInstance = it->second;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    m_hoveredInstance->pointerInside = true;
    m_hoveredInstance->inputDispatcher.pointerEnter(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial
    );
    if (m_hoveredInstance->barConfig.autoHide && m_hoveredInstance->sceneRoot != nullptr) {
      revealAutoHideBar(*m_hoveredInstance);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (m_hoveredInstance != nullptr) {
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*m_hoveredInstance) : false;
      if (m_hoveredInstance->barConfig.autoHide && !suppressAutoHide) {
        startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    m_hoveredInstance->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  }
  case PointerEvent::Type::Button: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    bool pressed = (event.state == 1); // WL_POINTER_BUTTON_STATE_PRESSED
    consumed = m_hoveredInstance->inputDispatcher.pointerButton(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
    );
    if (pressed && event.button == BTN_RIGHT && !consumed) {
      const float sx = static_cast<float>(event.sx);
      const float sy = static_cast<float>(event.sy);
      const bool insideAnySection = pointInsideNode(m_hoveredInstance->startSection, sx, sy) ||
                                    pointInsideNode(m_hoveredInstance->centerSection, sx, sy) ||
                                    pointInsideNode(m_hoveredInstance->endSection, sx, sy);
      const bool insideAnyWidget = widgetAtPoint(*m_hoveredInstance, sx, sy) != nullptr;
      if (!insideAnySection && !insideAnyWidget) {
        auto& panelManager = PanelManager::instance();
        if (panelManager.isOpenPanel("control-center")) {
          panelManager.closePanel();
        } else {
          float anchorX = sx;
          float anchorY = sy;
          if (m_platform != nullptr && m_hoveredInstance->output != nullptr) {
            if (const auto* out = m_platform->findOutputByWl(m_hoveredInstance->output);
                out != nullptr && out->logicalWidth > 0 && out->logicalHeight > 0) {
              const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*m_hoveredInstance, *out);
              anchorX += surfaceX;
              anchorY += surfaceY;
            }
          }
          panelManager.openPanel(
              "control-center", PanelOpenRequest{
                                    .output = m_hoveredInstance->output,
                                    .anchorX = anchorX,
                                    .anchorY = anchorY,
                                    .hasAnchorPosition = true,
                                    .context = "home",
                                    .sourceBarName = m_hoveredInstance->barConfig.name
                                }
          );
        }
        consumed = true;
      }
    }
    break;
  }
  case PointerEvent::Type::Axis: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    m_hoveredInstance->inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  }

  // Trigger redraw if any widget changed visual state
  if (m_hoveredInstance != nullptr && m_hoveredInstance->sceneRoot != nullptr &&
      (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
    if (m_hoveredInstance->sceneRoot->layoutDirty()) {
      m_hoveredInstance->surface->requestLayout();
    } else {
      m_hoveredInstance->surface->requestRedraw();
    }
  }

  return consumed;
}

BarInstance* Bar::instanceForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaceMap.find(surface);
  return it != m_surfaceMap.end() ? it->second : nullptr;
}

BarInstance* Bar::instanceForOutput(wl_output* output) const noexcept { return instanceForBar(output, {}); }

BarInstance* Bar::instanceForBar(wl_output* output, std::string_view barName) const noexcept {
  if (output == nullptr) {
    return nullptr;
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (barName.empty() || instance->barConfig.name == barName) {
      return instance.get();
    }
  }
  return nullptr;
}

std::string Bar::dispatchScriptedWidgetIpc(std::string_view args) {
  std::string widgetName;
  std::string target;
  std::string event;
  if (!takeIpcWord(args, widgetName) || !takeIpcWord(args, target) || !takeIpcWord(args, event)) {
    return std::string("error: usage: ") + std::string(kScriptedWidgetIpcUsage) + "\n";
  }

  const std::string payload(StringUtils::trimLeftView(args));

  const auto parsedTarget = parseScriptedWidgetIpcTarget(target);
  if (!parsedTarget.has_value()) {
    return std::string("error: usage: ") + std::string(kScriptedWidgetIpcUsage) + "\n";
  }

  auto collectWidget = [&](Widget* widget, std::vector<ScriptedWidgetIpcCandidate>& candidates) {
    if (widget == nullptr || widget->configName() != std::string_view(widgetName)) {
      return;
    }
    auto* scripted = dynamic_cast<ScriptedWidget*>(widget);
    if (scripted != nullptr) {
      candidates.push_back({.widget = scripted});
    }
  };

  auto collectGroup = [&](std::vector<std::unique_ptr<Widget>>& widgets,
                          std::vector<ScriptedWidgetIpcCandidate>& candidates) {
    for (auto& widget : widgets) {
      collectWidget(widget.get(), candidates);
    }
  };

  auto collectInstance = [&](BarInstance& instance, std::vector<ScriptedWidgetIpcCandidate>& candidates) {
    if (parsedTarget->hasBarName && instance.barConfig.name != parsedTarget->barName) {
      return;
    }
    collectGroup(instance.startWidgets, candidates);
    collectGroup(instance.centerWidgets, candidates);
    collectGroup(instance.endWidgets, candidates);
  };

  std::vector<ScriptedWidgetIpcCandidate> candidates;

  if (parsedTarget->allOutputs) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr) {
        collectInstance(*instance, candidates);
      }
    }
  } else {
    wl_output* output = nullptr;

    if (parsedTarget->outputSelector == "focused") {
      output = m_platform != nullptr ? m_platform->preferredInteractiveOutput() : nullptr;
    } else {
      std::vector<wl_output*> matches;
      if (m_platform != nullptr) {
        for (const auto& candidate : m_platform->outputs()) {
          if (candidate.output != nullptr && outputMatchesSelector(parsedTarget->outputSelector, candidate)) {
            matches.push_back(candidate.output);
          }
        }
      }
      if (matches.size() > 1) {
        return "error: target '" + target + "' matched multiple outputs; use a connector name or 'all'\n";
      }
      if (!matches.empty()) {
        output = matches.front();
      }
    }

    if (output != nullptr) {
      for (const auto& instance : m_instances) {
        if (instance != nullptr && instance->output == output) {
          collectInstance(*instance, candidates);
        }
      }
    }
  }

  if (candidates.empty()) {
    return "error: no scripted widget instance matched '" + widgetName + "' on target '" + target + "'\n";
  }

  if (!parsedTarget->allOutputs && candidates.size() > 1) {
    return "error: target '" + target +
           "' matched multiple scripted widget instances; use '<target>:<bar-name>' "
           "or 'all'\n";
  }

  ScriptedWidgetIpcCounts counts;
  for (const auto& candidate : candidates) {
    if (candidate.widget != nullptr) {
      recordScriptedWidgetIpcResult(counts, candidate.widget->dispatchIpcEvent(event, payload));
    }
  }

  if (counts.failed > 0) {
    if (counts.handled > 0) {
      return "error: dispatched " + std::to_string(counts.handled) + ", failed " + std::to_string(counts.failed) + "\n";
    }
    return "error: scripted widget onIpc callback failed\n";
  }
  if (counts.handled > 0) {
    return "ok: dispatched " + std::to_string(counts.handled) + "\n";
  }
  if (counts.missingHost > 0) {
    return "error: matched scripted widget is not ready\n";
  }
  if (counts.missingCallback > 0) {
    return "error: matched scripted widget has no onIpc callback\n";
  }
  return "error: no scripted widget instance matched '" + widgetName + "' on target '" + target + "'\n";
}

std::string Bar::setBarAutoHideIpc(std::string_view args) {
  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  const auto parts = StringUtils::splitWhitespace(StringUtils::trim(args));
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-auto-hide-set <on|off|true|false|1|0> [bar-name] [monitor-selector]\n";
  }

  const std::string value = parts[0];
  bool enabled = false;
  if (value == "on" || value == "true" || value == "1") {
    enabled = true;
  } else if (value == "off" || value == "false" || value == "0") {
    enabled = false;
  } else {
    return "error: invalid value (use on/off, true/false, 1/0)\n";
  }

  std::string barName = parts.size() >= 2 ? parts[1] : "default";
  const bool knownBar =
      std::any_of(m_config->config().bars.begin(), m_config->config().bars.end(), [&](const BarConfig& bar) {
        return bar.name == barName;
      });
  if (!knownBar) {
    std::vector<std::string> knownBars;
    knownBars.reserve(m_config->config().bars.size());
    for (const auto& bar : m_config->config().bars) {
      knownBars.push_back(bar.name);
    }
    const std::string suffix =
        knownBars.empty() ? std::string() : std::string("; known: ") + StringUtils::join(knownBars, ", ");
    return "error: unknown bar \"" + barName + "\"" + suffix + "\n";
  }

  auto applyTransientAutoHide = [this, enabled](BarInstance& instance) {
    instance.barConfig.autoHide = enabled;
    instance.ipcLayoutReleased = false;
    instance.animations.cancelForOwner(instance.slideRoot);

    if (enabled) {
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(instance) : false;
      instance.hideOpacity =
          (instance.pointerInside || instance.attachedPopupCount > 0 || suppressAutoHide) ? 1.0f : 0.0f;
      if (instance.slideRoot != nullptr) {
        instance.slideRoot->setOpacity(1.0f);
      }
    } else {
      instance.hideOpacity = 1.0f;
      if (instance.slideRoot != nullptr) {
        instance.slideRoot->setOpacity(1.0f);
      }
    }

    syncBarSlideLayerTransform(instance);
    if (instance.surface != nullptr) {
      const auto spec = computeBarSurfaceSpec(instance.barConfig, m_config->config().shell.shadow);
      instance.surface->setMargins(spec.marginTop, spec.marginRight, spec.marginBottom, spec.marginLeft);
      instance.surface->requestSize(spec.surfaceWidth, spec.surfaceHeight);
    }
    syncBarAutoHideInputRegion(instance);
    syncBarSurfaceChrome(instance);
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
  };

  if (parts.size() < 3) {
    std::size_t updated = 0;
    for (const auto& instance : m_instances) {
      if (instance == nullptr || instance->barConfig.name != barName) {
        continue;
      }
      applyTransientAutoHide(*instance);
      ++updated;
    }
    if (updated == 0) {
      return "error: no instances matched bar \"" + barName + "\"\n";
    }
    return "ok\n";
  }

  if (m_platform == nullptr) {
    return "error: bar service not initialized\n";
  }

  const std::string selector = parts[2];
  std::vector<std::string> matches;
  std::vector<std::string> knownOutputs;
  for (const auto& output : m_platform->outputs()) {
    if (output.connectorName.empty()) {
      continue;
    }
    knownOutputs.push_back(output.connectorName);
    if (outputMatchesSelector(selector, output)) {
      matches.push_back(output.connectorName);
    }
  }

  std::sort(knownOutputs.begin(), knownOutputs.end());
  knownOutputs.erase(std::unique(knownOutputs.begin(), knownOutputs.end()), knownOutputs.end());
  std::sort(matches.begin(), matches.end());
  matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

  if (matches.empty()) {
    std::string error = "error: unknown monitor selector \"" + selector + "\"";
    if (!knownOutputs.empty()) {
      error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
    }
    error += "\n";
    return error;
  }
  if (matches.size() > 1) {
    return "error: monitor selector \"" + selector +
           "\" matched multiple outputs: " + StringUtils::join(matches, ", ") + "\n";
  }

  std::size_t updated = 0;
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->barConfig.name != barName || instance->output == nullptr) {
      continue;
    }
    const auto it = std::find_if(
        m_platform->outputs().begin(), m_platform->outputs().end(),
        [&instance](const WaylandOutput& output) { return output.output == instance->output; }
    );
    if (it == m_platform->outputs().end() || it->connectorName != matches.front()) {
      continue;
    }
    applyTransientAutoHide(*instance);
    ++updated;
  }
  if (updated == 0) {
    return "error: no instances matched bar \"" + barName + "\" on \"" + matches.front() + "\"\n";
  }
  return "ok\n";
}

void Bar::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "bar-show",
      [this](const std::string&) -> std::string {
        show();
        return "ok\n";
      },
      "bar-show", "Show the bar"
  );

  ipc.registerHandler(
      "bar-hide",
      [this](const std::string&) -> std::string {
        hide();
        return "ok\n";
      },
      "bar-hide", "Hide the bar and release its layout gap"
  );

  ipc.registerHandler(
      "bar-toggle",
      [this](const std::string&) -> std::string {
        toggle();
        return "ok\n";
      },
      "bar-toggle", "Toggle bar visibility (participates in auto-hide when enabled)"
  );

  ipc.registerHandler(
      "bar-auto-hide-set", [this](const std::string& args) -> std::string { return setBarAutoHideIpc(args); },
      "bar-auto-hide-set <on|off|true|false|1|0> [bar-name] [monitor-selector]", "Set auto-hide state for a bar"
  );

  ipc.registerHandler(
      "scripted-widget", [this](const std::string& args) -> std::string { return dispatchScriptedWidgetIpc(args); },
      std::string(kScriptedWidgetIpcUsage), "Dispatch an event to a scripted bar widget"
  );
}
