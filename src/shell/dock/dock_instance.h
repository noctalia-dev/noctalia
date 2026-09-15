#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/scene/input_dispatcher.h"
#include "shell/dock/dock_items.h"
#include "shell/dock/dock_model.h"
#include "ui/signal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class Box;
class CompositorPlatform;
class ConfigService;
class Flex;
class LayerSurface;
class Node;
class RenderContext;
struct wl_output;

namespace shell::dock {

  struct DockInstance {
    std::uint32_t outputName = 0;
    wl_output* output = nullptr;
    std::int32_t scale = 1;
    // Output scale is not an integer, so a flush dock has to overlap the screen edge.
    bool fractionalScale = false;
    std::int32_t outputLogicalX = 0;
    std::int32_t outputLogicalY = 0;
    std::int32_t outputLogicalWidth = 0;
    std::int32_t outputLogicalHeight = 0;
    std::unique_ptr<LayerSurface> surface;
    // sceneRoot must be destroyed before `animations` — ~Node() calls cancelForOwner().
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    Node* slideRoot = nullptr;
    float slideHiddenDx = 0.0F;
    float slideHiddenDy = 0.0F;
    Box* shadow = nullptr;
    Box* panel = nullptr;
    Flex* row = nullptr;
    InputDispatcher inputDispatcher;
    std::vector<shell::dock::DockItemView> items;
    DockSnapshot snapshot;
    bool pointerInside = false;
    float hoverPointerMain = 0.0F;
    bool hoverPointerValid = false;
    InputArea* launcherArea = nullptr;
    Node* launcherIconNode = nullptr;
    float launcherRestMainPos = 0.0F;
    float launcherRestCrossPos = 0.0F;
    float launcherHoverMainOffset = 0.0F;
    float launcherVisualScale = -1.0F;
    AnimationManager::Id launcherScaleAnimId = 0;
    // Auto-hide: tracks visibility [0,1] driven by hover.
    float hideOpacity = 1.0F;
    AnimationManager::Id hideAnimId = 0;
    // smart_auto_hide: active workspace empty, all windows minimized, or overview open — keep the dock visible.
    bool smartAutoHidePinnedVisible = false;
    Signal<>::ScopedConnection paletteConn;
    bool suppressItemClick = false; // set on press-release when a hold/drag consumed the gesture

    // Drag-to-reorder state.
    struct DragState {
      bool active = false;         // drag mode is live
      bool armed = false;          // hold timer has fired; drag begins on next motion
      bool pinned = false;         // source item is pinned (only pinned items may be reordered)
      std::size_t sourceIndex = 0; // index into snapshot.items being dragged
      std::size_t targetIndex = 0; // current intended insert position (among pinned items)
      float startMain = 0.0F;      // pointer main-axis position when press was received
      float currentMain = 0.0F;    // current pointer main-axis position
      Timer holdTimer;             // fires after hold delay to arm the drag
    } drag;
  };

  struct DockInstanceDependencies {
    CompositorPlatform& platform;
    ConfigService& config;
    RenderContext& renderContext;
  };

  struct DockInstanceCallbacks {
    std::function<bool(DockInstance&)> syncModel;
    std::function<void(DockInstance&)> rebuildItems;
    std::function<void(DockInstance&)> updateVisuals;
  };

  void prepareFrame(
      DockInstance& instance, DockInstanceDependencies deps, const DockInstanceCallbacks& callbacks, bool needsUpdate,
      bool needsLayout
  );
  void buildScene(DockInstance& instance, DockInstanceDependencies deps, const DockInstanceCallbacks& callbacks);
  void resizeSurface(DockInstance& instance, const DockConfig& cfg, const ShellConfig::ShadowConfig& shadowConfig);
  void applyPanelPalette(DockInstance& instance, const DockConfig& cfg);
  void syncDockSlideLayerTransform(DockInstance& instance, const DockConfig& cfg);
  void applyDockCompositorBlur(DockInstance& instance, const DockConfig& cfg);
  void startHideFadeOut(DockInstance& instance, ConfigService& config);
  void revealAutoHideDock(DockInstance& instance, ConfigService& config);

} // namespace shell::dock
