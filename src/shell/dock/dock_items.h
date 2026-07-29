#pragma once

#include "render/animation/animation_manager.h"
#include "system/desktop_entry.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Box;
class ConfigService;
class Flex;
class Glyph;
class IconResolver;
class Image;
class InputArea;
class Label;
class RenderContext;
struct DockConfig;

namespace shell::dock {

  struct DockInstance;
  struct DockSnapshot;

  struct DockItemView {
    InputArea* area = nullptr;
    std::array<Box*, 3> dotIndicators{};
    Box* badge = nullptr;
    Label* badgeLabel = nullptr;
    Image* iconImage = nullptr;
    Glyph* iconGlyph = nullptr;
    float restMainPos = 0.0f;
    float restCrossPos = 0.0f;
    float hoverMainOffset = 0.0f;
    float dragMainOffset = 0.0f; // additional offset applied while dragging this item
    bool isDragGhost = false;    // drawn at reduced opacity as the placeholder slot
    float visualScale = -1.0f;
    float visualOpacity = -1.0f;
    AnimationManager::Id scaleAnimId = 0;
    AnimationManager::Id opacityAnimId = 0;
  };

  struct DockItemAction {
    DesktopEntry entry;
    std::string idLower;
    std::string startupWmClassLower;
    std::string windowLookupIdLower;
    std::string windowLookupWmClassLower;
  };

  struct DockItemModelDependencies {
    ConfigService& config;
  };

  struct DockItemSceneDependencies {
    DockItemModelDependencies model;
    RenderContext& renderContext;
    IconResolver& iconResolver;
  };

  struct DockItemCallbacks {
    std::function<void(DockInstance&, const DockItemAction&)> activateOrLaunch;
    std::function<void(DockInstance&)> toggleLauncher;
    std::function<void(DockInstance&, const DockItemAction&)> openItemMenu;
    // Drag-to-reorder: only called when the source item is pinned.
    std::function<void(DockInstance&, std::size_t itemIndex, float mainPos)> beginDrag;
    std::function<void(DockInstance&, float mainPos)> updateDrag;
    std::function<void(DockInstance&, bool commit)> endDrag;
  };

  [[nodiscard]] std::string_view dockLauncherIconGlyph(const DockConfig& cfg);
  [[nodiscard]] std::unique_ptr<Flex> makeDockItemRow(const DockConfig& cfg, bool vertical);
  void rebuildItems(
      DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot,
      const DockItemCallbacks& callbacks
  );
  void updateVisuals(DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot);
  [[nodiscard]] bool
  updateHoverZoom(DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot, float deltaMs);
  [[nodiscard]] bool
  syncHoverPointerFromScene(DockInstance& instance, const DockConfig& cfg, float sceneX, float sceneY);
  void clearHoverZoom(DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot);
  void syncDockItemRestPositions(DockInstance& instance, const DockConfig& cfg);
  void applyDragVisuals(DockInstance& instance, const DockConfig& cfg);
  void clearDragVisuals(DockInstance& instance, const DockConfig& cfg);
  void dismissDockTooltip();
  [[nodiscard]] std::size_t computeDragTargetIndex(const DockInstance& instance, const DockConfig& cfg, float mainPos);

} // namespace shell::dock
