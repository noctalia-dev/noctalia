#pragma once

#include "config/config_service.h"
#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/bar/widget.h"
#include "shell/panel/attached_panel_context.h"
#include "ui/signal.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class Box;
class Flex;
class Node;

struct BarCapsuleRun {
  Node* shell = nullptr;
  Box* bg = nullptr;
  Flex* container = nullptr;
  Node* content = nullptr;
  WidgetBarCapsuleSpec spec{};
  float contentScale = 1.0F;
  // Capsule geometry can exist without a painted fill or border.
  bool hasPaintedCapsuleBackground = false;
  // Direct member widgets of this run (for nested runs: only the widgets owned directly by
  // this group; nested groups live in `children`).
  std::vector<Widget*> widgets;
  // Nested inner-group runs, rendered with their shells as flex items inside this run's
  // container (visual group-in-group nesting, one level deep). Empty for flat runs.
  std::vector<BarCapsuleRun> children;
  // Interleaving of direct widgets and nested children in container order. Entries with
  // `isChild == false` index into `widgets`; entries with `isChild == true` index into
  // `children`. Empty for flat runs (container order then equals `widgets` order).
  struct MemberRef {
    bool isChild = false;
    std::size_t index = 0;
  };
  std::vector<MemberRef> memberOrder;
  // Hover highlight overlays, parallel to `widgets` for group runs; one shared box for single runs.
  std::vector<Box*> hoverBoxes;
  bool accordion = false;
  // Clips accordion members to the reveal window (inside the capsule padding).
  Node* accordionClip = nullptr;
  BarAccordionDirection accordionDirection = BarAccordionDirection::End;
  // Index into `widgets` (visual order) of the always-visible member (config members[0]).
  std::size_t accordionVisibleIndex = 0;
  bool accordionExpanded = false;
  float accordionProgress = 0.0F; // 0 = collapsed, 1 = fully expanded
};

struct BarInstance {
  std::uint32_t outputName = 0;
  wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::int32_t outputLogicalX = 0;
  std::int32_t outputLogicalY = 0;
  std::int32_t outputLogicalWidth = 0;
  std::int32_t outputLogicalHeight = 0;
  std::size_t barIndex = 0;
  BarConfig barConfig;
  std::unique_ptr<LayerSurface> surface;
  // sceneRoot must be destroyed before `animations` — ~Node() calls cancelForOwner().
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  Node* slideRoot = nullptr;
  float slideHiddenDx = 0.0F;
  float slideHiddenDy = 0.0F;
  InputDispatcher inputDispatcher;
  // Gestures for the parts of the bar no widget covers. The sink is never mounted in the scene; it
  // is used only for its scroll-detent accumulator, so dead-zone scrolling quantizes like a widget.
  noctalia::bar::WidgetActionBindings deadZoneBindings;
  InputArea deadZoneAxisSink;
  float hideOpacity = 1.0F;
  // bar-hide/toggle IPC on non-autohide bars: release compositor exclusive zone until bar-show (v4 isVisible=false).
  bool ipcLayoutReleased = false;
  // bar-auto-hide-set off keeps autoHide true until the reveal completes; block hover helpers from replacing it.
  bool autoHideDisablePending = false;
  Timer autoHideDelayTimer;
  // smart_auto_hide: active workspace empty (or overview open) — keep the bar visible.
  bool smartAutoHidePinnedVisible = false;
  bool pointerInside = false;
  float lastPointerSx = 0.0F;
  float lastPointerSy = 0.0F;
  std::size_t attachedPopupCount = 0;

  // Bar background, shadow, and layout sections (start/center/end along main axis)
  Box* bg = nullptr;
  Box* shadow = nullptr;
  Node* shadowLeftClip = nullptr;
  Node* shadowRightClip = nullptr;
  Box* shadowLeft = nullptr;
  Box* shadowRight = nullptr;
  Node* contentClip = nullptr;
  // Unclipped layer between the bar background and contentClip; hosts the hover pills of
  // capsule-less widgets so they neither affect layout nor get clipped at section boundaries.
  Node* hoverUnderlay = nullptr;
  Node* startSlot = nullptr;
  Node* centerSlot = nullptr;
  Node* endSlot = nullptr;
  Flex* startSection = nullptr;
  Flex* centerSection = nullptr;
  Flex* endSection = nullptr;

  std::vector<std::unique_ptr<Widget>> startWidgets;
  std::vector<std::unique_ptr<Widget>> centerWidgets;
  std::vector<std::unique_ptr<Widget>> endWidgets;
  std::vector<BarCapsuleRun> startCapsuleRuns;
  std::vector<BarCapsuleRun> centerCapsuleRuns;
  std::vector<BarCapsuleRun> endCapsuleRuns;

  // Maps each widget's root node to its Widget so hover-change events resolve to the owning widget.
  std::unordered_map<const Node*, Widget*> widgetByRoot;
  Widget* hoverHighlightWidget = nullptr;

  Signal<>::ScopedConnection paletteConn;
  std::optional<AttachedPanelGeometry> attachedPanelGeometry;
};
