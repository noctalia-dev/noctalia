#pragma once

#include "config/config_types.h"
#include "core/ui_phase.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class AnimationManager;
class Box;
struct PointerEvent;

class Widget {
public:
  using UpdateCallback = std::function<void()>;
  using RedrawCallback = std::function<void()>;
  using FrameTickRequestCallback = std::function<void()>;
  using PanelToggleCallback = std::function<void(
      std::string_view panelId, std::string_view context, std::optional<float> anchorSurfaceX,
      std::optional<float> anchorSurfaceY
  )>;

  virtual ~Widget();

  virtual void create() = 0;
  void layout(Renderer& renderer, float containerWidth, float containerHeight) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    doLayout(renderer, containerWidth, containerHeight);
  }
  void update(Renderer& renderer) {
    UiPhaseScope updatePhase(UiPhase::Update);
    doUpdate(renderer);
  }
  virtual void onFrameTick(float deltaMs) { (void)deltaMs; }
  [[nodiscard]] virtual bool needsFrameTick() const { return false; }
  [[nodiscard]] virtual bool onPointerEvent(const PointerEvent& event) {
    (void)event;
    return false;
  }
  [[nodiscard]] virtual bool reservesMiddleClick() const noexcept { return false; }

  [[nodiscard]] virtual bool noGapAroundMe() const noexcept { return false; }
  // Layout-only bar widgets (spacers): clicks pass through to bar dead-zone handlers.
  [[nodiscard]] virtual bool isBarClickThrough() const noexcept { return false; }

  [[nodiscard]] Node* root() const noexcept { return m_root ? m_root.get() : m_rootPtr; }
  [[nodiscard]] float width() const noexcept;
  [[nodiscard]] float height() const noexcept;

  std::unique_ptr<Node> releaseRoot();

  void setAnimationManager(AnimationManager* mgr) noexcept;
  void setUpdateCallback(UpdateCallback callback);
  void setRedrawCallback(RedrawCallback callback);
  void setFrameTickRequestCallback(FrameTickRequestCallback callback);
  void setPanelToggleCallback(PanelToggleCallback callback);
  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  void setLabelFontWeight(FontWeight fontWeight) noexcept { m_labelFontWeight = fontWeight; }
  [[nodiscard]] FontWeight labelFontWeight() const noexcept { return m_labelFontWeight; }
  void setLabelFontFamily(std::string family) noexcept { m_labelFontFamily = std::move(family); }
  [[nodiscard]] const std::string& labelFontFamily() const noexcept { return m_labelFontFamily; }
  void setConfigName(std::string name) { m_configName = std::move(name); }
  [[nodiscard]] std::string_view configName() const noexcept { return m_configName; }
  void setAnchor(bool anchor) noexcept { m_anchor = anchor; }
  [[nodiscard]] bool isAnchor() const noexcept { return m_anchor; }

  void setBarCapsuleSpec(WidgetBarCapsuleSpec spec) noexcept { m_barCapsuleSpec = std::move(spec); }
  void setWidgetForeground(std::optional<ColorSpec> color) noexcept { m_widgetForeground = color; }
  void setWidgetIconColor(std::optional<ColorSpec> color) noexcept { m_widgetIconColor = color; }
  [[nodiscard]] const WidgetBarCapsuleSpec& barCapsuleSpec() const noexcept { return m_barCapsuleSpec; }
  void setBarCapsuleScene(Node* shell, Box* box) noexcept;
  [[nodiscard]] Node* barCapsuleShell() const noexcept { return m_capsuleShell; }
  [[nodiscard]] Box* barCapsuleBox() const noexcept { return m_capsuleBox; }
  // Outermost node for flex layout / anchor alignment (capsule shell when enabled).
  [[nodiscard]] Node* layoutBoundsNode() const noexcept { return m_capsuleShell != nullptr ? m_capsuleShell : root(); }
  [[nodiscard]] float resolvedBarCapsuleRadius(float width, float height) const noexcept;

  // Whether the bar should paint the decorative capsule for this frame (spec enabled + visible ink).
  [[nodiscard]] virtual bool shouldShowBarCapsule() const;

  // Resolved primary label color: `[widget.*] color` when set, else `capsule_foreground` when capsule styling is
  // enabled, else `fallback` (e.g. colorSpecFromRole(OnSurface)).
  [[nodiscard]] ColorSpec widgetForegroundOr(const ColorSpec& fallback) const noexcept;
  // Resolved icon color: `[widget.*] icon_color` when set, else the foreground chain
  // (`color` → `capsule_foreground` → `fallback`), so a bare `color` still tints icons.
  [[nodiscard]] ColorSpec widgetIconColorOr(const ColorSpec& fallback) const noexcept;

protected:
  void requestUpdate();
  void requestRedraw();
  void requestFrameTick();
  void requestPanelToggle(
      std::string_view panelId, std::string_view context = {}, std::optional<float> anchorSurfaceX = std::nullopt,
      std::optional<float> anchorSurfaceY = std::nullopt
  );
  void setRoot(std::unique_ptr<Node> root) { m_root = std::move(root); }
  void clearReleasedRoot() noexcept { m_rootPtr = nullptr; }
  virtual void doLayout(Renderer& renderer, float containerWidth, float containerHeight) = 0;
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }

  float m_contentScale = 1.0f;
  FontWeight m_labelFontWeight = FontWeight::Medium;
  std::string m_labelFontFamily; // empty = inherit renderer-global family
  std::string m_configName;
  bool m_anchor = false;
  AnimationManager* m_animations = nullptr;
  UpdateCallback m_updateCallback;
  RedrawCallback m_redrawCallback;
  FrameTickRequestCallback m_frameTickRequestCallback;
  PanelToggleCallback m_panelToggleCallback;
  WidgetBarCapsuleSpec m_barCapsuleSpec{};
  std::optional<ColorSpec> m_widgetForeground;
  std::optional<ColorSpec> m_widgetIconColor;
  Node* m_capsuleShell = nullptr;
  Box* m_capsuleBox = nullptr;

private:
  std::unique_ptr<Node> m_root;
  Node* m_rootPtr = nullptr;
};
