#pragma once

#include "config/config_types.h"
#include "core/ui_phase.h"
#include "ipc/ipc_invocation_context.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/bar/widget_action.h"
#include "shell/bar/widget_gesture.h"
#include "ui/palette.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

class AnimationManager;
class Box;
class InputArea;
struct PointerEvent;

namespace noctalia::bar {
  class WidgetActionDispatcher;
}

class Widget {
public:
  // Whether a panel action opens the panel or toggles it.
  enum class PanelActivation : std::uint8_t { Toggle, Open };

  using UpdateCallback = std::function<void()>;
  using RedrawCallback = std::function<void()>;
  using FrameTickRequestCallback = std::function<void()>;
  using PanelToggleCallback = std::function<void(
      std::string_view panelId, std::string_view context, std::optional<float> anchorSurfaceX,
      std::optional<float> anchorSurfaceY, PanelActivation activation
  )>;

  virtual ~Widget();

  virtual void create() = 0;
  void layout(Renderer& renderer, float containerWidth, float containerHeight) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    doLayout(renderer, containerWidth, containerHeight);
    syncOuterFromRoot();
  }
  void update(Renderer& renderer) {
    UiPhaseScope updatePhase(UiPhase::Update);
    doUpdate(renderer);
    syncOuterFromRoot();
  }
  virtual void onFrameTick(float deltaMs) { (void)deltaMs; }
  [[nodiscard]] virtual bool needsFrameTick() const { return false; }
  [[nodiscard]] virtual bool onPointerEvent(const PointerEvent& event) {
    (void)event;
    return false;
  }

  [[nodiscard]] virtual bool noGapAroundMe() const noexcept { return false; }
  // Layout-only or non-interactive bar widgets: clicks pass through to bar dead-zone handlers.
  [[nodiscard]] virtual bool isBarClickThrough() const noexcept { return m_nonInteractive; }
  // Widgets with interactive sub-items (workspaces, taskbar, tray) manage hover per item and
  // opt out of the bar's generic whole-widget hover highlight.
  [[nodiscard]] virtual bool wantsBarHoverHighlight() const noexcept { return !m_nonInteractive; }

  // The node this widget built. Widgets size and position it in doLayout().
  [[nodiscard]] Node* root() const noexcept { return m_innerRoot; }
  // The node the bar mounts: a transparent InputArea wrapping root(), carrying gesture actions.
  [[nodiscard]] Node* outerNode() const noexcept { return m_outer ? m_outer.get() : m_outerPtr; }
  [[nodiscard]] float width() const noexcept;
  [[nodiscard]] float height() const noexcept;

  std::unique_ptr<Node> releaseRoot();

  // Merges the four binding layers and installs handlers on the gesture area. Re-runs on reload.
  void resolveGestureBindings(
      std::string_view widgetType, const WidgetConfig* widgetConfig,
      const noctalia::bar::WidgetActionBindings::ActionTable* barActions, std::string_view barContext,
      const noctalia::bar::WidgetActionDispatcher* dispatcher
  );
  void setActionContext(IpcInvocationContext context) { m_actionContext = std::move(context); }
  [[nodiscard]] const noctalia::bar::WidgetActionBindings& gestureBindings() const noexcept {
    return m_gestureBindings;
  }

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
  void setNonInteractive(bool nonInteractive) noexcept;
  [[nodiscard]] bool nonInteractive() const noexcept { return m_nonInteractive; }
  // Bar-owned: blocks all pointer input while the member is clipped out of a collapsed accordion.
  void setBarPointerSuppressed(bool suppressed) noexcept;
  [[nodiscard]] bool barPointerSuppressed() const noexcept { return m_barPointerSuppressed; }
  [[nodiscard]] const WidgetBarCapsuleSpec& barCapsuleSpec() const noexcept { return m_barCapsuleSpec; }
  void setBarCapsuleScene(Node* shell, Box* box) noexcept;
  [[nodiscard]] Node* barCapsuleShell() const noexcept { return m_capsuleShell; }
  [[nodiscard]] Box* barCapsuleBox() const noexcept { return m_capsuleBox; }
  void setBarHoverBox(Box* box) noexcept { m_hoverBox = box; }
  [[nodiscard]] Box* barHoverBox() const noexcept { return m_hoverBox; }
  void setBarHoverProgress(float progress) noexcept { m_hoverProgress = progress; }
  [[nodiscard]] float barHoverProgress() const noexcept { return m_hoverProgress; }
  // Outermost node for flex layout / anchor alignment (capsule shell when enabled).
  [[nodiscard]] Node* layoutBoundsNode() const noexcept {
    return m_capsuleShell != nullptr ? m_capsuleShell : outerNode();
  }
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
      std::optional<float> anchorSurfaceY = std::nullopt, PanelActivation activation = PanelActivation::Toggle
  );
  void setRoot(std::unique_ptr<Node> root);
  void clearReleasedRoot() noexcept {
    m_outerPtr = nullptr;
    m_innerRoot = nullptr;
    m_gestureArea = nullptr;
  }
  // Runs the action bound to `gesture`, if any. Returns whether it was handled.
  bool dispatchGesture(noctalia::bar::Gesture gesture);
  // Called just before a bound action runs, so a widget can snapshot state for an optimistic
  // update. Match on `action` when the update only makes sense for one verb.
  virtual void onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) {
    (void)gesture;
    (void)action;
  }
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
  bool m_nonInteractive = false;
  bool m_barPointerSuppressed = false;
  Node* m_capsuleShell = nullptr;
  Box* m_capsuleBox = nullptr;
  Box* m_hoverBox = nullptr;
  float m_hoverProgress = 0.0f;

private:
  void installGestureHandlers();
  [[nodiscard]] bool bindingRepeatsEveryScrollStep(noctalia::bar::Gesture gesture) const;
  // An enabled InputArea captures hover (and the highlight) even with no accepted buttons, so the
  // wrapper stays inert until something is actually bound to it.
  void updateGestureAreaEnabled() noexcept;
  void syncOuterHitTestVisible() noexcept;
  // The wrapper carries no geometry or visibility of its own; it mirrors root(), which widgets
  // size in doLayout() and hide in doUpdate() (hide_when_no_media, hide_when_off, ...).
  void syncOuterFromRoot() noexcept;

  // m_outer owns m_innerRoot as its only child until releaseRoot() hands it to the bar.
  std::unique_ptr<Node> m_outer;
  Node* m_outerPtr = nullptr;
  Node* m_innerRoot = nullptr;
  InputArea* m_gestureArea = nullptr;
  // The widget's own root area and what it claimed before any binding was applied.
  InputArea* m_innerArea = nullptr;
  std::uint32_t m_innerBaseButtons = 0;
  std::uint32_t m_innerBaseScrollDirections = 0;
  noctalia::bar::WidgetActionBindings m_gestureBindings;
  noctalia::bar::ScrollRepeatMode m_scrollRepeatMode = noctalia::bar::ScrollRepeatMode::Auto;
  const noctalia::bar::WidgetActionDispatcher* m_actionDispatcher = nullptr;
  IpcInvocationContext m_actionContext;
};
