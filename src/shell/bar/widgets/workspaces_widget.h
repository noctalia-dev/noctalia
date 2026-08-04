#pragma once

#include "compositors/compositor_platform.h"
#include "render/animation/animation_manager.h"
#include "shell/bar/widget.h"
#include "system/icon_resolver.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Box;
class ConfigService;
class Image;
class InputArea;
class Label;

enum class WorkspacesStyle : std::uint8_t {
  Regular,
  Minimal,
  FocusHint,
};

enum class WorkspacesLabelSource : std::uint8_t {
  Id,
  Name,
};

class WorkspacesWidget : public Widget {
public:
  struct Options {
    WorkspacesStyle style = WorkspacesStyle::Regular;
    WorkspacesLabelSource labelSource = WorkspacesLabelSource::Id;
    bool showLabels = true;
    ColorSpec focusedColor = colorSpecFromRole(ColorRole::Primary);
    ColorSpec occupiedColor = colorSpecFromRole(ColorRole::Secondary);
    ColorSpec emptyColor = colorSpecFromRole(ColorRole::Secondary);
    ColorSpec urgentColor = colorSpecFromRole(ColorRole::Error);
    bool changeColorOnHover = true;
    std::size_t maxLabelChars = 1;
    bool labelsOnlyWhenOccupied = false;
    bool hideWhenEmpty = false;
    float pillScale = 1.0F;
    float activePillSize = 2.2F;
    float inactivePillSize = 1.0F;
    bool focusedOutputOnly = false;
  };

  WorkspacesWidget(CompositorPlatform& platform, ConfigService& config, wl_output* output, Options options);
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

  [[nodiscard]] static std::optional<std::size_t> numericWorkspaceId(const Workspace& workspace);
  [[nodiscard]] std::string workspaceLabel(const Workspace& workspace, std::size_t displayIndex) const;
  [[nodiscard]] std::string activeWindowAppId() const;
  [[nodiscard]] std::string resolveIconPath(const std::string& appId);
  [[nodiscard]] float focusedPillIconSize() const noexcept;
  [[nodiscard]] float focusedPillDotSize() const noexcept;
  [[nodiscard]] float focusedPillActiveMainAxisSize(
      float textWidth, float textHeight, bool showLabel, bool hasIcon, float baseSize, float padding
  ) const noexcept;
  void buildDesktopIconIndex();
  void syncActiveWindowIcon(Renderer& renderer, Item& item);
  [[nodiscard]] bool shouldShowWorkspaceLabel(const Workspace& workspace, std::string_view label) const noexcept;
  [[nodiscard]] bool isMinimal() const noexcept { return m_style == WorkspacesStyle::Minimal; }
  [[nodiscard]] bool isFocusHint() const noexcept { return m_style == WorkspacesStyle::FocusHint; }
  [[nodiscard]] bool isWorkspaceHidden(const Workspace& workspace) const noexcept;
  void syncWidgetVisibility(bool showWidget);
  void recalculateItemMetrics(Renderer& renderer, Item& item, const Workspace& workspace, std::size_t displayIndex);
  void ensureItemLabel(Renderer& renderer, Item& item, const Workspace& workspace);
  void setWorkspaceClickHandler(InputArea& area, const Workspace& workspace);
  void applyItemVisualStyle(Item& item);
  void updateHoverOverlay();
  [[nodiscard]] bool shouldHoldPreviousVisualWorkspace(
      const Workspace& previousVisualWorkspace, const Workspace& currentWorkspace
  ) const noexcept;
  [[nodiscard]] bool releaseHeldVisualStyles();

  struct Item {
    InputArea* area = nullptr;
    Box* indicator = nullptr;
    Label* text = nullptr;
    Image* icon = nullptr;
    Workspace workspace;
    Workspace visualWorkspace;
    std::string key;
    std::string label;
    std::string iconPath;
    bool showLabel = false;
    bool showIcon = false;
    bool active = false;
    bool exiting = false;
    bool releaseVisualAfterAnimation = false;
    float inactiveWidth = 0.0F;
    float activeWidth = 0.0F;
    float fromWidth = 0.0F;
    float targetX = 0.0F;
    float targetWidth = 0.0F;
    float currentX = 0.0F;
    float currentWidth = 0.0F;
    float fromOpacity = 1.0F;
    float targetOpacity = 1.0F;
    float currentOpacity = 1.0F;
  };

  struct ItemSnapshot {
    std::string key;
    Workspace workspace;
    std::string label;
    bool showLabel = false;
    float width = 0.0F;
    float opacity = 1.0F;
  };

  [[nodiscard]] ColorSpec workspaceFillColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceTextColor(const Workspace& workspace) const;
  [[nodiscard]] static ColorRole onRoleForFill(ColorRole fill);
  [[nodiscard]] static ColorSpec readableColorForFill(const ColorSpec& fill);
  [[nodiscard]] bool isFocusedOutput() const;

  CompositorPlatform& m_platform;
  ConfigService& m_configService;
  wl_output* m_output = nullptr;
  WorkspacesLabelSource m_labelSource = WorkspacesLabelSource::Id;
  bool m_showLabels = true;
  std::size_t m_maxLabelChars = 1;
  bool m_labelsOnlyWhenOccupied = false;
  bool m_hideWhenEmpty = false;
  float m_pillScale = 1.0F;
  float m_activePillSize = 2.2F;
  float m_inactivePillSize = 1.0F;
  WorkspacesStyle m_style = WorkspacesStyle::Regular;
  bool m_focusedOutputOnly = false;
  bool m_changeColorOnHover = true;
  bool m_wasFocusedOutput = true;
  bool m_activeUsesFocusedColor = true;
  std::string m_cachedActiveWindowAppId;
  IconResolver m_iconResolver;
  std::unordered_map<std::string, std::string> m_appIcons;
  std::uint64_t m_desktopEntriesVersion = 0;
  Node* m_container = nullptr;
  std::vector<Workspace> m_cachedState;
  std::vector<Item> m_items;
  std::vector<ItemSnapshot> m_rebuildSnapshot;
  bool m_rebuildPending = true;
  bool m_iconColorizeRefreshPending = false;
  std::uint64_t m_textMetricsGeneration = 0;
  Signal<>::ScopedConnection m_appIconColorizeConn;

  float m_gap = 0.0F;
  float m_indicatorHeight = 0.0F;
  Box* m_hoverOverlay = nullptr;
  float m_hoverProgress = 0.0F;
  InputArea* m_hoveredArea = nullptr;
  bool m_isVertical = false;

  AnimationManager::Id m_animId = 0;
  ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_emptyColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_urgentColor = colorSpecFromRole(ColorRole::Error);
};
