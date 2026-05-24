#pragma once

#include "compositors/compositor_platform.h"
#include "render/animation/animation_manager.h"
#include "shell/bar/widget.h"
#include "ui/palette.h"

#include <cstdint>
#include <optional>
#include <string>
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

  WorkspacesWidget(
      CompositorPlatform& platform, wl_output* output, DisplayMode displayMode, ColorSpec focusedColor,
      ColorSpec occupiedColor, ColorSpec emptyColor, std::size_t maxLabelChars, bool hideWhenEmpty, float pillScale,
      bool minimal
  );
  ~WorkspacesWidget() override;

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void computeTargets();
  void retarget(Renderer& renderer);
  void updateContainerSize();
  void startAnimation();
  void cancelAnimation();
  void applyItemLayout(std::size_t i);
  [[nodiscard]] float workspacePillRadius(float width, float height) const noexcept;
  [[nodiscard]] std::optional<std::size_t> activeWorkspaceIndex() const;
  void activateAdjacentWorkspace(int direction);

  [[nodiscard]] static std::optional<std::size_t> numericWorkspaceId(const Workspace& workspace);
  [[nodiscard]] std::string workspaceLabel(const Workspace& workspace, std::size_t displayIndex) const;
  [[nodiscard]] DisplayMode effectiveDisplayMode() const noexcept;
  void syncWidgetVisibility(bool showWidget);

  struct Item {
    InputArea* area = nullptr;
    Box* indicator = nullptr;
    Label* text = nullptr;
    std::string label;
    bool showLabel = false;
    bool active = false;
    float inactiveWidth = 0.0f;
    float activeWidth = 0.0f;
    float inkCenterOffset = 0.0f;
    float inkVCenterOffset = 0.0f;
    float fromX = 0.0f;
    float fromWidth = 0.0f;
    float targetX = 0.0f;
    float targetWidth = 0.0f;
    float currentX = 0.0f;
    float currentWidth = 0.0f;
  };

  [[nodiscard]] ColorSpec workspaceFillColor(const Workspace& workspace) const;
  [[nodiscard]] ColorSpec workspaceTextColor(const Workspace& workspace) const;
  [[nodiscard]] static ColorRole onRoleForFill(ColorRole fill);
  [[nodiscard]] static ColorSpec readableColorForFill(const ColorSpec& fill);

  CompositorPlatform& m_platform;
  wl_output* m_output = nullptr;
  DisplayMode m_displayMode = DisplayMode::None;
  std::size_t m_maxLabelChars = 1;
  bool m_hideWhenEmpty = false;
  float m_pillScale = 1.0f;
  bool m_minimal = false;
  Node* m_container = nullptr;
  std::vector<Workspace> m_cachedState;
  std::vector<Item> m_items;
  bool m_rebuildPending = true;
  std::uint64_t m_textMetricsGeneration = 0;

  float m_gap = 0.0f;
  float m_indicatorHeight = 0.0f;
  bool m_isVertical = false;

  AnimationManager::Id m_animId = 0;
  ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  ColorSpec m_emptyColor = colorSpecFromRole(ColorRole::Secondary);
};
