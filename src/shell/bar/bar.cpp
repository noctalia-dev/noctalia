#include "shell/bar/bar.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/scoped_timer.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/bar/widget.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/panel/panel_manager.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("bar");
  constexpr std::chrono::milliseconds kWorkspaceRevealDebounce{80};
  constexpr std::chrono::milliseconds kWorkspacePeekHold{450};
  constexpr std::int32_t kAutoHideTriggerPx = 3;
  constexpr float kAutoHideSlideExtraPx = 4.0f;

  [[nodiscard]] std::string activeWorkspaceId(const std::vector<Workspace>& workspaces) {
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        if (!workspace.id.empty()) {
          return workspace.id;
        }
        if (!workspace.name.empty()) {
          return workspace.name;
        }
        return std::to_string(workspace.index);
      }
    }
    return {};
  }

  [[nodiscard]] bool barConfigUsesSlideSurface(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  [[nodiscard]] bool barSupportsSlideBehavior(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  [[nodiscard]] bool barPointerHideAllowed(const BarInstance& instance) noexcept {
    if (instance.barConfig.smartAutoHide) {
      return !instance.smartAutoHidePinnedVisible;
    }
    return instance.barConfig.autoHide;
  }

  [[nodiscard]] bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!workspace.id.empty() && assignmentKey == workspace.id) {
      return true;
    }
    if (!workspace.name.empty() && assignmentKey == workspace.name) {
      return true;
    }
    if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
      return true;
    }
    return false;
  }

  [[nodiscard]] bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
    const auto workspaces = platform.workspaces(output);
    const Workspace* active = nullptr;
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }
    if (active == nullptr) {
      return false;
    }

    const auto assignments = platform.workspaceWindowAssignments(output);
    for (const auto& assignment : assignments) {
      if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!assignments.empty()) {
      return false;
    }
    return active->occupied;
  }

  [[nodiscard]] bool smartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
    if (platform.hasOverviewState() && platform.isOverviewOpen()) {
      return true;
    }
    return !activeWorkspaceHasWindows(platform, output);
  }

  [[nodiscard]] FontWeight parseWidgetLabelFontWeight(const WidgetConfig& config, FontWeight fallback) {
    const auto it = config.settings.find("font_weight");
    if (it == config.settings.end()) {
      return fallback;
    }

    if (const auto* raw = std::get_if<std::int64_t>(&it->second)) {
      return static_cast<FontWeight>(*raw);
    }
    return fallback;
  }

  // `[widget.*] font_family` override; empty/whitespace or absent inherits the bar-resolved family.
  [[nodiscard]] std::string parseWidgetLabelFontFamily(const WidgetConfig& config, const std::string& fallback) {
    const auto it = config.settings.find("font_family");
    if (it == config.settings.end()) {
      return fallback;
    }
    if (const auto* raw = std::get_if<std::string>(&it->second)) {
      std::string trimmed = StringUtils::trim(*raw);
      if (!trimmed.empty()) {
        return trimmed;
      }
    }
    return fallback;
  }

  [[nodiscard]] int barAutoHideEdgeGutter(const BarConfig& cfg) noexcept {
    if (!barConfigUsesSlideSurface(cfg) || cfg.marginEdge <= 0) {
      return 0;
    }
    return cfg.marginEdge;
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
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr || widget->isBarClickThrough() || widget->root() == nullptr || !widget->root()->visible()) {
        continue;
      }
      if (Node::hitTest(widget->root(), sceneX, sceneY) != nullptr || pointInsideNode(widget->root(), sceneX, sceneY)) {
        return widget;
      }
    }
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr || widget->isBarClickThrough()) {
        continue;
      }
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
    const auto mTop = static_cast<float>(surface->marginTop());
    const auto mRight = static_cast<float>(surface->marginRight());
    const auto mBottom = static_cast<float>(surface->marginBottom());
    const auto mLeft = static_cast<float>(surface->marginLeft());
    const auto surfW = static_cast<float>(surface->width());
    const auto surfH = static_cast<float>(surface->height());
    const auto outputW = static_cast<float>(outputInfo.effectiveLogicalWidth());
    const auto outputH = static_cast<float>(outputInfo.effectiveLogicalHeight());

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

  bool isBarDeadZone(const BarInstance& instance, float sceneX, float sceneY) {
    if (widgetAtPoint(instance, sceneX, sceneY) != nullptr) {
      return false;
    }
    return pointInsideNode(instance.startSection, sceneX, sceneY)
        || pointInsideNode(instance.centerSection, sceneX, sceneY)
        || pointInsideNode(instance.endSection, sceneX, sceneY)
        || pointInsideNode(instance.sceneRoot.get(), sceneX, sceneY);
  }

  void executeDeadZoneCommand(const std::string& command) {
    if (command.empty()) {
      return;
    }
    if (!process::runAsync(command)) {
      kLog.warn("bar dead zone command failed: {}", command);
    }
  }

  float pointerScrollDelta(const PointerEvent& event) {
    if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL && event.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      return 0.0f;
    }

    if (event.axisValue120 != 0) {
      return static_cast<float>(event.axisValue120) / 120.0f;
    }
    if (event.axisDiscrete != 0) {
      return static_cast<float>(event.axisDiscrete);
    }
    if (event.axisValue != 0.0) {
      return static_cast<float>(event.axisValue);
    }
    return 0.0f;
  }

  void openControlCenterAtBarPointer(
      BarInstance& instance, float sx, float sy, CompositorPlatform* platform, std::string_view sourceBarName
  ) {
    auto& panelManager = PanelManager::instance();
    if (panelManager.isOpenPanel("control-center")) {
      panelManager.closePanel();
      return;
    }

    float anchorX = sx;
    float anchorY = sy;
    if (platform != nullptr && instance.output != nullptr) {
      if (const auto* out = platform->findOutputByWl(instance.output); out != nullptr && out->hasUsableGeometry()) {
        const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(instance, *out);
        anchorX += surfaceX;
        anchorY += surfaceY;
      }
    }
    panelManager.openPanel(
        "control-center",
        PanelOpenRequest{
            .output = instance.output,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .hasAnchorPosition = true,
            .context = "home",
            .sourceBarName = std::string(sourceBarName),
        }
    );
  }

  bool handleBarDeadZoneButton(
      BarInstance& instance, float sx, float sy, std::uint32_t button, CompositorPlatform* platform
  ) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }

    const auto& deadZone = instance.barConfig.deadZone;
    if (button == BTN_LEFT && !deadZone.command.empty()) {
      executeDeadZoneCommand(deadZone.command);
      return true;
    }
    if (button == BTN_RIGHT) {
      if (!deadZone.rightCommand.empty()) {
        executeDeadZoneCommand(deadZone.rightCommand);
        return true;
      }
      openControlCenterAtBarPointer(instance, sx, sy, platform, instance.barConfig.name);
      return true;
    }
    if (button == BTN_MIDDLE && !deadZone.middleCommand.empty()) {
      executeDeadZoneCommand(deadZone.middleCommand);
      return true;
    }
    return false;
  }

  bool handleBarDeadZoneAxis(BarInstance& instance, float sx, float sy, const PointerEvent& event) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }

    const float delta = pointerScrollDelta(event);
    if (delta == 0.0f) {
      return false;
    }

    const auto& deadZone = instance.barConfig.deadZone;
    const std::string& command = delta < 0.0f ? deadZone.scrollUpCommand : deadZone.scrollDownCommand;
    if (command.empty()) {
      return false;
    }

    executeDeadZoneCommand(command);
    return true;
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

  ColorSpec withOpacity(const ColorSpec& color, float opacity) {
    ColorSpec out = color;
    out.alpha = std::clamp(out.alpha * std::clamp(opacity, 0.0f, 1.0f), 0.0f, 1.0f);
    return out;
  }

  // Hover highlight: peak fill alpha of the widget-foreground tint, and the cross-axis inset
  // (logical px, content-scaled) of per-member pills inside capsule groups.
  constexpr float kWidgetHoverFillAlpha = 0.1f;
  constexpr float kGroupHoverCrossInset = 2.0f;

  // Sizes a run's hover overlays after the capsule geometry is final. Single runs (and plain
  // widgets) get one overlay covering the whole shell; group runs get one pill per member so
  // only the hovered member lights up.
  void placeCapsuleHoverBoxes(
      BarCapsuleRun& run, bool isVertical, float shellW, float shellH, float contentX, float contentY,
      float capsuleRadius
  ) {
    if (run.hoverBoxes.empty()) {
      return;
    }
    if (run.container == nullptr) {
      Box* box = run.hoverBoxes.front();
      if (box == nullptr) {
        return;
      }
      box->setPosition(0.0f, 0.0f);
      box->setSize(shellW, shellH);
      box->setRadius(capsuleRadius);
      return;
    }
    const float scale = run.contentScale;
    const float crossInset = kGroupHoverCrossInset * scale;
    // Same breathing room a single capsule would give the member; pills may reach into the
    // gap toward a neighbor, which is fine — only the hovered member's pill is visible.
    const float mainPad = run.spec.padding * scale;
    const float shellMain = isVertical ? shellH : shellW;
    const float shellCross = isVertical ? shellW : shellH;
    const float contentMain = isVertical ? contentY : contentX;
    const float crossExtent = std::max(0.0f, shellCross - 2.0f * crossInset);
    for (std::size_t i = 0; i < run.widgets.size() && i < run.hoverBoxes.size(); ++i) {
      Node* root = run.widgets[i] != nullptr ? run.widgets[i]->root() : nullptr;
      Box* box = run.hoverBoxes[i];
      if (root == nullptr || box == nullptr) {
        continue;
      }
      // Skip hidden members — Flex leaves them at stale (0,0) geometry.
      if (!root->visible() || !root->participatesInLayout()) {
        box->setSize(0.0f, 0.0f);
        continue;
      }
      const float rootStart = contentMain + (isVertical ? root->y() : root->x());
      const float rootExtent = isVertical ? root->height() : root->width();
      const float mainStart = std::max(0.0f, rootStart - mainPad);
      const float mainExtent = std::max(0.0f, std::min(shellMain, rootStart + rootExtent + mainPad) - mainStart);
      if (isVertical) {
        box->setPosition(crossInset, mainStart);
        box->setSize(crossExtent, mainExtent);
      } else {
        box->setPosition(mainStart, crossInset);
        box->setSize(mainExtent, crossExtent);
      }
      const float pillRadius = std::max(0.0f, std::min(box->width(), box->height()) * 0.5f);
      box->setRadius(std::min(pillRadius, std::max(0.0f, capsuleRadius - crossInset)));
    }
  }

  // Extends each member's hit target across the capsule padding and half the gap to its
  // neighbors, so hover/click coverage matches the capsule ink instead of stopping at the
  // content edge. Runs after applyBarWidgetHitTargets (which replaces outsets each layout).
  void extendCapsuleHitTargets(std::vector<BarCapsuleRun>& runs, bool isVertical) {
    for (auto& run : runs) {
      if (run.shell == nullptr || run.hoverBoxes.empty()) {
        continue;
      }

      // Single runs (capsule or ghost pill): the hover overlay rect is the hit region.
      if (run.container == nullptr) {
        Widget* widget = !run.widgets.empty() ? run.widgets.front() : nullptr;
        Box* box = run.hoverBoxes.front();
        auto* area = widget != nullptr ? dynamic_cast<InputArea*>(widget->root()) : nullptr;
        if (area == nullptr || box == nullptr || !area->visible() || !area->participatesInLayout()) {
          continue;
        }
        const float areaStart = isVertical ? area->y() : area->x();
        const float areaEnd = areaStart + (isVertical ? area->height() : area->width());
        const float boxStart = isVertical ? box->y() : box->x();
        const float boxEnd = boxStart + (isVertical ? box->height() : box->width());
        auto outset = area->hitTestOutset();
        if (isVertical) {
          outset.top += std::max(0.0f, areaStart - boxStart);
          outset.bottom += std::max(0.0f, boxEnd - areaEnd);
        } else {
          outset.left += std::max(0.0f, areaStart - boxStart);
          outset.right += std::max(0.0f, boxEnd - areaEnd);
        }
        area->setHitTestOutset(outset);
        continue;
      }

      // Tile only laid-out members; hidden ones keep stale geometry.
      // Use Node visibility, some widgets (e.g. tray) root on Flex, not InputArea.
      std::vector<std::size_t> laidOut;
      laidOut.reserve(run.widgets.size());
      for (std::size_t i = 0; i < run.widgets.size(); ++i) {
        Widget* widget = run.widgets[i];
        auto* root = widget != nullptr ? widget->root() : nullptr;
        if (root == nullptr || !root->visible() || !root->participatesInLayout()) {
          continue;
        }
        laidOut.push_back(i);
      }

      const float shellMain = isVertical ? run.shell->height() : run.shell->width();
      const float containerMain = isVertical ? run.container->y() : run.container->x();
      auto memberStart = [&](std::size_t i) {
        const Node* root = run.widgets[i]->root();
        return containerMain + (isVertical ? root->y() : root->x());
      };
      auto memberEnd = [&](std::size_t i) {
        const Node* root = run.widgets[i]->root();
        return memberStart(i) + (isVertical ? root->height() : root->width());
      };
      for (std::size_t vi = 0; vi < laidOut.size(); ++vi) {
        const std::size_t i = laidOut[vi];
        auto* area = dynamic_cast<InputArea*>(run.widgets[i]->root());
        if (area == nullptr) {
          continue;
        }
        const float sliceStart = vi > 0 ? (memberEnd(laidOut[vi - 1]) + memberStart(i)) * 0.5f : 0.0f;
        const float sliceEnd =
            vi + 1 < laidOut.size() ? (memberEnd(i) + memberStart(laidOut[vi + 1])) * 0.5f : shellMain;
        auto outset = area->hitTestOutset();
        const float before = std::max(0.0f, memberStart(i) - sliceStart);
        const float after = std::max(0.0f, sliceEnd - memberEnd(i));
        if (isVertical) {
          outset.top += before;
          outset.bottom += after;
        } else {
          outset.left += before;
          outset.right += after;
        }
        area->setHitTestOutset(outset);
      }
    }
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
    const auto concave = barConcaveShape(barConfig);
    const int insetL = static_cast<int>(std::ceil(std::max(0.0f, concave.logicalInset.left)));
    const int insetT = static_cast<int>(std::ceil(std::max(0.0f, concave.logicalInset.top)));
    const int insetR = static_cast<int>(std::ceil(std::max(0.0f, concave.logicalInset.right)));
    const int insetB = static_cast<int>(std::ceil(std::max(0.0f, concave.logicalInset.bottom)));
    // Reserve room for a concave-corner spike on the inner edge (opaque bar material),
    // in addition to the shadow bleed that renders beyond the spike tips.
    const int concaveBulge = static_cast<int>(std::lround(concave.innerBulge));

    const std::int32_t edgeMargin = barEdgeLayerMargin(barConfig, shadowConfig);

    BarSurfaceSpec spec;
    if (!vertical) {
      spec.marginLeft = std::max(0, mEnds - sb.left - insetL);
      spec.marginRight = std::max(0, mEnds - sb.right - insetR);
      if (isBottom) {
        if (edgeGutter > 0) {
          // Surface reaches the screen edge (no layer margin); the margin is folded
          // into the surface as a gutter on the edge side. Do not add the edge-side
          // bleed here — it lives inside the gutter, not beyond it.
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginBottom = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + std::min(mEdge, sb.down));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.down + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginTop = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(std::min(mEdge, sb.up) + barConfig.thickness + sb.down + concaveBulge);
        }
      }
    } else {
      spec.marginTop = std::max(0, mEnds - sb.up - insetT);
      spec.marginBottom = std::max(0, mEnds - sb.down - insetB);
      if (isRight) {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginRight = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + std::min(mEdge, sb.right));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.right + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginLeft = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(std::min(mEdge, sb.left) + barConfig.thickness + sb.right + concaveBulge);
        }
      }
    }

    spec.exclusiveZone = barConfig.reserveSpace ? reservedBarExclusiveZone(barConfig, shadowConfig) : 0;
    return spec;
  }

  [[nodiscard]] float barInnerSurfaceExtension(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight
  ) {
    const auto base = computeBarSurfaceSpec(cfg, shadow);
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const float current = isVertical ? surfaceWidth : surfaceHeight;
    const auto normal = static_cast<float>(isVertical ? base.surfaceWidth : base.surfaceHeight);
    return std::max(0.0f, current - normal);
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
    return a.name == b.name
        && a.position == b.position
        && a.enabled == b.enabled
        && a.autoHide == b.autoHide
        && a.smartAutoHide == b.smartAutoHide
        && a.reserveSpace == b.reserveSpace
        && a.layer == b.layer
        && a.thickness == b.thickness
        // Corner radii feed the concave-corner bulge, which changes the surface size.
        && a.radiusTopLeft == b.radiusTopLeft
        && a.radiusTopRight == b.radiusTopRight
        && a.radiusBottomLeft == b.radiusBottomLeft
        && a.radiusBottomRight == b.radiusBottomRight
        && a.concaveEdgeCorners == b.concaveEdgeCorners
        && a.marginEnds == b.marginEnds
        && a.marginEdge == b.marginEdge
        && a.shadow == b.shadow
        && sameShadowSurface
        && a.monitorOverrides == b.monitorOverrides;
  }

  bool barSurfaceOrderRequiresRecreate(const std::vector<BarConfig>& previous, const std::vector<BarConfig>& next) {
    std::vector<std::string> preserved;
    preserved.reserve(previous.size());
    for (const auto& oldBar : previous) {
      const auto it = std::ranges::find(next, oldBar.name, &BarConfig::name);
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
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight,
      float innerSurfaceExtension = 0.0f
  ) {
    const auto barThickness = static_cast<float>(cfg.thickness);
    const auto marginEnds = static_cast<float>(cfg.marginEnds);
    const auto marginEdge = static_cast<float>(cfg.marginEdge);
    const bool isBottom = cfg.position == "bottom";
    const bool isRight = cfg.position == "right";
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const auto sbi = shell::surface_shadow::bleed(cfg.shadow, shadow);
    const auto concave = barConcaveShape(cfg);
    const float insetL = std::ceil(std::max(0.0f, concave.logicalInset.left));
    const float insetT = std::ceil(std::max(0.0f, concave.logicalInset.top));
    const float insetR = std::ceil(std::max(0.0f, concave.logicalInset.right));
    const float insetB = std::ceil(std::max(0.0f, concave.logicalInset.bottom));
    const auto bleedLeft = static_cast<float>(sbi.left);
    const auto bleedRight = static_cast<float>(sbi.right);
    const auto bleedUp = static_cast<float>(sbi.up);
    const auto bleedDown = static_cast<float>(sbi.down);
    // For bottom/right bars the inner edge is the origin side, so the concave spike
    // pushes the body inward by its bulge. Top/left bars grow away from the origin
    // and need no body shift. Gutter (auto-hide) placements derive from the surface
    // size, which already includes the bulge, so they shift automatically.
    const float concaveBulge = concave.innerBulge;

    if (isVertical) {
      // Vertical bar: edge gap is left/right, ends inset is top/bottom.
      const float y = std::min(marginEnds, bleedUp) + insetT;
      float x = isRight ? (bleedLeft + concaveBulge) : std::min(marginEdge, bleedLeft);
      if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
        // The gutter equals marginEdge and sits between the screen edge and the bar.
        // Position the bar exactly marginEdge from the edge so it matches the
        // non-auto-hide placement; the edge-side shadow bleeds into the gutter.
        if (isRight) {
          x = surfaceWidth - static_cast<float>(gutter) - barThickness;
        } else {
          x = static_cast<float>(gutter);
        }
      } else if (isRight) {
        x += innerSurfaceExtension;
      }
      return {
          .x = x,
          .y = y,
          .width = barThickness,
          .height = surfaceHeight - y - std::min(marginEnds, bleedDown) - insetB,
      };
    }

    // Horizontal bar: edge gap is top/bottom, ends inset is left/right.
    const float x = std::min(marginEnds, bleedLeft) + insetL;
    float y = isBottom ? (bleedUp + concaveBulge) : std::min(marginEdge, bleedUp);
    if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
      if (isBottom) {
        y = surfaceHeight - static_cast<float>(gutter) - barThickness;
      } else {
        y = static_cast<float>(gutter);
      }
    } else if (isBottom) {
      y += innerSurfaceExtension;
    }
    return {
        .x = x,
        .y = y,
        .width = surfaceWidth - x - std::min(marginEnds, bleedRight) - insetR,
        .height = barThickness,
    };
  }

  [[nodiscard]] InputRect
  barContentInputRegion(const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, int surfW, int surfH) {
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH));
    const auto barVisual = computeBarVisualGeometry(
        cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH), innerSurfaceExtension
    );
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

    const auto concave = barConcaveShape(instance.barConfig);
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight);
    const auto barVisual =
        computeBarVisualGeometry(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight, innerSurfaceExtension);
    // Shadow follows the same shape as the background: the body expanded outward by
    // the concave inset into the visual rect, so concave spikes cast a matching shadow.
    const float barAreaW = barVisual.width + concave.logicalInset.left + concave.logicalInset.right;
    const float barAreaH = barVisual.height + concave.logicalInset.top + concave.logicalInset.bottom;
    const float bgOpacity = std::clamp(instance.barConfig.backgroundOpacity, 0.0f, 1.0f);
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    const float shadowX = barVisual.x - concave.logicalInset.left + static_cast<float>(shadowOff.x);
    const float shadowY = barVisual.y - concave.logicalInset.top + static_cast<float>(shadowOff.y);
    RoundedRectStyle shadowStyle = shell::surface_shadow::style(
        shadowConfig, bgOpacity,
        shell::surface_shadow::Shape{
            .corners = concave.corners, .logicalInset = concave.logicalInset, .radius = concave.radii
        }
    );

    const bool panelShadowExclusion = instance.attachedPanelGeometry.has_value()
        && instance.attachedPanelGeometry->width > 0.0f
        && instance.attachedPanelGeometry->height > 0.0f;
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

    // Capsule cross-size is a fraction of the bar thickness (capsule_thickness), the same for every capsule
    // regardless of per-widget content scale. The max() guard keeps a thin bar from yielding a 0px capsule.
    const float capsuleCross = std::max(1.0f, std::round(slotCross * instance.barConfig.capsuleThickness));
    auto finalizeCapsules = [isVertical, capsuleCross, &renderer](std::vector<BarCapsuleRun>& runs) {
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
          placeCapsuleHoverBoxes(run, isVertical, iw, ih, 0.0f, 0.0f, std::min(iw, ih) * 0.5f);
          continue;
        }
        const float pad = run.spec.padding * scale;
        const float padMain = pad;
        // Cross-size is the fixed capsuleCross, independent of per-widget content scale: scaling a widget
        // enlarges its glyph/text inside the fixed-height pill rather than resizing the capsule (so a
        // differently scaled member can't grow or split its capsule group). The main axis is content plus
        // per-widget padding, so an icon-only widget reads as a near-circular pill at the default padding
        // and widens as padding increases.
        const float shellMain = (isVertical ? ih : iw) + 2.0f * padMain;
        const float shellCross = capsuleCross;
        const float shellW = isVertical ? shellCross : shellMain;
        const float shellH = isVertical ? shellMain : shellCross;
        const float contentX = (shellW - iw) * 0.5f;
        const float contentY = (shellH - ih) * 0.5f;
        shell->setSize(shellW, shellH);
        bg->setVisible(true);
        bg->setPosition(0.0f, 0.0f);
        bg->setSize(shellW, shellH);
        content->setPosition(contentX, contentY);
        const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
        const float capsuleRadius = radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(shellW, shellH)
                                                            : std::max(0.0f, std::min(shellW, shellH) * 0.5f);
        bg->setRadius(capsuleRadius);
        placeCapsuleHoverBoxes(run, isVertical, shellW, shellH, contentX, contentY, capsuleRadius);
      }
    };
    finalizeCapsules(instance.startCapsuleRuns);
    finalizeCapsules(instance.centerCapsuleRuns);
    finalizeCapsules(instance.endCapsuleRuns);

    // When bar touches screen edge, put the padding inside the sections, and extend the hit targets of
    // the first/last widgets to cover the area. So clicking on the screen edge still triggers the widget.
    const bool screenEdgeClick = instance.barConfig.marginEnds == 0 && padding > 0;
    const float paddingInsideSection = screenEdgeClick ? padding : 0.0f;
    const float contentMainStart = screenEdgeClick ? 0.0f : padding;
    const float contentMainEnd =
        std::max(contentMainStart, (isVertical ? barAreaH : barAreaW) - (screenEdgeClick ? 0.0f : padding));
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

    if (screenEdgeClick) {
      if (isVertical) {
        instance.startSection->setPadding(paddingInsideSection, 0.0f, 0.0f, 0.0f);
        instance.endSection->setPadding(0.0f, 0.0f, paddingInsideSection, 0.0f);
      } else {
        instance.startSection->setPadding(0.0f, 0.0f, 0.0f, paddingInsideSection);
        instance.endSection->setPadding(0.0f, paddingInsideSection, 0.0f, 0.0f);
      }
    } else {
      instance.startSection->setPadding(0.0f);
      instance.endSection->setPadding(0.0f);
    }

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
    float startSlotMain;
    float endSlotMain;
    if (!instance.centerWidgets.empty()) {
      startSlotMain = std::max(0.0f, centerSlotStart - contentMainStart);
      endSlotMain = std::max(0.0f, contentMainEnd - centerSlotEnd);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, centerSlotStart, centerSlotMain);
      configureSlot(instance.endSlot, centerSlotEnd, endSlotMain);
    } else {
      // Allow start/end sections to take the full width if center is empty
      const float startNaturalMain = isVertical ? instance.startSection->height() : instance.startSection->width();
      const float endNaturalMain = isVertical ? instance.endSection->height() : instance.endSection->width();

      // Prioritize end section, because control center is likely to be there, and we don't
      // want it to be clipped by a super long start section, so the user loses access to settings.
      endSlotMain = std::min(endNaturalMain, contentMainSpan);
      startSlotMain = std::min(startNaturalMain, contentMainSpan - endSlotMain);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, contentMainStart + startSlotMain, 0.0f);
      configureSlot(instance.endSlot, contentMainEnd - endSlotMain, endSlotMain);
    }

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
    extendCapsuleHitTargets(instance.startCapsuleRuns, isVertical);
    extendCapsuleHitTargets(instance.centerCapsuleRuns, isVertical);
    extendCapsuleHitTargets(instance.endCapsuleRuns, isVertical);

    // Ghost pills for capsule-less widgets: positioned on the bar-level underlay with the
    // metrics an enabled capsule would have (capsuleCross across the bar, capsule padding
    // along it). Runs after sections are positioned so absolute coordinates are final; the
    // widget's hit target is widened to match the pill.
    if (instance.hoverUnderlay != nullptr) {
      float underlayX = 0.0f;
      float underlayY = 0.0f;
      Node::absolutePosition(instance.hoverUnderlay, underlayX, underlayY);
      auto placeGhostPills = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
        for (auto& widget : widgets) {
          Box* box = widget->barHoverBox();
          Node* root = widget->root();
          if (box == nullptr || root == nullptr || widget->barCapsuleShell() != nullptr) {
            continue;
          }
          if (!root->visible() || !root->participatesInLayout()) {
            box->setSize(0.0f, 0.0f);
            continue;
          }
          float rootX = 0.0f;
          float rootY = 0.0f;
          Node::absolutePosition(root, rootX, rootY);
          rootX -= underlayX;
          rootY -= underlayY;
          const float mainExtent = isVertical ? root->height() : root->width();
          const float pad = mainExtent > 0.5f ? widget->barCapsuleSpec().padding * widget->contentScale() : 0.0f;
          const float hoverW = isVertical ? capsuleCross : root->width() + 2.0f * pad;
          const float hoverH = isVertical ? root->height() + 2.0f * pad : capsuleCross;
          box->setPosition(
              isVertical ? rootX + (root->width() - capsuleCross) * 0.5f : rootX - pad,
              isVertical ? rootY - pad : rootY + (root->height() - capsuleCross) * 0.5f
          );
          box->setSize(hoverW, hoverH);
          box->setRadius(widget->resolvedBarCapsuleRadius(hoverW, hoverH));
          if (auto* area = dynamic_cast<InputArea*>(root)) {
            auto outset = area->hitTestOutset();
            if (isVertical) {
              outset.top += pad;
              outset.bottom += pad;
            } else {
              outset.left += pad;
              outset.right += pad;
            }
            area->setHitTestOutset(outset);
          }
        }
      };
      placeGhostPills(instance.startWidgets);
      placeGhostPills(instance.centerWidgets);
      placeGhostPills(instance.endWidgets);
    }
    if (screenEdgeClick) {
      if (!instance.startSection->children().empty()) {
        auto node = instance.startSection->children().front().get();
        auto hitTestOutset = node->hitTestOutset();
        if (isVertical) {
          hitTestOutset.top += paddingInsideSection;
        } else {
          hitTestOutset.left += paddingInsideSection;
        }
        node->setHitTestOutset(hitTestOutset);
      }
      if (!instance.endSection->children().empty()) {
        auto node = instance.endSection->children().back().get();
        auto hitTestOutset = node->hitTestOutset();
        if (isVertical) {
          hitTestOutset.bottom += paddingInsideSection;
        } else {
          hitTestOutset.right += paddingInsideSection;
        }
        node->setHitTestOutset(hitTestOutset);
      }
    }
  }

  void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) {
    for (auto& widget : widgets) {
      if (widget != nullptr && widget->needsFrameTick()) {
        widget->onFrameTick(deltaMs);
      }
    }
  }

  bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::ranges::any_of(widgets, [](const auto& widget) {
      return widget != nullptr && widget->needsFrameTick();
    });
  }

} // namespace

Bar::Bar() = default;

bool Bar::initialize(const BarServices& services) {
  m_platform = &services.platform;
  m_config = &services.config;
  m_notifications = services.notifications;
  m_tray = services.tray;
  m_audio = services.audio;
  m_easyEffects = services.easyEffects;
  m_upower = services.upower;
  m_sysmon = services.sysmon;
  m_powerProfiles = services.powerProfiles;
  m_network = services.network;
  m_idleInhibitor = services.idleInhibitor;
  m_mpris = services.mpris;
  m_audioSpectrum = services.audioSpectrum;
  m_httpClient = services.httpClient;
  m_weatherService = services.weather;
  m_renderContext = services.renderContext;
  m_nightLight = services.nightLight;
  m_themeService = services.theme;
  m_bluetooth = services.bluetooth;
  m_brightness = services.brightness;
  m_lockKeys = services.lockKeys;
  m_clipboard = services.clipboard;
  m_fileWatcher = services.fileWatcher;
  m_screenshots = services.screenshots;
  m_scriptApi = services.scriptApi;

  m_widgetFactory = std::make_unique<WidgetFactory>(services);

  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastPlugins = m_config->config().plugins;
  m_config->addReloadCallback(
      [this]() {
        const auto& cfg = m_config->config();
        if (cfg.bars == m_lastBars
            && cfg.widgets == m_lastWidgets
            && cfg.shell.shadow == m_lastShadow
            && cfg.plugins == m_lastPlugins) {
          return;
        }
        reload();
      },
      "bar"
  );

  return true;
}

BarServices Bar::services() const {
  return {
      .platform = *m_platform,
      .config = *m_config,
      .notifications = m_notifications,
      .tray = m_tray,
      .audio = m_audio,
      .easyEffects = m_easyEffects,
      .upower = m_upower,
      .sysmon = m_sysmon,
      .powerProfiles = m_powerProfiles,
      .network = m_network,
      .idleInhibitor = m_idleInhibitor,
      .mpris = m_mpris,
      .audioSpectrum = m_audioSpectrum,
      .httpClient = m_httpClient,
      .weather = m_weatherService,
      .renderContext = m_renderContext,
      .nightLight = m_nightLight,
      .theme = m_themeService,
      .bluetooth = m_bluetooth,
      .brightness = m_brightness,
      .lockKeys = m_lockKeys,
      .clipboard = m_clipboard,
      .fileWatcher = m_fileWatcher,
      .screenshots = m_screenshots,
      .scriptApi = m_scriptApi,
  };
}

void Bar::onSecondTick() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
    }
  }
}

void Bar::reload() {
  noctalia::profiling::ScopedTimer t(kLog, "bar: reload (all instances)");
  kLog.info("reloading config");
  const auto previousBars = m_lastBars;
  const auto previousShadow = m_lastShadow;
  const bool recreateForOrder = barSurfaceOrderRequiresRecreate(previousBars, m_config->config().bars);
  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastPlugins = m_config->config().plugins;
  m_widgetFactory = std::make_unique<WidgetFactory>(services());

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

  // Exclusive-zone geometry on an output depends on the order its bar surfaces are
  // created: bars on the same edge stack in creation order, and bars on adjacent
  // edges (e.g. top + left) compete for the shared corner the same way. Rebuilding
  // one bar's surface in place while recreating another would commit them out of
  // config order and reshuffle that geometry. So if any bar on an output needs a
  // surface recreate, recreate every bar on that output — syncInstances rebuilds
  // them in config order. Scoped per output so other monitors are untouched.
  const auto needsSurfaceRecreate = [&](const BarInstance& inst) -> bool {
    auto it = newBarsByName.find(inst.barConfig.name);
    if (it == newBarsByName.end()) {
      return true;
    }
    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return true;
    }
    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return true;
    }
    return !barConfigSurfaceFieldsEqual(inst.barConfig, resolved, previousShadow, m_lastShadow);
  };
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instUp : m_instances) {
    if (needsSurfaceRecreate(*instUp)) {
      outputsNeedingRecreate.insert(instUp->outputName);
    }
  }

  // For each existing instance, decide whether to rebuild contents in place
  // (surface preserved → no exclusive-zone churn) or destroy (will be recreated
  // by syncInstances below). Any bar on an output flagged above is destroyed so the
  // whole output is rebuilt in config order.
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
    if (outputsNeedingRecreate.contains(inst.outputName)) {
      return destroy();
    }
    if (it == newBarsByName.end()) {
      return destroy();
    }

    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return destroy();
    }

    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
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

void Bar::onWorkspaceChanged() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  bool anyChanged = false;
  for (const auto& output : m_platform->outputs()) {
    const std::string activeId = activeWorkspaceId(m_platform->workspaces(output.output));
    if (activeId.empty()) {
      continue;
    }

    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (!last.empty() && last != activeId) {
      m_pendingWorkspaceRevealOutputs.insert(output.name);
      anyChanged = true;
    }
    last = activeId;
  }

  if (anyChanged) {
    m_workspaceRevealDebounce.start(kWorkspaceRevealDebounce, [this]() { applyPendingWorkspaceReveal(); });
  }
  scheduleSmartAutoHideReevaluation();
}

void Bar::scheduleSmartAutoHideReevaluation() {
  if (m_smartAutoHideReevalQueued) {
    return;
  }
  m_smartAutoHideReevalQueued = true;
  DeferredCall::callLater([this]() {
    m_smartAutoHideReevalQueued = false;
    reevaluateSmartAutoHide();
  });
}

void Bar::reevaluateSmartAutoHide() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  for (const auto& instanceUp : m_instances) {
    BarInstance* instance = instanceUp.get();
    if (instance == nullptr
        || !instance->barConfig.enabled
        || !instance->barConfig.smartAutoHide
        || instance->surface == nullptr) {
      continue;
    }

    const bool wantsPinned = smartAutoHideWantsPinnedVisible(*m_platform, instance->output);
    const bool pinnedChanged = wantsPinned != instance->smartAutoHidePinnedVisible;
    instance->smartAutoHidePinnedVisible = wantsPinned;

    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;

    bool needsRedraw = pinnedChanged;
    if (wantsPinned) {
      if (instance->hideOpacity < 1.0f || pinnedChanged) {
        revealAutoHideBar(*instance);
        syncBarAutoHideInputRegion(*instance);
        syncBarSurfaceChrome(*instance);
        needsRedraw = true;
      }
    } else if (!instance->pointerInside && instance->attachedPopupCount == 0 && !suppressAutoHide) {
      if ((instance->hideOpacity > 0.0f || pinnedChanged) && !isWorkspacePeekActive()) {
        startHideFadeOut(*instance);
        needsRedraw = true;
      }
    }

    if (needsRedraw && instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

bool Bar::isWorkspacePeekActive() const noexcept {
  return m_workspaceRevealDebounce.active() || m_workspacePeekHideTimer.active();
}

void Bar::applyPendingWorkspaceReveal() {
  if (m_platform == nullptr) {
    return;
  }

  const auto pendingOutputs = std::move(m_pendingWorkspaceRevealOutputs);
  m_pendingWorkspaceRevealOutputs.clear();

  std::vector<BarInstance*> peeked;
  peeked.reserve(m_instances.size());
  for (const std::uint32_t outputName : pendingOutputs) {
    for (const auto& instanceUp : m_instances) {
      auto* instance = instanceUp.get();
      if (instance == nullptr
          || instance->outputName != outputName
          || !instance->barConfig.enabled
          || !instance->barConfig.isAutoHideEnabled()
          || !instance->barConfig.showOnWorkspaceSwitch
          || instance->surface == nullptr) {
        continue;
      }

      revealAutoHideBar(*instance);
      if (instance->pointerInside) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        peeked.push_back(instance);
      }
    }
  }

  if (peeked.empty()) {
    return;
  }

  m_workspacePeekHideTimer.start(kWorkspacePeekHold, [this, peeked = std::move(peeked)]() {
    for (BarInstance* instance : peeked) {
      if (instance == nullptr || !instance->barConfig.isAutoHideEnabled() || instance->pointerInside) {
        continue;
      }
      if (instance->barConfig.smartAutoHide && instance->smartAutoHidePinnedVisible) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        startHideFadeOut(*instance);
      }
    }
  });
}

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
    if (instance == nullptr
        || !barPointerHideAllowed(*instance)
        || instance->pointerInside
        || instance->attachedPopupCount > 0) {
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
  return std::ranges::any_of(m_instances, [](const auto& inst) { return inst->surface && inst->surface->isRunning(); });
}

bool Bar::instanceEffectivelyVisible(const BarInstance& instance) const noexcept {
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > 0.5f;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > 0.5f;
}

bool Bar::instanceAcceptsPointerInput(const BarInstance& instance) const noexcept {
  return barSupportsSlideBehavior(instance.barConfig) || !instance.ipcLayoutReleased;
}

bool Bar::isVisible() const noexcept {
  return std::ranges::any_of(m_instances, [this](const auto& inst) { return instanceEffectivelyVisible(*inst); });
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
  if (barSupportsSlideBehavior(instance.barConfig)) {
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
  instance.animations.cancelForOwner(instance.slideRoot);
  instance.slideRoot->setOpacity(1.0f);
  if (!visible) {
    clearInstancePointerState(instance);
  }
  const float current = instance.hideOpacity;
  const float target = visible ? 1.0f : 0.0f;
  instance.animations.animate(
      current, target, Style::animNormal, visible ? Easing::EaseOutCubic : Easing::EaseInQuad,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this]() {
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestRedraw();
        }
      },
      instance.slideRoot
  );
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
  syncIdleInhibitorAnchors();
}

void Bar::syncIdleInhibitorAnchors() {
  if (m_idleInhibitor != nullptr) {
    m_idleInhibitor->resyncAnchorSurfaces();
  }
}

bool Bar::barContentVisuallyShown(const BarInstance& instance) const noexcept {
  constexpr float kShownThreshold = 0.02f;
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > kShownThreshold;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > kShownThreshold;
}

bool Bar::shouldReserveExclusiveZone(const BarInstance& instance) const noexcept {
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
  if (!wlOutput->hasUsableGeometry()) {
    return rects;
  }
  const std::int32_t outputW = wlOutput->effectiveLogicalWidth();
  const std::int32_t outputH = wlOutput->effectiveLogicalHeight();

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
    const auto surfW = static_cast<std::int32_t>(surface->width());
    const auto surfH = static_cast<std::int32_t>(surface->height());

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

std::vector<wl_surface*> Bar::caffeineAnchorSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr || !instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    if (!barContentVisuallyShown(*instance)) {
      continue;
    }
    if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
      surfaces.push_back(s);
    }
  }
  return surfaces;
}

bool Bar::canAttachPanelToBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return false;
  }
  return barSupportsSlideBehavior(instance->barConfig) || instanceEffectivelyVisible(*instance);
}

std::optional<std::string> Bar::layerForBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return std::nullopt;
  }
  return instance->barConfig.layer;
}

LayerShellLayer Bar::highestLayerForOutput(wl_output* output) const noexcept {
  LayerShellLayer highest = LayerShellLayer::Top;
  for (const auto& instance : m_instances) {
    if (instance->output != output || !instance->barConfig.enabled) {
      continue;
    }
    const LayerShellLayer layer = layerShellLayerFromConfig(instance->barConfig.layer);
    if (static_cast<std::uint32_t>(layer) > static_cast<std::uint32_t>(highest)) {
      highest = layer;
    }
  }
  return highest;
}

bool Bar::isAttachedPanelBarSettled(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || !barSupportsSlideBehavior(instance->barConfig)) {
    return true;
  }
  constexpr float kSettledThreshold = 0.999f;
  return instance->hideOpacity >= kSettledThreshold;
}

void Bar::revealAutoHideForAttachedPanel(wl_output* output, std::string_view barName) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance != nullptr) {
    revealAutoHideBar(*instance);
  }
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
  if (instance->attachedPopupCount > 0) {
    return;
  }
  instance->pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == surface;
  if (instance->pointerInside) {
    instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
    instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
    instance->inputDispatcher.pointerEnter(
        instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
    );
  } else {
    instance->inputDispatcher.pointerLeave();
  }
  if (instance->surface != nullptr) {
    instance->surface->requestRedraw();
  }
  if (!instance->pointerInside && m_hoveredInstance == instance) {
    m_hoveredInstance = nullptr;
  } else if (instance->pointerInside) {
    m_hoveredInstance = instance;
  }
  if (!barPointerHideAllowed(*instance) || instance->pointerInside) {
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
    if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
      // bar-hide IPC always frees layout on non-autohide bars (v4 isVisible=false), regardless of reserve_space.
      instance->ipcLayoutReleased = true;
    }
  }
  applyIpcVisibility(false);
}

void Bar::suppressDisplay() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = true;
  m_wasVisibleBeforeOverlaySuppress = isVisible();
  hide();
}

void Bar::unsuppressDisplay() {
  if (!m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = false;
  if (m_wasVisibleBeforeOverlaySuppress) {
    show();
  }
}

void Bar::toggle() {
  const bool anyEffectivelyVisible = std::ranges::any_of(m_instances, [this](const auto& inst) {
    return inst != nullptr && instanceEffectivelyVisible(*inst);
  });

  if (anyEffectivelyVisible) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
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

  std::erase_if(m_lastActiveWorkspaceByOutput, [&outputs](const auto& pair) {
    return std::ranges::find(outputs, pair.first, &WaylandOutput::name) == outputs.end();
  });

  for (const auto& output : outputs) {
    if (!output.done || !output.hasUsableGeometry()) {
      continue;
    }
    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (last.empty()) {
      last = activeWorkspaceId(m_platform->workspaces(output.output));
    }
  }

  // Remove instances for outputs that no longer exist
  std::erase_if(m_instances, [&outputs, this](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool found = it != outputs.end() && it->done && it->hasUsableGeometry();
    if (!found) {
      kLog.info("removing instance for output {}", inst->outputName);
    }
    if (!found) {
      if (inst->surface != nullptr) {
        m_surfaceMap.erase(inst->surface->wlSurface());
      }
      if (m_hoveredInstance == inst.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    return !found;
  });

  // Create instances for each bar definition × each output
  for (std::size_t barIdx = 0; barIdx < bars.size(); ++barIdx) {
    for (const auto& output : outputs) {
      if (!output.done || !output.hasUsableGeometry()) {
        continue;
      }

      bool exists = std::ranges::any_of(m_instances, [&output, barIdx](const auto& inst) {
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

  syncIdleInhibitorAnchors();
  reevaluateSmartAutoHide();
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
      .layer = layerShellLayerFromConfig(barConfig.layer),
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
  std::erase_if(m_instances, [outputName, this](const auto& inst) {
    if (inst->surface != nullptr) {
      m_surfaceMap.erase(inst->surface->wlSurface());
    }
    if (m_hoveredInstance == inst.get()) {
      m_hoveredInstance = nullptr;
    }
    return inst->outputName == outputName;
  });
}

void Bar::populateWidgets(BarInstance& instance) {
  const auto& widgetConfigs = m_config->config().widgets;
  const auto labelFontWeight = static_cast<FontWeight>(instance.barConfig.fontWeight);
  const std::string barFontFamily = (instance.barConfig.fontFamily && !instance.barConfig.fontFamily->empty())
      ? *instance.barConfig.fontFamily
      : m_config->config().shell.fontFamily;
  // Creates one widget for `name`. When `groupSpec` is set the widget is a member of a capsule group and
  // takes the group's capsule style + foreground; otherwise it resolves its own per-widget/bar capsule.
  auto createWidget = [&](const std::string& name, const WidgetBarCapsuleSpec* groupSpec,
                          const std::optional<ColorSpec>* groupForeground, std::vector<std::unique_ptr<Widget>>& dest) {
    const WidgetConfig* wcPtr = nullptr;
    if (auto it = widgetConfigs.find(name); it != widgetConfigs.end()) {
      wcPtr = &it->second;
    }
    const float contentScale = resolveWidgetContentScale(instance.barConfig.scale, wcPtr, "widget." + name + ".scale");
    auto widget = m_widgetFactory->create(
        name, instance.output, contentScale, instance.barConfig.position, instance.barConfig.name,
        static_cast<float>(instance.barConfig.widgetSpacing)
    );
    if (widget == nullptr) {
      return;
    }
    widget->setConfigName(name);
    if (wcPtr != nullptr) {
      widget->setAnchor(wcPtr->getBool("anchor", false));
      widget->setNonInteractive(!wcPtr->getBool("interactive", true));
      if (!wcPtr->getBool("enabled", true)) {
        return;
      }
    }
    widget->setBarCapsuleSpec(
        groupSpec != nullptr ? *groupSpec : resolveWidgetBarCapsuleSpec(instance.barConfig, wcPtr)
    );
    widget->setLabelFontWeight(
        wcPtr != nullptr ? parseWidgetLabelFontWeight(*wcPtr, labelFontWeight) : labelFontWeight
    );
    widget->setLabelFontFamily(wcPtr != nullptr ? parseWidgetLabelFontFamily(*wcPtr, barFontFamily) : barFontFamily);
    if (wcPtr != nullptr && wcPtr->hasSetting("color")) {
      widget->setWidgetForeground(wcPtr->getOptionalColorSpec("color", "widget." + name + ".color"));
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetForeground(*groupForeground);
    } else if (instance.barConfig.widgetColor.has_value()) {
      widget->setWidgetForeground(instance.barConfig.widgetColor);
    }
    if (wcPtr != nullptr && wcPtr->hasSetting("icon_color")) {
      widget->setWidgetIconColor(wcPtr->getOptionalColorSpec("icon_color", "widget." + name + ".icon_color"));
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetIconColor(*groupForeground);
    } else if (instance.barConfig.widgetIconColor.has_value()) {
      widget->setWidgetIconColor(instance.barConfig.widgetIconColor);
    }
    dest.push_back(std::move(widget));
  };

  // Expands a lane's entries: group tokens become contiguous member widgets sharing the group's capsule.
  auto createWidgets = [&](const std::vector<std::string>& names, std::vector<std::unique_ptr<Widget>>& dest) {
    for (const auto& name : names) {
      if (isCapsuleGroupToken(name)) {
        const BarCapsuleGroupStyle* group = findBarCapsuleGroupStyle(instance.barConfig, capsuleGroupTokenId(name));
        if (group == nullptr || !group->enabled) {
          continue;
        }
        const WidgetBarCapsuleSpec groupSpec = capsuleSpecFromGroup(instance.barConfig, *group);
        for (const auto& member : group->members) {
          createWidget(member, &groupSpec, &group->foreground, dest);
        }
        continue;
      }
      createWidget(name, nullptr, nullptr, dest);
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
    debugWidget->setLabelFontFamily(barFontFamily);
    debugWidget->create();
    instance.endWidgets.insert(instance.endWidgets.begin(), std::move(debugWidget));
  }
#endif
}

void Bar::attachWidgetsToSections(BarInstance& instance) {
  const bool isVertical = instance.barConfig.position == "left" || instance.barConfig.position == "right";
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const bool hoverHighlight = instance.barConfig.hoverHighlight;

  instance.widgetByRoot.clear();
  instance.hoverHighlightWidget = nullptr;
  if (instance.hoverUnderlay != nullptr) {
    while (!instance.hoverUnderlay->children().empty()) {
      instance.hoverUnderlay->removeChild(instance.hoverUnderlay->children().back().get());
    }
  }

  // Hover overlay: sits above the capsule fill (same zIndex, later sibling) and below the
  // content; fill/visibility are driven by the hover animation.
  auto addHoverBox = [hoverHighlight](Widget& widget, Node& shell) -> Box* {
    if (!hoverHighlight || !widget.wantsBarHoverHighlight()) {
      return nullptr;
    }
    Box* boxPtr = nullptr;
    shell.addChild(
        ui::box({
            .out = &boxPtr,
            .fill = withOpacity(widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.0f),
            .visible = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );
    widget.setBarHoverBox(boxPtr);
    return boxPtr;
  };

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
      if (auto* plugin = dynamic_cast<PluginWidget*>(widget.get()); plugin != nullptr) {
        plugin->setUpdateDeferralCallback([]() {
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
          if (const auto* out = m_platform->findOutputByWl(inst->output); out != nullptr && out->hasUsableGeometry()) {
            const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*inst, *out);
            anchorX += surfaceX;
            anchorY += surfaceY;
          }
        }
        PanelManager::instance().togglePanel(
            std::string(panelId),
            PanelOpenRequest{
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
      if (widget->root() != nullptr) {
        instance.widgetByRoot[widget->root()] = widget.get();
      }
    }

    capsuleRuns.clear();

    auto addPlainWidget = [&](Widget& widget) {
      widget.setBarCapsuleScene(nullptr, nullptr);
      // No capsule: the hover pill lives on the bar-level underlay (unclipped, no layout
      // footprint) and is positioned after sections are laid out.
      if (hoverHighlight
          && instance.hoverUnderlay != nullptr
          && !widget.isBarClickThrough()
          && !widget.noGapAroundMe()) {
        addHoverBox(widget, *instance.hoverUnderlay);
      }
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
      Box* hoverPtr = addHoverBox(widget, *shellPtr);
      shellPtr->addChild(widget.releaseRoot());
      widget.setBarCapsuleScene(shellPtr, bgPtr);
      if (auto* area = dynamic_cast<InputArea*>(widget.root())) {
        area->setTooltipAnchorNode(shellPtr);
      }
      capsuleRuns.push_back(
          BarCapsuleRun{
              .shell = shellPtr,
              .bg = bgPtr,
              .container = nullptr,
              .content = widget.root(),
              .spec = cap,
              .contentScale = widget.contentScale(),
              .widgets = {&widget},
              .hoverBoxes = hoverPtr != nullptr ? std::vector<Box*>{hoverPtr} : std::vector<Box*>{},
          }
      );
      auto* added = section->addChild(std::move(shell));
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    // Members of the same group share one resolved style by construction (see resolveWidgetBarCapsuleSpec),
    // so adjacency + matching group ID is sufficient to merge. Per-widget scale does not split the group:
    // the run is sized from its largest member's scale below so a differently scaled member still fits.
    auto canJoinCapsuleGroup = [](const Widget& first, const Widget& next) {
      const auto& firstSpec = first.barCapsuleSpec();
      const auto& nextSpec = next.barCapsuleSpec();
      return firstSpec.enabled
          && nextSpec.enabled
          && !first.isAnchor()
          && !next.isAnchor()
          && !firstSpec.group.empty()
          && firstSpec.group == nextSpec.group;
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
      while (runEnd < widgets.size()
             && widgets[runEnd] != nullptr
             && widgets[runEnd]->root() != nullptr
             && canJoinCapsuleGroup(*widget, *widgets[runEnd])) {
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

      std::vector<Box*> hoverBoxes;
      if (hoverHighlight) {
        hoverBoxes.reserve(runEnd - index);
        for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
          hoverBoxes.push_back(addHoverBox(*widgets[memberIndex], *shellPtr));
        }
      }

      auto inner = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
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
      run.hoverBoxes = std::move(hoverBoxes);

      for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
        auto& member = widgets[memberIndex];
        member->setBarCapsuleScene(shellPtr, bgPtr);
        run.widgets.push_back(member.get());
        auto* added = innerPtr->addChild(member->releaseRoot());
        if (auto* area = dynamic_cast<InputArea*>(member->root())) {
          area->setTooltipAnchorNode(shellPtr);
        }
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

void Bar::updateWidgetHoverHighlight(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = nullptr;
  for (const Node* node = hoveredArea; node != nullptr; node = node->parent()) {
    if (auto it = instance.widgetByRoot.find(node); it != instance.widgetByRoot.end()) {
      target = it->second;
      break;
    }
  }
  if (target != nullptr && target->barHoverBox() == nullptr) {
    target = nullptr;
  }
  if (target == instance.hoverHighlightWidget) {
    return;
  }
  if (instance.hoverHighlightWidget != nullptr) {
    animateWidgetHoverHighlight(instance, *instance.hoverHighlightWidget, false);
  }
  instance.hoverHighlightWidget = target;
  if (target != nullptr) {
    animateWidgetHoverHighlight(instance, *target, true);
  }
}

void Bar::animateWidgetHoverHighlight(BarInstance& instance, Widget& widget, bool hovered) {
  Box* box = widget.barHoverBox();
  if (box == nullptr) {
    return;
  }
  const ColorSpec fill = widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
  instance.animations.cancelForOwner(box);
  instance.animations.animate(
      widget.barHoverProgress(), hovered ? 1.0f : 0.0f, Style::animFast, Easing::EaseOutCubic,
      [&widget, box, fill](float progress) {
        widget.setBarHoverProgress(progress);
        box->setVisible(progress > 0.001f);
        box->setFill(withOpacity(fill, kWidgetHoverFillAlpha * progress));
      },
      {}, box
  );
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig) {
  noctalia::profiling::ScopedTimer t(kLog, std::format("bar: rebuild contents [{}]", newConfig.name));

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
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
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
  return widgetsNeedFrameTick(instance.startWidgets)
      || widgetsNeedFrameTick(instance.centerWidgets)
      || widgetsNeedFrameTick(instance.endWidgets);
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
  if (barConfigUsesSlideSurface(instance.barConfig)) {
    const bool fullSurface = instance.pointerInside
        || instance.attachedPopupCount > 0
        || instance.hideOpacity > 0.5f
        || (instance.barConfig.smartAutoHide && instance.smartAutoHidePinnedVisible);
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, fullSurface));
    return;
  }
  instance.surface->setInputRegion(
      {barContentInputRegion(instance.barConfig, m_config->config().shell.shadow, surfW, surfH)}
  );
}

void Bar::revealAutoHideBar(BarInstance& instance) {
  if (instance.autoHideDisablePending) {
    return;
  }
  if (!barSupportsSlideBehavior(instance.barConfig) || instance.surface == nullptr || instance.slideRoot == nullptr) {
    return;
  }

  instance.ipcLayoutReleased = false;
  instance.animations.cancelForOwner(instance.slideRoot);
  const float current = instance.hideOpacity;
  wl_output* output = instance.output;
  const std::string barName = instance.barConfig.name;
  const auto notifyAttachedPanel = [output, barName]() {
    PanelManager::instance().onAttachedBarRevealSettled(output, barName);
  };

  constexpr float kSettledThreshold = 0.999f;
  if (current >= kSettledThreshold) {
    const int surfW = static_cast<int>(instance.surface->width());
    const int surfH = static_cast<int>(instance.surface->height());
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
    syncBarSurfaceChrome(instance);
    instance.surface->requestRedraw();
    notifyAttachedPanel();
    return;
  }

  instance.animations.animate(
      current, 1.0f, Style::animNormal, Easing::EaseOutCubic,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      notifyAttachedPanel, instance.slideRoot
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
  if (instance.barConfig.autoHide
      || instance.barConfig.smartAutoHide
      || instance.ipcLayoutReleased
      || instance.hideOpacity < 0.999f) {
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
  const auto concave = barConcaveShape(instance.barConfig);
  // The bg node is the visual rect; tessellateShape expects the body rect and
  // expands it outward by logicalInset itself. With all-convex corners and no
  // inset it takes its own fast path down to a plain rounded rect.
  const int insetL = static_cast<int>(std::lround(concave.logicalInset.left));
  const int insetT = static_cast<int>(std::lround(concave.logicalInset.top));
  const int insetR = static_cast<int>(std::lround(concave.logicalInset.right));
  const int insetB = static_cast<int>(std::lround(concave.logicalInset.bottom));
  auto blurStrips = Surface::tessellateShape(
      px + insetL, py + insetT, pw - insetL - insetR, ph - insetT - insetB, concave.corners, concave.logicalInset,
      concave.radii
  );
  instance.surface->setBlurRegion(blurStrips);
}

void Bar::startHideFadeOut(BarInstance& instance) {
  if (instance.autoHideDisablePending || instance.smartAutoHidePinnedVisible) {
    return;
  }
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
      },
      instance.slideRoot
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
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto shadowOffset = shadowDirectionOffset(shadowConfig.direction);
  const float shadowSize = shell::surface_shadow::enabled(instance.barConfig.shadow, shadowConfig)
      ? static_cast<float>(shell::surface_shadow::kBlurRadius)
      : 0.0f;
  const auto shadowOffsetX = static_cast<float>(shadowOffset.x);
  const auto shadowOffsetY = static_cast<float>(shadowOffset.y);
  const bool isBottom = instance.barConfig.position == "bottom";
  const bool isRight = instance.barConfig.position == "right";
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto concave = barConcaveShape(instance.barConfig);

  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
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

    auto hoverUnderlay = std::make_unique<Node>();
    hoverUnderlay->setHitTestVisible(false);
    hoverUnderlay->setSize(static_cast<float>(w), static_cast<float>(h));
    instance.hoverUnderlay = instance.slideRoot->addChild(std::move(hoverUnderlay));

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
      return ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
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
    instance.inputDispatcher.setHoverChangeCallback([this, inst = &instance](InputArea* /*old*/, InputArea* next) {
      if (next != nullptr) {
        next->setTooltipPlacement(tooltipPlacementAwayFromEdge(inst->barConfig.position));
      }
      TooltipManager::instance().onHoverChange(next, inst->surface->layerSurface(), inst->output);
      updateWidgetHoverHighlight(*inst, next);
    });

    if (instance.barConfig.smartAutoHide && m_platform != nullptr) {
      instance.smartAutoHidePinnedVisible = smartAutoHideWantsPinnedVisible(*m_platform, instance.output);
    }
    if (barConfigUsesSlideSurface(instance.barConfig)) {
      instance.slideRoot->setOpacity(1.0f);
      const bool startHidden =
          instance.barConfig.smartAutoHide ? !instance.smartAutoHidePinnedVisible : instance.barConfig.autoHide;
      instance.hideOpacity = startHidden ? 0.0f : 1.0f;
    } else {
      instance.slideRoot->setOpacity(1.0f);
      instance.hideOpacity = 1.0f;
    }

    instance.surface->setSceneRoot(instance.sceneRoot.get());
  }

  // Update root size on reconfigure
  instance.sceneRoot->setSize(w, h);
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }
  if (instance.hoverUnderlay != nullptr) {
    instance.hoverUnderlay->setSize(w, h);
  }

  // Background covers only the bar visual area (not the shadow extension).
  // Keep it exactly aligned with the shadow shape; the shadow shader now
  // draws only outside the rect, so any size mismatch is visible at corners.
  if (instance.bg != nullptr) {
    const RoundedRectStyle bgStyle{
        .fill = colorForRole(ColorRole::Surface, instance.barConfig.backgroundOpacity),
        .border = resolveColorSpec(instance.barConfig.border),
        .fillMode = FillMode::Solid,
        .corners = concave.corners,
        .logicalInset = concave.logicalInset,
        .radius = concave.radii,
        .softness = 0.0f,
        .borderWidth = instance.barConfig.borderWidth,
    };
    instance.bg->setStyle(bgStyle);
    // (barAreaX/Y/W/H) is the body; the shader expands outward by logicalInset into
    // the visual rect, so the node must be sized to the visual rect.
    instance.bg->setPosition(barAreaX - concave.logicalInset.left, barAreaY - concave.logicalInset.top);
    instance.bg->setSize(
        barAreaW + concave.logicalInset.left + concave.logicalInset.right,
        barAreaH + concave.logicalInset.top + concave.logicalInset.bottom
    );
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
  // Concave spikes extend past the body on the inner edge; include them so the bar
  // slides fully off-screen when hidden.
  contentLeft -= concave.logicalInset.left;
  contentTop -= concave.logicalInset.top;
  contentRight += concave.logicalInset.right;
  contentBottom += concave.logicalInset.bottom;
  const auto hiddenDelta = computeAutoHideHiddenDelta(
      isVertical, isBottom, isRight, w, h, contentLeft, contentTop, contentRight, contentBottom
  );
  instance.slideHiddenDx = hiddenDelta.first;
  instance.slideHiddenDy = hiddenDelta.second;
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
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

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
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

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

  if (targetInstance != nullptr
      && event.type == PointerEvent::Type::Button
      && event.button == BTN_MIDDLE
      && event.pressed
      && m_config != nullptr
      && m_config->config().shell.middleClickOpensWidgetSettings) {
    auto* widget = widgetAtPoint(*targetInstance, static_cast<float>(event.sx), static_cast<float>(event.sy));
    if (widget != nullptr
        && !widget->reservesMiddleClick(static_cast<float>(event.sx), static_cast<float>(event.sy))
        && !widget->configName().empty()
        && m_openWidgetSettingsCallback) {
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
      if (event.type == PointerEvent::Type::Button && event.button == BTN_RIGHT && event.pressed) {
        const auto sx = static_cast<float>(event.sx);
        const auto sy = static_cast<float>(event.sy);
        const auto& deadZone = targetInstance->barConfig.deadZone;
        if (!deadZone.rightCommand.empty() && isBarDeadZone(*targetInstance, sx, sy)) {
          executeDeadZoneCommand(deadZone.rightCommand);
        } else {
          openControlCenterAtBarPointer(*targetInstance, sx, sy, m_platform, targetInstance->barConfig.name);
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
    BarInstance* const entered = m_hoveredInstance;
    entered->lastPointerSx = static_cast<float>(event.sx);
    entered->lastPointerSy = static_cast<float>(event.sy);
    entered->pointerInside = true;
    entered->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    // pointerEnter can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != entered) {
      break;
    }
    if (barSupportsSlideBehavior(m_hoveredInstance->barConfig) && m_hoveredInstance->sceneRoot != nullptr) {
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
      if (barPointerHideAllowed(*m_hoveredInstance) && !suppressAutoHide) {
        startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr)
      break;
    BarInstance* const hovered = m_hoveredInstance;
    hovered->lastPointerSx = static_cast<float>(event.sx);
    hovered->lastPointerSy = static_cast<float>(event.sy);
    hovered->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    // pointerMotion can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != hovered) {
      break;
    }
    break;
  }
  case PointerEvent::Type::Button: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    bool pressed = event.pressed;
    consumed = m_hoveredInstance->inputDispatcher.pointerButton(sx, sy, event.button, pressed);
    if (pressed && !consumed) {
      if (handleBarDeadZoneButton(*m_hoveredInstance, sx, sy, event.button, m_platform)) {
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
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    const bool axisConsumed = m_hoveredInstance->inputDispatcher.pointerAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    if (!axisConsumed) {
      handleBarDeadZoneAxis(*m_hoveredInstance, sx, sy, event);
    }
    break;
  }
  }

  // Trigger redraw if any widget changed visual state
  if (m_hoveredInstance != nullptr
      && m_hoveredInstance->sceneRoot != nullptr
      && (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
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

std::optional<std::string> Bar::collectBarIpcInstances(
    std::optional<std::string> barName, std::optional<std::string> monitorSelector,
    std::vector<BarInstance*>& instancesOut
) {
  instancesOut.clear();

  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  if (barName.has_value()) {
    const bool knownBar = std::ranges::contains(m_config->config().bars, *barName, &BarConfig::name);
    if (!knownBar) {
      if (!monitorSelector.has_value()) {
        monitorSelector = std::move(barName);
        barName = std::nullopt;
      } else {
        std::vector<std::string> knownBars;
        knownBars.reserve(m_config->config().bars.size());
        for (const auto& bar : m_config->config().bars) {
          knownBars.push_back(bar.name);
        }
        const std::string suffix =
            knownBars.empty() ? std::string() : std::string("; known: ") + StringUtils::join(knownBars, ", ");
        return "error: unknown bar \"" + std::string(*barName) + "\"" + suffix + "\n";
      }
    }
  }

  const auto matchesBar = [&](const BarInstance& instance) {
    return !barName.has_value() || instance.barConfig.name == *barName;
  };

  if (!monitorSelector.has_value()) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && matchesBar(*instance)) {
        instancesOut.push_back(instance.get());
      }
    }
    if (instancesOut.empty()) {
      if (barName.has_value()) {
        return "error: no instances matched bar \"" + std::string(*barName) + "\"\n";
      }
      return "error: no bar instances are active\n";
    }
    return std::nullopt;
  }

  if (m_platform == nullptr) {
    return "error: bar service not initialized\n";
  }

  const std::string selector(*monitorSelector);
  std::vector<std::string> outputMatches;
  std::vector<std::string> knownOutputs;
  for (const auto& output : m_platform->outputs()) {
    if (output.connectorName.empty()) {
      continue;
    }
    knownOutputs.push_back(output.connectorName);
    if (outputMatchesSelector(selector, output)) {
      outputMatches.push_back(output.connectorName);
    }
  }

  std::ranges::sort(knownOutputs);
  knownOutputs.erase(std::ranges::unique(knownOutputs).begin(), knownOutputs.end());
  std::ranges::sort(outputMatches);
  outputMatches.erase(std::ranges::unique(outputMatches).begin(), outputMatches.end());

  if (outputMatches.empty()) {
    std::string error = "error: unknown monitor selector \"" + selector + "\"";
    if (!knownOutputs.empty()) {
      error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
    }
    error += "\n";
    return error;
  }
  if (outputMatches.size() > 1) {
    return "error: monitor selector \""
        + selector
        + "\" matched multiple outputs: "
        + StringUtils::join(outputMatches, ", ")
        + "\n";
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output == nullptr || !matchesBar(*instance)) {
      continue;
    }
    const auto it = std::find_if(
        m_platform->outputs().begin(), m_platform->outputs().end(),
        [&instance](const WaylandOutput& output) { return output.output == instance->output; }
    );
    if (it != m_platform->outputs().end() && it->connectorName == outputMatches.front()) {
      instancesOut.push_back(instance.get());
    }
  }

  if (instancesOut.empty()) {
    std::string error = "error: no instances matched";
    if (barName.has_value()) {
      error += " bar \"" + std::string(*barName) + "\"";
    }
    error += " on \"" + outputMatches.front() + "\"\n";
    return error;
  }

  return std::nullopt;
}

namespace {

  [[nodiscard]] std::optional<std::string> parseBarVisibilityIpcArgs(
      std::string_view command, std::string_view args, std::optional<std::string>& barName,
      std::optional<std::string>& monitorSelector
  ) {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() > 2) {
      return "error: usage: " + std::string(command) + " [bar-name] [monitor-selector]\n";
    }
    barName = std::nullopt;
    monitorSelector = std::nullopt;
    if (!parts.empty() && !parts[0].empty()) {
      barName = parts[0];
    }
    if (parts.size() >= 2 && !parts[1].empty()) {
      monitorSelector = parts[1];
    }
    return std::nullopt;
  }

} // namespace

std::string Bar::showBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-show", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::hideBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-hide", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (!barSupportsSlideBehavior(instance->barConfig)) {
      instance->ipcLayoutReleased = true;
    }
    setInstanceIpcVisible(*instance, false);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const bool anyEffectivelyVisible = std::ranges::any_of(targets, [this](const BarInstance* instance) {
    return instance != nullptr && instanceEffectivelyVisible(*instance);
  });

  if (anyEffectivelyVisible) {
    for (BarInstance* instance : targets) {
      if (!barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
      setInstanceIpcVisible(*instance, false);
      syncBarSurfaceChrome(*instance);
    }
    return "ok\n";
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarReserveSpaceIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-reserve-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (instance != nullptr) {
      instance->barConfig.reserveSpace = !instance->barConfig.reserveSpace;
      syncBarExclusiveZone(*instance);
    }
  }
  return "ok\n";
}

std::string Bar::setBarAutoHideIpc(std::string_view args) {
  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-auto-hide-set <on|off|true|false|1|0> [bar-name] [monitor-selector]\n";
  }

  const std::string& value = parts[0];
  bool enabled = false;
  if (value == "on" || value == "true" || value == "1") {
    enabled = true;
  } else if (value == "off" || value == "false" || value == "0") {
    enabled = false;
  } else {
    return "error: invalid value (use on/off, true/false, 1/0)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  auto applyTransientAutoHide = [this, enabled](BarInstance& instance) {
    auto applySurfaceSpec = [this](BarInstance& inst) {
      if (inst.surface == nullptr) {
        return;
      }
      const auto spec = computeBarSurfaceSpec(inst.barConfig, m_config->config().shell.shadow);
      inst.surface->setMargins(spec.marginTop, spec.marginRight, spec.marginBottom, spec.marginLeft);
      inst.surface->requestSize(spec.surfaceWidth, spec.surfaceHeight);
    };

    instance.ipcLayoutReleased = false;
    instance.autoHideDisablePending = false;
    instance.animations.cancelForOwner(instance.slideRoot);

    if (enabled) {
      instance.barConfig.autoHide = true;
      applySurfaceSpec(instance);
      if (instance.slideRoot != nullptr) {
        instance.slideRoot->setOpacity(1.0f);
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(instance) : false;
      if (instance.pointerInside || instance.attachedPopupCount > 0 || suppressAutoHide) {
        revealAutoHideBar(instance);
      } else {
        startHideFadeOut(instance);
      }
      return;
    }

    if (instance.barConfig.autoHide && instance.hideOpacity < 0.999f) {
      const float current = instance.hideOpacity;
      instance.autoHideDisablePending = true;
      instance.animations.animate(
          current, 1.0f, Style::animNormal, Easing::EaseOutCubic,
          [this, inst = &instance](float v) {
            inst->hideOpacity = v;
            syncBarSlideLayerTransform(*inst);
            syncBarSurfaceChrome(*inst);
          },
          [this, inst = &instance, applySurfaceSpec]() {
            inst->autoHideDisablePending = false;
            inst->barConfig.autoHide = false;
            applySurfaceSpec(*inst);
            syncBarSlideLayerTransform(*inst);
            syncBarAutoHideInputRegion(*inst);
            syncBarSurfaceChrome(*inst);
            if (inst->surface != nullptr) {
              inst->surface->requestRedraw();
            }
          },
          instance.slideRoot
      );
      if (instance.surface != nullptr) {
        instance.surface->requestRedraw();
      }
      return;
    }

    instance.barConfig.autoHide = false;
    instance.autoHideDisablePending = false;
    instance.hideOpacity = 1.0f;
    if (instance.slideRoot != nullptr) {
      instance.slideRoot->setOpacity(1.0f);
    }
    applySurfaceSpec(instance);
    syncBarSlideLayerTransform(instance);
    syncBarAutoHideInputRegion(instance);
    syncBarSurfaceChrome(instance);
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
  };

  for (BarInstance* instance : targets) {
    applyTransientAutoHide(*instance);
  }
  return "ok\n";
}

std::string Bar::setBarLayerIpc(std::string_view args) {
  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-layer-set <top|overlay> [bar-name] [monitor-selector]\n";
  }

  const std::string& layer = parts[0];
  if (layer != "top" && layer != "overlay") {
    return "error: invalid layer (use top or overlay)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const LayerShellLayer shellLayer = layerShellLayerFromConfig(layer);
  for (BarInstance* instance : targets) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    instance->surface->setLayer(shellLayer);
    instance->barConfig.layer = layer;
  }

  return "ok\n";
}

void Bar::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "bar-show", [this](const std::string& args) -> std::string { return showBarIpc(args); },
      "bar-show [bar-name] [monitor-selector]", "Show one or all bars"
  );

  ipc.registerHandler(
      "bar-hide", [this](const std::string& args) -> std::string { return hideBarIpc(args); },
      "bar-hide [bar-name] [monitor-selector]", "Hide one or all bars and release their layout gaps"
  );

  ipc.registerHandler(
      "bar-toggle", [this](const std::string& args) -> std::string { return toggleBarIpc(args); },
      "bar-toggle [bar-name] [monitor-selector]", "Toggle visibility for one or all bars"
  );

  ipc.registerHandler(
      "bar-reserve-toggle", [this](const std::string& args) -> std::string { return toggleBarReserveSpaceIpc(args); },
      "bar-reserve-toggle [bar-name] [monitor-selector]", "Toggle reserve space for one or all bars"
  );

  ipc.registerHandler(
      "bar-auto-hide-set", [this](const std::string& args) -> std::string { return setBarAutoHideIpc(args); },
      "bar-auto-hide-set <on|off|true|false|1|0> [bar-name] [monitor-selector]", "Set auto-hide state for a bar"
  );

  ipc.registerHandler(
      "bar-layer-set", [this](const std::string& args) -> std::string { return setBarLayerIpc(args); },
      "bar-layer-set <top|overlay> [bar-name] [monitor-selector]", "Set one or all bar layers"
  );
}
