#pragma once

#include "compositors/compositor_platform.h"
#include "render/animation/animation_manager.h"
#include "shell/bar/widget.h"
#include "ui/palette.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Box;
class InputArea;
class Label;

class WorkspacesWidget : public Widget {
public:
  enum class DisplayMode : std::uint8_t {
    None,
    Id,
    Name,
  };

  struct Options {
    DisplayMode displayMode = DisplayMode::Id;
    ColorSpec focusedColor = colorSpecFromRole(ColorRole::Primary);
    ColorSpec occupiedColor = colorSpecFromRole(ColorRole::Secondary);
    ColorSpec emptyColor = colorSpecFromRole(ColorRole::Secondary);
    std::size_t maxLabelChars = 1;
    bool labelsOnlyWhenOccupied = false;
    bool hideWhenEmpty = false;
    float pillScale = 1.0f;
    float activePillSize = 2.2f;
    float inactivePillSize = 1.0f;
    bool minimal = false;
    bool focusedOutputOnly = false;
  };

  WorkspacesWidget(CompositorPlatform& platform, wl_output* output, Options options);
  ~WorkspacesWidget() override;

  void create() override;
  [[nodiscard]] bool wantsBarHoverHighlight() const noexcept override { return false; }

private:
  struct Item;

  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void computeTargets();
  void updateItemFlowPositions();
  void retarget(Renderer& renderer);
  void updateContainerSize();
  void startAnimation();
  void cancelAnimation();
  void finishAnimation();
  void applyItemLayout(Item& item);
  void applyItemLayouts();
  void snapshotItemsForRebuild();
  void scheduleRebuildFromSnapshot();
  [[nodiscard]] float workspacePillRadius(float width, float height) const noexcept;
  [[nodiscard]] float workspaceMainAxisMinWidth(float baseSize, bool active) const noexcept;
  [[nodiscard]] std::optional<std::size_t> activeWorkspaceIndex() const;
  void activateAdjacentWorkspace(int direction);

  [[nodiscard]] static std::optional<std::size_t> numericWorkspaceId(const Workspace& workspace);
  [[nodiscard]] std::string workspaceLabel(const Workspace& workspace, std::size_t displayIndex) const;
  [[nodiscard]] bool shouldShowWorkspaceLabel(const Workspace& workspace, std::string_view label) const noexcept;
  [[nodiscard]] DisplayMode effectiveDisplayMode() const noexcept;
  [[nodiscard]] bool isWorkspaceHidden(const Workspace& workspace) const noexcept;
  void syncWidgetVisibility(bool showWidget);
  void recalculateItemMetrics(Renderer& renderer, Item& item, const Workspace& workspace, std::size_t displayIndex);
  void ensureItemLabel(Renderer& renderer, Item& item, const Workspace& workspace);
  void setWorkspaceClickHandler(InputArea& area, const Workspace& workspace);
  void applyItemVisualStyle(Item& item);
  void updateHoverOverlay();
  [[nodiscard]] static bool shouldHoldPreviousVisualWorkspace(
      const Workspace& previousVisualWorkspace, const Workspace& currentWorkspace
  ) noexcept;
  [[nodiscard]] bool releaseHeldVisualStyles();

  struct Item {
    InputArea* area = nullptr;
    Box* indicator = nullptr;
    Label* text = nullptr;
    Workspace workspace;
    Workspace visualWorkspace;
    std::string key;
    std::string label;
    bool showLabel = false;
    bool active = false;
    bool exiting = false;
    bool releaseVisualAfterAnimation = false;
    float inactiveWidth = 0.0f;
    float activeWidth = 0.0f;
    float fromWidth = 0.0f;
    float targetX = 0.0f;
    float targetWidth = 0.0f;
    float currentX = 0.0f;
    float currentWidth = 0.0f;
    float fromOpacity = 1.0f;
    float targetOpacity = 1.0f;
    float currentOpacity = 1.0f;
  };

  struct ItemSnapshot {
    std::string key;
    Workspace workspace;
    std::string label;
    bool showLabel = false;
    float width = 0.0f;
    float opacity = 1.0f;
  };

  [[nodiscard]] ColorSpec workspaceFillColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceTextColor(const Workspace& workspace) const;
  [[nodiscard]] static ColorRole onRoleForFill(ColorRole fill);
  [[nodiscard]] static ColorSpec readableColorForFill(const ColorSpec& fill);
  [[nodiscard]] bool isFocusedOutput() const;

  CompositorPlatform& m_platform;
  wl_output* m_output = nullptr;
  DisplayMode m_displayMode = DisplayMode::None;
  std::size_t m_maxLabelChars = 1;
  bool m_labelsOnlyWhenOccupied = false;
  bool m_hideWhenEmpty = false;
  float m_pillScale = 1.0f;
  float m_activePillSize = 2.2f;
  float m_inactivePillSize = 1.0f;
  bool m_minimal = false;
  bool m_focusedOutputOnly = false;
  bool m_wasFocusedOutput = true;
  bool m_activeUsesFocusedColor = true;
  Node* m_container = nullptr;
  std::vector<Workspace> m_cachedState;
  std::vector<Item> m_items;
  std::vector<ItemSnapshot> m_rebuildSnapshot;
  bool m_rebuildPending = true;
  std::uint64_t m_textMetricsGeneration = 0;

  float m_gap = 0.0f;
  float m_indicatorHeight = 0.0f;
  Box* m_hoverOverlay = nullptr;
  float m_hoverProgress = 0.0f;
  InputArea* m_hoveredArea = nullptr;
  bool m_isVertical = false;

  AnimationManager::Id m_animId = 0;
  ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_emptyColor = colorSpecFromRole(ColorRole::Secondary);
};
